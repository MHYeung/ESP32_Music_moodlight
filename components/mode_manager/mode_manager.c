#include "mode_manager.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#define APP_NVS_NAMESPACE    "moodlight"
#define APP_NVS_SCHEMA_VER   2u

static const char *TAG = "mode_manager";
static QueueHandle_t s_event_queue;
static app_state_t   s_state;
static portMUX_TYPE  s_state_lock = portMUX_INITIALIZER_UNLOCKED;

/* ── NVS helpers ─────────────────────────────────────────────────────────── */

static void nvs_load_settings(app_state_t *s)
{
    nvs_handle_t h;
    if (nvs_open(APP_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "No NVS namespace found, using defaults");
        return;
    }

    uint16_t schema = 0;
    if (nvs_get_u16(h, "schema_v", &schema) != ESP_OK || schema != APP_NVS_SCHEMA_VER) {
        ESP_LOGW(TAG, "NVS schema mismatch (stored=%u, current=%u), using defaults",
                 schema, APP_NVS_SCHEMA_VER);
        nvs_close(h);
        return;
    }

    uint8_t u8; uint16_t u16;

    if (nvs_get_u8 (h, "mode",      &u8)  == ESP_OK) s->mode                  = (mood_mode_t)u8;
    if (nvs_get_u16(h, "hue",       &u16) == ESP_OK) s->manual.color.hue      = u16;
    if (nvs_get_u8 (h, "sat",       &u8)  == ESP_OK) s->manual.color.sat      = u8;
    if (nvs_get_u8 (h, "val",       &u8)  == ESP_OK) s->manual.color.val      = u8;
    if (nvs_get_u8 (h, "br_cap",    &u8)  == ESP_OK) s->led.brightness_cap    = u8;
    if (nvs_get_u16(h, "br_period", &u16) == ESP_OK) s->breathing.period_ms   = u16;
    if (nvs_get_u8 (h, "br_min",    &u8)  == ESP_OK) s->breathing.min_val     = u8;
    if (nvs_get_u8 (h, "br_max",    &u8)  == ESP_OK) s->breathing.max_val     = u8;
    if (nvs_get_u8 (h, "br_pal",    &u8)  == ESP_OK) s->breathing.palette_id  = u8;
    if (nvs_get_u8 (h, "power",     &u8)  == ESP_OK) s->flags.power_on        = (bool)u8;
    if (nvs_get_u16(h, "bpm",       &u16) == ESP_OK) s->beat.bpm              = u16;
    if (nvs_get_u8 (h, "beat_pct",  &u8)  == ESP_OK) s->beat.on_pct           = u8;
    if (nvs_get_u8 (h, "mus_sens",  &u8)  == ESP_OK) s->music.sensitivity     = u8;
    if (nvs_get_u8 (h, "mus_floor", &u8)  == ESP_OK) s->music.noise_floor     = u8;
    if (nvs_get_u8 (h, "mus_alpha",  &u8) == ESP_OK) s->music.ema_alpha       = u8;
    if (nvs_get_u8 (h, "mus_spread", &u8) == ESP_OK) s->music.hue_spread      = u8;

    size_t cp_len = sizeof(s->custom_palette);
    nvs_get_blob(h, "cp_blob", &s->custom_palette, &cp_len);

    nvs_close(h);
    ESP_LOGI(TAG, "Settings restored from NVS");
}

