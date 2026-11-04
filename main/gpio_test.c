#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "car_config.h"

static const char *TAG = "gpio_test";
static const gpio_num_t pins[] = {
    CAR_DRIVE_IN1, CAR_DRIVE_IN2, CAR_STEER_IN1, CAR_STEER_IN2,
};

static void all_low(void)
{
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        ESP_ERROR_CHECK(gpio_set_level(pins[i], 0));
    }
}

void app_main(void)
{
    uint64_t mask = 0;
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        configASSERT(GPIO_IS_VALID_OUTPUT_GPIO(pins[i]));
        ESP_ERROR_CHECK(gpio_reset_pin(pins[i]));
        ESP_ERROR_CHECK(gpio_set_level(pins[i], 0));
        mask |= 1ULL << pins[i];
    }
    const gpio_config_t config = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
    all_low();
    ESP_LOGW(TAG, "STANDALONE GPIO TEST: disconnect motor driver; Wi-Fi disabled");
    ESP_LOGI(TAG, "All LOW for 5 seconds before starting");
    vTaskDelay(pdMS_TO_TICKS(5000));

    while (true) {
        for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
            all_low();
            ESP_ERROR_CHECK(gpio_set_level(pins[i], 1));
            ESP_LOGI(TAG, "GPIO%d HIGH for 2 seconds; other test pins LOW", pins[i]);
            vTaskDelay(pdMS_TO_TICKS(2000));
            all_low();
            ESP_LOGI(TAG, "All LOW for 1 second");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        ESP_LOGI(TAG, "Cycle complete; all LOW for 3 seconds");
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
