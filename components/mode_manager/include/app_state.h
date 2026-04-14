#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CUSTOM_PALETTE_SIZE 5

typedef enum {
    MODE_SINGLE_COLOR = 0,
    MODE_PALETTE_BREATHING = 1,
} mood_mode_t;

typedef struct {
    uint16_t hue;
    uint8_t sat;
    uint8_t val;
} hsv_t;

typedef struct {
    uint8_t zone_count;
    uint8_t led_count_total;
    uint8_t brightness_cap;
    bool power_safe_mode;
} led_cfg_t;

typedef struct {
    hsv_t color;
} manual_cfg_t;

typedef struct {
    uint16_t period_ms;
    uint8_t min_val;
    uint8_t max_val;
    uint8_t palette_id;
} breathing_cfg_t;

typedef struct {
    bool wifi_connected;
    bool web_client_active;
    bool led_ready;
    bool nvs_dirty;
    bool power_on;
    uint32_t last_change_ms;
} runtime_flags_t;

typedef struct {
    hsv_t colors[CUSTOM_PALETTE_SIZE];
} custom_palette_data_t;

typedef struct {
    uint16_t schema_version;
    mood_mode_t mode;
    led_cfg_t led;
    manual_cfg_t manual;
    breathing_cfg_t breathing;
    custom_palette_data_t custom_palette;
    runtime_flags_t flags;
} app_state_t;

typedef enum {
    APP_EVT_SET_MODE = 0,
    APP_EVT_SET_MANUAL_COLOR,
    APP_EVT_SET_POWER,
    APP_EVT_SET_BRIGHTNESS_CAP,
    APP_EVT_SET_PALETTE,
    APP_EVT_SET_BREATHING_CFG,
    APP_EVT_SET_CUSTOM_PALETTE,
    APP_EVT_WIFI_STATUS,
    APP_EVT_WEB_CLIENT_STATUS,
    APP_EVT_PERSIST_NOW,
} app_event_type_t;

typedef struct {
    app_event_type_t type;
    union {
        mood_mode_t mode;
        hsv_t manual_color;
        uint8_t palette_id;
        uint8_t brightness_cap;
        breathing_cfg_t breathing;
        custom_palette_data_t custom_palette;
        bool flag;
    } data;
} app_event_t;
