#include "car_config.h"
#include "motor.h"
#include <string.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>


static const char *TAG = "car";
// 电机锁，保护电机控制
static SemaphoreHandle_t motor_lock;
// 录制缓冲、回放进度和硬件输出共用同一把锁，避免手动接管与自动切帧交错。
static motion_t timeline;
static TaskHandle_t motor_task;
static esp_timer_handle_t motor_timer;
// 上一次的转向和转速
static int last_throttle, last_steering;
// 是否刹车
static bool braking;
// 上一次的命令时间
static int64_t last_command;
// 转向请求
static int steer_request;
// 转向开始时间和转向就绪时间
static int64_t steer_started, steer_ready;
// 转向锁定
static bool steer_locked;

_Static_assert(CAR_STEER_START_DUTY > 0 && CAR_STEER_START_DUTY <= 100,
               "Invalid start duty");
_Static_assert(CAR_STEER_HOLD_DUTY >= 0 &&
                   CAR_STEER_HOLD_DUTY <= CAR_STEER_START_DUTY,
               "Invalid hold duty");
_Static_assert((CAR_STEER_MAX_MS == 0 ||
                CAR_STEER_MAX_MS >= CAR_STEER_START_MS) &&
                   CAR_STEER_START_MS > 0,
               "Invalid steering timing");

static void duty(int channel, unsigned value) {
  ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, value));
  ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, channel));
}
// channel 是这台电机的第一路 LEDC 通道编号，例如传入 2 时，两路编号是 2 和 3。
// speed 是带方向的 PWM 百分比，例如 -50 表示反向、占空比 50%。     0需要为正   2需要为正   
static void wheel(int channel, int speed) { // static 表示只在本文件内使用，void 表示没有返回值。
  // speed < 0 时条件表达式得到 1，选 channel + 1，否则得到 0，选 channel。
  int active = channel + (speed < 0 ? 1 : 0);
  // 这里把 0 和 1 对调，得到另一路。比如 active 是 3 时，inactive 就是 2。
  int inactive = channel + (speed < 0 ? 0 : 1);
  // 换向时，inactive 就是上一方向用的通道。先把它的占空比设为 0，再驱动新方向。
  duty(inactive, 0);
  // 取速度的绝对值，将百分比换算成 10 位 PWM 的计数值。
  // 例如 50% 对应 512。speed 为 0 时，两路的占空比都为 0。
  duty(active, (unsigned)(speed < 0 ? -speed : speed) * 1024 / 100);
}

// All steering state and motor writes are protected by motor_lock.
static void steering_update(int64_t now) {
  int output = 0;
  // 如果有转向请求，并且没有锁定，并且当前时间大于转向就绪时间
  if (steer_request && !steer_locked && now >= steer_ready) {
    // 计算转向时间
    int64_t elapsed = now - steer_started;
    if (CAR_STEER_MAX_MS > 0 && elapsed >= CAR_STEER_MAX_MS * 1000LL) {
      steer_locked = true;
      ESP_LOGW(TAG, "Steering timeout: release turn button to rearm");
    } else {
      int power = elapsed < CAR_STEER_START_MS * 1000LL ? CAR_STEER_START_DUTY
                                                        : CAR_STEER_HOLD_DUTY;
      output = steer_request * power;
    }
  }
  // 如果输出和上一次的输出不一样，则更新输出
  if (output != last_steering) {
    wheel(2, CAR_STEER_REVERSED ? -output : output);
    last_steering = output;
    ESP_LOGI(TAG, "Steering output=%d locked=%d", output, steer_locked);
  }
}




static void steering_command(int request, int64_t now) {
  request = (request > 0) - (request < 0);
  if (!request) {
    if (steer_request)
      steer_ready = now + CAR_STEER_REVERSE_MS * 1000LL;
    steer_request = 0;
    steer_locked = false;
  } else if (!steer_locked && request != steer_request) {
    // Changing directly from left to right cannot extend the total on-time.
    if (steer_request) {
      wheel(2, 0);
      last_steering = 0;
      steer_ready = now + CAR_STEER_REVERSE_MS * 1000LL;
    } else {
      if (steer_ready < now)
        steer_ready = now;
      steer_started = steer_ready;
    }
    steer_request = request;
  }
  steering_update(now);
}

