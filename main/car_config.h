#pragma once

// TC1508A: INA/INB = propulsion motor A, INC/IND = steering motor B.
#define CAR_DRIVE_IN1  4
#define CAR_DRIVE_IN2  5
#define CAR_STEER_IN1 6
#define CAR_STEER_IN2 7
// Set to 1 to reverse the corresponding motor direction.
#define CAR_DRIVE_REVERSED  0
#define CAR_STEER_REVERSED 0
#define CAR_PWM_HZ 1000
#define CAR_TIMEOUT_MS 800
#define CAR_AP_SSID "Control-Car"
#define CAR_AP_PASSWORD "car123456"

// One full-duty pulse per press; no holding current. Release to rearm.
#define CAR_STEER_START_DUTY 100
#define CAR_STEER_HOLD_DUTY 0
#define CAR_STEER_START_MS 150
#define CAR_STEER_MAX_MS 150
#define CAR_STEER_REVERSE_MS 100
