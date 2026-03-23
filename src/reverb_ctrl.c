/**
 * @file reverb_ctrl.c
 * @brief Reverb control popup — joypad-navigable parameter editor
 *
 * Copyright (c) 2026 Mike Wolak <mikewolak@gmail.com>
 * All rights reserved.
 *
 * Layout:
 *   ──── REVERB CONTROL ──── START=close ────
 *   Bypass:  [ON]  OFF
 *   Preset:  < ROOM >
 *   Wet/Dry: ████████████░░░░░░░░  50%
 *   Decay:   ██████████░░░░░░░░░░  40%
 *   ──── CHANNEL SENDS ────
 *   CH1: ████████░░░░  50%
 *   CH2: ████████░░░░  50%
 *   ...
 *   CH8: ████████░░░░  50%
 *   ──────────────────────────────────────────
 *
 * Controls:
 *   Up/Down:    Navigate parameters
 *   Left/Right: Adjust value
 *   A:          Toggle bypass
 *   START:      Close popup
 */

#include "reverb_ctrl.h"
#include <string.h>

/* MW command wrappers */
extern enum mw_err mw_aud_reverb_enable(uint8_t on);
extern enum mw_err mw_aud_reverb_preset(uint8_t preset);
extern enum mw_err mw_aud_reverb_mix(int16_t wet);
extern enum mw_err mw_aud_reverb_decay(int16_t gain);
extern enum mw_err mw_aud_reverb_send(uint8_t ch, uint8_t level);

/* ── Popup geometry ──────────────────────────────────────────────────────── */
#define POP_TOP     4
#define POP_BOT     25
#define ROW_HEADER  4
#define ROW_BYPASS  6
#define ROW_PRESET  7
#define ROW_MIX     8
#define ROW_DECAY   9
#define ROW_SEP     10
#define ROW_SEND0   11       /* CH1..CH8 on rows 11..18 */
#define ROW_BOTTOM  20

#define NUM_PARAMS  12       /* bypass, preset, mix, decay, send0..send7 */
#define PAR_BYPASS  0
#define PAR_PRESET  1
#define PAR_MIX     2
#define PAR_DECAY   3
#define PAR_SEND0   4        /* 4..11 = channel sends */

/* ── Preset names ────────────────────────────────────────────────────────── */
static const char *preset_names[] = { "ROOM", "HALL", "PLATE", "CAVE" };
#define NUM_PRESETS 4

/* ── State ───────────────────────────────────────────────────────────────── */
static bool         s_active = FALSE;
static rc_redraw_fn s_redraw;
static u8           s_cursor = 0;      /* selected parameter */

/* Local mirror of reverb state */
static bool         s_enabled = FALSE;
static u8           s_preset = 0;
static u8           s_mix = 128;       /* 0–255 display scale */
static u8           s_decay = 128;
static u8           s_send[8] = {0};   /* per-channel send 0–255 */

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void clear_rows(u16 top, u16 bot)
{
    u16 r;
    for (r = top; r <= bot; r++)
        VDP_drawText("                                        ", 0, r);
}

static void draw_bar(u16 col, u16 row, u8 val, u8 width, u16 pal)
{
    char buf[26];
    u8 filled = (val * width + 127) / 255;
    u8 i;
    for (i = 0; i < width; i++)
        buf[i] = (i < filled) ? '#' : '.';
    buf[width] = '\0';
    VDP_setTextPalette(pal);
    VDP_drawText(buf, col, row);
    VDP_setTextPalette(PAL0);
}

static void draw_pct(u16 col, u16 row, u8 val)
{
    char buf[6];
    u16 pct = (val * 100 + 127) / 255;
    char *p = buf;
    if (pct >= 100) { *p++ = '1'; *p++ = '0'; *p++ = '0'; }
    else if (pct >= 10) { *p++ = '0' + pct / 10; *p++ = '0' + pct % 10; }
    else { *p++ = ' '; *p++ = '0' + pct; }
    *p++ = '%'; *p++ = ' '; *p = '\0';
    VDP_drawText(buf, col, row);
}

/* ── Send reverb commands ────────────────────────────────────────────────── */

