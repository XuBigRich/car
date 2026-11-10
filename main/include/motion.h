#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 记录只存在 RAM，不反复擦写 Flash。单次60秒、最多2048次状态变化，溢出时停车并作废。
#define MOTION_CAPACITY 2048
#define MOTION_MAX_US 60000000ULL
#define MOTION_LEASE_US 800000ULL
typedef struct { char direction; uint8_t speed; uint32_t us; } motion_frame_t;
typedef enum { MOTION_IDLE, MOTION_RECORDING, MOTION_READY, MOTION_PLAYING, MOTION_EXPORT } motion_state_t;
typedef struct {
    motion_state_t state;
    uint32_t session;
    bool skill;
    const char *reason;
    size_t count;
} motion_status_t;

// 核心与硬件解耦：clock 获取板载单调微秒时钟，apply 实际驱动电机。调用方必须持有电机锁。
typedef struct {
    motion_status_t status;
    motion_frame_t frames[MOTION_CAPACITY];
    uint64_t (*clock)(void *);
    void (*apply)(void *, char, unsigned);
    void *context;
    uint64_t started, changed, contact, deadline;
    char direction;
    unsigned speed;
    size_t index;
} motion_t;
void motion_init(motion_t *, uint64_t (*)(void *), void (*)(void *, char, unsigned), void *);
bool motion_valid(char direction, unsigned speed);
void motion_manual(motion_t *, char direction, unsigned speed);
void motion_cancel(motion_t *, const char *reason);
void motion_heartbeat(motion_t *);
void motion_poll(motion_t *);
bool motion_begin(motion_t *, uint32_t session, bool skill);
bool motion_end(motion_t *, uint32_t session);
bool motion_return(motion_t *, uint32_t session, unsigned forward, unsigned backward);
bool motion_load(motion_t *, uint32_t session, const motion_frame_t *, size_t count);
bool motion_ack(motion_t *, uint32_t session);
