/*
 * Mosaico board simulator.
 *
 * Firmware sources compile unchanged against the stand-in ESP-IDF headers in
 * sim/board/include and run on the host. The model keeps the parts of the
 * board that firmware gets wrong in practice:
 *
 *  - CO5300 panel, 480x480 RGB565. It starts showing a boot splash, as the
 *    Recovery bootloader leaves it, so firmware has to clear what it does not
 *    draw.
 *  - QSPI colour transfers are queued (depth 10, like the BSP's panel IO) and
 *    only read their source buffer when they complete, which happens when the
 *    application task blocks. Reusing a buffer before its completion callback
 *    shows up as wrong pixels.
 *  - draw_bitmap() rejects start >= end like the real driver; windows that are
 *    outside the panel or break the CO5300 alignment (x by 4, y by 2) are
 *    counted as violations.
 *  - The CST9220 touch driver only reports what esp_lcd_touch_read_data()
 *    latched, and each latched sample is consumed by one get_data().
 *  - Flash data partitions are host files; mapped ones are read-only.
 *  - app_main() runs on its own thread; stack use past the firmware's
 *    main-task size is reported (measured at every vTaskDelay()).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BOARD_PANEL_W 480
#define BOARD_PANEL_H 480

/* RGB565 colour the panel shows before the firmware draws anything. */
#define BOARD_SPLASH_COLOR 0x7befu

/*
 * The CO5300 takes RGB565 high byte first, and the ESP32-S31 (like the host)
 * is little-endian: a pixel in a draw_bitmap() buffer is the colour with its
 * bytes swapped. The panel and its snapshots hold buffer values; these turn
 * a colour into one and back. Black and white read the same either way.
 */
static inline uint16_t board_rgb565_to_wire(uint16_t rgb565)
{
    return (uint16_t)((rgb565 << 8) | (rgb565 >> 8));
}

static inline uint16_t board_wire_to_rgb565(uint16_t wire)
{
    return board_rgb565_to_wire(wire);
}

typedef enum {
    /* Time only advances when the application blocks: deterministic and as
     * fast as the host allows. For tests. */
    BOARD_CLOCK_VIRTUAL,
    /* Blocking really sleeps. For the interactive window. */
    BOARD_CLOCK_REAL,
} board_clock_t;

/* Runs on the application thread every time it calls vTaskDelay(), after all
 * queued panel transfers have completed. One firmware loop iteration is one
 * call, so this is where a test looks at the panel and moves the finger. */
typedef void (*board_frame_hook_t)(uint32_t frame, void *ctx);

/* A flash data partition, backed by a host file. A file shorter than `size`
 * reads as erased flash (0xff) past its end; size 0 means the file's size. */
typedef struct {
    const char *label;
    uint8_t subtype;
    const char *path;
    uint32_t size;
} board_partition_t;

#define BOARD_MAX_PARTITIONS 8

typedef struct {
    board_clock_t clock;
    /* Data partitions the firmware can find; the table is copied. */
    board_partition_t partitions[BOARD_MAX_PARTITIONS];
    int partition_count;
    board_frame_hook_t frame_hook;
    void *hook_ctx;
    /* Stop echoing warnings and errors after this many (they are still
     * counted). 0 echoes everything. */
    uint32_t max_echoed_problems;
    /* Echo info lines too. */
    bool echo_info;
} board_config_t;

typedef struct {
    uint32_t transfers;           /* draw_bitmap() calls accepted */
    uint64_t pixels;              /* pixels those transfers carried */
    uint32_t rejected_transfers;  /* draw_bitmap() calls the driver refused */
    uint32_t window_violations;   /* out of panel or misaligned */
    uint32_t warnings;            /* ESP_LOGW lines */
    uint32_t errors;              /* ESP_LOGE lines */
    uint32_t touch_reads;         /* esp_lcd_touch_read_data() calls */
    uint32_t frames;              /* vTaskDelay() calls */
    uint32_t stack_peak;          /* deepest app_main stack use seen, bytes */
} board_stats_t;

/* Firmware entry point. */
void app_main(void);

/* Starts app_main() on the application thread and returns. */
void board_start(const board_config_t *config);

/* Sets what the touch controller currently senses, in panel pixels. Safe to
 * call from any thread. */
void board_touch_set(bool pressed, int x, int y);

/* Copies the panel (BOARD_PANEL_W * BOARD_PANEL_H pixels, row-major, as the
 * firmware's buffers held them: see board_wire_to_rgb565()). Safe to call
 * from any thread. */
void board_panel_snapshot(uint16_t *dst);

board_stats_t board_stats(void);

/* True once app_main() has returned. Safe to call from any thread. */
bool board_app_returned(void);

/* Interactive window showing the panel; the mouse is the finger. Must be
 * called on the main thread after board_start(); does not return. macOS only. */
void board_window_run(const char *title);