static void send_enable(void)
{
    mw_aud_reverb_enable(s_enabled ? 1 : 0);
}

static void send_preset(void)
{
    mw_aud_reverb_preset(s_preset);
}

static void send_mix(void)
{
    /* Convert 0-255 display to Q1.14: 0=dry, 0x4000=full wet
     * 255 display → 0x4000 Q14: val * 0x4000 / 255 */
    int16_t q14 = (int16_t)((u32)s_mix * 0x4000 / 255);
    mw_aud_reverb_mix(q14);
}

static void send_decay(void)
{
    /* Convert 0-255 display to Q1.14 gain: useful range ~0x2000..0x3C00
     * Map 0→0x2000, 255→0x3C00 */
    int16_t q14 = (int16_t)(0x2000 + ((u32)s_decay * (0x3C00 - 0x2000) / 255));
    mw_aud_reverb_decay(q14);
}

static void send_ch_send(u8 ch)
{
    mw_aud_reverb_send(ch, s_send[ch]);
}

/* ── Draw popup ──────────────────────────────────────────────────────────── */

static void draw_cursor(u8 param, bool selected)
{
    u16 row;
    if (param == PAR_BYPASS)     row = ROW_BYPASS;
    else if (param == PAR_PRESET) row = ROW_PRESET;
    else if (param == PAR_MIX)    row = ROW_MIX;
    else if (param == PAR_DECAY)  row = ROW_DECAY;
    else                          row = ROW_SEND0 + (param - PAR_SEND0);

    VDP_setTextPalette(selected ? PAL1 : PAL0);
    VDP_drawText(selected ? ">" : " ", 1, row);
    VDP_setTextPalette(PAL0);
}

static void draw_bypass_row(void)
{
    VDP_setTextPalette(s_enabled ? PAL1 : PAL2);
    VDP_drawText(s_enabled ? "  Reverb:  [ON]  off " : "  Reverb:   on  [OFF]", 1, ROW_BYPASS);
    VDP_setTextPalette(PAL0);
}

static void draw_preset_row(void)
{
    char buf[40];
    memset(buf, ' ', 39); buf[39] = '\0';
    memcpy(buf + 2, "Preset:  < ", 11);
    {
        const char *name = preset_names[s_preset];
        u8 len = strlen(name);
        memcpy(buf + 13, name, len);
        memcpy(buf + 13 + len, " >", 2);
    }
    VDP_setTextPalette(PAL3);
    VDP_drawText(buf, 1, ROW_PRESET);
    VDP_setTextPalette(PAL0);
}

static void draw_mix_row(void)
{
    VDP_setTextPalette(PAL3);
    VDP_drawText("  Mix:   ", 1, ROW_MIX);
    draw_bar(10, ROW_MIX, s_mix, 20, PAL1);
    draw_pct(31, ROW_MIX, s_mix);
}

static void draw_decay_row(void)
{
    VDP_setTextPalette(PAL3);
    VDP_drawText("  Decay: ", 1, ROW_DECAY);
    draw_bar(10, ROW_DECAY, s_decay, 20, PAL1);
    draw_pct(31, ROW_DECAY, s_decay);
}

static void draw_send_row(u8 ch)
{
    char label[10];
    u16 row = ROW_SEND0 + ch;
    label[0] = ' '; label[1] = ' ';
    label[2] = 'C'; label[3] = 'H';
    label[4] = '1' + ch;
    label[5] = ':'; label[6] = ' '; label[7] = '\0';
    VDP_setTextPalette(PAL3);
    VDP_drawText(label, 1, row);
    draw_bar(9, row, s_send[ch], 20, PAL1);
    draw_pct(30, row, s_send[ch]);
}

