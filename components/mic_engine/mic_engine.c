#include "mic_engine.h"

#include <math.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "dsps_fft2r.h"
#include "dsps_wind.h"

/* ── INMP441 wiring — change these to match your schematic ──────────────── */
#define MIC_SCK_IO      GPIO_NUM_6   /* Bit-clock  (BCLK) */
#define MIC_WS_IO       GPIO_NUM_7   /* Word-select (LRCLK) */
#define MIC_SD_IO        GPIO_NUM_8   /* Serial data (SD) */

/* ── Audio capture parameters ────────────────────────────────────────────── */
#define TWO_PI           6.2831853f
#define MIC_SAMPLE_RATE  16000
#define MIC_FFT_SIZE     256         /* Must be power-of-2; governs freq resolution */
#define MIC_DMA_DESC     4           /* Number of DMA descriptors (ring depth) */
#define MIC_TASK_STACK   6144
#define MIC_TASK_PRIO    4

static const char *TAG = "mic_engine";

static i2s_chan_handle_t  s_rx_chan  = NULL;
static TaskHandle_t       s_task_h  = NULL;
static volatile bool      s_running = false;

/* Shared analysis result, protected by spinlock */
static portMUX_TYPE      s_lock     = portMUX_INITIALIZER_UNLOCKED;
static music_analysis_t  s_analysis = {0};

/* EMA state — alpha is written by the LED render task via mic_engine_set_ema_alpha() */
static portMUX_TYPE      s_alpha_lock = portMUX_INITIALIZER_UNLOCKED;
static float             s_ema_alpha  = 0.20f; /* default α=0.20 */
static float             s_smooth_amp = 0.0f;  /* carries between DMA frames */

/* Static buffers to keep heap / task-stack usage predictable */
static int32_t s_raw_buf[MIC_FFT_SIZE];
static float   s_fft_buf[MIC_FFT_SIZE * 2]; /* complex interleaved: re,im,re,im,... */
static float   s_window[MIC_FFT_SIZE];

/* ── internal task ───────────────────────────────────────────────────────── */

