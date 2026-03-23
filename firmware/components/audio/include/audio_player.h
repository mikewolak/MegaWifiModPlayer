/**
 * @file audio_player.h
 * @brief MegaWifi Audio Subsystem
 *
 * Copyright (c) 2026 Mike Wolak <mikewolak@gmail.com>
 * All rights reserved.
 *
 * Part of the MegaWifi MOD Player project.
 * https://github.com/mikewolak/MegaWifiModPlayer
 */
/**
 * audio_player.h — MP3 audio player with ring buffer architecture
 *
 * The PWM ISR always runs at ~44 kHz, consuming stereo samples from a
 * lock-free ring buffer. When the buffer is empty or playback is
 * paused/stopped, the ISR outputs silence (midscale).
 *
 * A decoder task decodes MP3 frames and pushes LEDC-ready samples into
 * the ring buffer. Genesis-side commands (play/stop/pause/seek) change
 * state flags — they never block the audio output.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Audio source abstraction ────────────────────────────────────────────── */
typedef struct {
    /**
     * Read up to `len` bytes into `buf`.
     * Returns bytes read (>0), 0 on end-of-stream, <0 on error.
     * Called from the decoder task (not ISR).
     */
    int  (*read)(void *ctx, uint8_t *buf, int len);
    /**
     * Seek to absolute byte offset. NULL if seeking not supported.
     * Returns 0 on success, <0 on error.
     */
    int  (*seek)(void *ctx, uint32_t offset);
    void *ctx;
} audio_src_t;

/* ── Player state ────────────────────────────────────────────────────────── */
typedef enum {
    AUDIO_STATE_IDLE     = 0,   /* no source, ISR outputs silence */
    AUDIO_STATE_PLAYING,        /* decoder active, ISR reads ring buffer */
    AUDIO_STATE_PAUSED,         /* decoder paused, ISR outputs silence, buffer preserved */
    AUDIO_STATE_ERROR,
} audio_state_t;

/**
 * Initialise the audio player and start the PWM ISR.
 * After this call the ISR is running and outputting silence.
 * Must be called once at startup.
 */
esp_err_t audio_player_init(void);

/**
 * Start playback from the given source.
 * Spawns the decoder task which fills the ring buffer.
 * Source must remain valid until stop or end-of-stream.
 */
esp_err_t audio_player_play(const audio_src_t *src);

/** Pause: ISR outputs silence, ring buffer data preserved. */
void audio_player_pause(void);

/** Resume: ISR resumes reading from ring buffer. */
void audio_player_resume(void);

/** Stop: flush ring buffer, kill decoder task, ISR outputs silence. */
void audio_player_stop(void);

/** Seek to byte offset in source. Flushes buffer, resets decoder. */
esp_err_t audio_player_seek(uint32_t offset);

/** Query current state. */
audio_state_t audio_player_state(void);

/** Deinit everything — stops ISR, releases resources. */
void audio_player_deinit(void);

#ifdef __cplusplus
}
#endif
