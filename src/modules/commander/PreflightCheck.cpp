/****************************************************************************
*
*   Copyright (c) 2012-2017 PX4 Development Team. All rights reserved.
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
* @file PreflightCheck.cpp
*
* Preflight check for main system components
*
* @author Lorenz Meier <lorenz@px4.io>
* @author Johan Jansen <jnsn.johan@gmail.com>
*/

#include "PreflightCheck.h"
#include "health_flag_helper.h"
#include "rc_check.h"

#include <parameters/param.h>
#include <systemlib/mavlink_log.h>
#include <uORB/Subscription.hpp>
#include <uORB/topics/airspeed.h>
#include <uORB/topics/differential_pressure.h>
#include <uORB/topics/estimator_status.h>
#include <uORB/topics/sensor_accel.h>
#include <uORB/topics/sensor_baro.h>
#include <uORB/topics/sensor_gyro.h>
#include <uORB/topics/sensor_mag.h>
#include <uORB/topics/sensor_preflight.h>
#include <uORB/topics/subsystem_info.h>
#include <uORB/topics/system_power.h>

using namespace time_literals;

namespace Preflight
{

static bool check_calibration(const char *param_template, int32_t device_id)
{
	bool calibration_found = false;

	char s[20];
	int instance = 0;

	/* old style transition: check param values */
	while (!calibration_found) {
		sprintf(s, param_template, instance);
		const param_t parm = param_find_no_notification(s);

		/* if the calibration param is not present, abort */
		if (parm == PARAM_INVALID) {
			break;
		}

		/* if param get succeeds */
		int32_t calibration_devid = -1;

		if (param_get(parm, &calibration_devid) == PX4_OK) {

			/* if the devid matches, exit early */
			if (device_id == calibration_devid) {
				calibration_found = true;
				break;
			}
		}

		instance++;
	}

	return calibration_found;
}

static bool magnometerCheck(orb_advert_t *mavlink_log_pub, vehicle_status_s &status, unsigned instance, bool optional,
			    int32_t &device_id, bool report_fail)
{
	const bool exists = (orb_exists(ORB_ID(sensor_mag), instance) == PX4_OK);
	bool calibration_valid = false;
	bool mag_valid = false;

	if (exists) {

		uORB::Subscription<sensor_mag_s> magnetometer{ORB_ID(sensor_mag), 0, instance};

		mag_valid = (hrt_elapsed_time(&magnetometer.get().timestamp) < 1_s);

		if (!mag_valid) {
			if (report_fail) {
				mavlink_log_critical(mavlink_log_pub, "PREFLIGHT FAIL: MAG #%u invalid", instance);
			}
		}

		device_id = magnetometer.get().device_id;

		calibration_valid = check_calibration("CAL_MAG%u_ID", device_id);

		if (!calibration_valid) {
			if (report_fail) {
				mavlink_log_critical(mavlink_log_pub, "PREFLIGHT FAIL: MAG #%u UNCALIBRATED", instance);
			}
		}

	} else {
		if (!optional && report_fail) {
			mavlink_log_critical(mavlink_log_pub, "PREFLIGHT FAIL: NO MAG SENSOR #%u", instance);
		}
	}

	const bool success = calibration_valid && mag_valid;

	if (instance == 0) {
		set_health_flags(subsystem_info_s::SUBSYSTEM_TYPE_MAG, exists, !optional, success, status);

	} else if (instance == 1) {
		set_health_flags(subsystem_info_s::SUBSYSTEM_TYPE_MAG2, exists, !optional, success, status);
	}

	return success;
}

static bool imuConsistencyCheck(orb_advert_t *mavlink_log_pub, vehicle_status_s &status, bool report_status)
{
	bool success = true; // start with a pass and change to a fail if any test fails
	float test_limit = 1.0f; // pass limit re-used for each test

	// Get sensor_preflight data if available and exit with a fail recorded if not
	int sensors_sub = orb_subscribe(ORB_ID(sensor_preflight));
	sensor_preflight_s sensors = {};

	if (orb_copy(ORB_ID(sensor_preflight), sensors_sub, &sensors) != PX4_OK) {
		goto out;
	}

	// Use the difference between IMU's to detect a bad calibration.
	// If a single IMU is fitted, the value being checked will be zero so this check will always pass.
	param_get(param_find("COM_ARM_IMU_ACC"), &test_limit);

	if (sensors.accel_inconsistency_m_s_s > test_limit) {
		if (report_status) {
			mavlink_log_critical(mavlink_log_pub, "PREFLIGHT FAIL: ACCELS INCONSISTENT - CHECK CAL");
			set_health_flags_healthy(subsystem_info_s::SUBSYSTEM_TYPE_ACC, false, status);
			set_health_flags_healthy(subsystem_info_s::SUBSYSTEM_TYPE_ACC2, false, status);
		}

		success = false;
		goto out;

	} else if (sensors.accel_inconsistency_m_s_s > test_limit * 0.8f) {
		if (report_status) {
			mavlink_log_info(mavlink_log_pub, "PREFLIGHT ADVICE: ACCELS INCONSISTENT - CHECK CAL");
		}
	}

	// Fail if gyro difference greater than 5 deg/sec and notify if greater than 2.5 deg/sec
	param_get(param_find("COM_ARM_IMU_GYR"), &test_limit);

	if (sensors.gyro_inconsistency_rad_s > test_limit) {
		if (report_status) {
			mavlink_log_critical(mavlink_log_pub, "PREFLIGHT FAIL: GYROS INCONSISTENT - CHECK CAL");
			set_health_flags_healthy(subsystem_info_s::SUBSYSTEM_TYPE_GYRO, false, status);
			set_health_flags_healthy(subsystem_info_s::SUBSYSTEM_TYPE_GYRO2, false, status);
		}

		success = false;
		goto out;

	} else if (sensors.gyro_inconsistency_rad_s > test_limit * 0.5f) {
		if (report_status) {
			mavlink_log_info(mavlink_log_pub, "PREFLIGHT ADVICE: GYROS INCONSISTENT - CHECK CAL");
		}
	}

out:
	orb_unsubscribe(sensors_sub);
	return success;
}

// return false if the magnetomer measurements are inconsistent
static bool magConsistencyCheck(orb_advert_t *mavlink_log_pub, vehicle_status_s &status, bool report_status)
{
	// get the sensor preflight data
	int sensors_sub = orb_subscribe(ORB_ID(sensor_preflight));
	struct sensor_preflight_s sensors = {};

	if (orb_copy(ORB_ID(sensor_preflight), sensors_sub, &sensors) != 0) {
		// can happen if not advertised (yet)
		return true;
	}

	orb_unsubscribe(sensors_sub);

	// Use the difference between sensors to detect a bad calibration, orientation or magnetic interference.
	// If a single sensor is fitted, the value being checked will be zero so this check will always pass.
	float test_limit;
	param_get(param_find("COM_ARM_MAG"), &test_limit);

	if (sensors.mag_inconsistency_ga > test_limit) {
		if (report_status) {
			mavlink_log_critical(mavlink_log_pub, "PREFLIGHT FAIL: MAG SENSORS INCONSISTENT");
			set_health_flags_healthy(subsystem_info_s::SUBSYSTEM_TYPE_MAG, false, status);
			set_health_flags_healthy(subsystem_info_s::SUBSYSTEM_TYPE_MAG2, false, status);
		}

		return false;
	}

	return true;
}

static bool accelerometerCheck(orb_advert_t *mavlink_log_pub, vehicle_status_s &status, unsigned instance,
			       bool optional, bool dynamic, int32_t &device_id, bool report_fail)
{
	const bool exists = (orb_exists(ORB_ID(sensor_accel), instance) == PX4_OK);
	bool calibration_valid = false;
	bool accel_valid = true;

	if (exists) {

		uORB::Subscription<sensor_accel_s> accel{ORB_ID(sensor_accel), 0, instance};

		accel_valid = (hrt_elapsed_time(&accel.get().timestamp) < 1_s);

		if (!accel_valid) {
			if (report_fail) {
				mavlink_log_critical(mavlink_log_pub, "PREFLIGHT FAIL: ACCEL #%u invalid", instance);
			}
		}

		device_id = accel.get().device_id;

		calibration_valid = check_calibration("CAL_ACC%u_ID", device_id);

		if (!calibration_valid) {
			if (report_fail) {
				mavlink_log_critical(mavlink_log_pub, "PREFLIGHT FAIL: ACCEL #%u UNCALIBRATED", instance);
			}

		} else {

			if (dynamic) {
				const float accel_magnitude = sqrtf(accel.get().x * accel.get().x
								    + accel.get().y * accel.get().y
								    + accel.get().z * accel.get().z);

				if (accel_magnitude < 4.0f || accel_magnitude > 15.0f /* m/s^2 */) {
					if (report_fail) {
						mavlink_log_critical(mavlink_log_pub, "PREFLIGHT FAIL: ACCEL RANGE, hold still on arming");
					}

					/* this is frickin' fatal */
					accel_valid = false;
				}
			}
		}

	} else {
		if (!optional && report_fail) {
			mavlink_log_critical(mavlink_log_pub, "PREFLIGHT FAIL: NO ACCEL SENSOR #%u", instance);
		}
	}

	const bool success = calibration_valid && accel_valid;

	if (instance == 0) {
		set_health_flags(subsystem_info_s::SUBSYSTEM_TYPE_ACC, exists, !optional, success, status);

	} else if (instance == 1) {
		set_health_flags(subsystem_info_s::SUBSYSTEM_TYPE_ACC2, exists, !optional, success, status);
	}

	return success;
}

static bool gyroCheck(orb_advert_t *mavlink_log_pub, vehicle_status_s &status, unsigned instance, bool optional,
		      int32_t &device_id, bool report_fail)
{
	const bool exists = (orb_exists(ORB_ID(sensor_gyro), instance) == PX4_OK);
	bool calibration_valid = false;
	bool gyro_valid = false;

	if (exists) {

		uORB::Subscription<sensor_gyro_s> gyro{ORB_ID(sensor_gyro), 0, instance};

		gyro_valid = (hrt_elapsed_time(&gyro.get().timestamp) < 1_s);

		if (!gyro_valid) {
			if (report_fail) {
				mavlink_log_critical(mavlink_log_pub, "PREFLIGHT FAIL: GYRO #%u invalid", instance);
			}
		}

		device_id = gyro.get().device_id;

		calibration_valid = check_calibration("CAL_GYRO%u_ID", device_id);

		if (!calibration_valid) {
			if (report_fail) {
				mavlink_log_critical(mavlink_log_pub, "PREFLIGHT FAIL: GYRO #%u UNCALIBRATED", instance);
			}
		}

	} else {
		if (!optional && report_fail) {
			mavlink_log_critical(mavlink_log_pub, "PREFLIGHT FAIL: NO GYRO SENSOR #%u", instance);
		}
	}

	if (instance == 0) {
		set_health_flags(subsystem_info_s::SUBSYSTEM_TYPE_GYRO, exists, !optional, calibration_valid && gyro_valid, status);

	} else if (instance == 1) {
		set_health_flags(subsystem_info_s::SUBSYSTEM_TYPE_GYRO2, exists, !optional, calibration_valid && gyro_valid, status);
	}

	return calibration_valid && gyro_valid;
}

static bool baroCheck(orb_advert_t *mavlink_log_pub, vehicle_status_s &status, unsigned instance, bool optional,
		      int32_t &device_id, bool report_fail)
{
	const bool exists = (orb_exists(ORB_ID(sensor_baro), instance) == PX4_OK);
	bool baro_valid = false;

	if (exists) {
		uORB::Subscription<sensor_baro_s> baro{ORB_ID(sensor_baro), 0, instance};

		baro_valid = (hrt_elapsed_time(&baro.get().timestamp) < 1_s);

		if (!baro_valid) {
			if (report_fail) {
				mavlink_log_critical(mavlink_log_pub, "PREFLIGHT FAIL: BARO #%u invalid", instance);
			}
		}


	} else {
		if (!optional && report_fail) {
			mavlink_log_critical(mavlink_log_pub, "PREFLIGHT FAIL: NO BARO SENSOR #%u", instance);
		}
	}

	if (instance == 0) {
		set_health_flags(subsystem_info_s::SUBSYSTEM_TYPE_ABSPRESSURE, exists, !optional, baro_valid, status);
	}

	return baro_valid;
}

static bool airspeedCheck(orb_advert_t *mavlink_log_pub, vehicle_status_s &status, bool optional, bool report_fail,
			  bool prearm)
{
	bool present = true;
	bool success = true;

	int fd_airspeed = orb_subscribe(ORB_ID(airspeed));
	airspeed_s airspeed = {};

	int fd_diffpres = orb_subscribe(ORB_ID(differential_pressure));
	differential_pressure_s differential_pressure = {};

	if ((orb_copy(ORB_ID(differential_pressure), fd_diffpres, &differential_pressure) != PX4_OK) ||
	    (hrt_elapsed_time(&differential_pressure.timestamp) > 1_s)) {
		if (report_fail && !optional) {
			mavlink_log_critical(mavlink_log_pub, "PREFLIGHT FAIL: AIRSPEED SENSOR MISSING");
		}

		present = false;
		success = false;
		goto out;
	}

	if ((orb_copy(ORB_ID(airspeed), fd_airspeed, &airspeed) != PX4_OK) ||
	    (hrt_elapsed_time(&airspeed.timestamp) > 1_s)) {
		if (report_fail && !optional) {
			mavlink_log_critical(mavlink_log_pub, "PREFLIGHT FAIL: AIRSPEED SENSOR MISSING");
		}

		present = false;
		success = false;
		goto out;
	}

	/*
	 * Check if voter thinks the confidence is low. High-end sensors might have virtually zero noise
	 * on the bench and trigger false positives of the voter. Therefore only fail this
	 * for a pre-arm check, as then the cover is off and the natural airflow in the field
	 * will ensure there is not zero noise.
	 */
	if (prearm && fabsf(airspeed.confidence) < 0.95f) {
		if (report_fail) {
			mavlink_log_critical(mavlink_log_pub, "PREFLIGHT FAIL: AIRSPEED SENSOR STUCK");
		}

		present = true;
		success = false;
		goto out;
	}

	/**
	 * Check if differential pressure is off by more than 15Pa which equals ~5m/s when measuring no airspeed.
	 * Negative and positive offsets are considered. Do not check anymore while arming because pitot cover
	 * might have been removed.
	 */
	if (fabsf(differential_pressure.differential_pressure_filtered_pa) > 15.0f && !prearm) {
		if (report_fail) {
			mavlink_log_critical(mavlink_log_pub, "PREFLIGHT FAIL: CHECK AIRSPEED CAL OR PITOT");
		}

		present = true;
		success = false;
		goto out;
	}

out:
	set_health_flags(subsystem_info_s::SUBSYSTEM_TYPE_DIFFPRESSURE, present, !optional, success, status);

	orb_unsubscribe(fd_airspeed);
	orb_unsubscribe(fd_diffpres);

	return success;
}

static bool powerCheck(orb_advert_t *mavlink_log_pub, vehicle_status_s &status, bool report_fail, bool prearm)
{
	bool success = true;

	if (!prearm) {
		// Ignore power check after arming.
		return true;

	} else {
		int system_power_sub = orb_subscribe(ORB_ID(system_power));

		system_power_s system_power;

		if (orb_copy(ORB_ID(system_power), system_power_sub, &system_power) == PX4_OK) {

			if (hrt_elapsed_time(&system_power.timestamp) < 200_ms) {

				/* copy avionics voltage */
				float avionics_power_rail_voltage = system_power.voltage5v_v;

				// avionics rail
				// Check avionics rail voltages
				if (avionics_power_rail_voltage < 4.5f) {
					success = false;

					if (report_fail) {
						mavlink_log_critical(mavlink_log_pub, "PREFLIGHT FAIL: Avionics power low: %6.2f Volt", (double)avionics_power_rail_voltage);
					}

				} else if (avionics_power_rail_voltage < 4.9f) {
					if (report_fail) {
						mavlink_log_critical(mavlink_log_pub, "CAUTION: Avionics power low: %6.2f Volt", (double)avionics_power_rail_voltage);
					}

				} else if (avionics_power_rail_voltage > 5.4f) {
					if (report_fail) {
						mavlink_log_critical(mavlink_log_pub, "CAUTION: Avionics power high: %6.2f Volt", (double)avionics_power_rail_voltage);
					}
				}
			}
		}

		orb_unsubscribe(system_power_sub);
	}

	return success;
}

static bool ekf2Check(orb_advert_t *mavlink_log_pub, vehicle_status_s &vehicle_status, bool optional, bool report_fail,
		      bool enforce_gps_required)
{
	bool success = true; // start with a pass and change to a fail if any test fails
	bool present = true;
	float test_limit = 1.0f; // pass limit re-used for each test

	bool gps_success = true;
	bool gps_present = true;

	// Get estimator status data if available and exit with a fail recorded if not
	int sub = orb_subscribe(ORB_ID(estimator_status));
	estimator_status_s status = {};

	if (orb_copy(ORB_ID(estimator_status), sub, &status) != PX4_OK) {
		present = false;
		goto out;
	}

	// Check if preflight check performed by estimator has failed
	if (status.pre_flt_fail) {
		if (report_fail) {
			mavlink_log_critical(mavlink_log_pub, "PREFLIGHT FAIL: EKF INTERNAL CHECKS");
		}

		success = false;
		goto out;
	}

	// check vertical position innovation test ratio
	param_get(param_find("COM_ARM_EKF_HGT"), &test_limit);

	if (status.hgt_test_ratio > test_limit) {
		if (report_fail) {
			mavlink_log_critical(mavlink_log_pub, "PREFLIGHT FAIL: EKF HGT ERROR");
		}

		success = false;
		goto out;
	}

	// check velocity innovation test ratio
	param_get(param_find("COM_ARM_EKF_VEL"), &test_limit);

	if (status.vel_test_ratio > test_limit) {
		if (report_fail) {
			mavlink_log_critical(mavlink_log_pub, "PREFLIGHT FAIL: EKF VEL ERROR");
		}

		success = false;
		goto out;
	}

	// check horizontal position innovation test ratio
	param_get(param_find("COM_ARM_EKF_POS"), &test_limit);

	if (status.pos_test_ratio > test_limit) {
		if (report_fail) {
			mavlink_log_critical(mavlink_log_pub, "PREFLIGHT FAIL: EKF HORIZ POS ERROR");
		}

		success = false;
		goto out;
	}

	// check magnetometer innovation test ratio
	param_get(param_find("COM_ARM_EKF_YAW"), &test_limit);

	if (status.mag_test_ratio > test_limit) {
		if (report_fail) {
			mavlink_log_critical(mavlink_log_pub, "PREFLIGHT FAIL: EKF YAW ERROR");
		}

		success = false;
		goto out;
	}

	// check accelerometer delta velocity bias estimates
	param_get(param_find("COM_ARM_EKF_AB"), &test_limit);

	if (fabsf(status.states[13]) > test_limit || fabsf(status.states[14]) > test_limit
	    || fabsf(status.states[15]) > test_limit) {
		if (report_fail) {
			mavlink_log_critical(mavlink_log_pub, "PREFLIGHT FAIL: EKF HIGH IMU ACCEL BIAS");
		}

		success = false;
		goto out;
	}

	// check gyro delta angle bias estimates
	param_get(param_find("COM_ARM_EKF_GB"), &test_limit);

	if (fabsf(status.states[10]) > test_limit || fabsf(status.states[11]) > test_limit
	    || fabsf(status.states[12]) > test_limit) {
		if (report_fail) {
			mavlink_log_critical(mavlink_log_pub, "PREFLIGHT FAIL: EKF HIGH IMU GYRO BIAS");
		}

		success = false;
		goto out;
	}

	// If GPS aiding is required, declare fault condition if the required GPS quality checks are failing
	if (enforce_gps_required) {
		bool ekf_gps_fusion = status.control_mode_flags & (1 << 2);
		bool ekf_gps_check_fail = status.gps_check_fail_flags > 0;

		if (!ekf_gps_fusion) {
			// The EKF is not using GPS
			if (ekf_gps_check_fail) {
				// Poor GPS quality is the likely cause
				if (report_fail) {
					mavlink_log_critical(mavlink_log_pub, "PREFLIGHT FAIL: GPS QUALITY POOR");
				}

				gps_success = false;

			} else {
				// Likely cause unknown
				if (report_fail) {
					mavlink_log_critical(mavlink_log_pub, "PREFLIGHT FAIL: EKF NOT USING GPS");
				}

				gps_success = false;
				gps_present = false;
			}

			success = false;
			goto out;

		} else {
			// The EKF is using GPS so check for bad quality on key performance indicators
			bool gps_quality_fail = ((status.gps_check_fail_flags & ((1 << estimator_status_s::GPS_CHECK_FAIL_MIN_SAT_COUNT)
						  + (1 << estimator_status_s::GPS_CHECK_FAIL_MIN_GDOP)
						  + (1 << estimator_status_s::GPS_CHECK_FAIL_MAX_HORZ_ERR)
						  + (1 << estimator_status_s::GPS_CHECK_FAIL_MAX_VERT_ERR)
						  + (1 << estimator_status_s::GPS_CHECK_FAIL_MAX_SPD_ERR))) > 0);

			if (gps_quality_fail) {
				if (report_fail) {
					mavlink_log_critical(mavlink_log_pub, "PREFLIGHT FAIL: GPS QUALITY POOR");
				}

				gps_success = false;
				success = false;
				goto out;
			}
		}
	}

out:
	set_health_flags(subsystem_info_s::SUBSYSTEM_TYPE_AHRS, present, !optional, success && present, vehicle_status);
	set_health_flags(subsystem_info_s::SUBSYSTEM_TYPE_GPS, gps_present, enforce_gps_required, gps_success, vehicle_status);

	orb_unsubscribe(sub);

	return success;
}

bool preflightCheck(orb_advert_t *mavlink_log_pub, vehicle_status_s &status,
		    vehicle_status_flags_s &status_flags, bool checkGNSS, bool reportFailures, bool prearm,
		    const hrt_abstime &time_since_boot)
{
	if (time_since_boot < 2_s) {
		// the airspeed driver filter doesn't deliver the actual value yet
		reportFailures = false;
	}

	const bool hil_enabled = (status.hil_state == vehicle_status_s::HIL_STATE_ON);

	bool checkSensors = !hil_enabled;
	const bool checkRC = (status.rc_input_mode == vehicle_status_s::RC_IN_MODE_DEFAULT);
	const bool checkDynamic = !hil_enabled;
	const bool checkPower = (status_flags.condition_power_input_valid && !status_flags.circuit_breaker_engaged_power_check);

	bool checkAirspeed = false;

	/* Perform airspeed check only if circuit breaker is not
	 * engaged and it's not a rotary wing */
	if (!status_flags.circuit_breaker_engaged_airspd_check && (!status.is_rotary_wing || status.is_vtol)) {
		checkAirspeed = true;
	}

	reportFailures = (reportFailures && status_flags.condition_system_hotplug_timeout && !status_flags.condition_calibration_enabled);

	bool failed = false;

	/* ---- MAG ---- */
	if (checkSensors) {
		bool prime_found = false;

		int32_t prime_id = -1;
		param_get(param_find("CAL_MAG_PRIME"), &prime_id);

		int32_t sys_has_mag = 1;
		param_get(param_find("SYS_HAS_MAG"), &sys_has_mag);

		bool mag_fail_reported = false;

		/* check all sensors individually, but fail only for mandatory ones */
		for (unsigned i = 0; i < max_optional_mag_count; i++) {
			const bool required = (i < max_mandatory_mag_count) && (sys_has_mag == 1);
			const bool report_fail = (reportFailures && !failed && !mag_fail_reported);

			int32_t device_id = -1;

			if (magnometerCheck(mavlink_log_pub, status, i, !required, device_id, report_fail)) {

				if ((prime_id > 0) && (device_id == prime_id)) {
					prime_found = true;
				}

			} else {
				if (required) {
					failed = true;
					mag_fail_reported = true;
				}
			}
		}

		/* check if the primary device is present */
		if (!prime_found) {
			if (reportFailures && !failed) {
				mavlink_log_critical(mavlink_log_pub, "Primary compass not found");
			}

			set_health_flags(subsystem_info_s::SUBSYSTEM_TYPE_MAG, false, true, false, status);
			failed = true;
		}

		/* mag consistency checks (need to be performed after the individual checks) */
		if (!magConsistencyCheck(mavlink_log_pub, status, (reportFailures && !failed))) {
			failed = true;
		}
	}

	/* ---- ACCEL ---- */
	if (checkSensors) {
		bool prime_found = false;
		int32_t prime_id = -1;
		param_get(param_find("CAL_ACC_PRIME"), &prime_id);

		bool accel_fail_reported = false;

		/* check all sensors individually, but fail only for mandatory ones */
		for (unsigned i = 0; i < max_optional_accel_count; i++) {
			const bool required = (i < max_mandatory_accel_count);
			const bool report_fail = (reportFailures && !failed && !accel_fail_reported);

			int32_t device_id = -1;

			if (accelerometerCheck(mavlink_log_pub, status, i, !required, checkDynamic, device_id, report_fail)) {

				if ((prime_id > 0) && (device_id == prime_id)) {
					prime_found = true;
				}

			} else {
				if (required) {
					failed = true;
					accel_fail_reported = true;
				}
			}
		}

		/* check if the primary device is present */
		if (!prime_found) {
			if (reportFailures && !failed) {
				mavlink_log_critical(mavlink_log_pub, "Primary accelerometer not found");
			}

			set_health_flags(subsystem_info_s::SUBSYSTEM_TYPE_ACC, false, true, false, status);
			failed = true;
		}
	}

	/* ---- GYRO ---- */
	if (checkSensors) {
		bool prime_found = false;
		int32_t prime_id = -1;
		param_get(param_find("CAL_GYRO_PRIME"), &prime_id);

		bool gyro_fail_reported = false;

		/* check all sensors individually, but fail only for mandatory ones */
		for (unsigned i = 0; i < max_optional_gyro_count; i++) {
			const bool required = (i < max_mandatory_gyro_count);
			const bool report_fail = (reportFailures && !failed && !gyro_fail_reported);

			int32_t device_id = -1;

			if (gyroCheck(mavlink_log_pub, status, i, !required, device_id, report_fail)) {

				if ((prime_id > 0) && (device_id == prime_id)) {
					prime_found = true;
				}

			} else {
				if (required) {
					failed = true;
					gyro_fail_reported = true;
				}
			}
		}

		/* check if the primary device is present */
		if (!prime_found) {
			if (reportFailures && !failed) {
				mavlink_log_critical(mavlink_log_pub, "Primary gyro not found");
			}

			set_health_flags(subsystem_info_s::SUBSYSTEM_TYPE_GYRO, false, true, false, status);
			failed = true;
		}
	}

	/* ---- BARO ---- */
	if (checkSensors) {
		bool prime_found = false;

		int32_t prime_id = -1;
		param_get(param_find("CAL_BARO_PRIME"), &prime_id);

		int32_t sys_has_baro = 1;
		param_get(param_find("SYS_HAS_BARO"), &sys_has_baro);

		bool baro_fail_reported = false;

		/* check all sensors, but fail only for mandatory ones */
		for (unsigned i = 0; i < max_optional_baro_count; i++) {
			const bool required = (i < max_mandatory_baro_count) && (sys_has_baro == 1);
			const bool report_fail = (reportFailures && !failed && !baro_fail_reported);

			int32_t device_id = -1;

			if (baroCheck(mavlink_log_pub, status, i, !required, device_id, report_fail)) {
				if ((prime_id > 0) && (device_id == prime_id)) {
					prime_found = true;
				}

			} else {
				if (required) {
					failed = true;
					baro_fail_reported = true;
				}
			}
		}

		// TODO there is no logic in place to calibrate the primary baro yet
		// // check if the primary device is present
		if (!prime_found && false) {
			if (reportFailures && !failed) {
				mavlink_log_critical(mavlink_log_pub, "Primary barometer not operational");
			}

			set_health_flags(subsystem_info_s::SUBSYSTEM_TYPE_ABSPRESSURE, false, true, false, status);
			failed = true;
		}
	}

	/* ---- IMU CONSISTENCY ---- */
	// To be performed after the individual sensor checks have completed
	if (checkSensors) {
		if (!imuConsistencyCheck(mavlink_log_pub, status, (reportFailures && !failed))) {
			failed = true;
		}
	}

	/* ---- AIRSPEED ---- */
	if (checkAirspeed) {
		int32_t optional = 0;
		param_get(param_find("FW_ARSP_MODE"), &optional);

		if (!airspeedCheck(mavlink_log_pub, status, (bool)optional, reportFailures && !failed, prearm) && !(bool)optional) {
			failed = true;
		}
	}

	/* ---- RC CALIBRATION ---- */
	if (checkRC) {
		if (rc_calibration_check(mavlink_log_pub, reportFailures && !failed, status.is_vtol) != OK) {
			if (reportFailures) {
				mavlink_log_critical(mavlink_log_pub, "RC calibration check failed");
			}

			failed = true;

			set_health_flags(subsystem_info_s::SUBSYSTEM_TYPE_RCRECEIVER, status_flags.rc_signal_found_once, true, false, status);
			status_flags.rc_calibration_valid = false;

		} else {
			// The calibration is fine, but only set the overall health state to true if the signal is not currently lost
			status_flags.rc_calibration_valid = true;
			set_health_flags(subsystem_info_s::SUBSYSTEM_TYPE_RCRECEIVER, status_flags.rc_signal_found_once, true, !status.rc_signal_lost, status);
		}
	}

	/* ---- SYSTEM POWER ---- */
	if (checkPower) {
		if (!powerCheck(mavlink_log_pub, status, (reportFailures && !failed), prearm)) {
			failed = true;
		}
	}

	/* ---- Navigation EKF ---- */
	// only check EKF2 data if EKF2 is selected as the estimator and GNSS checking is enabled
	int32_t estimator_type;
	param_get(param_find("SYS_MC_EST_GROUP"), &estimator_type);

	if (estimator_type == 2) {
		// don't report ekf failures for the first 10 seconds to allow time for the filter to start
		bool report_ekf_fail = (time_since_boot > 10_s);

		if (!ekf2Check(mavlink_log_pub, status, false, reportFailures && report_ekf_fail && !failed, checkGNSS)) {
			failed = true;
		}
	}

	/* Report status */
	return !failed;
}

}
