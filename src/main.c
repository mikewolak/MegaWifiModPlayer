/*
 * main.c — MegaWifi MOD Player for Sega Genesis
 *
 * VU meters use SGDK's low-level sprite API (VDP_setSpriteFull).
 * SAT buffer updated in RAM, flushed via VDP_updateSprites.
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

#define VU_NUM_ROWS     16
#define VU_TOTAL_PX     128
#define VU_BOTTOM_Y     (ROW_VU_END * 8 + 7)

/* Channel X positions and width */
#define VU_CH_PX_WIDTH  32
#define VU_CH_X(ch)     (24 + (ch) * 72)

#define VOL_BAR_OFFSET  7
#define VOL_BAR_WIDTH   26

/* Tiles */
#define T_BLACK         (TILE_USER_INDEX + 0)
#define T_BLUE          (TILE_USER_INDEX + 1)
#define T_SPR_GREEN     (TILE_USER_INDEX + 2)   /* 4 tiles for 32px wide sprite */
#define T_SPR_YELLOW    (TILE_USER_INDEX + 6)
#define T_SPR_RED       (TILE_USER_INDEX + 10)
#define T_SPR_WHITE     (TILE_USER_INDEX + 14)

/* Sprite layout: 4 channels × 16 rows + 4 peaks = 68 sprites */
#define VU_SPR_PER_CH   16
#define VU_SPR_TOTAL    (4 * (VU_SPR_PER_CH + 1))  /* 68 */

/* ── State ───────────────────────────────────────────────────────────────── */
#define PS_STOPPED  0
#define PS_PLAYING  1
#define PS_PAUSED   2

static u8  g_state = PS_STOPPED;
static u8  g_master_vol = 255;
static u8  g_vu_raw[4] = {0};
static u8  g_vu_px[4] = {0};
static u8  g_vu_peak[4] = {0};
static u8  g_vu_decay[4] = {0};
static u8  g_pattern = 0;
static u8  g_row = 0;
static u8  g_song_len = 0;
static bool g_mw_connected = FALSE;

static uint16_t cmd_buf[MW_BUFLEN / 2];

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static char *itoa_simple(u32 val, char *buf)
{
    char tmp[12]; int i = 0;
    if (val == 0) { *buf++ = '0'; *buf = '\0'; return buf; }
    while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
    while (i > 0) *buf++ = tmp[--i];
    *buf = '\0'; return buf;
}

static void hex_byte(u8 val, char *buf)
{
    const char h[] = "0123456789ABCDEF";
    buf[0] = h[(val >> 4) & 0xF]; buf[1] = h[val & 0xF];
}

/* ── Tiles + palettes ────────────────────────────────────────────────────── */

static void make_solid(u32 *t, u8 c)
{
    u32 r = 0; u8 i;
    for (i = 0; i < 8; i++) r |= ((u32)c << (28 - i * 4));
    for (i = 0; i < 8; i++) t[i] = r;
}

static void load_tiles(void)
{
    u32 t[8]; u8 i;
    make_solid(t, 0); VDP_loadTileData(t, T_BLACK, 1, CPU);
    make_solid(t, 5); VDP_loadTileData(t, T_BLUE, 1, CPU);
    make_solid(t, 1);
    for (i = 0; i < 4; i++) VDP_loadTileData(t, T_SPR_GREEN + i, 1, CPU);
    make_solid(t, 2);
    for (i = 0; i < 4; i++) VDP_loadTileData(t, T_SPR_YELLOW + i, 1, CPU);
    make_solid(t, 3);
    for (i = 0; i < 4; i++) VDP_loadTileData(t, T_SPR_RED + i, 1, CPU);
    make_solid(t, 4);
    for (i = 0; i < 4; i++) VDP_loadTileData(t, T_SPR_WHITE + i, 1, CPU);
}

static void setup_palettes(void)
{
    PAL_setColor(0,  RGB24_TO_VDPCOLOR(0x000000));
    PAL_setColor(15, RGB24_TO_VDPCOLOR(0xFFFFFF));
    PAL_setColor(16, RGB24_TO_VDPCOLOR(0x000000));
    PAL_setColor(17, RGB24_TO_VDPCOLOR(0x00DD44));  /* 1: green */
    PAL_setColor(18, RGB24_TO_VDPCOLOR(0xDDCC00));  /* 2: yellow */
    PAL_setColor(19, RGB24_TO_VDPCOLOR(0xEE2222));  /* 3: red */
    PAL_setColor(20, RGB24_TO_VDPCOLOR(0xFFFFFF));  /* 4: white */
    PAL_setColor(21, RGB24_TO_VDPCOLOR(0x2288EE));  /* 5: blue */
    PAL_setColor(31, RGB24_TO_VDPCOLOR(0x00FF66));
    PAL_setColor(32, RGB24_TO_VDPCOLOR(0x000000));
    PAL_setColor(47, RGB24_TO_VDPCOLOR(0xFF4444));
    PAL_setColor(48, RGB24_TO_VDPCOLOR(0x000000));
    PAL_setColor(63, RGB24_TO_VDPCOLOR(0x00CCFF));
}

