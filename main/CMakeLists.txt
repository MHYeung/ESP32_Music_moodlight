idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos
        nvs_flash
        esp_event
        esp_netif
        esp_wifi
        esp_http_server
        mode_manager
        led_engine
        mic_engine
        joltwallet__littlefs
)

littlefs_create_partition_image(littlefs ../data FLASH_IN_PROJECT)
