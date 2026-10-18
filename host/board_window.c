/*
 * board_window.c - Mosaintosh on the simulated Mosaico board, in a window.
 * The mouse is the finger: press, drag and let go as on the touch panel.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>

#include "board.h"

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s rom.bin disk.img\n", argv[0]);
        return 2;
    }
    const board_config_t cfg = {
        .clock = BOARD_CLOCK_REAL,
        .echo_info = true,
        .partitions = {
            { .label = "macrom", .subtype = 0x41, .path = argv[1], .size = 0x20000 },
            { .label = "macdisk", .subtype = 0x42, .path = argv[2], .size = 0x9e0000 },
        },
        .partition_count = 2,
    };
    board_start(&cfg);
    board_window_run("Mosaintosh on Mosaico (simulated)");
    return 0;
}
