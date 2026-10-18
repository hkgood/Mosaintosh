/*
 * mac_disk.h - the Mac's startup disk, from a flash partition.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "disc.h"

/* Flash partition holding a raw, bootable HFS volume. */
#define MAC_DISK_PARTITION "macdisk"

/*
 * Maps the partition and fills in `out` for umac_init(). Flash cannot take
 * the Mac's writes, so they go to a copy-on-write overlay of up to
 * `overlay_bytes` in PSRAM: the Mac sees a writable disk, and every reset
 * starts again from the image in flash.
 */
bool mac_disk_open(disc_descr_t *out, size_t overlay_bytes);
