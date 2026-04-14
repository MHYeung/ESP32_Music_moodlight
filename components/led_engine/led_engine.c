#include "led_engine.h"

#include <math.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#define LED_STRIP_GPIO  GPIO_NUM_10
#define LED_STRIP_COUNT 20
#define PALETTE_COLOR_COUNT 5
#define TWO_PI 6.2831853f

static const char *TAG = "led_engine";
static led_strip_handle_t s_strip = NULL;
static uint8_t  s_palette_indices[LED_STRIP_COUNT];
static mood_mode_t s_prev_mode    = MODE_SINGLE_COLOR;
static uint8_t  s_prev_palette    = 0xFF;
static float    s_breath_phase    = 0.0f;

typedef struct {
    hsv_t colors[PALETTE_COLOR_COUNT];
} hsv_palette_t;

static const hsv_palette_t s_palettes[] = {
    {.colors = {{15, 255, 255}, {28, 250, 255}, {350, 220, 255}, {300, 200, 255}, {45, 180, 255}}},
    {.colors = {{180, 255, 255}, {195, 255, 220}, {210, 255, 180}, {225, 220, 220}, {165, 255, 180}}},
    {.colors = {{80, 255, 160}, {95, 240, 210}, {120, 230, 170}, {140, 200, 130}, {60, 255, 120}}},
    {.colors = {{300, 255, 255}, {325, 255, 255}, {185, 255, 255}, {45, 255, 255}, {260, 255, 255}}},
};
static const uint8_t s_palette_count = sizeof(s_palettes) / sizeof(s_palettes[0]);

static void hsv_to_rgb(const hsv_t *hsv, uint8_t *r, uint8_t *g, uint8_t *b)
{
    float h  = (float)(hsv->hue % 360);
    float s  = hsv->sat / 255.0f;
    float v  = hsv->val / 255.0f;
    float c  = v * s;
    float x  = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m  = v - c;
    float rp = 0, gp = 0, bp = 0;
    if (h < 60)       { rp = c; gp = x; }
    else if (h < 120) { rp = x; gp = c; }
    else if (h < 180) { gp = c; bp = x; }
    else if (h < 240) { gp = x; bp = c; }
    else if (h < 300) { rp = x; bp = c; }
    else              { rp = c; bp = x; }
    *r = (uint8_t)((rp + m) * 255.0f);
    *g = (uint8_t)((gp + m) * 255.0f);
    *b = (uint8_t)((bp + m) * 255.0f);
}

static uint8_t scale_val(uint8_t val, uint8_t brightness_cap)
{
    uint16_t v = (uint16_t)val * brightness_cap / 255U;
    return (v > 255U) ? 255U : (uint8_t)v;
}

esp_err_t led_engine_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num        = LED_STRIP_GPIO,
        .max_leds              = LED_STRIP_COUNT,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .led_model             = LED_MODEL_WS2812,
        .flags.invert_out      = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src          = RMT_CLK_SRC_DEFAULT,
        .resolution_hz    = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags.with_dma   = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip));
    ESP_ERROR_CHECK(led_strip_clear(s_strip));
    for (uint8_t i = 0; i < LED_STRIP_COUNT; i++) {
        s_palette_indices[i] = (uint8_t)(esp_random() % PALETTE_COLOR_COUNT);
    }
    ESP_LOGI(TAG, "LED engine ready on GPIO%d, %d LEDs", LED_STRIP_GPIO, LED_STRIP_COUNT);
    return ESP_OK;
}

esp_err_t led_engine_set_all_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    if (s_strip == NULL) return ESP_ERR_INVALID_STATE;
    for (uint8_t i = 0; i < LED_STRIP_COUNT; i++) {
        ESP_ERROR_CHECK(led_strip_set_pixel(s_strip, i, red, green, blue));
    }
    return led_strip_refresh(s_strip);
}

esp_err_t led_engine_test_pattern(void)
{
    if (s_strip == NULL) return ESP_ERR_INVALID_STATE;
    const uint8_t colors[][3] = {{32, 0, 0}, {0, 32, 0}, {0, 0, 32}, {24, 24, 24}, {0, 0, 0}};
    for (size_t c = 0; c < (sizeof(colors) / sizeof(colors[0])); c++) {
        ESP_ERROR_CHECK(led_engine_set_all_rgb(colors[c][0], colors[c][1], colors[c][2]));
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    ESP_LOGI(TAG, "WS2812B test pattern complete");
    return ESP_OK;
}

esp_err_t led_engine_render(const app_state_t *state)
{
    if (state == NULL || s_strip == NULL) return ESP_ERR_INVALID_STATE;

    uint8_t active_leds = state->led.led_count_total;
    if (active_leds == 0 || active_leds > LED_STRIP_COUNT) {
        active_leds = LED_STRIP_COUNT;
    }

    if (!state->flags.power_on) {
        for (uint8_t i = 0; i < LED_STRIP_COUNT; i++) {
            led_strip_set_pixel(s_strip, i, 0, 0, 0);
        }
        return led_strip_refresh(s_strip);
    }

    if (state->mode == MODE_SINGLE_COLOR) {
        hsv_t color = state->manual.color;
        color.val = scale_val(color.val, state->led.brightness_cap);
        uint8_t r = 0, g = 0, b = 0;
        hsv_to_rgb(&color, &r, &g, &b);
        for (uint8_t i = 0; i < active_leds; i++) {
            led_strip_set_pixel(s_strip, i, r, g, b);
        }
    } else {
        uint8_t palette_id = state->breathing.palette_id;
        if (palette_id >= s_palette_count) palette_id = 0;
        const hsv_palette_t *palette = &s_palettes[palette_id];

        const float step = 0.03f * (2600.0f / (float)(state->breathing.period_ms ? state->breathing.period_ms : 2600));
        s_breath_phase += step;
        bool new_cycle = false;
        if (s_breath_phase > TWO_PI) {
            s_breath_phase -= TWO_PI;
            new_cycle = true;
        }
        if (state->mode != s_prev_mode || palette_id != s_prev_palette || new_cycle) {
            for (uint8_t i = 0; i < active_leds; i++) {
                s_palette_indices[i] = (uint8_t)(esp_random() % PALETTE_COLOR_COUNT);
            }
        }
        const float   breath_norm = 0.5f * (sinf(s_breath_phase) + 1.0f);
        const uint8_t breath_val  = (uint8_t)(state->breathing.min_val +
                                    breath_norm * (float)(state->breathing.max_val - state->breathing.min_val));
        const uint8_t output_val  = scale_val(breath_val, state->led.brightness_cap);
        for (uint8_t i = 0; i < active_leds; i++) {
            hsv_t color = palette->colors[s_palette_indices[i] % PALETTE_COLOR_COUNT];
            color.val = output_val;
            uint8_t r = 0, g = 0, b = 0;
            hsv_to_rgb(&color, &r, &g, &b);
            led_strip_set_pixel(s_strip, i, r, g, b);
        }
    }

    s_prev_mode    = state->mode;
    s_prev_palette = state->breathing.palette_id;
    for (uint8_t px = active_leds; px < LED_STRIP_COUNT; px++) {
        led_strip_set_pixel(s_strip, px, 0, 0, 0);
    }
    return led_strip_refresh(s_strip);
}
