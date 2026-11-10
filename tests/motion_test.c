#include "motion.h"
#include <stdlib.h>
#define assert(value) do { if (!(value)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); exit(1); } } while (0)
#include <stdio.h>
#include <string.h>

// 用虚拟板载时钟和假电机测试，不打开串口，也不会让真实小车运动。
typedef struct { motion_t m; uint64_t time, apply_cost; char output; unsigned speed, calls; } rig_t;
static uint64_t clock_read(void *ctx) { return ((rig_t *)ctx)->time; }
static void output(void *ctx, char d, unsigned speed) {
    rig_t *r = ctx; r->output = d; r->speed = speed; r->calls++; r->time += r->apply_cost;
}
static void init(rig_t *r) { memset(r, 0, sizeof(*r)); motion_init(&r->m, clock_read, output, r); }
static void record_route(rig_t *r, uint32_t session, bool skill) {
    assert(motion_begin(&r->m, session, skill));
    r->time = 10000; motion_manual(&r->m, 'f', 50);
    r->time = 110000; motion_manual(&r->m, 'f', 50); // 心跳不分帧。
    r->time = 210000; motion_manual(&r->m, 'q', 60);
    r->time = 510000; assert(motion_end(&r->m, session));
    assert(r->m.status.count == 3);
    assert(r->m.frames[1].direction == 'f' && r->m.frames[1].us == 200000);
    assert(r->m.frames[2].direction == 'q' && r->m.frames[2].us == 300000);
}
int main(void) {
    rig_t r; init(&r); record_route(&r, 1, false);
    assert(r.m.status.state == MOTION_READY && r.output == 's');
    assert(motion_return(&r.m, 1, 100, 100));
    assert(r.output == 'z' && r.speed == 60);
    uint64_t deadline = r.m.deadline; unsigned calls = r.calls;
    r.time = 600000; assert(motion_return(&r.m, 1, 100, 100));
    assert(r.m.deadline == deadline && r.calls == calls); // 重试执行不重启。
    r.time = 810000; motion_poll(&r.m); assert(r.output == 'b' && r.speed == 50);
    r.time = 1010000; motion_poll(&r.m); assert(r.output == 's');
    r.time = 1020000; motion_poll(&r.m);
    assert(r.m.status.state == MOTION_IDLE && r.m.status.count == 0 && !strcmp(r.m.status.reason, "done"));
    assert(r.m.frames[0].us == 0); // 完成后物理清空缓存。
    calls = r.calls; assert(motion_return(&r.m, 1, 100, 100)); assert(r.calls == calls);

    init(&r); record_route(&r, 2, true);
    assert(r.m.status.state == MOTION_EXPORT && r.m.status.count == 3);
    assert(motion_end(&r.m, 2) && r.m.status.count == 3); // 重试结束不重复写入。
    assert(!motion_ack(&r.m, 99)); assert(r.m.status.count == 3);
    assert(motion_ack(&r.m, 2) && r.m.status.count == 0);
    assert(motion_ack(&r.m, 2)); // 手机确认可重试。

    init(&r); record_route(&r, 3, false);
    motion_manual(&r.m, 'e', 35);
    assert(r.m.status.state == MOTION_IDLE && r.m.status.count == 0 && r.output == 'e');
    assert(motion_return(&r.m, 3, 100, 100)); assert(r.output == 'e'); // 已作废，不重放。

    motion_frame_t skill[] = {{'q', 40, 100000}, {'e', 65, 250000}};
    init(&r); r.apply_cost = 30000;
    assert(motion_load(&r.m, 10, skill, 2));
    assert(r.output == 'q' && r.m.deadline == r.time + 100000);
    calls = r.calls; assert(motion_load(&r.m, 10, skill, 2)); assert(r.calls == calls);
    r.time = r.m.deadline; motion_heartbeat(&r.m); motion_poll(&r.m);
    assert(r.output == 'e' && r.m.deadline == r.time + 250000);
    r.time = r.m.deadline; motion_heartbeat(&r.m); motion_poll(&r.m);
    assert(r.output == 's' && r.m.status.count == 0);

    init(&r); assert(motion_load(&r.m, 11, skill, 2));
    r.time = 800001; motion_poll(&r.m);
    assert(r.output == 's' && !strcmp(r.m.status.reason, "timeout") && r.m.status.count == 0);

    init(&r); assert(motion_begin(&r.m, 12, false));
    for (unsigned i = 0; i <= MOTION_CAPACITY; i++) { r.time++; motion_manual(&r.m, i % 2 ? 'b' : 'f', 40); }
    assert(r.output == 's' && !strcmp(r.m.status.reason, "limit") && r.m.status.count == 0);

    init(&r); assert(motion_begin(&r.m, 13, false));
    r.time = MOTION_MAX_US; motion_heartbeat(&r.m); motion_poll(&r.m);
    assert(r.output == 's' && !strcmp(r.m.status.reason, "limit"));
    init(&r); assert(motion_begin(&r.m, 14, false)); assert(motion_end(&r.m, 14));
    assert(!strcmp(r.m.status.reason, "empty"));
    assert(!motion_begin(&r.m, 0, false));
    motion_frame_t bad[] = {{'f', 101, 100}, {'q', 40, 0}};
    assert(!motion_load(&r.m, 15, bad, 2));

    // 两个方向分别校准：去程前进的返程倒车取150%，去程倒车的返程前进取120%。
    init(&r); record_route(&r, 16, false);
    assert(motion_return(&r.m, 16, 120, 150));
    assert(r.m.frames[0].direction == 'z' && r.m.frames[0].us == 450000);
    init(&r); assert(motion_begin(&r.m, 17, false));
    r.time = 10000; motion_manual(&r.m, 'b', 40);
    r.time = 210000; assert(motion_end(&r.m, 17));
    assert(motion_return(&r.m, 17, 120, 150));
    assert(r.m.frames[0].direction == 'f' && r.m.frames[0].us == 240000);

    // 导出已停车，长包下载不应沿用800毫秒行驶超时；30秒无确认后仍会清理。
    init(&r); record_route(&r, 18, true);
    r.time += 1000000; motion_poll(&r.m);
    assert(r.m.status.state == MOTION_EXPORT && r.m.status.count == 3);
    r.time += 30000000; motion_poll(&r.m);
    assert(r.m.status.count == 0 && r.output == 's');

    // 开始请求的响应丢失后重发，不得停车或重置已录到的内容。
    init(&r); assert(motion_begin(&r.m, 19, false));
    r.time = 10000; motion_manual(&r.m, 'f', 40);
    r.time = 20000; assert(motion_begin(&r.m, 19, false));
    assert(r.m.started == 0 && r.m.status.count == 1 && r.output == 'f');
    puts("PASS: board clock recording, heartbeat merge, reverse/calibration, local playback, cancellation, export ack, limits and idempotence");
    return 0;
}
