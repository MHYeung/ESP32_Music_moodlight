#include "led_engine.h"

#include <math.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "mic_engine.h"

#define LED_STRIP_GPIO GPIO_NUM_10
#define LED_STRIP_COUNT 31
#define STATUS_LED_GPIO GPIO_NUM_48
#define PALETTE_COLOR_COUNT 5
#define TWO_PI 6.2831853f
#define HUE_ROTATION_STEP 0.5f /* degrees per render frame (~15 deg/s at 33 ms) */

static const char *TAG = "led_engine";
static led_strip_handle_t s_strip = NULL;
static led_strip_handle_t s_status_strip = NULL;
static float s_breath_phase = 0.0f;
static float s_hue_offset = 0.0f; /* gradient rotation state */
static float    s_music_hue             = 0.0f;  /* EMA-smoothed displayed hue */
static float    s_music_peak            = 0.0f;  /* Peak-followed normalised amplitude */
static float    s_music_target_hue      = 0.0f;  /* Random target the EMA is chasing */
static uint64_t s_music_target_next_us  = 0;     /* When to re-roll the target */

/* α for hue smoothing — slow enough that transitions between random targets
 * feel like a smooth colour wash rather than teleporting between values.   */
#define MUSIC_HUE_ALPHA          0.03f
#define MUSIC_PEAK_DECAY         0.92f
#define MUSIC_MIN_HEIGHT_PCT     20   /* minimum percentage of LEDs to light up */
#define MUSIC_MIN_VAL            40

/* Random-target interval window. Each time the timer expires we pick a new
 * random hue AND a new random duration in this range, so the colour rhythm
 * isn't metronomic. 3 s min keeps transitions readable; 8 s max prevents a
 * stuck hue when the user is just sitting with music on.                   */
#define MUSIC_HUE_RETARGET_MIN_US  3000000ULL
#define MUSIC_HUE_RETARGET_MAX_US  8000000ULL

typedef struct
{
    hsv_t colors[PALETTE_COLOR_COUNT];
} hsv_palette_t;

static const hsv_palette_t s_palettes[] = {
    {.colors = {{15, 255, 255}, {28, 250, 255}, {350, 220, 255}, {300, 200, 255}, {45, 180, 255}}},
    {.colors = {{180, 255, 255}, {195, 255, 220}, {210, 255, 180}, {225, 220, 220}, {165, 255, 180}}},
    {.colors = {{80, 255, 160}, {95, 240, 210}, {120, 230, 170}, {140, 200, 130}, {60, 255, 120}}},
    {.colors = {{300, 255, 255}, {325, 255, 255}, {185, 255, 255}, {45, 255, 255}, {260, 255, 255}}},
};
static const uint8_t s_palette_count = sizeof(s_palettes) / sizeof(s_palettes[0]);

/* ── colour utilities ────────────────────────────────────────────────────── */

static void hsv_to_rgb(const hsv_t *hsv, uint8_t *r, uint8_t *g, uint8_t *b)
{
    float h = (float)(hsv->hue % 360);
    float s = hsv->sat / 255.0f;
    float v = hsv->val / 255.0f;
    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    float rp = 0, gp = 0, bp = 0;
    if (h < 60)
    {
        rp = c;
        gp = x;
    }
    else if (h < 120)
    {
        rp = x;
        gp = c;
    }
    else if (h < 180)
    {
        gp = c;
        bp = x;
    }
    else if (h < 240)
    {
        gp = x;
        bp = c;
    }
    else if (h < 300)
    {
        rp = x;
        bp = c;
    }
    else
    {
        rp = c;
        bp = x;
    }
    *r = (uint8_t)((rp + m) * 255.0f);
    *g = (uint8_t)((gp + m) * 255.0f);
    *b = (uint8_t)((bp + m) * 255.0f);
}

static uint8_t scale_val(uint8_t val, uint8_t brightness_cap)
{
    uint16_t v = (uint16_t)val * brightness_cap / 255U;
    return (v > 255U) ? 255U : (uint8_t)v;
}

/* ── init / helpers ──────────────────────────────────────────────────────── */

esp_err_t led_engine_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_STRIP_GPIO,
        .max_leds = LED_STRIP_COUNT,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip));
    ESP_ERROR_CHECK(led_strip_clear(s_strip));

    strip_config.strip_gpio_num = STATUS_LED_GPIO;
    strip_config.max_leds = 1;
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_status_strip));
    ESP_ERROR_CHECK(led_strip_clear(s_status_strip));

    ESP_LOGI(TAG, "LED engine ready on GPIO%d, %d LEDs", LED_STRIP_GPIO, LED_STRIP_COUNT);
    return ESP_OK;
}

esp_err_t led_engine_set_status(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_status_strip == NULL)
        return ESP_ERR_INVALID_STATE;
    ESP_ERROR_CHECK(led_strip_set_pixel(s_status_strip, 0, r, g, b));
    return led_strip_refresh(s_status_strip);
}

