/**
 * @file pwm_audio.c
 * @brief MegaWifi Audio Subsystem
 *
 * Copyright (c) 2026 Mike Wolak <mikewolak@gmail.com>
 * All rights reserved.
 *
 * Part of the MegaWifi MOD Player project.
 * https://github.com/mikewolak/MegaWifiModPlayer
 */
/**
 * pwm_audio.c — ESP32-C3 PWM Audio Driver Implementation
 */
#include "pwm_audio.h"

#include "driver/ledc.h"
#include "driver/gptimer.h"
#include "soc/ledc_struct.h"
#include "esp_attr.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "pwm_audio";

/* ── Internal state ─────────────────────────────────────────────────────── */
static struct {
    gptimer_handle_t    timer;
    pwm_audio_source_fn source_fn;
    void               *source_ctx;
    bool                muted;
    bool                initialised;
} s_ctx;

/* ── LEDC fast-path write (called from ISR at 44 kHz) ────────────────────── */
/*
 * ESP32-C3 LEDC duty register: bits [18:4] = integer, [3:0] = fraction.
 * conf1.duty_start latches the new value, conf0.low_speed_update commits it.
 */
static IRAM_ATTR void ledc_write_duty(uint8_t ch, uint16_t duty)
{
    LEDC.channel_group[0].channel[ch].duty.duty = (uint32_t)duty << 4;
    LEDC.channel_group[0].channel[ch].conf1.duty_start = 1;
    LEDC.channel_group[0].channel[ch].conf0.low_speed_update = 1;
}

/* ── Timer ISR ──────────────────────────────────────────────────────────── */
static IRAM_ATTR bool timer_isr(gptimer_handle_t timer,
                                 const gptimer_alarm_event_data_t *edata,
                                 void *user_ctx)
{
    pwm_audio_sample_t sample;

    if (s_ctx.muted || s_ctx.source_fn == NULL) {
        sample.l = PWM_AUDIO_MIDSCALE;
        sample.r = PWM_AUDIO_MIDSCALE;
    } else {
        sample = s_ctx.source_fn(s_ctx.source_ctx);
    }

    ledc_write_duty(LEDC_CHANNEL_0, sample.l);  /* GPIO2 — PWM_L */
    ledc_write_duty(LEDC_CHANNEL_1, sample.r);  /* GPIO4 — PWM_R */

    return false;   /* no high-priority task woken */
}

/* ── LEDC init ───────────────────────────────────────────────────────────── */
static esp_err_t ledc_setup(void)
{
    esp_err_t ret;

    /* Timer: 12-bit, ~19.5 kHz, APB 80 MHz (ESP32-C3 has no PLL_DIV clock) */
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_12_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = 19531,               /* 80MHz / 4096 = 19531 Hz */
        .clk_cfg         = LEDC_USE_APB_CLK,
    };
    ret = ledc_timer_config(&timer_cfg);
    if (ret != ESP_OK) return ret;

    /* Channel 0 — GPIO2 — PWM_L */
    ledc_channel_config_t ch_l = {
        .gpio_num   = PWM_AUDIO_GPIO_L,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = PWM_AUDIO_MIDSCALE,
        .hpoint     = 0,
        .intr_type  = LEDC_INTR_DISABLE,
    };
    ret = ledc_channel_config(&ch_l);
    if (ret != ESP_OK) return ret;

    /* Channel 1 — GPIO4 — PWM_R */
    ledc_channel_config_t ch_r = {
        .gpio_num   = PWM_AUDIO_GPIO_R,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_1,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = PWM_AUDIO_MIDSCALE,
        .hpoint     = 0,
        .intr_type  = LEDC_INTR_DISABLE,
    };
    ret = ledc_channel_config(&ch_r);
    return ret;
}

/* ── GPTimer init ────────────────────────────────────────────────────────── */
static esp_err_t gptimer_setup(void)
{
    esp_err_t ret;

    /*
     * 10 MHz tick resolution.
     * alarm_count = 227 → 10,000,000 / 227 = 44,053 Hz (0.1% error vs 44100)
     */
    gptimer_config_t timer_cfg = {
        .clk_src       = GPTIMER_CLK_SRC_DEFAULT,
        .direction     = GPTIMER_COUNT_UP,
        .resolution_hz = 10000000,          /* 10 MHz → 0.1 µs resolution */
    };
    ret = gptimer_new_timer(&timer_cfg, &s_ctx.timer);
    if (ret != ESP_OK) return ret;

    gptimer_alarm_config_t alarm_cfg = {
        .alarm_count                = 227,  /* 10MHz / 227 = 44,053 Hz */
        .reload_count               = 0,
        .flags.auto_reload_on_alarm = true,
    };
    ret = gptimer_set_alarm_action(s_ctx.timer, &alarm_cfg);
    if (ret != ESP_OK) return ret;

    gptimer_event_callbacks_t cbs = {
        .on_alarm = timer_isr,
    };
    ret = gptimer_register_event_callbacks(s_ctx.timer, &cbs, NULL);
    if (ret != ESP_OK) return ret;

    ret = gptimer_enable(s_ctx.timer);
    if (ret != ESP_OK) return ret;

    ret = gptimer_start(s_ctx.timer);
    return ret;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

esp_err_t pwm_audio_init(pwm_audio_source_fn source_fn, void *ctx)
{
    esp_err_t ret;

    if (s_ctx.initialised) {
        ESP_LOGW(TAG, "already initialised");
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.source_fn  = source_fn;
    s_ctx.source_ctx = ctx;

    ret = ledc_setup();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LEDC init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = gptimer_setup();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPTimer init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_ctx.initialised = true;
    ESP_LOGI(TAG, "init ok — GPIO%d(L) GPIO%d(R) 12-bit 19.5kHz PWM "
                  "timer@44053Hz", PWM_AUDIO_GPIO_L, PWM_AUDIO_GPIO_R);
    return ESP_OK;
}

void pwm_audio_set_source(pwm_audio_source_fn source_fn, void *ctx)
{
    /* Atomic enough on single-core RISC-V — pointer write is one instruction */
    s_ctx.source_ctx = ctx;
    s_ctx.source_fn  = source_fn;
}

void pwm_audio_mute(bool mute)
{
    s_ctx.muted = mute;
}

void pwm_audio_deinit(void)
{
    if (!s_ctx.initialised) return;

    gptimer_stop(s_ctx.timer);
    gptimer_disable(s_ctx.timer);
    gptimer_del_timer(s_ctx.timer);

    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);

    memset(&s_ctx, 0, sizeof(s_ctx));
    ESP_LOGI(TAG, "deinit ok");
}
