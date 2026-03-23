/*
 * main.c — MegaWifi MOD Player for Sega Genesis
 *
 * Per-pixel VU meters using partial-fill tiles on BG_B.
 * Packed 32-bit status word polled every frame.
 */

#include <genesis.h>
#include <task.h>
#include <string.h>
#include "ext/mw/megawifi.h"
#include "ext/mw/mw-msg.h"

extern void mw_set_draw_hook(void (*hook)(void));

extern enum mw_err mw_aud_play(void);
extern enum mw_err mw_aud_stop(void);
extern enum mw_err mw_aud_pause(void);
extern enum mw_err mw_aud_resume(void);
extern enum mw_err mw_aud_status(uint32_t *vu_word, uint32_t *pos_word);
extern enum mw_err mw_aud_set_vol(uint8_t vol);

#define FPS 60

/* ── Layout ──────────────────────────────────────────────────────────────── */
#define ROW_TITLE       0
#define ROW_TRACK       2
#define ROW_STATUS      3
#define ROW_VU_LABEL    5
#define ROW_VU_START    6
#define ROW_VU_END      21
#define ROW_VOL_LABEL   22
#define ROW_VOL_BAR     23
#define ROW_HELP        24
#define ROW_MARQUEE     26
#define ROW_FOOTER      27

#define VU_NUM_ROWS     (ROW_VU_END - ROW_VU_START + 1)  /* 16 */
#define VU_TOTAL_PX     (VU_NUM_ROWS * 8)                 /* 128 */

#define VU_CH_WIDTH     7
#define VU_CH_GAP       1
#define VU_CH_OFFSET    3

#define VOL_BAR_OFFSET  7
#define VOL_BAR_WIDTH   26

/* ── VU tiles ────────────────────────────────────────────────────────────── */
#define T_BLACK         (TILE_USER_INDEX + 0)
#define T_GREEN         (TILE_USER_INDEX + 1)
#define T_YELLOW        (TILE_USER_INDEX + 2)
#define T_RED           (TILE_USER_INDEX + 3)
#define T_PEAK          (TILE_USER_INDEX + 4)
#define T_BLUE          (TILE_USER_INDEX + 5)
#define T_GREEN_P(n)    (TILE_USER_INDEX + 6 + (n))
#define T_YELLOW_P(n)   (TILE_USER_INDEX + 14 + (n))
#define T_RED_P(n)      (TILE_USER_INDEX + 22 + (n))
#define T_BLUE_P(n)     (TILE_USER_INDEX + 30 + (n))

/* ── State ───────────────────────────────────────────────────────────────── */
#define PS_STOPPED  0
#define PS_PLAYING  1
#define PS_PAUSED   2

static u8               g_state = PS_STOPPED;
static u8               g_master_vol = 255;
static u8               g_vu_raw[4] = {0};   /* raw 0-127 from firmware */
static u8               g_vu_px[4] = {0};    /* smoothed display height */
static u8               g_vu_peak[4] = {0};
static u8               g_vu_decay[4] = {0};
static u8               g_pattern = 0;
static u8               g_row = 0;
static u8               g_song_len = 0;
static bool             g_mw_connected = FALSE;

static uint16_t cmd_buf[MW_BUFLEN / 2];

/* ── Helpers ─────────────────────────────────────────────────────────────── */

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
    const char h[] = "0123456789ABCDEF";
    buf[0] = h[(val >> 4) & 0xF];
    buf[1] = h[val & 0xF];
}

/* ── Tile generation ─────────────────────────────────────────────────────── */

static void make_solid(u32 *t, u8 c)
{
    u32 r = 0; u8 i;
    for (i = 0; i < 8; i++) r |= ((u32)c << (28 - i * 4));
    for (i = 0; i < 8; i++) t[i] = r;
}