/* ── Init sprites ────────────────────────────────────────────────────────── */

static void init_vu_sprites(void)
{
    u8 ch, r, idx;

    idx = 0;
    for (ch = 0; ch < 4; ch++) {
        s16 x = VU_CH_X(ch);

        for (r = 0; r < VU_SPR_PER_CH; r++) {
            u16 tile;
            if (r >= VU_SPR_PER_CH * 4 / 5)
                tile = T_SPR_RED;
            else if (r >= VU_SPR_PER_CH * 3 / 5)
                tile = T_SPR_YELLOW;
            else
                tile = T_SPR_GREEN;

            VDP_setSpriteFull(idx, x, -32,
                SPRITE_SIZE(4, 1),
                TILE_ATTR_FULL(PAL1, FALSE, FALSE, FALSE, tile),
                idx + 1);
            idx++;
        }

        /* Peak sprite */
        VDP_setSpriteFull(idx, x, -32,
            SPRITE_SIZE(4, 1),
            TILE_ATTR_FULL(PAL1, FALSE, FALSE, FALSE, T_SPR_WHITE),
            idx + 1);
        idx++;
    }

    /* Terminate link chain — last sprite links to 0 */
    VDP_setSpriteFull(idx - 1,
        VU_CH_X(3), -32,
        SPRITE_SIZE(4, 1),
        TILE_ATTR_FULL(PAL1, FALSE, FALSE, FALSE, T_SPR_WHITE),
        0);

    VDP_updateSprites(VU_SPR_TOTAL, CPU);
}

/* ── Update sprite Y positions ───────────────────────────────────────────── */

static void update_vu_sprites(void)
{
    u8 ch, r, idx;

    for (ch = 0; ch < 4; ch++) {
        u8 raw = g_vu_raw[ch];
        u8 cur = g_vu_px[ch];
        u8 base = ch * (VU_SPR_PER_CH + 1);

        if (raw > cur) {
            cur += ((raw - cur) * 3 + 2) >> 2;
        } else if (cur > 0) {
            u8 drop = (cur >> 3) + 1;
            if (drop > cur) cur = 0;
            else cur -= drop;
            if (cur < raw) cur = raw;
        }
        g_vu_px[ch] = cur;

        {
            u8 bars = (cur + 7) >> 3;
            for (r = 0; r < VU_SPR_PER_CH; r++) {
                s16 y;
                if (r < bars)
                    y = VU_BOTTOM_Y - (r + 1) * 8 + 1;
                else
                    y = -32;
                VDP_setSpritePosition(base + r, VU_CH_X(ch), y);
            }
        }

        /* Peak */
        if (cur > g_vu_peak[ch]) {
            g_vu_peak[ch] = cur;
            g_vu_decay[ch] = 20;
        } else if (g_vu_decay[ch] > 0) {
            g_vu_decay[ch]--;
        } else if (g_vu_peak[ch] > 0) {
            u8 drop = (g_vu_peak[ch] >> 4) + 1;
            g_vu_peak[ch] = (drop >= g_vu_peak[ch]) ? 0 : g_vu_peak[ch] - drop;
        }

        {
            u8 pi = base + VU_SPR_PER_CH;
            if (g_vu_peak[ch] > cur && g_vu_peak[ch] > 0)
                VDP_setSpritePosition(pi, VU_CH_X(ch), VU_BOTTOM_Y - g_vu_peak[ch]);
            else
                VDP_setSpritePosition(pi, VU_CH_X(ch), -32);
        }
    }

    VDP_updateSprites(VU_SPR_TOTAL, CPU);
}

/* ── Volume bar ──────────────────────────────────────────────────────────── */

static void draw_volume_bar(void)
{
    u8 filled = (g_master_vol * VOL_BAR_WIDTH + 127) / 255;
    u8 c;
    for (c = 0; c < VOL_BAR_WIDTH; c++) {
        u16 tile = (c < filled)
            ? TILE_ATTR_FULL(PAL1, TRUE, FALSE, FALSE, T_BLUE)
            : TILE_ATTR_FULL(PAL1, TRUE, FALSE, FALSE, T_BLACK);
        VDP_setTileMapXY(BG_A, tile, VOL_BAR_OFFSET + c, ROW_VOL_BAR);
    }
}

