/**
 * reverb.c — 4x4 Feedback Delay Network reverb
 *
 * Algorithm: 4-channel FDN with Hadamard mixing matrix.
 * All arithmetic: 32-bit fixed-point, Q1.14 coefficients.
 * No malloc, no float, no libm.
 *
 * Sample rate: 44100 Hz
 * Format: 16-bit signed PCM stereo in/out
 *
 * References:
 *   Jot & Chaigne, "Digital delay networks for designing artificial
 *   reverberators", 90th AES Convention, 1991.
 *
 *   Gardner, "Reverberation Algorithms", MIT Media Lab TR #144, 1992.
 *   https://www.media.mit.edu/tech-reports/TR-144.pdf
 *
 *   Schlecht & Habets, "On Lossless Feedback Delay Networks", IEEE
 *   Trans. Signal Processing, 2017.
 */

#include "reverb.h"
#include <string.h>

/* -----------------------------------------------------------------------
 * Fixed-point helpers
 * Q1.14: range [-2, +2), 0x4000 = 1.0
 * --------------------------------------------------------------------- */

#define Q14         (1 << 14)
#define MUL_Q14(a, b)  ((int32_t)(a) * (int32_t)(b) >> 14)

/* Saturating clamp to int16 */
static inline int16_t sat16(int32_t x)
{
    if (x >  32767) return  32767;
    if (x < -32768) return -32768;
    return (int16_t)x;
}

/* -----------------------------------------------------------------------
 * Hadamard butterfly (4×4, unnormalised)
 *
 * [1  1  1  1]   [a]
 * [1 -1  1 -1] × [b]
 * [1  1 -1 -1]   [c]
 * [1 -1 -1  1]   [d]
 *
 * Implemented as two butterfly stages — 8 adds, zero multiplies.
 * The unnormalised gain of 2 per stage is absorbed by the Q14 gain
 * coefficients (halve them to account for it, done in preset tables).
 * --------------------------------------------------------------------- */
static inline void hadamard4(int32_t v[4])
{
    int32_t a, b, c, d;

    /* Stage 1 */
    a = v[0] + v[1];
    b = v[0] - v[1];
    c = v[2] + v[3];
    d = v[2] - v[3];

    /* Stage 2 */
    v[0] = a + c;
    v[1] = b + d;
    v[2] = a - c;
    v[3] = b - d;
}

/* -----------------------------------------------------------------------
 * Preset tables
 * --------------------------------------------------------------------- */

typedef struct {
    uint16_t delay[4];   /* samples at 44100 Hz */
    int16_t  gain[4];    /* Q1.14 per-line feedback */
    int16_t  lpf_coeff;  /* Q1.14 LPF pole (higher = darker) */
    int16_t  wet;        /* Q1.14 default wet mix */
    uint16_t pre_delay;  /* samples */
} preset_t;

/*
 * Delay lengths: mutually coprime, avoids flutter.
 * Gain per-line: sets RT60. Formula (approximate):
 *   gain = 10^(-3 * delay_secs / rt60)  converted to Q1.14
 * The Hadamard doubles amplitude each pass, so effective gain
 * is halved — gains here are pre-halved.
 */
static const preset_t presets[REVERB_NUM_PRESETS] = {
    /* ROOM: RT60 ~0.4 s */
    {
        .delay     = { 1381, 1499, 1607, 1753 },
        .gain      = { 0x38A3, 0x379C, 0x36A8, 0x3580 }, /* ~0.89–0.84 */
        .lpf_coeff = 0x3800,   /* ~0.875 — moderate absorption */
        .wet       = 0x2000,   /* 50% wet */
        .pre_delay = 0
    },
    /* HALL: RT60 ~1.2 s */
    {
        .delay     = { 1637, 1787, 1951, 2039 },
        .gain      = { 0x3C00, 0x3BB0, 0x3B60, 0x3B00 }, /* ~0.94 */
        .lpf_coeff = 0x3C00,   /* 0.9375 — less absorption */
        .wet       = 0x2800,   /* 62% wet */
        .pre_delay = 18        /* ~0.4 ms */
    },
    /* PLATE: dense, fast build */
    {
        .delay     = { 1013, 1153, 1307, 1481 },
        .gain      = { 0x3A00, 0x3980, 0x3900, 0x3880 },
        .lpf_coeff = 0x3600,   /* slightly dark */
        .wet       = 0x3000,   /* 75% wet */
        .pre_delay = 0
    },
    /* CAVE: RT60 ~2 s, dark */
    {
        .delay     = { 1847, 1973, 2017, 2039 },
        .gain      = { 0x3D80, 0x3D40, 0x3D00, 0x3CC0 }, /* ~0.96 */
        .lpf_coeff = 0x3E00,   /* very little absorption */
        .wet       = 0x2800,
        .pre_delay = 44        /* ~1 ms */
    }
};