static void make_partial(u32 *t, u8 c, u8 lines)
{
    u32 r = 0; u8 i;
    for (i = 0; i < 8; i++) r |= ((u32)c << (28 - i * 4));
    for (i = 0; i < 8; i++) t[i] = (i >= (8 - lines)) ? r : 0;
}

static void load_vu_tiles(void)
{
    u32 t[8]; u8 n;

    make_solid(t, 0); VDP_loadTileData(t, T_BLACK, 1, CPU);
    make_solid(t, 1); VDP_loadTileData(t, T_GREEN, 1, CPU);
    make_solid(t, 2); VDP_loadTileData(t, T_YELLOW, 1, CPU);
    make_solid(t, 3); VDP_loadTileData(t, T_RED, 1, CPU);
    make_solid(t, 5); VDP_loadTileData(t, T_BLUE, 1, CPU);

    { u32 line = 0; u8 i;
      for (i = 0; i < 8; i++) line |= ((u32)4 << (28 - i * 4));
      for (i = 0; i < 8; i++) t[i] = 0;
      t[3] = line; t[4] = line;
      VDP_loadTileData(t, T_PEAK, 1, CPU);
    }

    for (n = 0; n < 8; n++) {
        make_partial(t, 1, n+1); VDP_loadTileData(t, T_GREEN_P(n), 1, CPU);
        make_partial(t, 2, n+1); VDP_loadTileData(t, T_YELLOW_P(n), 1, CPU);
        make_partial(t, 3, n+1); VDP_loadTileData(t, T_RED_P(n), 1, CPU);
        make_partial(t, 5, n+1); VDP_loadTileData(t, T_BLUE_P(n), 1, CPU);
    }
}

static void setup_palettes(void)
{
    PAL_setColor(0, RGB24_TO_VDPCOLOR(0x000000));
    PAL_setColor(15, RGB24_TO_VDPCOLOR(0xFFFFFF));
    PAL_setColor(16, RGB24_TO_VDPCOLOR(0x000000));
    PAL_setColor(17, RGB24_TO_VDPCOLOR(0x00DD44));  /* green */
    PAL_setColor(18, RGB24_TO_VDPCOLOR(0xDDCC00));  /* yellow */
    PAL_setColor(19, RGB24_TO_VDPCOLOR(0xEE2222));  /* red */
    PAL_setColor(20, RGB24_TO_VDPCOLOR(0xFFFFFF));  /* peak */
    PAL_setColor(21, RGB24_TO_VDPCOLOR(0x2288EE));  /* blue */
    PAL_setColor(31, RGB24_TO_VDPCOLOR(0x00FF66));
    PAL_setColor(32, RGB24_TO_VDPCOLOR(0x000000));
    PAL_setColor(47, RGB24_TO_VDPCOLOR(0xFF4444));
    PAL_setColor(48, RGB24_TO_VDPCOLOR(0x000000));
    PAL_setColor(63, RGB24_TO_VDPCOLOR(0x00CCFF));
}

/* ── VU bar: full redraw, per-pixel edge ─────────────────────────────────── */

static void vu_fill_row(u16 col, u16 row, u16 tile, u8 width)
{
    u8 c;
    for (c = 0; c < width; c++)
        VDP_setTileMapXY(BG_B, tile, col + c, row);
}