static void drive(int throttle, int steering, bool brake) {
  // 判断 是否倒车 ，如果是倒车，则 反转 转向 和 转速
  if (CAR_DRIVE_REVERSED)
    throttle = -throttle;
// 如果 刹车 或者 转向 和 上一次的转向 方向不一样，则 停止 电机//
  if (braking || throttle * last_throttle < 0) {
    wheel(0, 0);
    vTaskDelay(pdMS_TO_TICKS(30));
  }
  if (brake) {
    duty(0, 1024);
    duty(1, 1024);
  } else {
    wheel(0, throttle);
  }
  steering_command(brake ? 0 : steering, esp_timer_get_time());
  braking = brake;
  last_throttle = throttle;
}

// 仅此适配函数把协议方向映射成电机参数；录制和上传的技能都经过相同硬件保护。
static void motion_output(void *context, char direction, unsigned speed) {
  (void)context;
  int throttle = 0, steering = 0;
  switch (direction) {
  case 'f': throttle = speed; break;
  case 'b': throttle = -(int)speed; break;
  case 'l': steering = -1; break;
  case 'r': steering = 1; break;
  case 'q': throttle = speed; steering = -1; break;
  case 'e': throttle = speed; steering = 1; break;
  case 'z': throttle = -(int)speed; steering = -1; break;
  case 'c': throttle = -(int)speed; steering = 1; break;
  default: break;
  }
  drive(throttle, steering, direction == 'x');
}
static uint64_t motion_clock(void *context) { (void)context; return esp_timer_get_time(); }
// 高精度定时器只唤醒任务，不在定时器回调中等待电机锁或进行换向延时。
static void motor_wake(void *context) { (void)context; xTaskNotifyGive(motor_task); }
static void watchdog_task(void *arg) {
  (void)arg;
  while (true) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
    xSemaphoreTake(motor_lock, portMAX_DELAY);
    motion_poll(&timeline);
    steering_update(esp_timer_get_time());
    xSemaphoreGive(motor_lock);
  }
}

