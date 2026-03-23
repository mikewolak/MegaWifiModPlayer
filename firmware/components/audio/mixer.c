/**
 * @file mixer.c
 * @brief MegaWifi Audio Subsystem
 *
 * Copyright (c) 2026 Mike Wolak <mikewolak@gmail.com>
 * All rights reserved.
 *
 * Part of the MegaWifi MOD Player project.
 * https://github.com/mikewolak/MegaWifiModPlayer
 */
/**
 * mixer.c — 8-channel mono mixer with per-channel volume and pan
 *
 * All 8 channels are identical. Each can operate as either:
 *   SAMPLE: 8-bit PCM pointer with fixed-point rate conversion
 *   STREAM: ring buffer fed by a decoder task
 *
 * The ISR sums all active channels with per-channel volume and pan,
 * applies master volume, and clamps to 12-bit for LEDC output.
 */
#include "mixer.h"
#include "pwm_audio.h"
#include "reverb.h"
#include "esp_cpu.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_attr.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "mixer";

/* ── Ring buffer per stream channel ──────────────────────────────────────── */
#define RING_SIZE       8192
#define RING_MASK       (RING_SIZE - 1)
#define RING_WATERMARK  (RING_SIZE / 4)

/* ── Channel types ───────────────────────────────────────────────────────── */
typedef enum {
    CH_INACTIVE = 0,
    CH_SAMPLE,          /* 8-bit PCM with rate conversion */
    CH_STREAM,          /* ring buffer fed externally */
} ch_type_t;

/* ── Channel state ───────────────────────────────────────────────────────── */
typedef struct {
    ch_type_t       type;
    uint8_t         vol;            /* 0–255 */
    uint8_t         pan;            /* 0=hard left, 128=center, 255=hard right */
    uint8_t         reverb_send;    /* 0–255: how much of this channel goes to reverb */
    bool            active;

    /* Sample mode */
    const uint8_t  *data;           /* 8-bit unsigned PCM */
    uint32_t        len;
    uint32_t        phase;          /* 16.16 fixed-point position */
    uint32_t        rate;           /* 16.16 step per output sample */
    bool            loop;
    bool            lerp;

    /* Stream mode */
    uint16_t       *ring;           /* 12-bit sample ring buffer */
    volatile uint32_t ring_wr;
    volatile uint32_t ring_rd;
} channel_t;

/* ── Mixer state ─────────────────────────────────────────────────────────── */
static struct {
    channel_t       ch[MIXER_NUM_CHANNELS];
    uint8_t         master_vol;

    /* Per-channel peak amplitude (updated by ISR, read by app) */
    volatile uint8_t amplitude[MIXER_NUM_CHANNELS];

    /* Shared semaphore for stream channels — decoder waits on this */
    SemaphoreHandle_t stream_sem;
    volatile uint32_t consumed_since_signal;

    /* Ring buffer storage — allocated for stream channels only */
    uint16_t        ring_storage[2][RING_SIZE]; /* two stream channels max */

    /* Reverb send/return */
    reverb_state_t  reverb;
    bool            reverb_enabled;
    uint8_t         reverb_return;  /* return level 0-255 */

    /* ISR CPU profiling */
    volatile uint32_t isr_cycles_avg;   /* smoothed cycle count per ISR call */
    volatile uint8_t  cpu_pct;          /* 0-100 CPU busy percentage */

    bool            initialised;
} s_mix;

/* ── Ring buffer helpers ─────────────────────────────────────────────────── */

static inline uint32_t ch_ring_count(channel_t *c)
{
    return (c->ring_wr - c->ring_rd) & RING_MASK;
}

static inline uint32_t ch_ring_free(channel_t *c)
{
    return (RING_SIZE - 1) - ch_ring_count(c);
}

/* ── ISR: the mix routine ────────────────────────────────────────────────── */
/*
 * Signal flow (per-sample, 44 kHz):
 *
 *   CH0..7 ─┬─ vol ─┬─ pan ──→ dry_l, dry_r  (direct stereo bus)
 *            │       │
 *            │       └─ send ──→ reverb_bus    (mono send, pre-pan)
 *            │
 *   reverb_bus ──→ reverb_process() ──→ rev_l, rev_r
 *
 *   output = (dry_l + rev_l) * master ──→ LEDC left
 *            (dry_r + rev_r) * master ──→ LEDC right
 */
