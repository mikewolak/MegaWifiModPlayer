/*
 * main.c — MegaWifi MOD Player for Sega Genesis
 *
 * CD player-style transport UI with:
 *   - Play/Stop/Pause (A/B/C buttons)
 *   - Track timer (pattern:row)
 *   - Master volume control (Up/Down)
 *   - VU meter bars (text, 60 FPS from real mixer amplitudes)
 *   - Pixel-smooth scrolling track name marquee
 *   - Start: track selection (TODO)
 *
 * Communicates with ESP32-C3 firmware via MW_CMD_AUD_* commands.
 */

#include <genesis.h>
#include <task.h>
#include <string.h>
#include "ext/mw/megawifi.h"
#include "ext/mw/mw-msg.h"

extern void mw_set_draw_hook(void (*hook)(void));

/* Audio command wrappers — defined in our local megawifi.c */
extern enum mw_err mw_aud_play(void);
extern enum mw_err mw_aud_stop(void);
extern enum mw_err mw_aud_pause(void);
extern enum mw_err mw_aud_resume(void);
extern enum mw_err mw_aud_status(uint8_t *state, uint8_t *pattern,
		uint8_t *row, uint8_t *song_len, uint8_t *amplitudes);
extern enum mw_err mw_aud_set_vol(uint8_t vol);

#define FPS 60
#define MS_TO_FRAMES(ms)  ((((ms) * 60 / 500) + 1) / 2)

/* ── Screen layout (40×28 tiles) ─────────────────────────────────────────── */
#define ROW_TITLE       0
#define ROW_TRACK       2
#define ROW_AUTHOR      3
#define ROW_STATUS      5
#define ROW_VU_TOP      7
#define ROW_VU_LABEL    8
#define ROW_VU_START    9
#define ROW_VU_END      14
#define ROW_VU_BOT      15
#define ROW_VOLUME      17
#define ROW_HELP1       19
#define ROW_HELP2       20
#define ROW_HELP3       21
#define ROW_MARQUEE     24
#define ROW_FOOTER      27

/* ── Player state ────────────────────────────────────────────────────────── */
#define PS_STOPPED  0
#define PS_PLAYING  1
#define PS_PAUSED   2

static u8               g_state = PS_STOPPED;
static u8               g_master_vol = 255;
static u8               g_vu[8] = {0};
static u8               g_vu_peak[8] = {0};
static u8               g_vu_decay[8] = {0};
static u8               g_pattern = 0;
static u8               g_row = 0;
static u8               g_song_len = 0;
static char             g_track_name[24] = "space_debris";
static bool             g_mw_connected = FALSE;

/* ── MegaWifi ────────────────────────────────────────────────────────────── */
static uint16_t cmd_buf[MW_BUFLEN / 2];

