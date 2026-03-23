/**
 * audio_player.c — Helix fixed-point MP3 decoder task
 *
 * Decodes MP3 frames and pushes LEDC-ready samples into the mixer's
 * stream channels. Uses CH6 (left) and CH7 (right) — same as mod_player.
 */
#include "helix/mp3dec.h"

#include "audio_player.h"
#include "mixer.h"
#include "pwm_audio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "audio_player";

#define RING_PREFILL    4096

/* Stream channels — same as mod_player */
#define MP3_CH_L        6
#define MP3_CH_R        7

/* ── Internal state ─────────────────────────────────────────────────────── */
static struct {
    TaskHandle_t        decode_task;
    audio_src_t         src;
    volatile audio_state_t state;

    HMP3Decoder         hmp3;
    uint8_t             mp3_buf[16 * 1024];
    int                 mp3_buf_len;
    int                 mp3_buf_pos;

    short               pcm[MAX_NGRAN * MAX_NCHAN * MAX_NSAMP];

    bool                initialised;
} s_player;

/* ── Push decoded frame into mixer stream channels ───────────────────────── */
static void push_frame_to_mixer(int total_samples, int channels)
{
    const short *pcm = s_player.pcm;
    int stereo_samples = total_samples / channels;

    for (int i = 0; i < stereo_samples; i++) {
        while (mixer_stream_ring_free(MP3_CH_L) == 0 ||
               mixer_stream_ring_free(MP3_CH_R) == 0) {
            if (!mixer_stream_ring_wait(100)) {
                if (s_player.state == AUDIO_STATE_IDLE) return;
            }
        }

        uint16_t l = pwm_audio_pcm_to_ledc(pcm[i * channels]);
        uint16_t r = pwm_audio_pcm_to_ledc(
            pcm[i * channels + (channels > 1 ? 1 : 0)]);
        mixer_stream_ring_push(MP3_CH_L, l);
        mixer_stream_ring_push(MP3_CH_R, r);
    }
}

/* ── MP3 byte source helpers ─────────────────────────────────────────────── */

static int src_refill(void)
{
    int remaining = s_player.mp3_buf_len - s_player.mp3_buf_pos;
    if (remaining > 0 && s_player.mp3_buf_pos > 0) {
        memmove(s_player.mp3_buf, s_player.mp3_buf + s_player.mp3_buf_pos,
                remaining);
    }
    s_player.mp3_buf_pos = 0;
    s_player.mp3_buf_len = remaining;

    int space = (int)sizeof(s_player.mp3_buf) - remaining;
    if (space <= 0) return remaining;

    int got = s_player.src.read(s_player.src.ctx,
                                 s_player.mp3_buf + remaining, space);
    if (got > 0) {
        s_player.mp3_buf_len += got;
    }

    return got;
}

static int decode_one_frame(void)
{
    int avail = s_player.mp3_buf_len - s_player.mp3_buf_pos;
    if (avail < (int)sizeof(s_player.mp3_buf) / 2) {
        int got = src_refill();
        avail = s_player.mp3_buf_len - s_player.mp3_buf_pos;
        if (avail <= 0 && got <= 0) return 0;
    }

    if (avail <= 0) return 0;

    unsigned char *ptr = s_player.mp3_buf + s_player.mp3_buf_pos;
    int offset = MP3FindSyncWord(ptr, avail);
    if (offset < 0) {
        s_player.mp3_buf_pos = s_player.mp3_buf_len;
        return -1;
    }
    ptr += offset;
    avail -= offset;
    s_player.mp3_buf_pos += offset;

    unsigned char *inbuf = ptr;
    int bytes_left = avail;
    int err = MP3Decode(s_player.hmp3, &inbuf, &bytes_left,
                        s_player.pcm, 0);

    int consumed = avail - bytes_left;
    s_player.mp3_buf_pos += consumed;

    if (err == ERR_MP3_NONE) {
        MP3FrameInfo fi;
        MP3GetLastFrameInfo(s_player.hmp3, &fi);
        if (fi.outputSamps > 0) {
            push_frame_to_mixer(fi.outputSamps, fi.nChans);
        }
        return fi.outputSamps;
    } else if (err == ERR_MP3_INDATA_UNDERFLOW ||
               err == ERR_MP3_MAINDATA_UNDERFLOW) {
        return -1;
    } else {
        ESP_LOGW(TAG, "decode error %d, skipping", err);
        return -1;
    }
}

/* ── Decoder task ────────────────────────────────────────────────────────── */

