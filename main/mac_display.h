/*
 * mac_display.h - the Mac's 1-bit screen, framed, on the 480x480 CO5300 panel.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PANEL_W 480
#define PANEL_H 480

/*
 * The Mac's screen size comes from the umac build (DISP_WIDTH/DISP_HEIGHT).
 * It sits centred in the panel, pixel for pixel, with the frame (mac_frame.h)
 * around it. 416x416 is the largest square that stays clear of the panel's
 * rounded corners (about 48 px; the safe area is 12 px in with 64 px corners).
 */
#define MAC_SCREEN_W DISP_WIDTH
#define MAC_SCREEN_H DISP_HEIGHT
#define MAC_SCREEN_STRIDE (MAC_SCREEN_W / 8)
#define MAC_SCREEN_X ((PANEL_W - MAC_SCREEN_W) / 2)
#define MAC_SCREEN_Y ((PANEL_H - MAC_SCREEN_H) / 2)

/* CO5300 windows: x on multiples of 4, y on multiples of 2. */
_Static_assert(MAC_SCREEN_X % 4 == 0 && MAC_SCREEN_W % 4 == 0, "screen breaks CO5300 x alignment");
_Static_assert(MAC_SCREEN_Y % 2 == 0 && MAC_SCREEN_H % 2 == 0, "screen breaks CO5300 y alignment");

/* Draws the frame and blanks the screen area. */
bool mac_display_init(void);

/*
 * Sends the rows of `fb` (1 bpp, MSB first, 1 = black: the Mac's own layout)
 * that changed since the last call. The first call sends everything.
 * Returns the number of rows sent, or -1 if the panel stopped answering.
 */
int mac_display_update(const uint8_t *fb);
