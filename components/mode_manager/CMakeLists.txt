idf_component_register(
    SRCS "mode_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos nvs_flash
)