static void draw_volume_text(void)
{
    char buf[8]; char *p = buf;
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
    u8 ch; char label[41];

    VDP_setTextPalette(PAL3);
    VDP_drawText("====== MEGAWIFI MOD PLAYER ======", 3, ROW_TITLE);

    memset(label, ' ', 40); label[40] = '\0';
    for (ch = 0; ch < 4; ch++) {
        u16 col = VU_CH_X(ch) / 8;
        u16 lbl = col + (VU_CH_PX_WIDTH / 8 - 3) / 2;
        label[lbl] = 'C'; label[lbl+1] = 'H'; label[lbl+2] = '1' + ch;
    }
    VDP_drawText(label, 0, ROW_VU_LABEL);
    VDP_drawText("  Vol:", 0, ROW_VOL_LABEL);
    VDP_drawText(" [A]Play [B]Stop [C]Pause [^v]Vol  ", 2, ROW_HELP);
    VDP_drawText("================================", 3, ROW_FOOTER);
    VDP_setTextPalette(PAL0);
}

/* ── Status ──────────────────────────────────────────────────────────────── */

static u8 s_prev_state = 0xFF, s_prev_pat = 0xFF, s_prev_row = 0xFF;

static void draw_status(void)
{
    char buf[41]; u16 pal; char *p;
    if (g_state == s_prev_state && g_pattern == s_prev_pat && g_row == s_prev_row) return;
    s_prev_state = g_state; s_prev_pat = g_pattern; s_prev_row = g_row;

    memset(buf, ' ', 40); buf[40] = '\0';
    switch (g_state) {
        case PS_PLAYING: memcpy(buf+2, "> PLAYING", 9); pal = PAL1; break;
        case PS_PAUSED:  memcpy(buf+2, "| PAUSED", 8);  pal = PAL3; break;
        default:         memcpy(buf+2, ". STOPPED", 9);  pal = PAL2; break;
    }
    p = buf + 15;
    memcpy(p, "Pat ", 4); p += 4;
    hex_byte(g_pattern, p); p += 2; *p++ = '/';
    hex_byte(g_song_len, p); p += 2;
    memcpy(p, "  Row ", 6); p += 6;
    hex_byte(g_row, p); p += 2; memcpy(p, "/3F", 3);

    VDP_setTextPalette(pal);
    VDP_drawText(buf, 0, ROW_STATUS);
    VDP_setTextPalette(PAL0);
}

/* ── Poll ────────────────────────────────────────────────────────────────── */

static void poll_status(void)
{
    uint32_t vu_word, pos_word;
    if (!g_mw_connected) return;
    if (mw_aud_status(&vu_word, &pos_word) == MW_ERR_NONE) {
        g_state     = (vu_word >> 30) & 0x03;
        g_vu_raw[3] = (vu_word >> 23) & 0x7F;
        g_vu_raw[2] = (vu_word >> 16) & 0x7F;
        g_vu_raw[1] = (vu_word >> 9)  & 0x7F;
        g_vu_raw[0] = (vu_word >> 2)  & 0x7F;
        g_pattern   = (pos_word >> 24) & 0xFF;
        g_row       = (pos_word >> 16) & 0xFF;
        g_song_len  = (pos_word >> 8)  & 0xFF;
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

static void marquee_write_tile(u16 tc, u16 sp)
{
    u16 sl = (u16)strlen(marquee_text);
    u8 ch = marquee_text[sp % sl];
    VDP_setTileMapXY(BG_A,
        TILE_ATTR_FULL(PAL3, TRUE, FALSE, FALSE, TILE_FONT_INDEX + (ch - 32)),
        tc, ROW_MARQUEE);
}

static void marquee_init(void)
{
    u16 c;
    for (c = 0; c < 64; c++) marquee_write_tile(c, c);
    marquee_str_pos = 64; marquee_scroll_x = 0;
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

/* Draw hook */
static void frame_draw_hook(void)
{
    static u32 lf = 0;
    u32 f = vtimer;
    if (f == lf) return;
    lf = f;
    update_marquee();
    update_vu_sprites();
}

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

/* ── MegaWifi ────────────────────────────────────────────────────────────── */

static void user_tsk(void) { while (1) mw_process(); }

static bool megawifi_init(void)
{
    uint8_t fw_major, fw_minor; char *variant = NULL;
    if (mw_init(cmd_buf, MW_BUFLEN) != MW_ERR_NONE) return FALSE;
    TSK_userSet(user_tsk);
    if (mw_detect(&fw_major, &fw_minor, &variant) != MW_ERR_NONE) return FALSE;
    { char buf[41]; char *p;
      memset(buf, ' ', 40); buf[40] = '\0';
      p = buf + 2; memcpy(p, "FW: v", 5); p += 5;
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
    load_tiles();
    draw_static_ui();
    draw_status();
    draw_volume_bar();
    draw_volume_text();
    init_vu_sprites();

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
        draw_status();
        update_vu_sprites();
        handle_input();
        poll_status();
    }

    return 0;
}