static void draw_popup(void)
{
    u8 i;
    clear_rows(POP_TOP, POP_BOT);

    VDP_setTextPalette(PAL3);
    VDP_drawText("---- REVERB CONTROL ---- START=close ----", 0, ROW_HEADER);
    VDP_setTextPalette(PAL0);

    draw_bypass_row();
    draw_preset_row();
    draw_mix_row();
    draw_decay_row();

    VDP_setTextPalette(PAL3);
    VDP_drawText("---- CHANNEL SENDS ----", 1, ROW_SEP);
    VDP_setTextPalette(PAL0);

    for (i = 0; i < 8; i++)
        draw_send_row(i);

    VDP_setTextPalette(PAL3);
    VDP_drawText("-----------------------------------------", 0, ROW_BOTTOM);
    VDP_setTextPalette(PAL0);

    draw_cursor(s_cursor, TRUE);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void reverb_ctrl_init(rc_redraw_fn redraw_fn)
{
    s_redraw = redraw_fn;
    s_active = FALSE;
    s_cursor = 0;

    /* Set defaults — sent to firmware at boot */
    s_enabled = TRUE;
    s_preset = 3;       /* Cave */
    s_mix = 130;        /* 51% */
    s_decay = 209;      /* 82% */
    s_send[6] = 130;    /* CH7 51% */
    s_send[7] = 130;    /* CH8 51% */
}

bool reverb_ctrl_active(void)
{
    return s_active;
}

bool reverb_ctrl_is_enabled(void)
{
    return s_enabled;
}

void reverb_ctrl_send_defaults(void)
{
    send_enable();
    send_preset();
    send_mix();
    send_decay();
    send_ch_send(6);
    send_ch_send(7);
}

void reverb_ctrl_open(void)
{
    s_active = TRUE;
    s_cursor = 0;
    draw_popup();
}

void reverb_ctrl_frame(u16 press)
{
    if (!s_active) return;

    /* START: close popup */
    if (press & BUTTON_START) {
        s_active = FALSE;
        clear_rows(POP_TOP, POP_BOT);
        if (s_redraw) s_redraw();
        return;
    }

    /* Navigation */
    if (press & BUTTON_UP) {
        draw_cursor(s_cursor, FALSE);
        if (s_cursor > 0) s_cursor--;
        else s_cursor = NUM_PARAMS - 1;
        draw_cursor(s_cursor, TRUE);
    }
    if (press & BUTTON_DOWN) {
        draw_cursor(s_cursor, FALSE);
        if (s_cursor < NUM_PARAMS - 1) s_cursor++;
        else s_cursor = 0;
        draw_cursor(s_cursor, TRUE);
    }

    /* A button: toggle bypass */
    if ((press & BUTTON_A) && s_cursor == PAR_BYPASS) {
        s_enabled = !s_enabled;
        send_enable();
        draw_bypass_row();
        draw_cursor(s_cursor, TRUE);
    }

    /* Left/Right: adjust selected parameter */
    if (press & (BUTTON_LEFT | BUTTON_RIGHT)) {
        s8 dir = (press & BUTTON_RIGHT) ? 1 : -1;

        switch (s_cursor) {
        case PAR_PRESET:
            s_preset = (u8)((s_preset + dir + NUM_PRESETS) % NUM_PRESETS);
            send_preset();
            draw_preset_row();
            break;

        case PAR_MIX:
            if (dir > 0 && s_mix <= 245) s_mix += 10;
            else if (dir > 0) s_mix = 255;
            else if (dir < 0 && s_mix >= 10) s_mix -= 10;
            else s_mix = 0;
            send_mix();
            draw_mix_row();
            break;

        case PAR_DECAY:
            if (dir > 0 && s_decay <= 245) s_decay += 10;
            else if (dir > 0) s_decay = 255;
            else if (dir < 0 && s_decay >= 10) s_decay -= 10;
            else s_decay = 0;
            send_decay();
            draw_decay_row();
            break;

        default:
            /* Channel sends PAR_SEND0..PAR_SEND0+7 */
            if (s_cursor >= PAR_SEND0 && s_cursor < PAR_SEND0 + 8) {
                u8 ch = s_cursor - PAR_SEND0;
                if (dir > 0 && s_send[ch] <= 245) s_send[ch] += 10;
                else if (dir > 0) s_send[ch] = 255;
                else if (dir < 0 && s_send[ch] >= 10) s_send[ch] -= 10;
                else s_send[ch] = 0;
                send_ch_send(ch);
                draw_send_row(ch);
            }
            break;
        }
        draw_cursor(s_cursor, TRUE);
    }
}
