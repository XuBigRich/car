if(CONFIG_CAR_GPIO_TEST)
    set(car_source "gpio_test.c")
else()
    set(car_source "main.c")
endif()

idf_component_register(SRCS "${car_source}"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_driver_ledc esp_driver_gpio esp_wifi esp_event esp_netif esp_http_server nvs_flash esp_timer
                    EMBED_TXTFILES "index.html")
