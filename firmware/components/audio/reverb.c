/**
 * @file reverb.c
 * @brief Fixed-point Freeverb (Jezar/Schroeder-Moorer)
 *
 * Original algorithm: Jezar at Dreampoint, public domain.
 * Fixed-point C port: (c) 2026 Mike Wolak <mikewolak@gmail.com>
 *
 * 8 parallel comb filters → sum → 4 series allpass filters.
 * All arithmetic: int16/int32, no float.
 * ~23 KB RAM (comb + allpass buffers).
 */

#include "reverb.h"
#include <string.h>

/* Saturating shift-right with round-toward-zero for negative values */
static inline int16_t sat16(int32_t n, int rshift)
{
    if (n < 0) {
        n = n + ((1 << rshift) - 1);
    }
    n = n >> rshift;
    if (n > 32767) return 32767;
    if (n < -32768) return -32768;
    return (int16_t)n;
}

void reverb_init(reverb_state_t *rv)
{
    memset(rv, 0, sizeof(*rv));
    rv->combdamp1 = 6553;
    rv->combdamp2 = 26215;
    rv->combfeedback = 27524;    /* ~0.84 in Q15 — medium room */
}

/* Macro for one comb filter iteration — avoids 8× code duplication */
#define COMB_PROCESS(buf, idx, size, filt) do {           \
    bufout = buf[idx];                                    \
    sum += bufout;                                        \
    filt = sat16((int32_t)bufout * combdamp2              \
               + (int32_t)filt * combdamp1, 15);          \
    buf[idx] = sat16(input                                \
               + sat16((int32_t)filt * combfeedback, 15), 0); \
    if (++(idx) >= (size)) (idx) = 0;                     \
} while (0)

/* Macro for one allpass filter iteration */
#define ALLPASS_PROCESS(buf, idx, size) do {               \
    bufout = buf[idx];                                    \
    buf[idx] = output + (bufout >> 1);                    \
    output = sat16((int32_t)bufout - (int32_t)output, 1); \
    if (++(idx) >= (size)) (idx) = 0;                     \
} while (0)

int16_t reverb_process_sample(reverb_state_t *rv, int16_t raw_input)
{
    int16_t input, bufout, output;
    int32_t sum;
    int16_t combdamp1 = rv->combdamp1;
    int16_t combdamp2 = rv->combdamp2;
    int16_t combfeedback = rv->combfeedback;

    /* Scale input for numerical headroom (Jezar's magic number) */
    input = sat16((int32_t)raw_input * 8738, 17);
    sum = 0;

    /* 8 parallel comb filters */
    COMB_PROCESS(rv->comb1buf, rv->comb1index, COMB1_SIZE, rv->comb1filter);
    COMB_PROCESS(rv->comb2buf, rv->comb2index, COMB2_SIZE, rv->comb2filter);
    COMB_PROCESS(rv->comb3buf, rv->comb3index, COMB3_SIZE, rv->comb3filter);
    COMB_PROCESS(rv->comb4buf, rv->comb4index, COMB4_SIZE, rv->comb4filter);
    COMB_PROCESS(rv->comb5buf, rv->comb5index, COMB5_SIZE, rv->comb5filter);
    COMB_PROCESS(rv->comb6buf, rv->comb6index, COMB6_SIZE, rv->comb6filter);
    COMB_PROCESS(rv->comb7buf, rv->comb7index, COMB7_SIZE, rv->comb7filter);
    COMB_PROCESS(rv->comb8buf, rv->comb8index, COMB8_SIZE, rv->comb8filter);

    /* Scale comb sum */
    output = sat16(sum * 31457, 17);

    /* 4 series allpass filters */
    ALLPASS_PROCESS(rv->ap1buf, rv->ap1index, AP1_SIZE);
    ALLPASS_PROCESS(rv->ap2buf, rv->ap2index, AP2_SIZE);
    ALLPASS_PROCESS(rv->ap3buf, rv->ap3index, AP3_SIZE);
    ALLPASS_PROCESS(rv->ap4buf, rv->ap4index, AP4_SIZE);

    /* Final output scaling */
    return sat16((int32_t)output * 30, 0);
}

void reverb_set_roomsize(reverb_state_t *rv, int16_t val)
{
    /* val 0-32767 maps to feedback. Jezar default ~0.84 = 27524 in Q15 */
    rv->combfeedback = val;
}

void reverb_set_damping(reverb_state_t *rv, int16_t val)
{
    /* val = damping coefficient (0-32767). Higher = more HF absorption */
    rv->combdamp1 = val;
    rv->combdamp2 = 32768 - val;
}
