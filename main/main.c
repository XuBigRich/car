#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "car_config.h"

static const char *TAG = "car";
static SemaphoreHandle_t motor_lock;
static int last_throttle, last_steering;
static bool braking;
static int64_t last_command;
static int steer_request;
static int64_t steer_started, steer_ready;
static bool steer_locked;

_Static_assert(CAR_STEER_START_DUTY > 0 && CAR_STEER_START_DUTY <= 100, "Invalid start duty");
_Static_assert(CAR_STEER_HOLD_DUTY >= 0 && CAR_STEER_HOLD_DUTY <= CAR_STEER_START_DUTY, "Invalid hold duty");
_Static_assert(CAR_STEER_MAX_MS >= CAR_STEER_START_MS && CAR_STEER_START_MS > 0, "Invalid steering timing");
extern const char page_start[] asm("_binary_index_html_start");
extern const char page_end[] asm("_binary_index_html_end");

static void duty(int channel, unsigned value)
{
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, value));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, channel));
}

static void wheel(int channel, int speed)
{
    // Clear the inactive input first; never briefly assert both inputs on reversal.
    int active = channel + (speed < 0 ? 1 : 0);
    int inactive = channel + (speed < 0 ? 0 : 1);
    duty(inactive, 0);
    duty(active, (unsigned)(speed < 0 ? -speed : speed) * 1024 / 100);
}

// All steering state and motor writes are protected by motor_lock.
static void steering_update(int64_t now)
{
    int output = 0;
    if (steer_request && !steer_locked && now >= steer_ready) {
        int64_t elapsed = now - steer_started;
        if (elapsed >= CAR_STEER_MAX_MS * 1000LL) {
            steer_locked = true;
            ESP_LOGW(TAG, "Steering timeout: release turn button to rearm");
        } else {
            int power = elapsed < CAR_STEER_START_MS * 1000LL ?
                        CAR_STEER_START_DUTY : CAR_STEER_HOLD_DUTY;
            output = steer_request * power;
        }
    }
    if (output != last_steering) {
        wheel(2, CAR_STEER_REVERSED ? -output : output);
        last_steering = output;
        ESP_LOGI(TAG, "Steering output=%d locked=%d", output, steer_locked);
    }
}

static void steering_command(int request, int64_t now)
{
    request = (request > 0) - (request < 0);
    if (!request) {
        if (steer_request) steer_ready = now + CAR_STEER_REVERSE_MS * 1000LL;
        steer_request = 0;
        steer_locked = false;
    } else if (!steer_locked && request != steer_request) {
        // Changing directly from left to right cannot extend the total on-time.
        if (steer_request) {
            wheel(2, 0);
            last_steering = 0;
            steer_ready = now + CAR_STEER_REVERSE_MS * 1000LL;
        } else {
            if (steer_ready < now) steer_ready = now;
            steer_started = steer_ready;
        }
        steer_request = request;
    }
    steering_update(now);
}

static void drive(int throttle, int steering, bool brake)
{
    if (CAR_DRIVE_REVERSED) throttle = -throttle;
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

static void watchdog_task(void *arg)
{
    (void)arg;
    while (true) {
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

static esp_err_t page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, page_start, page_end - page_start - 1);
}

static esp_err_t drive_handler(httpd_req_t *req)
{
    char body[24];
    if (req->content_len == 0 || req->content_len >= sizeof(body))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid command");
    size_t received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) return ESP_FAIL;
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
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Speed must be 0..100");
    int throttle = 0, steering = 0;
    bool brake = false;
    switch (body[0]) {
    case 'f': throttle = speed; break;
    case 'b': throttle = -speed; break;
    case 'l': steering = -1; break;
    case 'r': steering = 1; break;
    case 'q': throttle = speed; steering = -1; break;
    case 'e': throttle = speed; steering = 1; break;
    case 'z': throttle = -speed; steering = -1; break;
    case 'c': throttle = -speed; steering = 1; break;
    case 's': break;
    case 'x': brake = true; break;
    default: return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid direction");
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

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        xSemaphoreTake(motor_lock, portMAX_DELAY);
        drive(0, 0, false);
        xSemaphoreGive(motor_lock);
    }
}

void app_main(void)
{
    motor_lock = xSemaphoreCreateMutex();
    configASSERT(motor_lock);
    const int pins[] = {CAR_DRIVE_IN1, CAR_DRIVE_IN2, CAR_STEER_IN1, CAR_STEER_IN2};
    uint64_t mask = 0;
    for (int i = 0; i < 4; ++i) {
        configASSERT(GPIO_IS_VALID_OUTPUT_GPIO(pins[i]));
        for (int j = 0; j < i; ++j) configASSERT(pins[i] != pins[j]);
        mask |= 1ULL << pins[i];
        ESP_ERROR_CHECK(gpio_reset_pin(pins[i]));
        ESP_ERROR_CHECK(gpio_set_level(pins[i], 0));
    }
    gpio_config_t gpio = {.pin_bit_mask = mask, .mode = GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&gpio));
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0, .freq_hz = CAR_PWM_HZ, .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    for (int i = 0; i < 4; ++i) {
        ledc_channel_config_t channel = {
            .gpio_num = pins[i], .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = i, .timer_sel = LEDC_TIMER_0, .duty = 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&channel));
    }
    configASSERT(xTaskCreate(watchdog_task, "motor_watchdog", 3072, NULL, 5, NULL) == pdPASS);
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();
    configASSERT(ap);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, wifi_event, NULL));
    wifi_config_t wifi = {.ap = {
        .ssid = CAR_AP_SSID, .password = CAR_AP_PASSWORD,
        .channel = 6, .max_connection = 1, .authmode = WIFI_AUTH_WPA2_PSK,
    }};
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi));
    ESP_ERROR_CHECK(esp_wifi_start());
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.recv_wait_timeout = 1;
    config.send_wait_timeout = 1;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    const httpd_uri_t page = {.uri = "/", .method = HTTP_GET, .handler = page_handler};
    const httpd_uri_t command = {.uri = "/api/drive", .method = HTTP_POST, .handler = drive_handler};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &page));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &command));
    ESP_LOGI(TAG, "Connect to %s; open http://192.168.4.1", CAR_AP_SSID);
}
