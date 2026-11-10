#pragma once
#include <stdbool.h>
#include "motion.h"
void motor_init(void);
void motor_command(int throttle, int steering, bool brake);
// 网络层只调用这些加锁入口，不能直接读写录制缓存或在回调里驱动自动动作。
bool motor_motion_begin(uint32_t session, bool skill);
bool motor_motion_end(uint32_t session);
bool motor_motion_return(uint32_t session, unsigned forward, unsigned backward);
bool motor_motion_load(uint32_t session, const motion_frame_t *frames, size_t count);
bool motor_motion_ack(uint32_t session);
void motor_motion_heartbeat(void);
void motor_motion_cancel(const char *reason);
void motor_motion_snapshot(motion_status_t *status, motion_frame_t *frames);
