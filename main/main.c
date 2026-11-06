#include "car_config.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>


static const char *TAG = "car";
// 电机锁，保护电机控制
static SemaphoreHandle_t motor_lock;
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
extern const char page_start[] asm("_binary_index_html_start");
extern const char page_end[] asm("_binary_index_html_end");

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

static void watchdog_task(void *arg) {
  (void)arg;
  // 轮询
  while (true) {
    // 获取锁，motor_lock 为 全局变量，在 main 函数中初始化，当前
    xSemaphoreTake(motor_lock, portMAX_DELAY);
    if ((last_throttle || steer_request || braking) &&
        esp_timer_get_time() - last_command > CAR_TIMEOUT_MS * 1000LL) {
      drive(0, 0, false);
      ESP_LOGW(TAG, "Control timeout: stopped");
    }
    steering_update(esp_timer_get_time());
    xSemaphoreGive(motor_lock);
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

static esp_err_t page_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, page_start, page_end - page_start - 1);
}

static esp_err_t drive_handler(httpd_req_t *req) {
  char body[24];
  if (req->content_len == 0 || req->content_len >= sizeof(body))
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid command");
  size_t received = 0;
  while (received < req->content_len) {
    int n = httpd_req_recv(req, body + received, req->content_len - received);
    if (n <= 0)
      return ESP_FAIL;
    received += n;
  }
  body[received] = 0;
  // Strict bounded parsing: direction, followed by one to three decimal digits.
  if (received < 3 || received > 5 || body[1] != ',')
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid command");
  int speed = 0;
  for (size_t i = 2; i < received; ++i) {
    if (body[i] < '0' || body[i] > '9')
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid speed");
    speed = speed * 10 + body[i] - '0';
  }
  if (speed > 100)
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "Speed must be 0..100");
  int throttle = 0, steering = 0;
  bool brake = false;
  switch (body[0]) {
  case 'f':
    throttle = speed;
    break;
  case 'b':
    throttle = -speed;
    break;
  case 'l':
    steering = -1;
    break;
  case 'r':
    steering = 1;
    break;
  case 'q':
    throttle = speed;
    steering = -1;
    break;
  case 'e':
    throttle = speed;
    steering = 1;
    break;
  case 'z':
    throttle = -speed;
    steering = -1;
    break;
  case 'c':
    throttle = -speed;
    steering = 1;
    break;
  case 's':
    break;
  case 'x':
    brake = true;
    break;
  default:
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid direction");
  }
  xSemaphoreTake(motor_lock, portMAX_DELAY);
  static char previous_direction;
  static int previous_speed = -1;
  if (previous_direction != body[0] || previous_speed != speed) {
    ESP_LOGI(TAG, "Command %c speed=%d throttle=%d steering=%d brake=%d",
             body[0], speed, throttle, steering, brake);
    previous_direction = body[0];
    previous_speed = speed;
  }
  drive(throttle, steering, brake);
  last_command = esp_timer_get_time();
  xSemaphoreGive(motor_lock);
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_sendstr(req, "OK");
}
// 处理wifi事件
static void wifi_event(void *arg, esp_event_base_t base, int32_t id,
                       void *data) {
  (void)arg;
  (void)base;
  (void)data;
  // 当wifi连接断开时，停止电机
  if (id == WIFI_EVENT_AP_STADISCONNECTED) {
    // 获取锁，motor_lock 为 全局变量，在 main 函数中初始化，当前
    xSemaphoreTake(motor_lock, portMAX_DELAY);
    drive(0, 0, false);
    // 释放锁
    xSemaphoreGive(motor_lock);
  }
}

void app_main(void) {
  // 创建一个互斥锁，用于保护电机控制的线程安全
  motor_lock = xSemaphoreCreateMutex();
  // 检查互斥锁是否创建成功，否则抛出异常重启电路
  configASSERT(motor_lock);
  // 将使用的四个引脚放入数组
  const int pins[] = {CAR_DRIVE_IN1, CAR_DRIVE_IN2, CAR_STEER_IN1,
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
  configASSERT(xTaskCreate(watchdog_task, "motor_watchdog", 3072, NULL, 5,
                           NULL) == pdPASS);
  // 初始化 mvs flash ，他是在flash存储上的  NVS 分区  ，断电不会丢失
  esp_err_t err = nvs_flash_init();
  // 如果写满了或者有新版本，则擦除重写
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    // 擦除分区 （ 只擦 nvs 这个分区）
    ESP_ERROR_CHECK(nvs_flash_erase());
    // 重新初始化
    err = nvs_flash_init();
  }
  // 判断是否有错误，有错误则重启电路
  configASSERT(err == ESP_OK);
  ESP_ERROR_CHECK(err);
  // 全局初始化 TCP/IP 栈，
  ESP_ERROR_CHECK(esp_netif_init());
  // 建默认事件循环，Wi-Fi 的断连事件靠它分发
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  // 真正创建 AP 网络接口：默认 192.168.4.1/24 + DHCP 服务器，手机连上后被分配
  // 192.168.4.x —— 这就是 README 里 http://192.168.4.1 的来源
  esp_netif_t *ap = esp_netif_create_default_wifi_ap();
  // 判断ap是否创建成功
  configASSERT(ap);
  // 初始化wifi(默认配置)
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  // 通过声明好的配置创建wifi
  ESP_ERROR_CHECK(esp_wifi_init(&init));
  // 注册wifi printf 事件
  ESP_ERROR_CHECK(esp_event_handler_register(
      WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, wifi_event, NULL));
  wifi_config_t wifi = {.ap = {
                            .ssid = CAR_AP_SSID,
                            .password = CAR_AP_PASSWORD,
                            .channel = 6,
                            .max_connection = 1,
                            .authmode = WIFI_AUTH_WPA2_PSK,
                        }};
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi));
  ESP_ERROR_CHECK(esp_wifi_start());
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.recv_wait_timeout = 1;
  config.send_wait_timeout = 1;
  httpd_handle_t server = NULL;
  ESP_ERROR_CHECK(httpd_start(&server, &config));
  const httpd_uri_t page = {
      .uri = "/", .method = HTTP_GET, .handler = page_handler};
  const httpd_uri_t command = {
      .uri = "/api/drive", .method = HTTP_POST, .handler = drive_handler};
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &page));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &command));
  ESP_LOGI(TAG, "Connect to %s; open http://192.168.4.1", CAR_AP_SSID);
}
