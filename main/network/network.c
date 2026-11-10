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
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>


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

// HTTP 服务串行处理请求，静态工作区只用于上传解析/快照，避免占用 HTTP 任务的小栈。
static motion_frame_t transfer_frames[MOTION_CAPACITY];
static bool number(const char *text, uint32_t *out) {
  if (!text || *text < '0' || *text > '9') return false;
  char *end; errno = 0;
  unsigned long long value = strtoull(text, &end, 10);
  if (errno || *end || value > UINT32_MAX) return false;
  *out = value; return true;
}
static char *read_body(httpd_req_t *req, size_t maximum) {
  if (!req->content_len || req->content_len > maximum) return NULL;
  char *body = malloc(req->content_len + 1);
  if (!body) return NULL;
  size_t count = 0;
  while (count < req->content_len) {
    int n = httpd_req_recv(req, body + count, req->content_len - count);
    if (n <= 0) { free(body); return NULL; }
    count += n;
  }
  body[count] = 0;
  // 拒绝内嵌零字符，防止正文后半段被字符串解析忽略。
  if (strlen(body) != count) { free(body); return NULL; }
  return body;
}
static esp_err_t motion_reply(httpd_req_t *req, bool include_frames) {
  motion_status_t status;
  motor_motion_snapshot(&status, include_frames ? transfer_frames : NULL);
  static const char *names[] = {"idle", "recording", "ready", "playing", "export"};
  char chunk[256];
  int n = snprintf(chunk, sizeof(chunk),
      "{\"protocol\":2,\"state\":\"%s\",\"session\":%" PRIu32 ",\"skill\":%s,\"reason\":\"%s\",\"count\":%u",
      names[status.state], status.session, status.skill ? "true" : "false", status.reason, (unsigned)status.count);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  if (httpd_resp_send_chunk(req, chunk, n) != ESP_OK) return ESP_FAIL;
  if (include_frames) {
    if (httpd_resp_send_chunk(req, ",\"frames\":[", HTTPD_RESP_USE_STRLEN) != ESP_OK) return ESP_FAIL;
    // 批量发送，避免每帧拆成独立的小网络包，让技能导出被大量 TCP 小包延迟拖慢。
    char batch[2048]; size_t used = 0;
    for (size_t i = 0; i < status.count; i++) {
      const motion_frame_t *frame = &transfer_frames[i];
      n = snprintf(chunk, sizeof(chunk), "%s{\"command\":\"%c,%u\",\"us\":%" PRIu32 "}",
          i ? "," : "", frame->direction, frame->speed, frame->us);
      if (used + n > sizeof(batch)) {
        if (httpd_resp_send_chunk(req, batch, used) != ESP_OK) return ESP_FAIL;
        used = 0;
      }
      memcpy(batch + used, chunk, n); used += n;
    }
    if (used && httpd_resp_send_chunk(req, batch, used) != ESP_OK) return ESP_FAIL;
    if (httpd_resp_send_chunk(req, "]", 1) != ESP_OK) return ESP_FAIL;
  }
  if (httpd_resp_send_chunk(req, "}", 1) != ESP_OK) return ESP_FAIL;
  return httpd_resp_send_chunk(req, NULL, 0);
}
// 控制操作都带 session：重发开始/结束/执行不重复录制或重复启动。
static esp_err_t motion_handler(httpd_req_t *req) {
  char *body = read_body(req, 80);
  if (!body) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid motion request");
  char *parts[5] = {body}; size_t count = 1; bool valid = true;
  for (char *p = body; *p; p++) if (*p == ',') {
    *p = 0;
    if (count == 5 || !p[1]) { valid = false; break; }
    parts[count++] = p + 1;
  }
  for (size_t i = 0; i < count; i++) if (!parts[i][0]) valid = false;
  uint32_t token = 0, f = 0, b = 0;
  bool ok = false, export_frames = false;
  if (valid && count == 1 && (!strcmp(parts[0], "ping") || !strcmp(parts[0], "status"))) {
    motor_motion_heartbeat(); ok = true;
  } else if (valid && count == 1 && !strcmp(parts[0], "cancel")) {
    motor_motion_cancel("cancel"); ok = true;
  } else if (valid && count >= 2 && number(parts[1], &token) && token) {
    if (count == 3 && !strcmp(parts[0], "start") && (!strcmp(parts[2], "r") || !strcmp(parts[2], "k")))
      ok = motor_motion_begin(token, !strcmp(parts[2], "k"));
    else if (count == 2 && !strcmp(parts[0], "end")) { ok = motor_motion_end(token); export_frames = ok; }
    else if (count == 2 && !strcmp(parts[0], "ack")) ok = motor_motion_ack(token);
    else if (count == 2 && !strcmp(parts[0], "export")) {
      motion_status_t status; motor_motion_snapshot(&status, NULL);
      ok = status.session == token && status.state == MOTION_EXPORT;
      if (ok) motor_motion_heartbeat();
      export_frames = ok;
    } else if (count == 4 && !strcmp(parts[0], "return") && number(parts[2], &f) && number(parts[3], &b))
      ok = motor_motion_return(token, f, b);
  }
  free(body);
  if (!ok) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid session or state");
  // 只有技能结束才需要返回明细；临时轨迹始终留在板子，手机不接管它的时间轴。
  motion_status_t status; motor_motion_snapshot(&status, NULL);
  return motion_reply(req, export_frames && status.state == MOTION_EXPORT);
}
// 技能上传格式：第一行 session，后续每行为 方向,油门,微秒；整包校验后才开始执行。
static esp_err_t sequence_handler(httpd_req_t *req) {
  char *body = read_body(req, 40000);
  if (!body) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid sequence size");
  char *line = body, *next = strchr(line, '\n'); uint32_t token = 0;
  bool ok = next != NULL; size_t count = 0;
  if (next) { *next++ = 0; ok = number(line, &token) && token; }
  while (ok && next && *next) {
    line = next; next = strchr(line, '\n'); if (next) *next++ = 0;
    if (count == MOTION_CAPACITY || !line[0] || line[1] != ',') { ok = false; break; }
    char *comma = strchr(line + 2, ','); uint32_t speed = 0, us = 0;
    if (!comma) { ok = false; break; }
    *comma++ = 0;
    ok = number(line + 2, &speed) && number(comma, &us) && motion_valid(line[0], speed) && us && us <= MOTION_MAX_US;
    if (ok) transfer_frames[count++] = (motion_frame_t){line[0], (uint8_t)speed, us};
  }
  // 整包和总时长都合法时才复制进执行缓冲；失败不会启动部分技能。
  if (ok) ok = motor_motion_load(token, transfer_frames, count);
  free(body);
  if (!ok) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid sequence");
  return motion_reply(req, false);
}

// 处理wifi事件
static void wifi_event(void *arg, esp_event_base_t base, int32_t id,
                       void *data) {
  (void)arg;
  (void)base;
  (void)data;
  // 当wifi连接断开时，停止电机
  if (id == WIFI_EVENT_AP_STADISCONNECTED) {
    motor_motion_cancel("disconnect");
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
  config.lru_purge_enable = true;
  config.stack_size = 8192;
  httpd_handle_t server = NULL;
  ESP_ERROR_CHECK(httpd_start(&server, &config));
  const httpd_uri_t command = {
      .uri = "/api/drive", .method = HTTP_POST, .handler = drive_handler};
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &command));
  const httpd_uri_t motion = {.uri = "/api/motion", .method = HTTP_POST, .handler = motion_handler};
  const httpd_uri_t sequence = {.uri = "/api/sequence", .method = HTTP_POST, .handler = sequence_handler};
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &motion));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &sequence));
  ESP_LOGI(TAG, "Connect to %s; use the Android controller", CAR_AP_SSID);
}


