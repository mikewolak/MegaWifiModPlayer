/**
 * mod_player.c — ProTracker MOD player task using micromod
 *
 * Renders MOD audio and pushes stereo output into two mixer stream
 * channels: CH6 (left, pan=0) and CH7 (right, pan=255).
 *
 * Uses micromod — fixed-point integer math, no FPU required.
 */
#include "mod_player.h"
#include "micromod.h"
#include "mixer.h"
#include "pwm_audio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "mod_player";

#define RENDER_SAMPLES  512
#define RING_PREFILL    4096

/* ── Internal state ─────────────────────────────────────────────────────── */
static struct {
    TaskHandle_t            task;
    volatile mod_state_t    state;
    int16_t                 render_buf[RENDER_SAMPLES * 2]; /* stereo interleaved */
    uint8_t                 ch_l;       /* mixer channel for left output */
    uint8_t                 ch_r;       /* mixer channel for right output */
    bool                    initialised;
} s_mod;

/* ── Push rendered stereo audio into mixer channels 6 and 7 ──────────────── */
static void push_to_mixer(int num_samples)
{
    for (int i = 0; i < num_samples; i++) {
        /* Wait for space in both channels */
        while (mixer_stream_ring_free(s_mod.ch_l) == 0 ||
               mixer_stream_ring_free(s_mod.ch_r) == 0) {
            if (!mixer_stream_ring_wait(100)) {
                if (s_mod.state == MOD_STATE_IDLE) return;
            }
        }

        /* Convert int16_t to 12-bit unsigned and push L/R separately */
        uint16_t l = pwm_audio_pcm_to_ledc(s_mod.render_buf[i * 2]);
        uint16_t r = pwm_audio_pcm_to_ledc(s_mod.render_buf[i * 2 + 1]);
        mixer_stream_ring_push(s_mod.ch_l, l);
        mixer_stream_ring_push(s_mod.ch_r, r);
    }
}

/* ── Render task ─────────────────────────────────────────────────────────── */
static void mod_task(void *arg)
{
    ESP_LOGI(TAG, "mod task: pre-filling...");

    /* Pre-fill ring buffers */
    while (mixer_stream_ring_free(s_mod.ch_l) > (8192 - RING_PREFILL)) {
        memset(s_mod.render_buf, 0, sizeof(s_mod.render_buf));
        micromod_get_audio(s_mod.render_buf, RENDER_SAMPLES);
        push_to_mixer(RENDER_SAMPLES);
    }

    ESP_LOGI(TAG, "mod task: pre-fill done, starting playback");
    mixer_stream_set_active(s_mod.ch_l, true);
    mixer_stream_set_active(s_mod.ch_r, true);
    s_mod.state = MOD_STATE_PLAYING;

    /* Main render loop */
    while (s_mod.state == MOD_STATE_PLAYING ||
           s_mod.state == MOD_STATE_PAUSED) {

        if (s_mod.state == MOD_STATE_PAUSED) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* Yield when buffers are healthy */
        if (mixer_stream_ring_free(s_mod.ch_l) < 8192 / 4) {
            mixer_stream_ring_wait(100);
            continue;
        }

        memset(s_mod.render_buf, 0, sizeof(s_mod.render_buf));
        micromod_get_audio(s_mod.render_buf, RENDER_SAMPLES);
        push_to_mixer(RENDER_SAMPLES);
    }

    ESP_LOGI(TAG, "mod task exiting");
    mixer_stream_set_active(s_mod.ch_l, false);
    mixer_stream_set_active(s_mod.ch_r, false);
    s_mod.state = MOD_STATE_IDLE;
    s_mod.task = NULL;
    vTaskDelete(NULL);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

esp_err_t mod_player_init(uint8_t ch_l, uint8_t ch_r)
{
    if (s_mod.initialised) return ESP_ERR_INVALID_STATE;

    memset(&s_mod, 0, sizeof(s_mod));
    s_mod.state = MOD_STATE_IDLE;
    s_mod.ch_l = ch_l;
    s_mod.ch_r = ch_r;

    /* Configure stream channels with stereo panning */
    mixer_stream_set_active(s_mod.ch_l, false);
    mixer_stream_set_active(s_mod.ch_r, false);
    mixer_set_pan(s_mod.ch_l, MIXER_PAN_LEFT);
    mixer_set_pan(s_mod.ch_r, MIXER_PAN_RIGHT);

    s_mod.initialised = true;
    ESP_LOGI(TAG, "init ok (micromod, CH%d=L CH%d=R)", ch_l, ch_r);
    return ESP_OK;
}

esp_err_t mod_player_play(const uint8_t *mod_data, uint32_t mod_len)
{
    if (!s_mod.initialised) return ESP_ERR_INVALID_STATE;

    /* Always restart from the top — rapid play = rapid restart */
    if (s_mod.state != MOD_STATE_IDLE) {
        mod_player_stop();
    }

    long result = micromod_initialise((signed char *)mod_data,
                                       PWM_AUDIO_SAMPLE_RATE);
    if (result < 0) {
        ESP_LOGE(TAG, "micromod_initialise failed: %ld", result);
        s_mod.state = MOD_STATE_ERROR;
        return ESP_FAIL;
    }

    long num_ch = micromod_mute_channel(-1);
    micromod_set_gain(num_ch <= 4 ? 64 : 32);

    char name[24];
    micromod_get_string(0, name);
    ESP_LOGI(TAG, "playing: \"%s\" (%ld channels)", name, num_ch);

    mixer_stream_ring_flush(s_mod.ch_l);
    mixer_stream_ring_flush(s_mod.ch_r);

    s_mod.state = MOD_STATE_PAUSED;

    BaseType_t ret = xTaskCreate(mod_task, "mod_play",
                                  4096, NULL, 5,
                                  &s_mod.task);
    if (ret != pdPASS) {
        s_mod.state = MOD_STATE_ERROR;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void mod_player_pause(void)
{
    if (s_mod.state == MOD_STATE_PLAYING) {
        s_mod.state = MOD_STATE_PAUSED;
        mixer_stream_set_active(s_mod.ch_l, false);
        mixer_stream_set_active(s_mod.ch_r, false);
    }
}

void mod_player_resume(void)
{
    if (s_mod.state == MOD_STATE_PAUSED) {
        s_mod.state = MOD_STATE_PLAYING;
        mixer_stream_set_active(s_mod.ch_l, true);
        mixer_stream_set_active(s_mod.ch_r, true);
    }
}

void mod_player_stop(void)
{
    if (s_mod.state == MOD_STATE_IDLE) return;

    s_mod.state = MOD_STATE_IDLE;
    mixer_stream_set_active(s_mod.ch_l, false);
    mixer_stream_set_active(s_mod.ch_r, false);

    if (s_mod.task) {
        int timeout = 50;
        while (s_mod.task != NULL && --timeout > 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    mixer_stream_ring_flush(s_mod.ch_l);
    mixer_stream_ring_flush(s_mod.ch_r);
}

void mod_player_set_position(int pos)
{
    micromod_set_position(pos);
}

mod_state_t mod_player_state(void)
{
    return s_mod.state;
}

void mod_player_deinit(void)
{
    if (!s_mod.initialised) return;
    mod_player_stop();
    memset(&s_mod, 0, sizeof(s_mod));
}
