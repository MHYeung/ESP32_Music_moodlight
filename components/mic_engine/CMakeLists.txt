idf_component_register(
    SRCS "mic_engine.c"
    INCLUDE_DIRS "include"
    REQUIRES driver freertos esp_driver_i2s
    PRIV_REQUIRES espressif__esp-dsp
)