static void draw_vu_bar(u8 ch)
{
    u16 col = VU_CH_OFFSET + ch * (VU_CH_WIDTH + VU_CH_GAP);
    u8 h = g_vu_px[ch];  /* 0-127 pixel height */
    if (h > VU_TOTAL_PX) h = VU_TOTAL_PX;

    u8 full_rows = h >> 3;        /* h / 8 */
    u8 partial   = h & 7;         /* h % 8 */
    u8 peak_h    = g_vu_peak[ch];
    u8 peak_row  = peak_h >> 3;
    u16 r;

    for (r = 0; r < VU_NUM_ROWS; r++) {
        u16 screen_row = ROW_VU_END - r;
        u16 tile;

        if (r < full_rows) {
            /* Full bar row — green/yellow/red by height */
            u8 pix = r * 8 + 4;
            if (pix >= VU_TOTAL_PX * 4 / 5)
                tile = TILE_ATTR_FULL(PAL1, FALSE, FALSE, FALSE, T_RED);
            else if (pix >= VU_TOTAL_PX * 3 / 5)
                tile = TILE_ATTR_FULL(PAL1, FALSE, FALSE, FALSE, T_YELLOW);
            else
                tile = TILE_ATTR_FULL(PAL1, FALSE, FALSE, FALSE, T_GREEN);
        } else if (r == full_rows && partial > 0) {
            /* Edge row — partial fill tile */
            u8 pix = r * 8 + partial / 2;
            u16 base;
            if (pix >= VU_TOTAL_PX * 4 / 5)
                base = T_RED_P(partial - 1);
            else if (pix >= VU_TOTAL_PX * 3 / 5)
                base = T_YELLOW_P(partial - 1);
            else
                base = T_GREEN_P(partial - 1);
            tile = TILE_ATTR_FULL(PAL1, FALSE, FALSE, FALSE, base);
        } else if (r == peak_row && peak_h > h && peak_h > 0) {
            /* Peak hold line */
            tile = TILE_ATTR_FULL(PAL1, FALSE, FALSE, FALSE, T_PEAK);
        } else {
            /* Empty */
            tile = TILE_ATTR_FULL(PAL1, FALSE, FALSE, FALSE, T_BLACK);
        }

        vu_fill_row(col, screen_row, tile, VU_CH_WIDTH);
    }
}

static void update_vu_meters(void)
{
    u8 ch;
    for (ch = 0; ch < 4; ch++) {
        u8 raw = g_vu_raw[ch];
        u8 cur = g_vu_px[ch];

        /*
         * Ballistic IIR filter:
         *   Attack: fast rise — chase the signal in ~3 frames
         *   Decay:  slow fall — smooth drop over ~12 frames
         */
        if (raw > cur) {
            /* Fast attack: jump 3/4 of the way there */
            cur += ((raw - cur) * 3 + 2) >> 2;
        } else if (cur > 0) {
            /* Slow decay: fall by 1/8 of current + 1 */
            u8 drop = (cur >> 3) + 1;
            if (drop > cur) cur = 0;
            else cur -= drop;
            if (cur < raw) cur = raw;
        }
        g_vu_px[ch] = cur;

        /* Peak hold — snap up, hold briefly, then smooth fall */
        if (cur > g_vu_peak[ch]) {
            g_vu_peak[ch] = cur;
            g_vu_decay[ch] = 20;    /* hold for ~0.3 sec */
        } else if (g_vu_decay[ch] > 0) {
            g_vu_decay[ch]--;
        } else if (g_vu_peak[ch] > 0) {
            /* Smooth gravity fall — accelerates as it drops */
            u8 drop = (g_vu_peak[ch] >> 4) + 1;
            g_vu_peak[ch] = (drop >= g_vu_peak[ch]) ? 0 : g_vu_peak[ch] - drop;
        }

        draw_vu_bar(ch);
    }
}

/* ── Volume bar ──────────────────────────────────────────────────────────── */

static void draw_volume_bar(void)
{
    u8 filled = (g_master_vol * VOL_BAR_WIDTH + 127) / 255;
    u8 c;
    for (c = 0; c < VOL_BAR_WIDTH; c++) {
        u16 tile = (c < filled)
            ? TILE_ATTR_FULL(PAL1, FALSE, FALSE, FALSE, T_BLUE)
            : TILE_ATTR_FULL(PAL1, FALSE, FALSE, FALSE, T_BLACK);
        VDP_setTileMapXY(BG_B, tile, VOL_BAR_OFFSET + c, ROW_VOL_BAR);
    }
}

