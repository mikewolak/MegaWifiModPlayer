/**
 * @file pwm_audio.h
 * @brief MegaWifi Audio Subsystem
 *
 * Copyright (c) 2026 Mike Wolak <mikewolak@gmail.com>
 * All rights reserved.
 *
 * Part of the MegaWifi MOD Player project.
 * https://github.com/mikewolak/MegaWifiModPlayer
 */
/**
 * pwm_audio.h — ESP32-C3 PWM Audio Driver
 *
 * Drives GPIO2 (PWM_L) and GPIO4 (PWM_R) via LEDC peripheral.
 * 12-bit resolution, 39 kHz PWM frequency, PLL_160M clock source.
 * Sample rate: 44,100 Hz driven by GPTimer ISR.
 *
 * Designed for MegaWifi Rev B cartridge audio injection path.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Hardware constants (match Rev B schematic) ─────────────────────────── */
#define PWM_AUDIO_GPIO_L        4           /* GPIO4  — PWM_L */
#define PWM_AUDIO_GPIO_R        5           /* GPIO5  — PWM_R */
#define PWM_AUDIO_RESOLUTION    12          /* bits   — 4096 levels */
#define PWM_AUDIO_MIDSCALE      2048        /* silence / DC bias */
#define PWM_AUDIO_SAMPLE_RATE   44100       /* Hz */
#define PWM_AUDIO_FRAME_SAMPLES 1152        /* minimp3 max samples/frame */

/* ── Stereo sample pair ──────────────────────────────────────────────────── */
typedef struct {
    uint16_t l;     /* left channel,  0–4095 */
    uint16_t r;     /* right channel, 0–4095 */
} pwm_audio_sample_t;

/* ── Callback: ISR calls this to get the next sample ────────────────────── */
/* Must be in IRAM. Return midscale on underrun. */
typedef pwm_audio_sample_t (*pwm_audio_source_fn)(void *ctx);

/**
 * Initialise LEDC peripheral and GPTimer.
 * After this call the PWM outputs are active at midscale (silence).
 * source_fn is called from ISR context — keep it fast and IRAM-safe.
 * Pass NULL for source_fn to output silence until pwm_audio_set_source().
 */
esp_err_t pwm_audio_init(pwm_audio_source_fn source_fn, void *ctx);

/** Replace the sample source without stopping the timer. Thread-safe. */
void pwm_audio_set_source(pwm_audio_source_fn source_fn, void *ctx);

/** Mute both channels (output midscale). Does not stop the timer. */
void pwm_audio_mute(bool mute);

/** Stop timer, release LEDC channels. */
void pwm_audio_deinit(void);

/** Convert signed 16-bit PCM to unsigned 12-bit LEDC value. */
static inline uint16_t pwm_audio_pcm_to_ledc(int16_t pcm)
{
    return (uint16_t)((pcm >> 4) + PWM_AUDIO_MIDSCALE);
}

#ifdef __cplusplus
}
#endif
