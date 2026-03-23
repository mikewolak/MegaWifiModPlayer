/*
 * main.c — MegaWifi MOD Player for Sega Genesis
 *
 * CD player-style transport UI with:
 *   - Play/Stop/Pause (A/B/C buttons)
 *   - Track timer (pattern:row)
 *   - Master volume control (Up/Down)
 *   - VU meter bars (sprites, 60 FPS)
 *   - Scrolling track name marquee
 *   - Start: track selection list
 *
 * Based on perf_test boilerplate for MegaWifi init.
 */

#include <genesis.h>
#include <task.h>
#include <string.h>
#include "ext/mw/megawifi.h"
#include "ext/mw/mw-msg.h"

extern void mw_set_draw_hook(void (*hook)(void));

#define FPS 60

/* ── Screen layout (40×28 tiles) ─────────────────────────────────────────── */
/*
 *  Row  0: ═══════ MEGAWIFI MOD PLAYER ═══════
 *  Row  1: (blank)
 *  Row  2:   Track: space_debris
 *  Row  3:   Author: Captain
 *  Row  4: (blank)
 *  Row  5:   ▶ PLAYING    Pat 03/1F  Row 24/3F
 *  Row  6: (blank)
 *  Row  7:   ╔══════════════════════════════╗
 *  Row  8:   ║  CH1  CH2  CH3  CH4         ║
 *  Row  9:   ║  ██   ██   ██   ██          ║
 *  Row 10:   ║  ██   ██   ██   ██          ║
 *  Row 11:   ║  ██   ██   ██   ██          ║
 *  Row 12:   ║  ██        ██               ║
 *  Row 13:   ║  ██        ██               ║
 *  Row 14:   ║                             ║
 *  Row 15:   ╚══════════════════════════════╝
 *  Row 16: (blank)
 *  Row 17:   Master Vol: ████████████░░░░ 75%
 *  Row 18: (blank)
 *  Row 19:   [A] Play  [B] Stop  [C] Pause
 *  Row 20:   [Start] Track Select
 *  Row 21:   [Up/Dn] Volume
 *  Row 22: (blank)
 *  Row 23-26: Scrolling marquee / metadata
 *  Row 27: ═══════════════════════════════════
 */

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
typedef enum {
    PS_STOPPED = 0,
    PS_PLAYING,
    PS_PAUSED,
} play_state_t;

static play_state_t     g_state = PS_STOPPED;
static u8               g_master_vol = 255;
static u8               g_vu[4] = {0};          /* per-channel amplitude 0-255 */
static u8               g_vu_peak[4] = {0};     /* peak hold for VU meters */
static u8               g_vu_decay[4] = {0};    /* decay counter */
static u16              g_pattern = 0;
static u16              g_row = 0;
static u16              g_total_patterns = 0x1F;
static char             g_track_name[32] = "space_debris";
static char             g_author[32] = "Captain";

/* Simulated VU for now — will come from firmware later */
static u16 g_vu_frame = 0;

/* ── MegaWifi ────────────────────────────────────────────────────────────── */
static uint16_t cmd_buf[MW_BUFLEN / 2];

