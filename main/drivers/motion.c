#include "motion.h"
#include <string.h>

// 所有时间均取板载时钟；网络响应和手机界面不参与分段计时。
static uint64_t now(motion_t *m) { return m->clock(m->context); }
bool motion_valid(char d, unsigned speed) { return d && strchr("fblrqezcsx", d) && speed <= 100; }
static bool moving(char d, unsigned speed) { return speed && strchr("fbqezc", d); }
static void apply(motion_t *m, char d, unsigned speed) {
    m->apply(m->context, d, speed);
    m->direction = d;
    m->speed = speed;
}
void motion_init(motion_t *m, uint64_t (*clock)(void *), void (*output)(void *, char, unsigned), void *context) {
    memset(m, 0, sizeof(*m));
    m->clock = clock; m->apply = output; m->context = context;
    m->direction = 's'; m->status.reason = "none"; m->contact = now(m);
}
void motion_heartbeat(motion_t *m) { m->contact = now(m); }
void motion_cancel(motion_t *m, const char *reason) {
    // 保留最近 session 作为幂等标记：重复的开始/执行请求不能把已完成任务再次启动。
    m->status.state = MOTION_IDLE; m->status.count = 0; m->status.reason = reason;
    m->index = 0;
    memset(m->frames, 0, sizeof(m->frames));
    apply(m, 's', 0);
}
// 把上一输出保持的微秒数写入缓冲区；重复心跳不调用这里，不会把持续动作切碎。
static bool append(motion_t *m, uint64_t end) {
    uint64_t duration = end - m->changed;
    if (!duration) return true;
    if (m->status.count >= MOTION_CAPACITY || end - m->started > MOTION_MAX_US) {
        motion_cancel(m, "limit"); return false;
    }
    m->frames[m->status.count++] = (motion_frame_t){m->direction, (uint8_t)m->speed, (uint32_t)duration};
    return true;
}
void motion_manual(motion_t *m, char d, unsigned speed) {
    if (!motion_valid(d, speed)) return;
    motion_heartbeat(m);
    // READY/PLAYING/EXPORT 下任何手动控制都作废临时内容；RECORDING 下允许继续采集。
    if (m->status.state != MOTION_IDLE && m->status.state != MOTION_RECORDING) motion_cancel(m, "manual");
    if (d == m->direction && speed == m->speed) return;
    char previous = m->direction;
    unsigned previous_speed = m->speed;
    apply(m, d, speed);
    uint64_t accepted = now(m);
    if (m->status.state == MOTION_RECORDING) {
        // 在驱动函数应用新状态后封闭上一帧，消除 HTTP 返回耗时对录制的影响。
        m->direction = previous; m->speed = previous_speed;
        if (!append(m, accepted)) return;
        m->direction = d; m->speed = speed; m->changed = accepted;
    }
}
bool motion_begin(motion_t *m, uint32_t session, bool skill) {
    if (!session) return false;
    if (m->status.session == session) { motion_heartbeat(m); return m->status.skill == skill; }
    motion_cancel(m, "none");
    m->status.session = session; m->status.skill = skill; m->status.state = MOTION_RECORDING;
    m->started = m->changed = m->contact = now(m);
    return true;
}
bool motion_end(motion_t *m, uint32_t session) {
    if (!session || m->status.session != session) return false;
    motion_heartbeat(m);
    if (m->status.state != MOTION_RECORDING) return true; // 重试结束录制不会重复添加末帧。
    // 先应用停止，再以板上时刻结算末帧；缓冲溢出或超时会在 append 中直接作废。
    char d = m->direction; unsigned speed = m->speed;
    apply(m, 's', 0);
    uint64_t end = now(m);
    m->direction = d; m->speed = speed;
    if (!append(m, end)) return true;
    m->direction = 's'; m->speed = 0;
    while (m->status.count && m->frames[m->status.count - 1].direction == 's') m->status.count--;
    bool active = false;
    for (size_t i = 0; i < m->status.count; i++)
        if (moving(m->frames[i].direction, m->frames[i].speed) || (m->status.skill && strchr("lrx", m->frames[i].direction))) active = true;
    if (!active) motion_cancel(m, "empty");
    else m->status.state = m->status.skill ? MOTION_EXPORT : MOTION_READY;
    return true;
}
static void start_frame(motion_t *m) {
    motion_frame_t frame = m->frames[m->index];
    apply(m, frame.direction, frame.speed);
    // 换向保护等硬件等待结束后再开始计时，不让网络或手机端延迟缩短这一帧。
    m->deadline = now(m) + frame.us;
}
static void play(motion_t *m) {
    m->status.state = MOTION_PLAYING; m->status.reason = "none"; m->index = 0;
    motion_heartbeat(m); start_frame(m);
}
bool motion_return(motion_t *m, uint32_t session, unsigned forward, unsigned backward) {
    if (!session || m->status.session != session || forward < 50 || forward > 200 || backward < 50 || backward > 200) return false;
    motion_heartbeat(m);
    if (m->status.state == MOTION_PLAYING || m->status.state == MOTION_IDLE) return true;
    if (m->status.state != MOTION_READY || m->status.skill) return false;
    // 只变换一次：倒序、反转行走方向，保持左右转向；校准只缩放对应方向的持续时间。
    size_t count = m->status.count;
    for (size_t i = 0; i < count / 2; i++) {
        motion_frame_t temp = m->frames[i]; m->frames[i] = m->frames[count - 1 - i]; m->frames[count - 1 - i] = temp;
    }
    for (size_t i = 0; i < count; i++) {
        motion_frame_t *f = &m->frames[i];
        switch (f->direction) {
            case 'f': f->direction = 'b'; break; case 'b': f->direction = 'f'; break;
            case 'q': f->direction = 'z'; break; case 'z': f->direction = 'q'; break;
            case 'e': f->direction = 'c'; break; case 'c': f->direction = 'e'; break;
        }
        if (moving(f->direction, f->speed)) {
            unsigned scale = strchr("fqe", f->direction) ? forward : backward;
            f->us = (uint32_t)(((uint64_t)f->us * scale) / 100);
            if (!f->us) f->us = 1;
        }
    }
    play(m); return true;
}
bool motion_load(motion_t *m, uint32_t session, const motion_frame_t *frames, size_t count) {
    if (!session || !count || count > MOTION_CAPACITY) return false;
    if (m->status.session == session) { motion_heartbeat(m); return true; } // 上传响应丢失时可重试，不重复启动。
    uint64_t total = 0;
    for (size_t i = 0; i < count; i++) {
        if (!motion_valid(frames[i].direction, frames[i].speed) || !frames[i].us || frames[i].us > MOTION_MAX_US) return false;
        total += frames[i].us;
    }
    if (total > MOTION_MAX_US) return false;
    motion_cancel(m, "none");
    memcpy(m->frames, frames, count * sizeof(*frames));
    m->status.session = session; m->status.skill = true; m->status.count = count;
    play(m); return true;
}
bool motion_ack(motion_t *m, uint32_t session) {
    if (!session || m->status.session != session) return false;
    if (m->status.state == MOTION_EXPORT) motion_cancel(m, "exported");
    return m->status.state == MOTION_IDLE;
}
void motion_poll(motion_t *m) {
    uint64_t time = now(m);
    // 技能导出时电机已停止，允许30秒无保活以容纳大包下载；真正 Wi-Fi 断连仍立即清除。
    uint64_t lease = m->status.state == MOTION_EXPORT ? 30000000ULL : MOTION_LEASE_US;
    if (time - m->contact > lease && (m->status.state != MOTION_IDLE || m->direction != 's')) {
        motion_cancel(m, "timeout"); return;
    }
    if (m->status.state == MOTION_RECORDING && time - m->started >= MOTION_MAX_US) {
        motion_cancel(m, "limit"); return;
    }
    if (m->status.state == MOTION_PLAYING && time >= m->deadline) {
        if (++m->index == m->status.count) motion_cancel(m, "done");
        else start_frame(m);
    }
}
