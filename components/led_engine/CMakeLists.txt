idf_component_register(
    SRCS "led_engine.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_rmt led_strip mode_manager
)