static void mic_task(void *arg)
{
    (void)arg;
    size_t bytes_read = 0;

    while (s_running) {
        esp_err_t err = i2s_channel_read(s_rx_chan, s_raw_buf, sizeof(s_raw_buf),
                                          &bytes_read, pdMS_TO_TICKS(200));
        if (err != ESP_OK || bytes_read == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        int frames = (int)(bytes_read / sizeof(int32_t));
        if (frames > MIC_FFT_SIZE) frames = MIC_FFT_SIZE;

        /* Build windowed complex input; INMP441 data is 24-bit MSB in 32-bit word */
        float rms_accum = 0.0f;
        for (int i = 0; i < frames; i++) {
            float sample = (float)(s_raw_buf[i] >> 8) / (float)(1 << 23);
            float windowed = sample * s_window[i];
            s_fft_buf[i * 2]     = windowed; /* real */
            s_fft_buf[i * 2 + 1] = 0.0f;    /* imag */
            rms_accum += sample * sample;
        }
        /* Zero-pad if fewer samples were returned than FFT size */
        for (int i = frames; i < MIC_FFT_SIZE; i++) {
            s_fft_buf[i * 2]     = 0.0f;
            s_fft_buf[i * 2 + 1] = 0.0f;
        }

        float rms = sqrtf(rms_accum / (float)frames);

        /* In-place radix-2 FFT followed by bit-reversal reordering */
        dsps_fft2r_fc32(s_fft_buf, MIC_FFT_SIZE);
        dsps_bit_rev2r_fc32(s_fft_buf, MIC_FFT_SIZE);

        /* Compute energy in MIC_BAND_COUNT equally-spaced frequency bands.
         * Skip bin 0 (DC offset). Each band covers (FFT_SIZE/2 / BAND_COUNT) bins. */
        int bins_per_band = (MIC_FFT_SIZE / 2) / MIC_BAND_COUNT;
        float band_energy[MIC_BAND_COUNT] = {0};
        for (int b = 0; b < MIC_BAND_COUNT; b++) {
            int start = b * bins_per_band + 1;
            int end   = start + bins_per_band;
            for (int k = start; k < end; k++) {
                float re = s_fft_buf[k * 2];
                float im = s_fft_buf[k * 2 + 1];
                band_energy[b] += re * re + im * im;
            }
        }

        /* Winner-takes-all dominant band (kept for reference) */
        uint8_t dominant = 0;
        for (int b = 1; b < MIC_BAND_COUNT; b++) {
            if (band_energy[b] > band_energy[dominant]) dominant = (uint8_t)b;
        }

        /* Weighted circular mean hue — all bands vote proportionally to energy.
         * Plain weighted average fails at the 0°/360° seam (e.g. averaging 350° and
         * 10° gives 180° instead of 0°). The atan2 formulation handles wrap-around
         * correctly by working in sin/cos space and converting back to degrees. */
        float sin_sum = 0.0f, cos_sum = 0.0f;
        for (int b = 0; b < MIC_BAND_COUNT; b++) {
            float angle = (float)b * (TWO_PI / MIC_BAND_COUNT);
            sin_sum += band_energy[b] * sinf(angle);
            cos_sum += band_energy[b] * cosf(angle);
        }
        float mean_rad = atan2f(sin_sum, cos_sum);
        if (mean_rad < 0.0f) mean_rad += TWO_PI;
        uint16_t w_hue = (uint16_t)(mean_rad * (360.0f / TWO_PI));

        uint16_t amp = (uint16_t)(rms * 65535.0f);
        if (amp > 65534) amp = 65534;

        /* EMA: smooth_amp[n] = α × raw[n] + (1−α) × smooth_amp[n−1]
         * α is read under a separate lock so the render task can update it
         * at any time without blocking the analysis path for long. */
        float alpha;
        taskENTER_CRITICAL(&s_alpha_lock);
        alpha = s_ema_alpha;
        taskEXIT_CRITICAL(&s_alpha_lock);

        s_smooth_amp = alpha * rms + (1.0f - alpha) * s_smooth_amp;
        uint16_t smooth_amp = (uint16_t)(s_smooth_amp * 65535.0f);

        taskENTER_CRITICAL(&s_lock);
        s_analysis.amplitude        = amp;
        s_analysis.smooth_amplitude = smooth_amp;
        s_analysis.dominant_band    = dominant;
        s_analysis.weighted_hue     = w_hue;
        taskEXIT_CRITICAL(&s_lock);
    }

    vTaskDelete(NULL);
}

/* ── public API ──────────────────────────────────────────────────────────── */

esp_err_t mic_engine_init(void)
{
    /* Initialise DSP FFT tables once */
    esp_err_t dsp_err = dsps_fft2r_init_fc32(NULL, MIC_FFT_SIZE);
    if (dsp_err != ESP_OK) {
        ESP_LOGE(TAG, "DSP FFT init failed: %s", esp_err_to_name(dsp_err));
        return dsp_err;
    }
    dsps_wind_hann_f32(s_window, MIC_FFT_SIZE);

    /* Create I2S RX-only channel */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = MIC_DMA_DESC;
    chan_cfg.dma_frame_num = MIC_FFT_SIZE;
    chan_cfg.auto_clear    = true;

    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Configure standard I2S for INMP441 (MSB-justified, 32-bit mono left) */
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                     I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_SCK_IO,
            .ws   = MIC_WS_IO,
            .dout = I2S_GPIO_UNUSED,
            .din  = MIC_SD_IO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(ret));
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "mic_engine ready — SCK=%d WS=%d SD=%d @ %d Hz",
             MIC_SCK_IO, MIC_WS_IO, MIC_SD_IO, MIC_SAMPLE_RATE);
    return ESP_OK;
}

esp_err_t mic_engine_start(void)
{
    if (s_rx_chan == NULL) return ESP_ERR_INVALID_STATE;
    if (s_running)         return ESP_OK; /* already running */

    esp_err_t ret = i2s_channel_enable(s_rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_running = true;
    BaseType_t ok = xTaskCreatePinnedToCore(mic_task, "mic_task",
                                             MIC_TASK_STACK, NULL,
                                             MIC_TASK_PRIO, &s_task_h, 1);
    if (ok != pdPASS) {
        s_running = false;
        i2s_channel_disable(s_rx_chan);
        ESP_LOGE(TAG, "Failed to create mic_task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "mic_engine started");
    return ESP_OK;
}

esp_err_t mic_engine_stop(void)
{
    if (!s_running) return ESP_OK;
    s_running = false;
    /* Give the task one DMA period to exit naturally */
    vTaskDelay(pdMS_TO_TICKS(300));
    if (s_rx_chan) {
        i2s_channel_disable(s_rx_chan);
    }
    ESP_LOGI(TAG, "mic_engine stopped");
    return ESP_OK;
}

void mic_engine_set_ema_alpha(float alpha)
{
    if (alpha < 0.01f) alpha = 0.01f;
    if (alpha > 0.99f) alpha = 0.99f;
    taskENTER_CRITICAL(&s_alpha_lock);
    s_ema_alpha = alpha;
    taskEXIT_CRITICAL(&s_alpha_lock);
}

void mic_engine_get_analysis(music_analysis_t *out)
{
    if (out == NULL) return;
    if (s_rx_chan == NULL) {
        *out = (music_analysis_t){0};
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    *out = s_analysis;
    taskEXIT_CRITICAL(&s_lock);
}
