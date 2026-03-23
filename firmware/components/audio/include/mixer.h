/**
 * @file mixer.h
 * @brief MegaWifi Audio Subsystem
 *
 * Copyright (c) 2026 Mike Wolak <mikewolak@gmail.com>
 * All rights reserved.
 *
 * Part of the MegaWifi MOD Player project.
 * https://github.com/mikewolak/MegaWifiModPlayer
 */
/**
 * mixer.h — 8-channel mono mixer with per-channel volume and pan
 *
 * All 8 channels are identical: mono 8-bit PCM or 12-bit ring buffer
 * with independent volume (0–255) and stereo panning (0–255).
 *
 * Two channel types:
 *   SAMPLE: pointer to 8-bit PCM data with fixed-point rate conversion
 *   STREAM: ring buffer fed by a decoder task (MOD/MP3)
 *
 * The ISR always runs at 44 kHz — outputs silence when nothing is active.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MIXER_NUM_CHANNELS  8

/* Panning constants */
#define MIXER_PAN_LEFT      0
#define MIXER_PAN_CENTER    128
#define MIXER_PAN_RIGHT     255

/* ── Initialisation ──────────────────────────────────────────────────────── */

esp_err_t mixer_init(void);
void mixer_deinit(void);

/* ── Stream channels — ring buffer fed by decoder task ───────────────────── */

/** How many samples are free in a stream channel's ring buffer. */
uint32_t mixer_stream_ring_free(uint8_t ch);

/** Push one 12-bit sample into a stream channel's ring buffer. */
void mixer_stream_ring_push(uint8_t ch, uint16_t sample);

/** Block until ring buffer has space. Returns false on timeout. */
bool mixer_stream_ring_wait(uint32_t timeout_ms);

/** Flush a stream channel's ring buffer. */
void mixer_stream_ring_flush(uint8_t ch);

/** Enable/disable a stream channel. */
void mixer_stream_set_active(uint8_t ch, bool active);

/* ── Sample channels — 8-bit PCM with rate conversion ────────────────────── */

/** Load 8-bit unsigned PCM data into a channel (data not copied). */
esp_err_t mixer_sample_load(uint8_t ch, const uint8_t *data, uint32_t len);

/** Start playing a sample channel. */
esp_err_t mixer_sample_play(uint8_t ch, uint32_t rate_hz, uint8_t vol,
                             uint8_t pan, bool loop, bool lerp);

/** Stop a sample channel. */
void mixer_sample_stop(uint8_t ch);

/* ── Per-channel controls (work on any channel type) ─────────────────────── */

/** Set channel volume (0–255). */
void mixer_set_vol(uint8_t ch, uint8_t vol);

/** Set channel panning (0=hard left, 128=center, 255=hard right). */
void mixer_set_pan(uint8_t ch, uint8_t pan);

/* ── Master volume ───────────────────────────────────────────────────────── */

void mixer_set_master_vol(uint8_t vol);

/** Get current master volume. */
uint8_t mixer_get_master_vol(void);

/**
 * Read peak amplitudes for all 8 channels (0–255 each).
 * Resets the peaks after reading (peak-and-clear for VU meters).
 * out must point to an array of MIXER_NUM_CHANNELS bytes.
 */
void mixer_get_amplitudes(uint8_t *out);

#ifdef __cplusplus
}
#endif