/* ── Palette setup ───────────────────────────────────────────────────────── */
static void setup_palettes(void)
{
    /* PAL0: white text on black (default) */
    PAL_setColor(0, RGB24_TO_VDPCOLOR(0x000000));   /* BG black */
    PAL_setColor(15, RGB24_TO_VDPCOLOR(0xFFFFFF));   /* text white */

    /* PAL1: green (playing state, VU bars) */
    PAL_setColor(16, RGB24_TO_VDPCOLOR(0x000000));
    PAL_setColor(17, RGB24_TO_VDPCOLOR(0x00CC44));   /* green */
    PAL_setColor(18, RGB24_TO_VDPCOLOR(0x00FF66));   /* bright green */
    PAL_setColor(19, RGB24_TO_VDPCOLOR(0xFFFF00));   /* yellow (peak) */
    PAL_setColor(20, RGB24_TO_VDPCOLOR(0xFF4444));   /* red (clip) */
    PAL_setColor(31, RGB24_TO_VDPCOLOR(0x00FF66));   /* bright green text */

    /* PAL2: red/orange (stopped, errors) */
    PAL_setColor(32, RGB24_TO_VDPCOLOR(0x000000));
    PAL_setColor(47, RGB24_TO_VDPCOLOR(0xFF4444));   /* red text */

    /* PAL3: cyan/blue (info, volume bar) */
    PAL_setColor(48, RGB24_TO_VDPCOLOR(0x000000));
    PAL_setColor(49, RGB24_TO_VDPCOLOR(0x0088CC));   /* blue */
    PAL_setColor(50, RGB24_TO_VDPCOLOR(0x00AAFF));   /* bright blue */
    PAL_setColor(63, RGB24_TO_VDPCOLOR(0x00CCFF));   /* cyan text */
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

/* Integer to decimal string */
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

/* Hex byte to string */
static void hex_byte(u8 val, char *buf)
{
    const char hex[] = "0123456789ABCDEF";
    buf[0] = hex[(val >> 4) & 0xF];
    buf[1] = hex[val & 0xF];
}

/* ── Draw static UI elements ─────────────────────────────────────────────── */

static void draw_static_ui(void)
{
    /* Title bar */
    draw_centered("=== MEGAWIFI MOD PLAYER ===", ROW_TITLE, PAL3);

    /* Track info */
    {
        char buf[40];
        memset(buf, 0, sizeof(buf));
        strcpy(buf, "  Track: ");
        strcat(buf, g_track_name);
        VDP_setTextPalette(PAL0);
        clear_row(ROW_TRACK);
        VDP_drawText(buf, 0, ROW_TRACK);
    }
    {
        char buf[40];
        memset(buf, 0, sizeof(buf));
        strcpy(buf, "  Author: ");
        strcat(buf, g_author);
        VDP_setTextPalette(PAL0);
        clear_row(ROW_AUTHOR);
        VDP_drawText(buf, 0, ROW_AUTHOR);
    }

    /* VU meter frame — using text characters for the border */
    VDP_setTextPalette(PAL3);
    VDP_drawText("+------------------------------+", 3, ROW_VU_TOP);
    VDP_drawText("|  CH1  CH2  CH3  CH4          |", 3, ROW_VU_LABEL);
    for (u16 r = ROW_VU_START; r <= ROW_VU_END; r++) {
        VDP_drawText("|                              |", 3, r);
    }
    VDP_drawText("+------------------------------+", 3, ROW_VU_BOT);
    VDP_setTextPalette(PAL0);

    /* Help text */
    VDP_setTextPalette(PAL3);
    VDP_drawText("  [A] Play  [B] Stop  [C] Pause", 0, ROW_HELP1);
    VDP_drawText("  [Start] Track Select", 0, ROW_HELP2);
    VDP_drawText("  [Up/Dn] Volume", 0, ROW_HELP3);
    VDP_setTextPalette(PAL0);

    /* Footer */
    draw_centered("===================================", ROW_FOOTER, PAL3);
}

/* ── Draw playback status ────────────────────────────────────────────────── */

static void draw_status(void)
{
    char buf[40];
    char hex[3] = {0};
    u16 pal;

    memset(buf, 0, sizeof(buf));

    switch (g_state) {
        case PS_PLAYING:
            strcpy(buf, "  > PLAYING    ");
            pal = PAL1;
            break;
        case PS_PAUSED:
            strcpy(buf, "  | PAUSED     ");
            pal = PAL3;
            break;
        case PS_STOPPED:
        default:
            strcpy(buf, "  . STOPPED    ");
            pal = PAL2;
            break;
    }

    /* Append pattern/row info */
    strcat(buf, "Pat ");
    hex_byte(g_pattern & 0xFF, hex);
    strcat(buf, hex);
    strcat(buf, "/");
    hex_byte(g_total_patterns & 0xFF, hex);
    strcat(buf, hex);
    strcat(buf, "  Row ");
    hex_byte(g_row & 0xFF, hex);
    strcat(buf, hex);
    strcat(buf, "/3F");

    VDP_setTextPalette(pal);
    clear_row(ROW_STATUS);
    VDP_drawText(buf, 0, ROW_STATUS);
    VDP_setTextPalette(PAL0);
}

/* ── Draw VU meters ──────────────────────────────────────────────────────── */

static void draw_vu_meters(void)
{
    /* VU meter area: rows 9-14 (6 rows), 4 channels
     * Each channel column is 4 chars wide, starting at col 6,12,18,24 */
    static const u16 ch_col[4] = { 6, 12, 18, 24 };
    u16 max_bars = ROW_VU_END - ROW_VU_START + 1;  /* 6 rows */

    for (u8 ch = 0; ch < 4; ch++) {
        /* Map amplitude 0-255 to 0-6 bars */
        u8 amp = g_vu[ch];
        u8 bars = (amp * max_bars + 127) / 255;

        /* Peak hold with decay */
        if (amp > g_vu_peak[ch]) {
            g_vu_peak[ch] = amp;
            g_vu_decay[ch] = 30;    /* hold for 30 frames (0.5 sec) */
        } else if (g_vu_decay[ch] > 0) {
            g_vu_decay[ch]--;
        } else if (g_vu_peak[ch] > 0) {
            g_vu_peak[ch] -= (g_vu_peak[ch] > 4) ? 4 : g_vu_peak[ch];
        }
        u8 peak_bar = (g_vu_peak[ch] * max_bars + 127) / 255;

        /* Draw bars bottom-up */
        for (u16 r = 0; r < max_bars; r++) {
            u16 row = ROW_VU_END - r;  /* bottom to top */
            u16 col = ch_col[ch];
            u16 pal;

            if (r < bars) {
                /* Active bar */
                if (r >= max_bars - 1)
                    pal = PAL2;     /* red for top bar (clip) */
                else if (r >= max_bars - 2)
                    pal = PAL1;     /* yellow zone */
                else
                    pal = PAL1;     /* green */
                VDP_setTextPalette(pal);
                VDP_drawText("####", col, row);
            } else if (r == peak_bar && peak_bar > 0) {
                /* Peak hold indicator */
                VDP_setTextPalette(PAL1);
                VDP_drawText("----", col, row);
            } else {
                /* Empty */
                VDP_setTextPalette(PAL3);
                VDP_drawText("    ", col, row);
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

    memset(buf, 0, sizeof(buf));
    strcpy(buf, "  Vol: ");
    p = buf + 7;

    for (u8 i = 0; i < bar_len; i++) {
        *p++ = (i < filled) ? '#' : '.';
    }
    *p++ = ' ';
    p = itoa_simple(pct, p);
    *p++ = '%';
    *p = '\0';

    VDP_setTextPalette(PAL1);
    clear_row(ROW_VOLUME);
    VDP_drawText(buf, 0, ROW_VOLUME);
    VDP_setTextPalette(PAL0);
}

/* ── Simulate VU data (until firmware sends real data) ───────────────────── */

static void simulate_vu(void)
{
    if (g_state != PS_PLAYING) {
        for (u8 i = 0; i < 4; i++) g_vu[i] = 0;
        return;
    }

    g_vu_frame++;

    /* Fake bouncing VU levels — each channel at different rates */
    for (u8 i = 0; i < 4; i++) {
        u16 phase = (g_vu_frame * (3 + i * 7)) & 0xFF;
        /* Simple triangle wave */
        u8 val = (phase < 128) ? (phase * 2) : (255 - (phase - 128) * 2);
        /* Scale and add some randomness from frame count */
        val = (val * (180 + (g_vu_frame >> (i + 2)) % 76)) >> 8;
        g_vu[i] = val;
    }

    /* Simulate pattern/row advancement */
    g_row = (g_vu_frame / 8) % 64;
    g_pattern = (g_vu_frame / (8 * 64)) % (g_total_patterns + 1);
}

/* ── Pixel-smooth scrolling marquee (VDP hardware scroll) ────────────────── */
/*
 * Uses VDP HSCROLL_TILE mode to scroll one row of BG_A by 1 pixel/frame.
 * New character tiles are streamed into the 64-tile-wide circular plane
 * as they scroll into view. Same technique as the stock ticker.
 */

static const char *marquee_text = "  ~~~  MEGAWIFI MOD PLAYER  ~~~  "
    "Space Debris by Captain (1993)  ~~~  "
    "4-channel ProTracker  ~~~  "
    "Helix / micromod fixed-point decode  ~~~  "
    "8-ch mixer with stereo pan  ~~~  ";
static u16 marquee_scroll_x = 0;
static u16 marquee_str_pos = 0;
static bool marquee_needs_init = TRUE;

/* Write one character tile into the BG_A tilemap at the given column */
static void marquee_write_tile(u16 tile_col, u16 str_pos)
{
    u16 slen = (u16)strlen(marquee_text);
    u8 ch = marquee_text[str_pos % slen];
    /* SGDK font tiles start at TILE_FONT_INDEX, ASCII char maps to tile offset */
    u16 tile_idx = TILE_FONT_INDEX + (ch - 32);
    VDP_setTileMapXY(BG_A,
        TILE_ATTR_FULL(PAL3, FALSE, FALSE, FALSE, tile_idx),
        tile_col, ROW_MARQUEE);
}

static void marquee_init(void)
{
    u16 c;
    /* Fill the entire 64-tile row with initial marquee content */
    for (c = 0; c < 64; c++)
        marquee_write_tile(c, c);
    marquee_str_pos = 64;
    marquee_scroll_x = 0;
    marquee_needs_init = FALSE;
}

static void update_marquee(void)
{
    s16 neg_scroll;

    if (marquee_needs_init) {
        marquee_init();
    }

    /* Advance scroll by 1 pixel per frame */
    marquee_scroll_x = (marquee_scroll_x + 1) & 0x1FF;
    neg_scroll = -(s16)marquee_scroll_x;
    VDP_setHorizontalScrollTile(BG_A, ROW_MARQUEE, &neg_scroll, 1, CPU);

    /* When scroll crosses a tile boundary (every 8 pixels), stream next char */
    if ((marquee_scroll_x & 7) == 0) {
        /* The tile column that just scrolled off the left edge */
        u16 tile_col = (u16)((marquee_scroll_x / 8 + 63) % 64);
        marquee_write_tile(tile_col, marquee_str_pos);
        marquee_str_pos++;
    }
}

/* ── Draw hook — called every frame during mw_process ────────────────────── */

static void frame_draw_hook(void)
{
    simulate_vu();
    draw_vu_meters();
    update_marquee();
}

/* ── Input handling ──────────────────────────────────────────────────────── */

static void handle_input(void)
{
    u16 joy = JOY_readJoypad(JOY_1);
    static u16 prev_joy = 0;
    u16 pressed = joy & ~prev_joy;  /* rising edge only */
    prev_joy = joy;

    if (pressed & BUTTON_A) {
        /* Play — always restart */
        g_state = PS_PLAYING;
        g_vu_frame = 0;
        g_pattern = 0;
        g_row = 0;
        for (u8 i = 0; i < 4; i++) { g_vu[i] = 0; g_vu_peak[i] = 0; }
        draw_status();
        /* TODO: send MW_CMD_AUD_PLAY to firmware */
    }

    if (pressed & BUTTON_B) {
        /* Stop */
        g_state = PS_STOPPED;
        for (u8 i = 0; i < 4; i++) { g_vu[i] = 0; g_vu_peak[i] = 0; }
        draw_status();
        /* TODO: send MW_CMD_AUD_STOP to firmware */
    }

    if (pressed & BUTTON_C) {
        /* Pause / Resume toggle */
        if (g_state == PS_PLAYING) {
            g_state = PS_PAUSED;
        } else if (g_state == PS_PAUSED) {
            g_state = PS_PLAYING;
        }
        draw_status();
        /* TODO: send MW_CMD_AUD_PAUSE/RESUME to firmware */
    }

    if (pressed & BUTTON_UP) {
        if (g_master_vol <= 245) g_master_vol += 10;
        else g_master_vol = 255;
        draw_volume();
        /* TODO: send MW_CMD_AUD_VOL to firmware */
    }

    if (pressed & BUTTON_DOWN) {
        if (g_master_vol >= 10) g_master_vol -= 10;
        else g_master_vol = 0;
        draw_volume();
        /* TODO: send MW_CMD_AUD_VOL to firmware */
    }

    if (pressed & BUTTON_START) {
        /* TODO: track select menu */
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

    return TRUE;
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(bool hard)
{
    /* VDP setup */
    VDP_setScreenWidth320();
    VDP_setScrollingMode(HSCROLL_TILE, VSCROLL_COLUMN);
    VDP_setTextPalette(PAL0);
    VDP_clearPlane(BG_A, TRUE);
    VDP_clearPlane(BG_B, TRUE);

    setup_palettes();

    /* Draw static UI */
    draw_static_ui();
    draw_status();
    draw_volume();

    /* Init MegaWifi */
    megawifi_init();
    mw_set_draw_hook(frame_draw_hook);

    /* Main loop */
    while (1) {
        VDP_waitVSync();

        /* Marquee scroll register must be written first in VBlank */
        update_marquee();

        /* Then handle input and update display */
        handle_input();
        simulate_vu();
        draw_status();
        draw_vu_meters();
    }

    return 0;
}
