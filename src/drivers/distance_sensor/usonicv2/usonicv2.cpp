/****************************************************************************
 *
 *   Copyright (c) 2020 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/


/**
 * @file USONICV2.cpp
 * @author Andrew McFarland <andrew@steamfoundry.ca
 *
 * Interface for the SeeedStudio Ultrasonic Ranger v2.0 (or SRF05 in single GPIO mode)
 * Driver based on the SRF05 modified to use a single GPIO.   Should be connected to the TRIGGER pin
 *
 * IMPORTANT - sensor sec says it needs a 10uSec pulse to turn it on.
 * Instead of wasting CPU on sleep, trying this by setting the GPIO high from the start,
 * then just setting it low to start the reading.  This should work I hope.
 */

#include "usonicv2.hpp"

// REMOVEME!!!!
#define HAVE_ULTRASOUND
//

#if defined(HAVE_ULTRASOUND)

#include <px4_arch/micro_hal.h>

USONICV2::USONICV2(const uint8_t rotation) :
	ScheduledWorkItem(MODULE_NAME, px4::wq_configurations::hp_default),
	_px4_rangefinder(0 /* no device type for GPIO input */, rotation)
{
	_px4_rangefinder.set_device_type(DRV_DIST_DEVTYPE_SRF05);   /*not setting it's own device type may be bad.   We'll see*/
	_px4_rangefinder.set_rangefinder_type(distance_sensor_s::MAV_DISTANCE_SENSOR_ULTRASOUND);
	_px4_rangefinder.set_min_distance(USONICV2_MIN_DISTANCE);
	_px4_rangefinder.set_max_distance(USONICV2_MAX_DISTANCE);
	_px4_rangefinder.set_fov(0.261799); // 15 degree FOV
}

USONICV2::~USONICV2()
{
	stop();
	perf_free(_sample_perf);
	perf_free(_comms_errors);
	perf_free(_sensor_resets);
}

void USONICV2::OnEdge(bool state)
{
	const hrt_abstime now = hrt_absolute_time();

	if (_state == STATE::WAIT_FOR_RISING || _state == STATE::WAIT_FOR_FALLING) {
		//PX4_INFO("Edge detected");
		if (state) {
			_rising_edge_time = now;
			_state = STATE::WAIT_FOR_FALLING;

		} else {
			_falling_edge_time = now;
			_state = STATE::SAMPLE;
			ScheduleNow();

		}
	}
}

int USONICV2::EchoInterruptCallback(int irq, void *context, void *arg)
{
	static_cast<USONICV2 *>(arg)->OnEdge(px4_arch_gpioread(GPIO_ULTRASOUND_ECHO));
	return 0;
}

int
USONICV2::init()
{
	px4_arch_configgpio(GPIO_ULTRASOUND_ECHO);
	px4_arch_gpiowrite(GPIO_ULTRASOUND_ECHO, 0);
	px4_arch_gpiosetevent(GPIO_ULTRASOUND_ECHO, true, true, false, &EchoInterruptCallback, this);
	_state = STATE::TRIGGER;
	ScheduleOnInterval(get_measure_interval());
	return PX4_OK;
}

void USONICV2::stop()
{

	_state = STATE::EXIT;
	px4_arch_gpiosetevent(GPIO_ULTRASOUND_ECHO, false, false, false, nullptr, nullptr);
	px4_arch_gpiowrite(GPIO_ULTRASOUND_ECHO, 1);
	ScheduleClear();
}

void
USONICV2::Run()
{
	if (should_exit()) {
		ScheduleClear();
		exit_and_cleanup();
		return;
	}

	switch (_state) {

	case STATE::TRIGGER: {
			px4_arch_gpiowrite(GPIO_ULTRASOUND_ECHO, 0);
			px4_usleep(2);
			px4_arch_gpiowrite(GPIO_ULTRASOUND_ECHO, 1);
			px4_usleep(10);
			px4_arch_gpiowrite(GPIO_ULTRASOUND_ECHO, 0);
			_falling_trigger_time  = hrt_absolute_time();
			_state = STATE::WAIT_FOR_RISING; //State switch after should beat the race condition with GPIO event triggering OnEdge()
			break;
		}

	case STATE::WAIT_FOR_RISING: {
			const hrt_abstime now = hrt_absolute_time();
			const hrt_abstime dt = now - _falling_trigger_time;

			/*Watch for timeout and reset the trigger*/
			if (dt >= USONICV2_CONVERSION_TIMEOUT) {
				perf_count(
					_sensor_resets); //This might be because there wasn't any answer the distance was too high.  Use common sense here
				/* DEBUGGING Logging*/
				PX4_INFO("usonicv2 No response to trigger");
				/* set trigger for next attempt*/
				_state = STATE::TRIGGER;

			}

			break;
		}

	case STATE::WAIT_FOR_FALLING:
		/*Watch for timeout and reset the trigger*/
		{
			perf_count(_sensor_resets);
			/* DEBUGGING Logging*/
			PX4_INFO("Output stayed high");
			/* set trigger for next attempt*/
			_state = STATE::TRIGGER;
			break;
		}

	case STATE::SAMPLE: {
			_state = STATE::MEASURE;
			measure();
			_state = STATE::TRIGGER;
			break;
		}

	case STATE::EXIT:
	default:
		break;
	}
}

