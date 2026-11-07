#include "car_config.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "nvs_flash.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>


static const char *TAG = "car_network";
#include "motor.h"
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
  motor_command(throttle, steering, brake);
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
    motor_command(0, 0, false);
  }
}

void network_init(void) {
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
  // 192.168.4.x —— 这就是 READ          67ME 里 http://192.168.4.1 的来源
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
  const httpd_uri_t command = {
      .uri = "/api/drive", .method = HTTP_POST, .handler = drive_handler};
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &command));
  ESP_LOGI(TAG, "Connect to %s; use the Android controller", CAR_AP_SSID);
}


