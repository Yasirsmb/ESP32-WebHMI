idf_component_register(
    SRCS "data_logger.c" "ota_manager.c" "gpio_manager.c" "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_http_server
        spiffs
        nvs_flash
        esp_netif
        json
        driver
        app_update
        esp_timer
        esp_adc
)

# This tells IDF to build a SPIFFS image from your data folder
spiffs_create_partition_image(spiffs ../data FLASH_IN_PROJECT)
