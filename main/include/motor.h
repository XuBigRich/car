#pragma once
#include <stdbool.h>
void motor_init(void);
void motor_command(int throttle, int steering, bool brake);