static void nvs_save_settings(const app_state_t *s)
{
    nvs_handle_t h;
    if (nvs_open(APP_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open for write failed");
        return;
    }

    nvs_set_u16(h, "schema_v",  APP_NVS_SCHEMA_VER);
    nvs_set_u8 (h, "mode",      (uint8_t)s->mode);
    nvs_set_u16(h, "hue",       s->manual.color.hue);
    nvs_set_u8 (h, "sat",       s->manual.color.sat);
    nvs_set_u8 (h, "val",       s->manual.color.val);
    nvs_set_u8 (h, "br_cap",    s->led.brightness_cap);
    nvs_set_u16(h, "br_period", s->breathing.period_ms);
    nvs_set_u8 (h, "br_min",    s->breathing.min_val);
    nvs_set_u8 (h, "br_max",    s->breathing.max_val);
    nvs_set_u8 (h, "br_pal",    s->breathing.palette_id);
    nvs_set_u8 (h, "power",     (uint8_t)s->flags.power_on);
    nvs_set_u16(h, "bpm",       s->beat.bpm);
    nvs_set_u8 (h, "beat_pct",  s->beat.on_pct);
    nvs_set_u8 (h, "mus_sens",  s->music.sensitivity);
    nvs_set_u8 (h, "mus_floor", s->music.noise_floor);
    nvs_set_u8 (h, "mus_alpha",  s->music.ema_alpha);
    nvs_set_u8 (h, "mus_spread", s->music.hue_spread);
    nvs_set_blob(h, "cp_blob",  &s->custom_palette, sizeof(s->custom_palette));

    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Settings persisted to NVS");
}

/* ── mode_task ───────────────────────────────────────────────────────────── */

static void mode_task(void *arg)
{
    (void)arg;
    app_event_t  event;
    TickType_t   last_dirty_tick = 0;

    while (1) {
        if (xQueueReceive(s_event_queue, &event, pdMS_TO_TICKS(50)) == pdTRUE) {
            bool became_dirty = false;
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
            case APP_EVT_SET_CUSTOM_PALETTE:
                s_state.custom_palette = event.data.custom_palette;
                s_state.flags.nvs_dirty = true;
                break;
            case APP_EVT_SET_BEAT_CFG:
                s_state.beat = event.data.beat;
                s_state.flags.nvs_dirty = true;
                break;
            case APP_EVT_SET_MUSIC_CFG:
                s_state.music = event.data.music_cfg;
                s_state.flags.nvs_dirty = true;
                break;
            case APP_EVT_WIFI_STATUS:
                s_state.flags.wifi_connected = event.data.flag;
                break;
            case APP_EVT_WEB_CLIENT_STATUS:
                s_state.flags.web_client_active = event.data.flag;
                break;
            case APP_EVT_PERSIST_NOW:
                s_state.flags.nvs_dirty = true;
                last_dirty_tick = 0; /* force immediate save on next check */
                break;
            default:
                break;
            }
            became_dirty = s_state.flags.nvs_dirty;
            s_state.flags.last_change_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            taskEXIT_CRITICAL(&s_state_lock);

            if (became_dirty) {
                last_dirty_tick = xTaskGetTickCount();
            }
        }

        /* Debounced NVS save: write 2 s after the last state change */
        taskENTER_CRITICAL(&s_state_lock);
        bool dirty = s_state.flags.nvs_dirty;
        taskEXIT_CRITICAL(&s_state_lock);

        if (dirty && (xTaskGetTickCount() - last_dirty_tick) >= pdMS_TO_TICKS(2000)) {
            app_state_t snap;
            taskENTER_CRITICAL(&s_state_lock);
            snap = s_state;
            s_state.flags.nvs_dirty = false;
            taskEXIT_CRITICAL(&s_state_lock);
            nvs_save_settings(&snap);
        }
    }
}

/* ── public API ──────────────────────────────────────────────────────────── */

esp_err_t mode_manager_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.schema_version          = APP_NVS_SCHEMA_VER;
    s_state.mode                    = MODE_SINGLE_COLOR;
    s_state.led.zone_count          = 1;
    s_state.led.led_count_total     = 31;
    s_state.led.brightness_cap      = 128;
    s_state.manual.color            = (hsv_t){.hue = 210, .sat = 255, .val = 100};
    s_state.breathing               = (breathing_cfg_t){.period_ms = 2600, .min_val = 16,
                                                         .max_val = 160, .palette_id = 0};
    s_state.beat                    = (beat_cfg_t){.bpm = 120, .on_pct = 20};
    s_state.music                   = (music_cfg_t){.sensitivity = 5, .noise_floor = 10,
                                                    .ema_alpha = 20, .hue_spread = 60};
    s_state.custom_palette.colors[0] = (hsv_t){.hue =   0, .sat = 255, .val = 200};
    s_state.custom_palette.colors[1] = (hsv_t){.hue =  30, .sat = 255, .val = 200};
    s_state.custom_palette.colors[2] = (hsv_t){.hue = 120, .sat = 255, .val = 200};
    s_state.custom_palette.colors[3] = (hsv_t){.hue = 200, .sat = 255, .val = 200};
    s_state.custom_palette.colors[4] = (hsv_t){.hue = 280, .sat = 255, .val = 200};
    s_state.flags.power_on          = true;

    /* Override defaults with whatever was persisted last */
    nvs_load_settings(&s_state);

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