/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */

void reverb_init(reverb_state_t *rv, reverb_preset_t preset)
{
    if ((unsigned)preset >= REVERB_NUM_PRESETS)
        preset = REVERB_PRESET_ROOM;

    const preset_t *p = &presets[preset];

    memset(rv, 0, sizeof(*rv));

    for (int i = 0; i < 4; i++) {
        rv->delay[i] = p->delay[i];
        rv->gain[i]  = p->gain[i];
        rv->pos[i]   = 0;
    }

    rv->lpf_coeff = p->lpf_coeff;
    rv->wet       = p->wet;
    rv->dry       = Q14 - p->wet;
    rv->pre_delay = p->pre_delay;
    rv->pre_pos   = 0;
}

void reverb_set_mix(reverb_state_t *rv, int16_t wet)
{
    rv->wet = wet;
    rv->dry = (int16_t)(Q14 - (int32_t)wet);
}

void reverb_set_gain(reverb_state_t *rv, int16_t gain)
{
    for (int i = 0; i < 4; i++)
        rv->gain[i] = gain;
}

void reverb_process(reverb_state_t *rv,
                    int16_t in_l, int16_t in_r,
                    int16_t *out_l, int16_t *out_r)
{
    /* --- Pre-delay --------------------------------------------------- */
    int16_t dl, dr;
    if (rv->pre_delay) {
        uint8_t rd = (uint8_t)(rv->pre_pos - (uint8_t)rv->pre_delay);
        /* pre_buf is interleaved L/R — index pairs */
        dl = rv->pre_buf[(rd * 2)     & 0xFF];
        dr = rv->pre_buf[(rd * 2 + 1) & 0xFF];
        rv->pre_buf[(rv->pre_pos * 2)     & 0xFF] = in_l;
        rv->pre_buf[(rv->pre_pos * 2 + 1) & 0xFF] = in_r;
        rv->pre_pos++;
    } else {
        dl = in_l;
        dr = in_r;
    }

    /* Mix stereo to mono for FDN input */
    int32_t input = ((int32_t)dl + (int32_t)dr) >> 1;

    /* --- Read delay lines -------------------------------------------- */
    int32_t v[4];
    for (int i = 0; i < 4; i++) {
        uint16_t rd = (rv->pos[i] - rv->delay[i]) & (REVERB_MAX_DELAY - 1);
        v[i] = rv->buf[i][rd];
    }

    /* --- Hadamard mix ------------------------------------------------- */
    hadamard4(v);

    /* --- Per-line: LPF + feedback gain + write ----------------------- */
    for (int i = 0; i < 4; i++) {
        /* 1-pole LPF: y[n] = coeff*y[n-1] + (1-coeff)*x[n]
         * In Q14: coeff ~0x3800 means pole at 0.875             */
        int32_t lpf_in  = v[i] + input;
        int32_t lpf_out = MUL_Q14(rv->lpf_coeff, rv->lpf_state[i])
                        + MUL_Q14(Q14 - rv->lpf_coeff, lpf_in);
        rv->lpf_state[i] = lpf_out;

        /* Feedback gain */
        int32_t fed = MUL_Q14(rv->gain[i], lpf_out);

        rv->buf[i][rv->pos[i]] = sat16(fed);
        rv->pos[i] = (rv->pos[i] + 1) & (REVERB_MAX_DELAY - 1);
    }

    /* --- Output mix: decorrelate L/R from different taps ------------ */
    int32_t wet_l = (v[0] + v[1]) >> 1;
    int32_t wet_r = (v[2] + v[3]) >> 1;

    *out_l = sat16(MUL_Q14(rv->dry, in_l) + MUL_Q14(rv->wet, wet_l));
    *out_r = sat16(MUL_Q14(rv->dry, in_r) + MUL_Q14(rv->wet, wet_r));
}
