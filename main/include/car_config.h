#pragma once

// TC1508A: INA/INB = propulsion motor A, INC/IND = steering motor B.
//计时器第2路通道 开发板第四引脚 ，控制驱动模块INT1 控制驱动目前猜测后退
#define CAR_DRIVE_IN1  4
//计时器第2路通道 开发板第五引脚 ，控制驱动模块INT2 控制驱动目前猜测前进 
#define CAR_DRIVE_IN2  5
//计时器第3路通道 开发板第六引脚 ，控制驱动模块INT3 控制转向目前猜测右转 
#define CAR_STEER_IN1 6
//计时器第4路通道 开发板第七引脚 ，控制驱动模块INT4  控制转向目前猜测左转 
#define CAR_STEER_IN2 7
// Set to 1 to reverse the corresponding motor direction.
//描述车辆驾驶反转
#define CAR_DRIVE_REVERSED  0
//描述车辆转向反转
#define CAR_STEER_REVERSED 0
#define CAR_PWM_HZ 1000
#define CAR_TIMEOUT_MS 800
//wifi名称
#define CAR_AP_SSID "Control-Car"
//wifi密码
#define CAR_AP_PASSWORD "car123456"

// Full duty while held. MAX_MS=0 disables the steering duration limit.
#define CAR_STEER_START_DUTY 100
#define CAR_STEER_HOLD_DUTY 100
#define CAR_STEER_START_MS 150
#define CAR_STEER_MAX_MS 0
#define CAR_STEER_REVERSE_MS 100
