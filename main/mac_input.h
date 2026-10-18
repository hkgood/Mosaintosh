/*
 * mac_input.h - the touch panel as the Mac's one-button mouse.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* False if there is no digitiser; the Mac then simply runs without a mouse. */
bool mac_input_init(void);

/*
 * Reads the digitiser and moves the Mac's cursor to the finger. Call between
 * umac_loop() calls, about once per vsync; `ram` is the Mac's RAM.
 */
void mac_input_poll(uint8_t *ram);

/* The same, with the touch state given in panel coordinates (for tests and
 * other input sources). */
void mac_input_feed(uint8_t *ram, bool down, int x, int y);
