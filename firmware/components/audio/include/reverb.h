/**
 * @file reverb.h
 * @brief Fixed-point Freeverb (Jezar/Schroeder-Moorer)
 *
 * Ported from PaulStoffregen/Audio (Teensy Audio Library), MIT license.
 * Original: Jezar at Dreampoint, public domain.
 * Port to pure C: (c) 2026 Mike Wolak <mikewolak@gmail.com>
 *
 * No float, no malloc, no external dependencies.
 * ~23 KB RAM for mono, int16 throughout.
 */
#ifndef REVERB_H
#define REVERB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Jezar's comb filter buffer sizes (44100 Hz) */
#define COMB1_SIZE  1116
#define COMB2_SIZE  1188
#define COMB3_SIZE  1277
#define COMB4_SIZE  1356
#define COMB5_SIZE  1422
#define COMB6_SIZE  1491
#define COMB7_SIZE  1557
#define COMB8_SIZE  1617

/* Allpass filter buffer sizes */
#define AP1_SIZE    556
#define AP2_SIZE    441
#define AP3_SIZE    341
#define AP4_SIZE    225

typedef struct {
    /* 8 comb filters */
    int16_t  comb1buf[COMB1_SIZE];
    int16_t  comb2buf[COMB2_SIZE];
    int16_t  comb3buf[COMB3_SIZE];
    int16_t  comb4buf[COMB4_SIZE];
    int16_t  comb5buf[COMB5_SIZE];
    int16_t  comb6buf[COMB6_SIZE];
    int16_t  comb7buf[COMB7_SIZE];
    int16_t  comb8buf[COMB8_SIZE];
    uint16_t comb1index, comb2index, comb3index, comb4index;
    uint16_t comb5index, comb6index, comb7index, comb8index;
    int16_t  comb1filter, comb2filter, comb3filter, comb4filter;
    int16_t  comb5filter, comb6filter, comb7filter, comb8filter;

    /* 4 allpass filters */
    int16_t  ap1buf[AP1_SIZE];
    int16_t  ap2buf[AP2_SIZE];
    int16_t  ap3buf[AP3_SIZE];
    int16_t  ap4buf[AP4_SIZE];
    uint16_t ap1index, ap2index, ap3index, ap4index;

    /* Parameters (fixed-point) */
    int16_t  combdamp1;     /* damping coefficient 1 */
    int16_t  combdamp2;     /* damping coefficient 2 = 32768 - damp1 */
    int16_t  combfeedback;  /* feedback level */
} reverb_state_t;

/** Initialise reverb. Clears all buffers, sets default params. */
void reverb_init(reverb_state_t *rv);

/**
 * Process one mono sample through the reverb.
 * Returns the wet output only (caller mixes with dry).
 */
int16_t reverb_process_sample(reverb_state_t *rv, int16_t input);

/**
 * Set room size. val: 0–32767 (larger = longer decay).
 * Default: ~22000.
 */
void reverb_set_roomsize(reverb_state_t *rv, int16_t val);

/**
 * Set damping. val: 0–32767 (larger = more high-freq absorption).
 * Default: ~6553.
 */
void reverb_set_damping(reverb_state_t *rv, int16_t val);

#ifdef __cplusplus
}
#endif

#endif /* REVERB_H */