/* 160 MHz / 44053 Hz = 3632 cycles per ISR call = 100% CPU */
#define ISR_CYCLES_100PCT  3632

static IRAM_ATTR pwm_audio_sample_t mixer_isr(void *ctx)
{
    uint32_t _isr_start = esp_cpu_get_cycle_count();

    int32_t sum_l = 0;
    int32_t sum_r = 0;
    int32_t reverb_send_bus = 0;    /* mono reverb send */

    for (int i = 0; i < MIXER_NUM_CHANNELS; i++) {
        channel_t *ch = &s_mix.ch[i];
        if (!ch->active) continue;

        int32_t sample = 0;

        if (ch->type == CH_STREAM) {
            uint32_t rd = ch->ring_rd;
            if (rd != ch->ring_wr) {
                sample = (int32_t)ch->ring[rd & RING_MASK] - 2048;
                ch->ring_rd = (rd + 1) & RING_MASK;
            }
        } else if (ch->type == CH_SAMPLE) {
            uint32_t pos = ch->phase >> 16;
            if (pos >= ch->len) {
                if (ch->loop) {
                    ch->phase = 0;
                    pos = 0;
                } else {
                    ch->active = false;
                    continue;
                }
            }

            if (ch->lerp && pos + 1 < ch->len) {
                uint8_t frac = (ch->phase >> 8) & 0xFF;
                int32_t s0 = (int32_t)ch->data[pos] - 128;
                int32_t s1 = (int32_t)ch->data[pos + 1] - 128;
                sample = s0 + ((s1 - s0) * frac >> 8);
            } else {
                sample = (int32_t)ch->data[pos] - 128;
            }

            sample <<= 4;
            ch->phase += ch->rate;
        }

        /* Apply per-channel volume */
        sample = sample * ch->vol >> 8;

        /* Track peak amplitude for VU meters */
        {
            int32_t abs_s = sample < 0 ? -sample : sample;
            if (abs_s > 2047) abs_s = 2047;
            uint8_t amp8 = abs_s >> 3;
            if (amp8 > s_mix.amplitude[i])
                s_mix.amplitude[i] = amp8;
        }

        /* Reverb send (pre-pan, mono) — each channel sends independently */
        if (ch->reverb_send > 0) {
            reverb_send_bus += sample * ch->reverb_send >> 8;
        }

        /* Pan into stereo field */
        sum_l += sample * (255 - ch->pan) >> 8;
        sum_r += sample * ch->pan >> 8;
    }

    /* ── Reverb send/return ─────────────────────────────────────────────── */
    if (s_mix.reverb_enabled && s_mix.reverb_return > 0) {
        /* Scale 12-bit send bus up to 16-bit for Freeverb */
        reverb_send_bus <<= 4;
        if (reverb_send_bus > 32767) reverb_send_bus = 32767;
        if (reverb_send_bus < -32768) reverb_send_bus = -32768;

        /* Process mono through Freeverb — returns wet only */
        int16_t rev_wet = reverb_process_sample(&s_mix.reverb,
                                                 (int16_t)reverb_send_bus);

        /* Scale return back to 12-bit and apply return level */
        int32_t ret = ((int32_t)rev_wet * s_mix.reverb_return >> 8) >> 4;
        sum_l += ret;
        sum_r += ret;  /* mono reverb to both channels */
    }

    /* Signal decoder */
    s_mix.consumed_since_signal++;
    if (s_mix.consumed_since_signal >= RING_WATERMARK) {
        s_mix.consumed_since_signal = 0;
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(s_mix.stream_sem, &woken);
    }

    /* Master volume and clamp to 12-bit */
    sum_l = (sum_l * s_mix.master_vol >> 8) + 2048;
    sum_r = (sum_r * s_mix.master_vol >> 8) + 2048;

    if (sum_l < 0) sum_l = 0;
    if (sum_l > 4095) sum_l = 4095;
    if (sum_r < 0) sum_r = 0;
    if (sum_r > 4095) sum_r = 4095;

    /* ISR CPU profiling — smoothed IIR, updated every call */
    {
        uint32_t elapsed = esp_cpu_get_cycle_count() - _isr_start;
        s_mix.isr_cycles_avg = (s_mix.isr_cycles_avg * 15 + elapsed) >> 4;
        s_mix.cpu_pct = (uint8_t)(s_mix.isr_cycles_avg * 100 / ISR_CYCLES_100PCT);
        if (s_mix.cpu_pct > 100) s_mix.cpu_pct = 100;
    }

    pwm_audio_sample_t out = { .l = (uint16_t)sum_l, .r = (uint16_t)sum_r };
    return out;
}

