/**
 * reverb.h — 4x4 Feedback Delay Network reverb
 *
 * Fixed-point, 44100 Hz, 16-bit signed PCM in/out.
 * No malloc, no float, no external dependencies.
 *
 * Usage:
 *   reverb_state_t rv;
 *   reverb_init(&rv, REVERB_PRESET_ROOM);
 *   reverb_process(&rv, in_l, in_r, &out_l, &out_r);
 */

#ifndef REVERB_H
#define REVERB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------
 * Tunables
 * --------------------------------------------------------------------- */

/** Longest delay line in samples. At 44100 Hz, 2048 = ~46 ms.
 *  Must be a power of 2. Increase for longer, lusher tails (costs RAM).
 *  4 lines × 2048 × 2 bytes = 16 KB total delay memory. */
#define REVERB_MAX_DELAY  2048

/* -----------------------------------------------------------------------
 * Presets
 * --------------------------------------------------------------------- */

typedef enum {
    REVERB_PRESET_ROOM   = 0,   /**< Small room, ~0.4 s RT60  */
    REVERB_PRESET_HALL   = 1,   /**< Concert hall, ~1.2 s RT60 */
    REVERB_PRESET_PLATE  = 2,   /**< Plate-style dense wash    */
    REVERB_PRESET_CAVE   = 3,   /**< Long dark cave, ~2 s RT60 */
    REVERB_NUM_PRESETS
} reverb_preset_t;

/* -----------------------------------------------------------------------
 * State
 * --------------------------------------------------------------------- */

/** Internal state — treat as opaque. Declare one per stereo instance. */
typedef struct {
    /* Delay line buffers — 4 lines, each REVERB_MAX_DELAY samples */
    int16_t  buf[4][REVERB_MAX_DELAY];

    /* Write heads */
    uint16_t pos[4];

    /* Delay lengths in samples (prime-ish, avoids flutter) */
    uint16_t delay[4];

    /* Per-line feedback gain in Q1.14 fixed point (0x4000 = 1.0) */
    int16_t  gain[4];

    /* Per-line 1-pole LPF state (air absorption), Q1.14 */
    int32_t  lpf_state[4];
    int16_t  lpf_coeff;   /**< LPF pole, Q1.14. Higher = darker tail. */

    /* Wet/dry mix in Q1.14 */
    int16_t  wet;
    int16_t  dry;

    /* Pre-delay tap in samples (0 = off) */
    uint16_t pre_delay;
    int16_t  pre_buf[256];
    uint8_t  pre_pos;
} reverb_state_t;

/* -----------------------------------------------------------------------
 * API
 * --------------------------------------------------------------------- */

/**
 * Initialise (or reinitialise) a reverb instance with a preset.
 * Safe to call at any time; zeros all delay lines.
 */
void reverb_init(reverb_state_t *rv, reverb_preset_t preset);

/**
 * Process one stereo sample pair.
 *
 * @param rv      Reverb state
 * @param in_l    Left input,  16-bit signed PCM
 * @param in_r    Right input, 16-bit signed PCM
 * @param out_l   Left output pointer
 * @param out_r   Right output pointer
 *
 * Call once per sample at 44100 Hz. Safe to call from an ISR if your
 * platform guarantees 32-bit atomics (no shared state outside rv).
 */
void reverb_process(reverb_state_t *rv,
                    int16_t in_l, int16_t in_r,
                    int16_t *out_l, int16_t *out_r);

/**
 * Adjust wet/dry mix at runtime without reinitialising.
 *
 * @param wet  0–32767  (0 = fully dry, 32767 ≈ fully wet)
 */
void reverb_set_mix(reverb_state_t *rv, int16_t wet);

/**
 * Adjust feedback (decay) at runtime.
 *
 * @param gain  Q1.14 feedback coefficient per line.
 *              Typical useful range: 0x2000 (short) – 0x3C00 (long).
 *              Values >= 0x4000 will cause instability.
 */
void reverb_set_gain(reverb_state_t *rv, int16_t gain);

#ifdef __cplusplus
}
#endif

#endif /* REVERB_H */