static void draw_volume_text(void)
{
    char buf[8];
    char *p = buf;
    u8 pct = (g_master_vol * 100 + 127) / 255;
    p = itoa_simple(pct, p);
    *p++ = '%'; *p++ = ' '; *p++ = ' '; *p = '\0';
    VDP_setTextPalette(PAL3);
    VDP_drawText(buf, VOL_BAR_OFFSET + VOL_BAR_WIDTH + 1, ROW_VOL_LABEL);
    VDP_setTextPalette(PAL0);
}

/* ── Static UI ───────────────────────────────────────────────────────────── */

static void draw_static_ui(void)
{
    u8 ch;
    char label[41];

    VDP_setTextPalette(PAL3);
    VDP_drawText("====== MEGAWIFI MOD PLAYER ======", 3, ROW_TITLE);

    memset(label, ' ', 40); label[40] = '\0';
    for (ch = 0; ch < 4; ch++) {
        u16 col = VU_CH_OFFSET + ch * (VU_CH_WIDTH + VU_CH_GAP);
        u16 lbl = col + (VU_CH_WIDTH - 3) / 2;
        label[lbl]   = 'C';
        label[lbl+1] = 'H';
        label[lbl+2] = '1' + ch;
    }
    VDP_drawText(label, 0, ROW_VU_LABEL);

    VDP_drawText("  Vol:", 0, ROW_VOL_LABEL);
    VDP_drawText(" [A]Play [B]Stop [C]Pause [^v]Vol  ", 2, ROW_HELP);
    VDP_drawText("================================", 3, ROW_FOOTER);
    VDP_setTextPalette(PAL0);
}

/* ── Status line ─────────────────────────────────────────────────────────── */

static void draw_status(void)
{
    char buf[41];
    u16 pal;
    char *p;

    memset(buf, ' ', 40); buf[40] = '\0';
    switch (g_state) {
        case PS_PLAYING: memcpy(buf+2, "> PLAYING", 9); pal = PAL1; break;
        case PS_PAUSED:  memcpy(buf+2, "| PAUSED", 8);  pal = PAL3; break;
        default:         memcpy(buf+2, ". STOPPED", 9);  pal = PAL2; break;
    }
    p = buf + 15;
    memcpy(p, "Pat ", 4); p += 4;
    hex_byte(g_pattern, p); p += 2;
    *p++ = '/';
    hex_byte(g_song_len, p); p += 2;
    memcpy(p, "  Row ", 6); p += 6;
    hex_byte(g_row, p); p += 2;
    memcpy(p, "/3F", 3);

    VDP_setTextPalette(pal);
    VDP_drawText(buf, 0, ROW_STATUS);
    VDP_setTextPalette(PAL0);
}

/* ── Poll — unpack packed 32-bit words ───────────────────────────────────── */

static void poll_status(void)
{
    uint32_t vu_word, pos_word;

    if (!g_mw_connected) return;

    if (mw_aud_status(&vu_word, &pos_word) == MW_ERR_NONE) {
        /* Word 0: VU + state */
        g_state      = (vu_word >> 30) & 0x03;
        g_vu_raw[3]  = (vu_word >> 23) & 0x7F;
        g_vu_raw[2]  = (vu_word >> 16) & 0x7F;
        g_vu_raw[1]  = (vu_word >> 9)  & 0x7F;
        g_vu_raw[0]  = (vu_word >> 2)  & 0x7F;

        /* Word 1: position */
        g_pattern  = (pos_word >> 24) & 0xFF;
        g_row      = (pos_word >> 16) & 0xFF;
        g_song_len = (pos_word >> 8)  & 0xFF;
        /* master_vol = pos_word & 0xFF — we track locally */
    }
}

/* ── Marquee ─────────────────────────────────────────────────────────────── */

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
    u16 ti = TILE_FONT_INDEX + (ch - 32);
    VDP_setTileMapXY(BG_A,
        TILE_ATTR_FULL(PAL3, TRUE, FALSE, FALSE, ti),
        tile_col, ROW_MARQUEE);
}

