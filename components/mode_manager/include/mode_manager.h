#pragma once

#include "app_state.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

esp_err_t mode_manager_init(void);
QueueHandle_t mode_manager_get_event_queue(void);
esp_err_t mode_manager_get_state_snapshot(app_state_t *out_state);