esp_err_t led_engine_set_all_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    if (s_strip == NULL)
        return ESP_ERR_INVALID_STATE;
    for (uint8_t i = 0; i < LED_STRIP_COUNT; i++)
    {
        ESP_ERROR_CHECK(led_strip_set_pixel(s_strip, i, red, green, blue));
    }
    return led_strip_refresh(s_strip);
}

esp_err_t led_engine_test_pattern(void)
{
    if (s_strip == NULL)
        return ESP_ERR_INVALID_STATE;
    const uint8_t colors[][3] = {{32, 0, 0}, {0, 32, 0}, {0, 0, 32}, {24, 24, 24}, {0, 0, 0}};
    for (size_t c = 0; c < (sizeof(colors) / sizeof(colors[0])); c++)
    {
        ESP_ERROR_CHECK(led_engine_set_all_rgb(colors[c][0], colors[c][1], colors[c][2]));
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    ESP_LOGI(TAG, "WS2812B test pattern complete");
    return ESP_OK;
}

/* ── render ──────────────────────────────────────────────────────────────── */

esp_err_t led_engine_render(const app_state_t *state)
{
    if (state == NULL || s_strip == NULL)
        return ESP_ERR_INVALID_STATE;

    uint8_t active_leds = state->led.led_count_total;
    if (active_leds == 0 || active_leds > LED_STRIP_COUNT)
    {
        active_leds = LED_STRIP_COUNT;
    }

    /* Power-off: blank the main strip only. The onboard status LED on GPIO48
     * keeps showing Wi-Fi state (yellow=connecting, green=connected, red=fail)
     * regardless of power_on, so the user always has a network indicator. */
    if (!state->flags.power_on)
    {
        for (uint8_t i = 0; i < LED_STRIP_COUNT; i++)
        {
            led_strip_set_pixel(s_strip, i, 0, 0, 0);
        }
        return led_strip_refresh(s_strip);
    }

    /* ── MODE_SINGLE_COLOR ─────────────────────────────────────────────── */
    if (state->mode == MODE_SINGLE_COLOR)
    {
        hsv_t color = state->manual.color;
        color.val = scale_val(color.val, state->led.brightness_cap);
        uint8_t r = 0, g = 0, b = 0;
        hsv_to_rgb(&color, &r, &g, &b);
        for (uint8_t i = 0; i < active_leds; i++)
        {
            led_strip_set_pixel(s_strip, i, r, g, b);
        }

        /* ── MODE_PALETTE_BREATHING (gradient wave) ────────────────────────── */
    }
    else if (state->mode == MODE_PALETTE_BREATHING)
    {
        /* Advance breath phase */
        float period_ms = (float)(state->breathing.period_ms ? state->breathing.period_ms : 2600);
        float step = 0.03f * (2600.0f / period_ms);
        s_breath_phase += step;
        if (s_breath_phase > TWO_PI)
            s_breath_phase -= TWO_PI;

        /* Slowly rotate the hue gradient each frame */
        s_hue_offset = fmodf(s_hue_offset + HUE_ROTATION_STEP, 360.0f);

        /* Sinusoidal brightness envelope (same for all LEDs) */
        float breath_norm = 0.5f * (sinf(s_breath_phase) + 1.0f);
        uint8_t breath_val = (uint8_t)(state->breathing.min_val +
                                       breath_norm * (float)(state->breathing.max_val - state->breathing.min_val));
        uint8_t output_val = scale_val(breath_val, state->led.brightness_cap);

        /* Select palette (custom when id >= built-in count) */
        hsv_palette_t custom_buf;
        const hsv_palette_t *palette;
        if (state->breathing.palette_id < s_palette_count)
        {
            palette = &s_palettes[state->breathing.palette_id];
        }
        else
        {
            for (uint8_t k = 0; k < PALETTE_COLOR_COUNT; k++)
            {
                custom_buf.colors[k] = state->custom_palette.colors[k];
            }
            palette = &custom_buf;
        }

        /* Paint each LED with a smoothly interpolated hue from the palette */
        for (uint8_t i = 0; i < active_leds; i++)
        {
            /* Map LED index onto palette control points [0, PALETTE_COLOR_COUNT-1] */
            float t = (float)i / (float)(active_leds > 1 ? active_leds - 1 : 1) * (float)(PALETTE_COLOR_COUNT - 1);
            int seg = (int)t;
            if (seg >= PALETTE_COLOR_COUNT - 1)
                seg = PALETTE_COLOR_COUNT - 2;
            float frac = t - (float)seg;

            const hsv_t *c0 = &palette->colors[seg];
            const hsv_t *c1 = &palette->colors[seg + 1];

            /* Shortest-path hue interpolation on the colour wheel */
            float h0 = (float)c0->hue;
            float h1 = (float)c1->hue;
            float dh = h1 - h0;
            if (dh > 180.0f)
                dh -= 360.0f;
            if (dh < -180.0f)
                dh += 360.0f;
            float hue = fmodf(h0 + dh * frac + s_hue_offset + 360.0f, 360.0f);
            uint8_t sat = (uint8_t)((float)c0->sat * (1.0f - frac) + (float)c1->sat * frac);

            hsv_t color = {.hue = (uint16_t)hue, .sat = sat, .val = output_val};
            uint8_t r = 0, g = 0, b = 0;
            hsv_to_rgb(&color, &r, &g, &b);
            led_strip_set_pixel(s_strip, i, r, g, b);
        }

        /* ── MODE_BEAT_FLASH ───────────────────────────────────────────────── */
    }
    else if (state->mode == MODE_BEAT_FLASH)
    {
        uint16_t bpm = state->beat.bpm > 0 ? state->beat.bpm : 120;
        uint32_t period_ms = 60000UL / bpm;
        uint32_t on_ms = period_ms * state->beat.on_pct / 100U;
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        bool led_on = (now_ms % period_ms) < on_ms;

        uint8_t r = 0, g = 0, b = 0;
        if (led_on)
        {
            hsv_t color = state->manual.color;
            color.val = scale_val(color.val, state->led.brightness_cap);
            hsv_to_rgb(&color, &r, &g, &b);
        }
        for (uint8_t i = 0; i < active_leds; i++)
        {
            led_strip_set_pixel(s_strip, i, r, g, b);
        }

        /* ── MODE_MUSIC_REACT ──────────────────────────────────────────────── */
    }
    else if (state->mode == MODE_MUSIC_REACT)
    {
        /* Forward user-adjusted EMA alpha to mic_engine before sampling */
        mic_engine_set_ema_alpha((float)state->music.ema_alpha / 100.0f);

        music_analysis_t analysis = {0};
        mic_engine_get_analysis(&analysis);

        /* ── Hue: pick a new RANDOM target every few seconds, then EMA-smooth
         * s_music_hue toward it. This replaces the old weighted_hue tracking,
         * which tended to lock onto a stable spectral centroid and never move.
         * Shortest-path interpolation across the 0°/360° seam is still needed
         * so the wheel doesn't spin the long way around. */
        uint64_t now_us = (uint64_t)esp_timer_get_time();
        if (now_us >= s_music_target_next_us) {
            s_music_target_hue     = (float)(esp_random() % 360u);
            uint64_t span          = MUSIC_HUE_RETARGET_MAX_US - MUSIC_HUE_RETARGET_MIN_US;
            s_music_target_next_us = now_us + MUSIC_HUE_RETARGET_MIN_US +
                                     ((uint64_t)esp_random() % span);
        }
        float dh = s_music_target_hue - s_music_hue;
        if (dh >  180.0f) dh -= 360.0f;
        if (dh < -180.0f) dh += 360.0f;
        s_music_hue = fmodf(s_music_hue + MUSIC_HUE_ALPHA * dh + 360.0f, 360.0f);

        /* ── Brightness: amplitude above noise floor, normalised by sensitivity */
        int32_t amp_above = (int32_t)analysis.smooth_amplitude - (int32_t)state->music.noise_floor * 256;
        if (amp_above < 0)
            amp_above = 0;
        float norm = (float)amp_above / (float)(state->music.sensitivity * 6553);
        if (norm > 1.0f)
            norm = 1.0f;
        if (norm > s_music_peak)
        {
            s_music_peak = norm;
        }
        else
        {
            s_music_peak *= MUSIC_PEAK_DECAY;
        }
        /* Idle floor: scales with active_leds so short strips still look right. */
        uint8_t min_height = (active_leds * MUSIC_MIN_HEIGHT_PCT) / 100;
        /* Linear remap: s_music_peak ∈ [0,1] → height ∈ [min_height, active_leds] */
        float height_f = (float)min_height +
                         s_music_peak * (float)(active_leds - min_height) + 0.5f;
        uint8_t height = (uint8_t)height_f;
        if (height > active_leds)
            height = active_leds; /* defensive, unreachable */
        /* Linear remap: s_music_peak ∈ [0,1] → val ∈ [MUSIC_MIN_VAL, 255]        */
        float val_f = (float)MUSIC_MIN_VAL +
                      s_music_peak * (255.0f - (float)MUSIC_MIN_VAL);
        uint8_t val = scale_val((uint8_t)val_f, state->led.brightness_cap);
        hsv_t color = {.hue = (uint16_t)s_music_hue, .sat = 255, .val = val};
        uint8_t r = 0, g = 0, b = 0;
        hsv_to_rgb(&color, &r, &g, &b);
        for (uint8_t i = 0; i < active_leds; i++)
        {
            if (i < height)
                led_strip_set_pixel(s_strip, i, r, g, b);
            else
                led_strip_set_pixel(s_strip, i, 0, 0, 0);
        }
    }

    /* Zero out unused tail pixels */
    for (uint8_t px = active_leds; px < LED_STRIP_COUNT; px++)
    {
        led_strip_set_pixel(s_strip, px, 0, 0, 0);
    }
    return led_strip_refresh(s_strip);
}