int
USONICV2::measure()
{
	perf_begin(_sample_perf);

	const hrt_abstime timestamp_sample = hrt_absolute_time();
	const hrt_abstime dt = _falling_edge_time - _rising_edge_time;

	const float current_distance = dt *  343.0f / 1e6f / 2.0f;

	if (dt > USONICV2_CONVERSION_TIMEOUT) {
		perf_count(_comms_errors);
		PX4_INFO("usonicv2 conversion timeout");

	} else {
		_px4_rangefinder.update(timestamp_sample, current_distance);
	}

	perf_end(_sample_perf);
	return PX4_OK;
}

void
USONICV2::printState()
{
	printf("Line is currently %s\n", px4_arch_gpioread(GPIO_ULTRASOUND_ECHO) ? "true" : "false");
	printf("Trigger time is %lli ,", _falling_trigger_time);
	printf("_rising_edge_time is %lli ,and ", _rising_edge_time);
	printf("_falling_edge_time is %lli \n", _falling_edge_time);
	return;

}

int
USONICV2::testSample()
{
	perf_begin(_sample_perf);
	_state = STATE::MEASURE;
	_falling_trigger_time = 0;
	_rising_edge_time = 0;
	_falling_edge_time = 0;
	printState();
	const hrt_abstime timestamp_sample = hrt_absolute_time();
	px4_arch_gpiowrite(GPIO_ULTRASOUND_ECHO, 0);
	px4_usleep(2);
	printState();
	px4_arch_gpiowrite(GPIO_ULTRASOUND_ECHO, 1);
	printState();
	px4_usleep(10);
	px4_arch_gpiowrite(GPIO_ULTRASOUND_ECHO, 0);
	_falling_trigger_time  = hrt_absolute_time();
	printState();

	while (!px4_arch_gpioread(GPIO_ULTRASOUND_ECHO)) {
		if (hrt_absolute_time() >= _falling_trigger_time + USONICV2_CONVERSION_TIMEOUT) {
			printf("Leading edge not detected\n");
			break;
		}
	}

	_rising_edge_time = hrt_absolute_time();

	while (px4_arch_gpioread(GPIO_ULTRASOUND_ECHO)) {
		if (hrt_absolute_time() >= _falling_trigger_time + USONICV2_CONVERSION_TIMEOUT) {
			printf("Falling edge not detected\n");
			break;;
		}
	}

	printState();
	const hrt_abstime dt = _falling_edge_time - _rising_edge_time;

	float current_distance = dt *  343.0f / 1e6f / 2.0f;
	printf("Time difference: ");
	printf("%lli", dt);
	printf(" \n");

	if (dt > USONICV2_CONVERSION_TIMEOUT) {
		perf_count(_comms_errors);
		PX4_INFO("usonicv2 conversion timeout");
		perf_end(_sample_perf);
		return 0;

	} else {
		_px4_rangefinder.update(timestamp_sample, current_distance);
	}

	perf_end(_sample_perf);
	return PX4_OK;
}

int USONICV2::custom_command(int argc, char *argv[])
{
	return print_usage("unknown command");
}


int USONICV2::task_spawn(int argc, char *argv[])
{

	int ch = 0;
	int myoptind = 1;
	const char *myoptarg = nullptr;

	uint8_t rotation = distance_sensor_s::ROTATION_DOWNWARD_FACING;

	while ((ch = px4_getopt(argc, argv, "R:", &myoptind, &myoptarg)) != EOF) {
		switch (ch) {
		case 'R':
			rotation = (uint8_t)atoi(myoptarg);
			PX4_INFO("Setting usonicv2 orientation to %d", (int)rotation);
			break;

		default:
			return print_usage();
		}
	}

	USONICV2 *instance = new USONICV2(rotation);

	if (instance) {
		_object.store(instance);
		_task_id = task_id_is_work_queue;

		if (instance->init() == PX4_OK) {
			return PX4_OK;
		}

	} else {
		PX4_ERR("alloc failed");
	}

	delete instance;
	_object.store(nullptr);
	_task_id = -1;

	return PX4_ERROR;
}

int USONICV2::print_usage(const char *reason)
{
	if (reason) {
		PX4_WARN("%s\n", reason);
	}

	PRINT_MODULE_DESCRIPTION(
		R"DESCR_STR(
  ### Description

  Driver for SRF05 Mode 2 (Single GPIO mode) and SeeedStudio Ultrasonic Ranger V2.0

  The sensor/driver must be enabled using the parameter SENS_EN_USONICV2.

  )DESCR_STR");

	PRINT_MODULE_USAGE_NAME("USONICV2", "driver");
	PRINT_MODULE_USAGE_SUBCATEGORY("distance_sensor");
	PRINT_MODULE_USAGE_COMMAND_DESCR("start", "Start driver");
	PRINT_MODULE_USAGE_PARAM_INT('R', 25, 0, 25, "Sensor rotation - downward facing by default", true);
	PRINT_MODULE_USAGE_COMMAND_DESCR("status", "Print driver status information");
	PRINT_MODULE_USAGE_COMMAND_DESCR("stop", "Stop driver");
	PRINT_MODULE_USAGE_DEFAULT_COMMANDS();
	return PX4_OK;
}

int
USONICV2::print_status()
{
	perf_print_counter(_sample_perf);
	perf_print_counter(_comms_errors);
	perf_print_counter(_sensor_resets);
	printf("poll interval:  %" PRIu32 " \n", get_measure_interval());
	printf("Test measurement\n");

	if (testSample() == PX4_OK) {
		printf("\nTest worked");

	} else {
		printf("\nTest failed");
	}

	return 0;
}

extern "C" __EXPORT int usonicv2_main(int argc, char *argv[])
{
	return USONICV2::main(argc, argv);
}
#else
# error ("GPIO_ULTRASOUND_ECHO not defined. Driver not supported.");
#endif
