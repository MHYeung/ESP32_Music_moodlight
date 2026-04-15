#pragma once

#include <stdint.h>
#include "esp_err.h"

/** Number of frequency bands produced by the FFT analysis. */
#define MIC_BAND_COUNT 8

/**
 * @brief Latest audio analysis snapshot.
 *
 * Updated continuously by the internal mic task at ~16 ms intervals.
 * Fetch with mic_engine_get_analysis() from any task/context.
 */
typedef struct {
    uint16_t amplitude;        /**< Raw RMS amplitude, 0–65535 */
    uint16_t smooth_amplitude; /**< EMA-smoothed amplitude — use this for LED mapping */
    uint8_t  dominant_band;    /**< Index of loudest FFT band, 0–(MIC_BAND_COUNT-1) */
    uint16_t weighted_hue;     /**< Energy-weighted circular mean hue, 0–359 degrees.
                                 *   All bands vote proportionally to their energy,
                                 *   computed via atan2(ΣE·sinθ, ΣE·cosθ).          */
} music_analysis_t;

/**
 * @brief Initialise I2S peripheral and allocate mic task resources.
 *        Call once from app_main before mic_engine_start().
 */
esp_err_t mic_engine_init(void);

/**
 * @brief Enable I2S RX channel and start the audio analysis task.
 */
esp_err_t mic_engine_start(void);

/**
 * @brief Disable I2S RX channel and suspend analysis.
 */
esp_err_t mic_engine_stop(void);

/**
 * @brief Copy the latest music_analysis_t snapshot into @p out.
 *        Safe to call from any task. Returns zeros if not initialised.
 */
void mic_engine_get_analysis(music_analysis_t *out);

/**
 * @brief Update the EMA smoothing factor used inside mic_task.
 *
 * @param alpha  Smoothing coefficient in [0.01, 0.99].
 *               Low α = heavy smoothing (sluggish).
 *               High α = light smoothing (reactive but noisy).
 *               Call each render frame so the UI knob takes effect immediately.
 */
void mic_engine_set_ema_alpha(float alpha);
