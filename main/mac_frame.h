/*
 * mac_frame.h - the monitor around the Mac's screen.
 *
 * An Apple II-style monitor bezel: a beige case, a dark recess bevelled into
 * it around the tube, and along the bottom a rainbow badge, the Mosaintosh
 * name and a power light. It is computed, not stored, and drawn once.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdint.h>

/*
 * Panel rows [y0, y0 + h), all PANEL_W pixels each, into `dst` in the
 * panel's wire order (RGB565, high byte first). The screen, a rectangle
 * with rounded corners, is black, as a tube that is off.
 */
void mac_frame_rows(uint16_t *dst, int y0, int h);

/*
 * Paints the frame over the ends of Mac screen row `y` that lie outside the
 * screen's rounded corners. `row` is that row's MAC_SCREEN_W pixels, in wire
 * order. The corners have the shape System 7 gives its desktop, so once the
 * Finder is up this only hides pixels the Mac has painted black itself;
 * before then, it is what keeps the screen rounded.
 */
void mac_frame_round_corners(uint16_t *row, int y);
