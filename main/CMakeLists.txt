if(CONFIG_CAR_GPIO_TEST)
    set(car_sources "diagnostics/gpio_test.c")
else()
    set(car_sources "main.c" "drivers/motor.c" "drivers/motion.c" "network/network.c")
endif()

idf_component_register(SRCS ${car_sources}
                       PRIV_INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_gpio esp_driver_ledc esp_timer
                                     esp_wifi esp_event esp_netif esp_http_server nvs_flash)
