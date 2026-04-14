#include "mode_manager.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/task.h"

static const char *TAG = "mode_manager";
static QueueHandle_t s_event_queue;
static app_state_t s_state;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;

static void mode_task(void *arg)
{
    (void)arg;
    app_event_t event;
    while (1) {
        if (xQueueReceive(s_event_queue, &event, pdMS_TO_TICKS(50)) == pdTRUE) {
            taskENTER_CRITICAL(&s_state_lock);
            switch (event.type) {
            case APP_EVT_SET_MODE:
                s_state.mode = event.data.mode;
                s_state.flags.nvs_dirty = true;
                break;
            case APP_EVT_SET_MANUAL_COLOR:
                s_state.manual.color = event.data.manual_color;
                s_state.flags.nvs_dirty = true;
                break;
            case APP_EVT_SET_POWER:
                s_state.flags.power_on = event.data.flag;
                s_state.flags.nvs_dirty = true;
                break;
            case APP_EVT_SET_BRIGHTNESS_CAP:
                s_state.led.brightness_cap = event.data.brightness_cap;
                s_state.flags.nvs_dirty = true;
                break;
            case APP_EVT_SET_PALETTE:
                s_state.breathing.palette_id = event.data.palette_id;
                s_state.flags.nvs_dirty = true;
                break;
            case APP_EVT_SET_BREATHING_CFG:
                s_state.breathing = event.data.breathing;
                s_state.flags.nvs_dirty = true;
                break;
            case APP_EVT_WIFI_STATUS:
                s_state.flags.wifi_connected = event.data.flag;
                break;
            case APP_EVT_WEB_CLIENT_STATUS:
                s_state.flags.web_client_active = event.data.flag;
                break;
            case APP_EVT_PERSIST_NOW:
            default:
                break;
            }
            s_state.flags.last_change_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            taskEXIT_CRITICAL(&s_state_lock);
        }
    }
}

esp_err_t mode_manager_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.schema_version = 1;
    s_state.mode = MODE_SINGLE_COLOR;
    s_state.led.zone_count = 1;
    s_state.led.led_count_total = 20;
    s_state.led.brightness_cap = 128;
    s_state.manual.color = (hsv_t){.hue = 210, .sat = 255, .val = 100};
    s_state.breathing = (breathing_cfg_t){.period_ms = 2600, .min_val = 16, .max_val = 160, .palette_id = 0};
    s_state.flags.power_on = true;

    s_event_queue = xQueueCreate(20, sizeof(app_event_t));
    if (s_event_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(mode_task, "mode_task", 4096, NULL, 8, NULL, tskNO_AFFINITY);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create mode task");
        return ESP_FAIL;
    }
    return ESP_OK;
}

QueueHandle_t mode_manager_get_event_queue(void)
{
    return s_event_queue;
}

esp_err_t mode_manager_get_state_snapshot(app_state_t *out_state)
{
    if (out_state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_state_lock);
    *out_state = s_state;
    taskEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}
