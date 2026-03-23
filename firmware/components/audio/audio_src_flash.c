/**
 * @file audio_src_flash.c
 * @brief MegaWifi Audio Subsystem
 *
 * Copyright (c) 2026 Mike Wolak <mikewolak@gmail.com>
 * All rights reserved.
 *
 * Part of the MegaWifi MOD Player project.
 * https://github.com/mikewolak/MegaWifiModPlayer
 */
/**
 * audio_src_flash.c — audio source backed by embedded flash data
 */
#include "audio_src_flash.h"
#include <string.h>

typedef struct {
    const uint8_t *data;
    size_t         len;
    size_t         pos;
} flash_ctx_t;

static flash_ctx_t s_flash_ctx;

static int flash_read(void *ctx, uint8_t *buf, int len)
{
    flash_ctx_t *c = (flash_ctx_t *)ctx;
    if (c->pos >= c->len) return 0;
    int n = (int)(c->len - c->pos);
    if (n > len) n = len;
    memcpy(buf, c->data + c->pos, n);
    c->pos += n;
    return n;
}

static int flash_seek(void *ctx, uint32_t offset)
{
    flash_ctx_t *c = (flash_ctx_t *)ctx;
    if (offset > c->len) return -1;
    c->pos = offset;
    return 0;
}

void audio_src_flash_init(audio_src_t *src, const uint8_t *data, size_t len)
{
    s_flash_ctx.data = data;
    s_flash_ctx.len  = len;
    s_flash_ctx.pos  = 0;
    src->read = flash_read;
    src->seek = flash_seek;
    src->ctx  = &s_flash_ctx;
}
