#pragma once

#include "app_state.h"
#include "esp_err.h"

esp_err_t led_engine_init(void);
esp_err_t led_engine_test_pattern(void);
esp_err_t led_engine_render(const app_state_t *state);
esp_err_t led_engine_set_all_rgb(uint8_t red, uint8_t green, uint8_t blue);
esp_err_t led_engine_set_status(uint8_t r, uint8_t g, uint8_t b);
