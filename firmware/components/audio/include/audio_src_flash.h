/**
 * audio_src_flash.h — audio source backed by embedded flash data
 */
#pragma once

#include "audio_player.h"

/**
 * Initialise an audio_src_t backed by a buffer in flash.
 * `data` and `len` point to the embedded MP3 (e.g. from EMBED_FILES).
 * The audio_src_t is valid for the lifetime of `data`.
 */
void audio_src_flash_init(audio_src_t *src, const uint8_t *data, size_t len);