static void marquee_init(void)
{
    u16 c;
    for (c = 0; c < 64; c++) marquee_write_tile(c, c);
    marquee_str_pos = 64;
    marquee_scroll_x = 0;
    marquee_needs_init = FALSE;
}

static void update_marquee(void)
{
    s16 neg;
    if (marquee_needs_init) marquee_init();
    marquee_scroll_x = (marquee_scroll_x + 1) & 0x1FF;
    neg = -(s16)marquee_scroll_x;
    VDP_setHorizontalScrollTile(BG_A, ROW_MARQUEE, &neg, 1, CPU);
    if ((marquee_scroll_x & 7) == 0) {
        u16 tc = (u16)((marquee_scroll_x / 8 + 63) % 64);
        marquee_write_tile(tc, marquee_str_pos);
        marquee_str_pos++;
    }
}

static void frame_draw_hook(void) { update_marquee(); }

/* ── Input ───────────────────────────────────────────────────────────────── */

static void handle_input(void)
{
    u16 joy, pressed;
    static u16 prev_joy = 0;

    JOY_update();
    joy = JOY_readJoypad(JOY_1);
    pressed = joy & ~prev_joy;
    prev_joy = joy;
    if (!g_mw_connected) return;

    if (pressed & BUTTON_A) mw_aud_play();
    if (pressed & BUTTON_B) mw_aud_stop();
    if (pressed & BUTTON_C) {
        if (g_state == PS_PLAYING) mw_aud_pause();
        else if (g_state == PS_PAUSED) mw_aud_resume();
    }
    if (pressed & BUTTON_UP) {
        g_master_vol = (g_master_vol <= 245) ? g_master_vol + 10 : 255;
        mw_aud_set_vol(g_master_vol);
        draw_volume_bar(); draw_volume_text();
    }
    if (pressed & BUTTON_DOWN) {
        g_master_vol = (g_master_vol >= 10) ? g_master_vol - 10 : 0;
        mw_aud_set_vol(g_master_vol);
        draw_volume_bar(); draw_volume_text();
    }
}

/* ── MegaWifi init ───────────────────────────────────────────────────────── */

static void user_tsk(void) { while (1) mw_process(); }

static bool megawifi_init(void)
{
    uint8_t fw_major, fw_minor;
    char *variant = NULL;
    if (mw_init(cmd_buf, MW_BUFLEN) != MW_ERR_NONE) return FALSE;
    TSK_userSet(user_tsk);
    if (mw_detect(&fw_major, &fw_minor, &variant) != MW_ERR_NONE) return FALSE;
    {
        char buf[41]; char *p;
        memset(buf, ' ', 40); buf[40] = '\0';
        p = buf + 2;
        memcpy(p, "FW: v", 5); p += 5;
        p = itoa_simple(fw_major, p); *p++ = '.';
        p = itoa_simple(fw_minor, p);
        if (variant) { *p++ = '-'; strcpy(p, variant); }
        VDP_drawText(buf, 0, ROW_TRACK);
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
    load_vu_tiles();
    draw_static_ui();
    draw_status();
    draw_volume_bar();
    draw_volume_text();

    JOY_init();

    VDP_setTextPalette(PAL3);
    VDP_drawText("  Connecting...                 ", 0, ROW_TRACK);
    VDP_setTextPalette(PAL0);

    g_mw_connected = megawifi_init();
    if (!g_mw_connected) {
        VDP_setTextPalette(PAL2);
        VDP_drawText("  MegaWifi not found            ", 0, ROW_TRACK);
        VDP_setTextPalette(PAL0);
    }

    mw_set_draw_hook(frame_draw_hook);

    while (1) {
        VDP_waitVSync();
        update_marquee();
        handle_input();
        poll_status();
        draw_status();
        update_vu_meters();
    }

    return 0;
}