/* ── Public API: init / deinit ───────────────────────────────────────────── */

esp_err_t mixer_init(void)
{
    if (s_mix.initialised) return ESP_ERR_INVALID_STATE;

    memset(&s_mix, 0, sizeof(s_mix));
    s_mix.master_vol = 255;

    /* Default all channels to center pan, full volume */
    for (int i = 0; i < MIXER_NUM_CHANNELS; i++) {
        s_mix.ch[i].vol = 255;
        s_mix.ch[i].pan = MIXER_PAN_CENTER;
    }

    s_mix.stream_sem = xSemaphoreCreateCounting(8, 0);
    if (!s_mix.stream_sem) return ESP_ERR_NO_MEM;

    esp_err_t ret = pwm_audio_init(mixer_isr, NULL);
    if (ret != ESP_OK) {
        vSemaphoreDelete(s_mix.stream_sem);
        return ret;
    }

    s_mix.initialised = true;
    ESP_LOGI(TAG, "init ok — %d uniform channels, pan+vol each",
             MIXER_NUM_CHANNELS);
    return ESP_OK;
}

void mixer_deinit(void)
{
    if (!s_mix.initialised) return;

    pwm_audio_deinit();
    vSemaphoreDelete(s_mix.stream_sem);
    memset(&s_mix, 0, sizeof(s_mix));
}

/* ── Public API: stream channels ─────────────────────────────────────────── */

static int stream_slot = 0;  /* tracks which ring_storage[] slot is next */

uint32_t mixer_stream_ring_free(uint8_t ch)
{
    if (ch >= MIXER_NUM_CHANNELS) return 0;
    channel_t *c = &s_mix.ch[ch];
    if (c->type != CH_STREAM || !c->ring) return 0;
    return ch_ring_free(c);
}

void mixer_stream_ring_push(uint8_t ch, uint16_t sample)
{
    if (ch >= MIXER_NUM_CHANNELS) return;
    channel_t *c = &s_mix.ch[ch];
    if (c->type != CH_STREAM || !c->ring) return;

    uint32_t wr = c->ring_wr;
    c->ring[wr & RING_MASK] = sample;
    c->ring_wr = (wr + 1) & RING_MASK;
}