static void decode_task(void *arg)
{
    ESP_LOGI(TAG, "decoder: pre-filling...");

    while (mixer_stream_ring_free(MP3_CH_L) > (8192 - RING_PREFILL)) {
        int ret = decode_one_frame();
        if (ret == 0) break;
    }

    ESP_LOGI(TAG, "decoder: pre-fill done, starting playback");
    mixer_stream_set_active(MP3_CH_L, true);
    mixer_stream_set_active(MP3_CH_R, true);
    s_player.state = AUDIO_STATE_PLAYING;

    while (s_player.state == AUDIO_STATE_PLAYING ||
           s_player.state == AUDIO_STATE_PAUSED) {

        if (s_player.state == AUDIO_STATE_PAUSED) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (mixer_stream_ring_free(MP3_CH_L) < 8192 / 4) {
            mixer_stream_ring_wait(100);
            continue;
        }

        int ret = decode_one_frame();
        if (ret == 0) break;
    }

    /* Drain */
    while (mixer_stream_ring_free(MP3_CH_L) < 8192 - 1 &&
           s_player.state == AUDIO_STATE_PLAYING) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ESP_LOGI(TAG, "decoder task exiting");
    mixer_stream_set_active(MP3_CH_L, false);
    mixer_stream_set_active(MP3_CH_R, false);
    s_player.state = AUDIO_STATE_IDLE;
    s_player.decode_task = NULL;
    vTaskDelete(NULL);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

esp_err_t audio_player_init(void)
{
    if (s_player.initialised) return ESP_ERR_INVALID_STATE;

    memset(&s_player, 0, sizeof(s_player));
    s_player.state = AUDIO_STATE_IDLE;

    s_player.hmp3 = MP3InitDecoder();
    if (!s_player.hmp3) return ESP_ERR_NO_MEM;

    /* Configure stream channels */
    mixer_stream_set_active(MP3_CH_L, false);
    mixer_stream_set_active(MP3_CH_R, false);
    mixer_set_pan(MP3_CH_L, MIXER_PAN_LEFT);
    mixer_set_pan(MP3_CH_R, MIXER_PAN_RIGHT);

    s_player.initialised = true;
    ESP_LOGI(TAG, "init ok (helix, CH%d=L CH%d=R)", MP3_CH_L, MP3_CH_R);
    return ESP_OK;
}

esp_err_t audio_player_play(const audio_src_t *src)
{
    if (!s_player.initialised) return ESP_ERR_INVALID_STATE;

    if (s_player.state != AUDIO_STATE_IDLE) {
        audio_player_stop();
    }

    s_player.src = *src;
    s_player.mp3_buf_len = 0;
    s_player.mp3_buf_pos = 0;

    mixer_stream_ring_flush(MP3_CH_L);
    mixer_stream_ring_flush(MP3_CH_R);

    s_player.state = AUDIO_STATE_PAUSED;

    BaseType_t ret = xTaskCreate(decode_task, "mp3_dec",
                                  16384, NULL, 5,
                                  &s_player.decode_task);
    if (ret != pdPASS) {
        s_player.state = AUDIO_STATE_ERROR;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void audio_player_pause(void)
{
    if (s_player.state == AUDIO_STATE_PLAYING) {
        s_player.state = AUDIO_STATE_PAUSED;
        mixer_stream_set_active(MP3_CH_L, false);
        mixer_stream_set_active(MP3_CH_R, false);
    }
}

void audio_player_resume(void)
{
    if (s_player.state == AUDIO_STATE_PAUSED) {
        s_player.state = AUDIO_STATE_PLAYING;
        mixer_stream_set_active(MP3_CH_L, true);
        mixer_stream_set_active(MP3_CH_R, true);
    }
}

void audio_player_stop(void)
{
    if (s_player.state == AUDIO_STATE_IDLE) return;

    s_player.state = AUDIO_STATE_IDLE;
    mixer_stream_set_active(MP3_CH_L, false);
    mixer_stream_set_active(MP3_CH_R, false);

    if (s_player.decode_task) {
        int timeout = 50;
        while (s_player.decode_task != NULL && --timeout > 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    mixer_stream_ring_flush(MP3_CH_L);
    mixer_stream_ring_flush(MP3_CH_R);
}

esp_err_t audio_player_seek(uint32_t offset)
{
    if (!s_player.initialised) return ESP_ERR_INVALID_STATE;
    if (!s_player.src.seek) return ESP_ERR_NOT_SUPPORTED;

    s_player.state = AUDIO_STATE_PAUSED;
    mixer_stream_set_active(MP3_CH_L, false);
    mixer_stream_set_active(MP3_CH_R, false);

    if (s_player.decode_task) {
        int timeout = 50;
        while (s_player.decode_task != NULL && --timeout > 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    mixer_stream_ring_flush(MP3_CH_L);
    mixer_stream_ring_flush(MP3_CH_R);
    s_player.mp3_buf_len = 0;
    s_player.mp3_buf_pos = 0;

    int err = s_player.src.seek(s_player.src.ctx, offset);
    if (err < 0) {
        s_player.state = AUDIO_STATE_ERROR;
        return ESP_FAIL;
    }

    s_player.state = AUDIO_STATE_PAUSED;
    BaseType_t ret = xTaskCreate(decode_task, "mp3_dec",
                                  16384, NULL, 5,
                                  &s_player.decode_task);
    if (ret != pdPASS) {
        s_player.state = AUDIO_STATE_ERROR;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

audio_state_t audio_player_state(void)
{
    return s_player.state;
}

void audio_player_deinit(void)
{
    if (!s_player.initialised) return;

    audio_player_stop();
    MP3FreeDecoder(s_player.hmp3);
    memset(&s_player, 0, sizeof(s_player));
}