/* ── Palette setup ───────────────────────────────────────────────────────── */
static void setup_palettes(void)
{
    PAL_setColor(0, RGB24_TO_VDPCOLOR(0x000000));
    PAL_setColor(15, RGB24_TO_VDPCOLOR(0xFFFFFF));

    /* PAL1: green (playing, VU bars) */
    PAL_setColor(16, RGB24_TO_VDPCOLOR(0x000000));
    PAL_setColor(17, RGB24_TO_VDPCOLOR(0x00CC44));
    PAL_setColor(18, RGB24_TO_VDPCOLOR(0x00FF66));
    PAL_setColor(19, RGB24_TO_VDPCOLOR(0xFFFF00));
    PAL_setColor(20, RGB24_TO_VDPCOLOR(0xFF4444));
    PAL_setColor(31, RGB24_TO_VDPCOLOR(0x00FF66));

    /* PAL2: red/orange (stopped) */
    PAL_setColor(32, RGB24_TO_VDPCOLOR(0x000000));
    PAL_setColor(47, RGB24_TO_VDPCOLOR(0xFF4444));

    /* PAL3: cyan/blue (info, marquee) */
    PAL_setColor(48, RGB24_TO_VDPCOLOR(0x000000));
    PAL_setColor(49, RGB24_TO_VDPCOLOR(0x0088CC));
    PAL_setColor(50, RGB24_TO_VDPCOLOR(0x00AAFF));
    PAL_setColor(63, RGB24_TO_VDPCOLOR(0x00CCFF));
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void clear_row(u16 row)
{
    VDP_drawText("                                        ", 0, row);
}

static void draw_centered(const char *text, u16 row, u16 pal)
{
    u16 len = strlen(text);
    u16 col = (40 - len) / 2;
    if (col > 39) col = 0;
    VDP_setTextPalette(pal);
    clear_row(row);
    VDP_drawText(text, col, row);
    VDP_setTextPalette(PAL0);
}

static char *itoa_simple(u32 val, char *buf)
{
    char tmp[12];
    int i = 0;
    if (val == 0) { *buf++ = '0'; *buf = '\0'; return buf; }
    while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
    while (i > 0) *buf++ = tmp[--i];
    *buf = '\0';
    return buf;
}

static void hex_byte(u8 val, char *buf)
{
    const char hex[] = "0123456789ABCDEF";
    buf[0] = hex[(val >> 4) & 0xF];
    buf[1] = hex[val & 0xF];
}

/* ── Draw static UI ──────────────────────────────────────────────────────── */

static void draw_static_ui(void)
{
    draw_centered("=== MEGAWIFI MOD PLAYER ===", ROW_TITLE, PAL3);

    {
        char buf[40] = "  Track: ";
        strcat(buf, g_track_name);
        clear_row(ROW_TRACK);
        VDP_drawText(buf, 0, ROW_TRACK);
    }

    /* VU meter frame */
    VDP_setTextPalette(PAL3);
    VDP_drawText("+------------------------------+", 3, ROW_VU_TOP);
    VDP_drawText("|  CH1  CH2  CH3  CH4          |", 3, ROW_VU_LABEL);
    {
        u16 r;
        for (r = ROW_VU_START; r <= ROW_VU_END; r++)
            VDP_drawText("|                              |", 3, r);
    }
    VDP_drawText("+------------------------------+", 3, ROW_VU_BOT);

    /* Help text */
    VDP_drawText("  [A] Play  [B] Stop  [C] Pause", 0, ROW_HELP1);
    VDP_drawText("  [Start] Track Select", 0, ROW_HELP2);
    VDP_drawText("  [Up/Dn] Volume", 0, ROW_HELP3);

    draw_centered("===================================", ROW_FOOTER, PAL3);
    VDP_setTextPalette(PAL0);
}

/* ── Draw status line ────────────────────────────────────────────────────── */

static void draw_status(void)
{
    char buf[40];
    char hex[3] = {0};
    u16 pal;

    memset(buf, 0, sizeof(buf));

    switch (g_state) {
        case PS_PLAYING: strcpy(buf, "  > PLAYING    "); pal = PAL1; break;
        case PS_PAUSED:  strcpy(buf, "  | PAUSED     "); pal = PAL3; break;
        default:         strcpy(buf, "  . STOPPED    "); pal = PAL2; break;
    }

    strcat(buf, "Pat ");
    hex_byte(g_pattern, hex); strcat(buf, hex);
    strcat(buf, "/");
    hex_byte(g_song_len, hex); strcat(buf, hex);
    strcat(buf, "  Row ");
    hex_byte(g_row, hex); strcat(buf, hex);
    strcat(buf, "/3F");

    VDP_setTextPalette(pal);
    clear_row(ROW_STATUS);
    VDP_drawText(buf, 0, ROW_STATUS);
    VDP_setTextPalette(PAL0);
}

/* ── Draw VU meters ──────────────────────────────────────────────────────── */

static void draw_vu_meters(void)
{
    static const u16 ch_col[4] = { 6, 12, 18, 24 };
    u16 max_bars = ROW_VU_END - ROW_VU_START + 1;
    u8 ch;

    for (ch = 0; ch < 4; ch++) {
        u8 amp = g_vu[ch];
        u8 bars = (amp * max_bars + 127) / 255;
        u16 r;

        /* Peak hold with decay */
        if (amp > g_vu_peak[ch]) {
            g_vu_peak[ch] = amp;
            g_vu_decay[ch] = 30;
        } else if (g_vu_decay[ch] > 0) {
            g_vu_decay[ch]--;
        } else if (g_vu_peak[ch] > 0) {
            g_vu_peak[ch] -= (g_vu_peak[ch] > 4) ? 4 : g_vu_peak[ch];
        }

        {
            u8 peak_bar = (g_vu_peak[ch] * max_bars + 127) / 255;

            for (r = 0; r < max_bars; r++) {
                u16 row = ROW_VU_END - r;
                u16 col = ch_col[ch];

                if (r < bars) {
                    VDP_setTextPalette(r >= max_bars - 1 ? PAL2 : PAL1);
                    VDP_drawText("####", col, row);
                } else if (r == peak_bar && peak_bar > 0) {
                    VDP_setTextPalette(PAL1);
                    VDP_drawText("----", col, row);
                } else {
                    VDP_setTextPalette(PAL3);
                    VDP_drawText("    ", col, row);
                }
            }
        }
    }
    VDP_setTextPalette(PAL0);
}

/* ── Draw volume bar ─────────────────────────────────────────────────────── */

static void draw_volume(void)
{
    char buf[40];
    u8 bar_len = 20;
    u8 filled = (g_master_vol * bar_len + 127) / 255;
    u8 pct = (g_master_vol * 100 + 127) / 255;
    char *p;
    u8 i;

    memset(buf, 0, sizeof(buf));
    strcpy(buf, "  Vol: ");
    p = buf + 7;

    for (i = 0; i < bar_len; i++)
        *p++ = (i < filled) ? '#' : '.';
    *p++ = ' ';
    p = itoa_simple(pct, p);
    *p++ = '%';
    *p = '\0';

    VDP_setTextPalette(PAL1);
    clear_row(ROW_VOLUME);
    VDP_drawText(buf, 0, ROW_VOLUME);
    VDP_setTextPalette(PAL0);
}

/* ── Poll firmware for status + amplitudes ───────────────────────────────── */

static void poll_status(void)
{
    u8 amps[8];
    u8 st, pat, rw, slen;

    if (!g_mw_connected) return;

    if (mw_aud_status(&st, &pat, &rw, &slen, amps) == MW_ERR_NONE) {
        g_state = st;
        g_pattern = pat;
        g_row = rw;
        g_song_len = slen;

        /* Use micromod's 4-channel amplitudes for VU meters */
        g_vu[0] = amps[0];
        g_vu[1] = amps[1];
        g_vu[2] = amps[2];
        g_vu[3] = amps[3];
    }
}

/* ── Pixel-smooth scrolling marquee (VDP hardware scroll) ────────────────── */

static const char *marquee_text = "  ~~~  MEGAWIFI MOD PLAYER  ~~~  "
    "Space Debris by Captain (1993)  ~~~  "
    "4-channel ProTracker  ~~~  "
    "Fixed-point decode on ESP32-C3  ~~~  "
    "8-ch stereo mixer with pan  ~~~  ";
static u16 marquee_scroll_x = 0;
static u16 marquee_str_pos = 0;
static bool marquee_needs_init = TRUE;

static void marquee_write_tile(u16 tile_col, u16 str_pos)
{
    u16 slen = (u16)strlen(marquee_text);
    u8 ch = marquee_text[str_pos % slen];
    u16 tile_idx = TILE_FONT_INDEX + (ch - 32);
    VDP_setTileMapXY(BG_A,
        TILE_ATTR_FULL(PAL3, FALSE, FALSE, FALSE, tile_idx),
        tile_col, ROW_MARQUEE);
}

static void marquee_init(void)
{
    u16 c;
    for (c = 0; c < 64; c++)
        marquee_write_tile(c, c);
    marquee_str_pos = 64;
    marquee_scroll_x = 0;
    marquee_needs_init = FALSE;
}

static void update_marquee(void)
{
    s16 neg_scroll;

    if (marquee_needs_init) marquee_init();

    marquee_scroll_x = (marquee_scroll_x + 1) & 0x1FF;
    neg_scroll = -(s16)marquee_scroll_x;
    VDP_setHorizontalScrollTile(BG_A, ROW_MARQUEE, &neg_scroll, 1, CPU);

    if ((marquee_scroll_x & 7) == 0) {
        u16 tile_col = (u16)((marquee_scroll_x / 8 + 63) % 64);
        marquee_write_tile(tile_col, marquee_str_pos);
        marquee_str_pos++;
    }
}

/* ── Draw hook — called every frame during mw_command waits ──────────────── */

static void frame_draw_hook(void)
{
    update_marquee();
}

/* ── Input handling ──────────────────────────────────────────────────────── */

static void handle_input(void)
{
    u16 joy = JOY_readJoypad(JOY_1);
    static u16 prev_joy = 0;
    u16 pressed = joy & ~prev_joy;
    prev_joy = joy;

    if (!g_mw_connected) return;

    if (pressed & BUTTON_A) {
        /* Play — always restart from top */
        mw_aud_play();
    }

    if (pressed & BUTTON_B) {
        mw_aud_stop();
    }

    if (pressed & BUTTON_C) {
        if (g_state == PS_PLAYING)
            mw_aud_pause();
        else if (g_state == PS_PAUSED)
            mw_aud_resume();
    }

    if (pressed & BUTTON_UP) {
        if (g_master_vol <= 245) g_master_vol += 10;
        else g_master_vol = 255;
        mw_aud_set_vol(g_master_vol);
        draw_volume();
    }

    if (pressed & BUTTON_DOWN) {
        if (g_master_vol >= 10) g_master_vol -= 10;
        else g_master_vol = 0;
        mw_aud_set_vol(g_master_vol);
        draw_volume();
    }
}

/* ── MegaWifi init ───────────────────────────────────────────────────────── */

static void user_tsk(void)
{
    while (1) mw_process();
}

static bool megawifi_init(void)
{
    uint8_t fw_major, fw_minor;
    char *variant = NULL;

    if (mw_init(cmd_buf, MW_BUFLEN) != MW_ERR_NONE) return FALSE;
    TSK_userSet(user_tsk);

    if (mw_detect(&fw_major, &fw_minor, &variant) != MW_ERR_NONE)
        return FALSE;

    /* Show firmware version */
    {
        char buf[40] = "  FW: v";
        char num[4];
        itoa_simple(fw_major, num); strcat(buf, num);
        strcat(buf, ".");
        itoa_simple(fw_minor, num); strcat(buf, num);
        if (variant) { strcat(buf, "-"); strcat(buf, variant); }
        VDP_drawText(buf, 0, ROW_AUTHOR);
    }

    return TRUE;
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(bool hard)
{
    VDP_setScreenWidth320();
    VDP_setScrollingMode(HSCROLL_TILE, VSCROLL_COLUMN);
    VDP_setTextPalette(PAL0);
    VDP_clearPlane(BG_A, TRUE);
    VDP_clearPlane(BG_B, TRUE);

    setup_palettes();
    draw_static_ui();
    draw_status();
    draw_volume();

    /* Show "Connecting..." */
    draw_centered("Connecting to MegaWifi...", ROW_AUTHOR, PAL3);

    g_mw_connected = megawifi_init();

    if (g_mw_connected) {
        draw_centered("Connected!", ROW_AUTHOR, PAL1);
        mw_set_draw_hook(frame_draw_hook);
    } else {
        draw_centered("MegaWifi not found", ROW_AUTHOR, PAL2);
    }

    /* Main loop */
    while (1) {
        VDP_waitVSync();

        /* Marquee scroll register first in VBlank */
        update_marquee();

        handle_input();
        poll_status();
        draw_status();
        draw_vu_meters();
    }

    return 0;
}
