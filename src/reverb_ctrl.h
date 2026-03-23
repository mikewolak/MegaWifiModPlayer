/**
 * @file reverb_ctrl.h
 * @brief Reverb control popup — joypad-navigable parameter editor
 *
 * Copyright (c) 2026 Mike Wolak <mikewolak@gmail.com>
 * All rights reserved.
 */
#ifndef REVERB_CTRL_H
#define REVERB_CTRL_H

#include <genesis.h>

typedef void (*rc_redraw_fn)(void);

/** Call once at startup. redraw_fn is called when popup closes. */
void reverb_ctrl_init(rc_redraw_fn redraw_fn);

/** TRUE while the popup is visible. */
bool reverb_ctrl_active(void);

/** Open the reverb control popup. */
void reverb_ctrl_open(void);

/** Call once per frame with freshly-pressed buttons. No-op if not active. */
void reverb_ctrl_frame(u16 press);

/** Returns TRUE if reverb is currently enabled. */
bool reverb_ctrl_is_enabled(void);

/** Send default reverb settings to firmware. Call after MegaWifi connects. */
void reverb_ctrl_send_defaults(void);

#endif /* REVERB_CTRL_H */