bool mixer_stream_ring_wait(uint32_t timeout_ms)
{
    return xSemaphoreTake(s_mix.stream_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void mixer_stream_ring_flush(uint8_t ch)
{
    if (ch >= MIXER_NUM_CHANNELS) return;
    channel_t *c = &s_mix.ch[ch];
    c->ring_rd = 0;
    c->ring_wr = 0;
}

void mixer_stream_set_active(uint8_t ch, bool active)
{
    if (ch >= MIXER_NUM_CHANNELS) return;
    channel_t *c = &s_mix.ch[ch];

    if (active && c->type != CH_STREAM) {
        /* Assign a ring buffer from storage pool */
        if (stream_slot >= 2) {
            ESP_LOGE(TAG, "no ring buffers left (max 2 stream channels)");
            return;
        }
        c->ring = s_mix.ring_storage[stream_slot++];
        c->ring_rd = 0;
        c->ring_wr = 0;
        c->type = CH_STREAM;
    }

    c->active = active;
}

/* ── Public API: sample channels ─────────────────────────────────────────── */

esp_err_t mixer_sample_load(uint8_t ch, const uint8_t *data, uint32_t len)
{
    if (ch >= MIXER_NUM_CHANNELS) return ESP_ERR_INVALID_ARG;

    channel_t *c = &s_mix.ch[ch];
    c->active = false;
    c->type = CH_SAMPLE;
    c->data = data;
    c->len = len;
    c->phase = 0;

    return ESP_OK;
}

esp_err_t mixer_sample_play(uint8_t ch, uint32_t rate_hz, uint8_t vol,
                             uint8_t pan, bool loop, bool lerp)
{
    if (ch >= MIXER_NUM_CHANNELS) return ESP_ERR_INVALID_ARG;

    channel_t *c = &s_mix.ch[ch];
    if (c->type != CH_SAMPLE || !c->data) return ESP_ERR_INVALID_STATE;

    c->phase = 0;
    c->rate = ((uint64_t)rate_hz << 16) / PWM_AUDIO_SAMPLE_RATE;
    c->vol = vol;
    c->pan = pan;
    c->loop = loop;
    c->lerp = lerp;
    c->active = true;

    return ESP_OK;
}

void mixer_sample_stop(uint8_t ch)
{
    if (ch < MIXER_NUM_CHANNELS && s_mix.ch[ch].type == CH_SAMPLE) {
        s_mix.ch[ch].active = false;
    }
}

/* ── Public API: per-channel controls ────────────────────────────────────── */

void mixer_set_vol(uint8_t ch, uint8_t vol)
{
    if (ch < MIXER_NUM_CHANNELS) {
        s_mix.ch[ch].vol = vol;
    }
}

void mixer_set_pan(uint8_t ch, uint8_t pan)
{
    if (ch < MIXER_NUM_CHANNELS) {
        s_mix.ch[ch].pan = pan;
    }
}

/* ── Public API: master volume ───────────────────────────────────────────── */

void mixer_set_master_vol(uint8_t vol)
{
    s_mix.master_vol = vol;
}

/* ── Public API: amplitude readout ───────────────────────────────────────── */

void mixer_get_amplitudes(uint8_t *out)
{
    for (int i = 0; i < MIXER_NUM_CHANNELS; i++) {
        out[i] = s_mix.amplitude[i];
        s_mix.amplitude[i] = 0;  /* reset after read — peak-and-clear */
    }
}

uint8_t mixer_get_master_vol(void)
{
    return s_mix.master_vol;
}

uint8_t mixer_get_cpu_pct(void)
{
    return s_mix.cpu_pct;
}

/* ── Public API: reverb send/return ──────────────────────────────────────── */

void mixer_reverb_init(uint8_t preset)
{
    (void)preset;   /* Freeverb has no presets — just init */
    reverb_init(&s_mix.reverb);
    s_mix.reverb_enabled = false;
    s_mix.reverb_return = 128;  /* 50% return level */
}

void mixer_reverb_enable(bool enable)
{
    s_mix.reverb_enabled = enable;
}

bool mixer_reverb_enabled(void)
{
    return s_mix.reverb_enabled;
}

void mixer_reverb_set_preset(uint8_t preset)
{
    /* Map "presets" to roomsize values */
    static const int16_t sizes[] = { 20000, 27000, 15000, 30000 };
    if (preset < 4)
        reverb_set_roomsize(&s_mix.reverb, sizes[preset]);
}

void mixer_reverb_set_mix(int16_t wet)
{
    /* "Mix" controls return level (0-32767 → 0-255) */
    s_mix.reverb_return = (uint8_t)(wet >> 7);
    if (s_mix.reverb_return == 0 && wet > 0) s_mix.reverb_return = 1;
}

void mixer_reverb_set_decay(int16_t gain)
{
    /* "Decay" maps to damping — higher = more HF absorption = darker */
    reverb_set_damping(&s_mix.reverb, gain);
}

void mixer_set_reverb_send(uint8_t ch, uint8_t send)
{
    if (ch < MIXER_NUM_CHANNELS) {
        s_mix.ch[ch].reverb_send = send;
    }
}

uint8_t mixer_get_reverb_send(uint8_t ch)
{
    if (ch < MIXER_NUM_CHANNELS) {
        return s_mix.ch[ch].reverb_send;
    }
    return 0;
}