void motor_init(void) {
  // 创建一个互斥锁，用于保护电机控制的线程安全
  motor_lock = xSemaphoreCreateMutex();
  // 检查互斥锁是否创建成功，否则抛出异常重启电路
  configASSERT(motor_lock);
  // 将使用的四个引脚放入数组
  const int pins[] = { CAR_DRIVE_IN2, CAR_DRIVE_IN1,CAR_STEER_IN1,
                      CAR_STEER_IN2};
  // 声明一个64位无符号变量mask
  uint64_t mask = 0;
  // 遍历数组，写死使用4个引脚所以轮询四边
  for (int i = 0; i < 4; ++i) {
    // 检查引脚是是有效的输出引脚，否则抛出异常重启电路，GPIO_IS_VALID_OUTPUT_GPIO
    // 检查引脚是有效的输出引脚
    configASSERT(GPIO_IS_VALID_OUTPUT_GPIO(pins[i]));
    // 检查引脚是否重复，否则抛出异常重启电路
    for (int j = 0; j < i; ++j)
      configASSERT(pins[i] != pins[j]);
    // 将所有检查通过的引脚 放入mask
    // ，由一个mask即可描述这个电路板上所有使用的引脚
    mask |= 1ULL << pins[i];
    // 将引脚设置为低电平，GPIO_RESET_PIN 将引脚上面的设置全部清除，包括
    // ：关中断、关输入、关输出、打开上拉、关下拉、RTC 去初始化、IOMUX
    // 功能选择切回 GPIO、释放引脚占用标记。
    ESP_ERROR_CHECK(gpio_reset_pin(pins[i]));
    // GPIO_SET_LEVEL 将引脚设置为指定电平
    ESP_ERROR_CHECK(gpio_set_level(pins[i], 0));
  }
  // 开始配置  使用引脚 的 输出模式，mask 为使用的引脚，GPIO_MODE_OUTPUT
  // 输出模式，GPIO_IS_VALID_GPIO 检查引脚是否有效

  gpio_config_t gpio = {.pin_bit_mask = mask, .mode = GPIO_MODE_OUTPUT};
  // 将写好的配置写入到开发板中
  ESP_ERROR_CHECK(gpio_config(&gpio));
  // 创建一个定时器，用于控制电机转速，CAR_PWM_HZ
  // 为定时器频率，CAR_PWM_RESOLUTION
  // 为定时器分辨率，LEDC_TIMER_0为使用的指定定时器，CAR_PWM_AUTO_CLK
  // 为定时器时钟源
  ledc_timer_config_t timer = {
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .duty_resolution = LEDC_TIMER_10_BIT,
      .timer_num = LEDC_TIMER_0,
      .freq_hz = CAR_PWM_HZ,
      .clk_cfg = LEDC_AUTO_CLK,
  };
  // 将写好的配置写入到开发板中
  ESP_ERROR_CHECK(ledc_timer_config(&timer));
  // 目的是为了给每个引脚设置单独的定时器 通道（每个通道可以写自己单独的逻辑）
  for (int i = 0; i < 4; ++i) {
    // 编写配置
    ledc_channel_config_t channel = {
        .gpio_num = pins[i],
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = i,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    // 设置将引脚和定时器通道绑定
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
  }
  // 创建一个任务（类似于Java里面的线程，区别是
  // 他死死系统，线程死只死线程不影响应用），watchdog_task
  // 为任务名称，3072为任务堆栈大小，NULL为任务参数，5为任务优先级，NULL为任务句柄
  motion_init(&timeline, motion_clock, motion_output, NULL);
  configASSERT(xTaskCreate(watchdog_task, "motor_watchdog", 4096, NULL, 5,
                           &motor_task) == pdPASS);
  esp_timer_create_args_t timer_args = {.callback = motor_wake, .name = "motion_tick"};
  ESP_ERROR_CHECK(esp_timer_create(&timer_args, &motor_timer));
  // 即使 FreeRTOS tick 为10毫秒，也使用板载高精度通知约每2毫秒检查一次动作边界。
  ESP_ERROR_CHECK(esp_timer_start_periodic(motor_timer, 2000));
}

// 手动网络接口仍兼容旧方向协议；录制由板子在应用控制状态时进行，不依赖手机发来的时间。
void motor_command(int throttle, int steering, bool brake) {
  char direction = brake ? 'x' : throttle > 0 ? (steering < 0 ? 'q' : steering > 0 ? 'e' : 'f') :
      throttle < 0 ? (steering < 0 ? 'z' : steering > 0 ? 'c' : 'b') : steering < 0 ? 'l' : steering > 0 ? 'r' : 's';
  unsigned speed = throttle < 0 ? -throttle : throttle;
  xSemaphoreTake(motor_lock, portMAX_DELAY);
  motion_manual(&timeline, direction, speed);
  last_command = esp_timer_get_time();
  xSemaphoreGive(motor_lock);
}
// 所有接口持锁调用纯状态机；HTTP 线程只提交操作，执行定时由 motor_task 推进。
bool motor_motion_begin(uint32_t session, bool skill) {
  xSemaphoreTake(motor_lock, portMAX_DELAY); bool ok = motion_begin(&timeline, session, skill); xSemaphoreGive(motor_lock); return ok;
}
bool motor_motion_end(uint32_t session) {
  xSemaphoreTake(motor_lock, portMAX_DELAY); bool ok = motion_end(&timeline, session); xSemaphoreGive(motor_lock); return ok;
}
bool motor_motion_return(uint32_t session, unsigned f, unsigned b) {
  xSemaphoreTake(motor_lock, portMAX_DELAY); bool ok = motion_return(&timeline, session, f, b); xSemaphoreGive(motor_lock); return ok;
}
bool motor_motion_load(uint32_t session, const motion_frame_t *frames, size_t count) {
  xSemaphoreTake(motor_lock, portMAX_DELAY); bool ok = motion_load(&timeline, session, frames, count); xSemaphoreGive(motor_lock); return ok;
}
bool motor_motion_ack(uint32_t session) {
  xSemaphoreTake(motor_lock, portMAX_DELAY); bool ok = motion_ack(&timeline, session); xSemaphoreGive(motor_lock); return ok;
}
void motor_motion_heartbeat(void) {
  xSemaphoreTake(motor_lock, portMAX_DELAY); motion_heartbeat(&timeline); xSemaphoreGive(motor_lock);
}
void motor_motion_cancel(const char *reason) {
  xSemaphoreTake(motor_lock, portMAX_DELAY); motion_cancel(&timeline, reason); xSemaphoreGive(motor_lock);
}
void motor_motion_snapshot(motion_status_t *status, motion_frame_t *frames) {
  xSemaphoreTake(motor_lock, portMAX_DELAY);
  *status = timeline.status;
  if (frames) memcpy(frames, timeline.frames, status->count * sizeof(*frames));
  xSemaphoreGive(motor_lock);
}
