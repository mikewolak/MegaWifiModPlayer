/**
 * @file mod_player.h
 * @brief MegaWifi Audio Subsystem
 *
 * Copyright (c) 2026 Mike Wolak <mikewolak@gmail.com>
 * All rights reserved.
 *
 * Part of the MegaWifi MOD Player project.
 * https://github.com/mikewolak/MegaWifiModPlayer
 */
/**
 * mod_player.h — ProTracker MOD player using micromod
 *
 * Renders MOD audio in a FreeRTOS task and pushes stereo samples
 * into the mixer's MP3 ring buffer (channel 0). Supports 4 and 8
 * channel ProTracker MOD files.
 *
 * Uses micromod — fixed-point integer math, no FPU required.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MOD_STATE_IDLE = 0,
    MOD_STATE_PLAYING,
    MOD_STATE_PAUSED,
    MOD_STATE_ERROR,
} mod_state_t;

/**
 * Initialise the MOD player. Mixer must be initialised first.
 * ch_l, ch_r: mixer channel indices for left and right output (0–7).
 * Use the same channel for both to get mono output.
 */
esp_err_t mod_player_init(uint8_t ch_l, uint8_t ch_r);

/**
 * Start playback of a MOD file from memory.
 * Data must remain valid for the lifetime of playback (not copied).
 */
esp_err_t mod_player_play(const uint8_t *mod_data, uint32_t mod_len);

/** Pause playback (mixer outputs silence, position preserved). */
void mod_player_pause(void);

/** Resume playback. */
void mod_player_resume(void);

/** Stop playback and release decoder task. */
void mod_player_stop(void);

/** Jump to a specific pattern position in the sequence. */
void mod_player_set_position(int pos);

/** Query current state. */
mod_state_t mod_player_state(void);

/** Deinit and release resources. */
void mod_player_deinit(void);

#ifdef __cplusplus
}
#endif
