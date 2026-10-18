/*
 * mosaintosh.c - a Macintosh Plus on the ESP-Mosaico board.
 *
 * umac emulates the machine; this file feeds it: the ROM and the startup disk
 * come from flash partitions, the screen goes to the panel, the finger is the
 * mouse, and emulated time is held to real time so the Mac's clock, double-
 * click timing and animations behave.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mac_disk.h"
#include "mac_display.h"
#include "mac_input.h"
#include "rom.h"
#include "umac.h"

static const char *TAG = "mosaintosh";

#define MAC_ROM_PARTITION "macrom"

/* Emulated time per umac_loop() call (UMAC_EXECLOOP_QUANTUM in umac's
 * main.c, which does not export it). */
#define LOOP_US 5000
#define VSYNC_US 16667
#define SECOND_US 1000000

/* The panel is refreshed every other retrace: 30 Hz is smooth for a cursor
 * and halves the time spent on transfers. */
#define DISPLAY_EVERY_VSYNCS 2

/* Writes the Mac makes to its disk, kept until reset. */
#define DISK_OVERLAY_BYTES (2u * 1024 * 1024)

/* Running slower than real time, the task still yields this often so the
 * idle task (and its watchdog) get to run. */
#define YIELD_EVERY_US 100000
/* A longer stall is not caught up afterwards; the Mac just loses the time. */
#define MAX_LAG_US 250000

#define STATS_EVERY_US (10 * SECOND_US)

static uint8_t *load_rom(void)
{
    const esp_partition_t *p =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, MAC_ROM_PARTITION);
    if (p == NULL || p->size < ROM_SIZE) {
        ESP_LOGE(TAG, "no '%s' partition of at least %d KB", MAC_ROM_PARTITION, ROM_SIZE / 1024);
        return NULL;
    }
    /* Internal RAM if it fits: the 68000 fetches most instructions from ROM. */
    uint8_t *rom = heap_caps_malloc(ROM_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (rom == NULL) rom = heap_caps_malloc(ROM_SIZE, MALLOC_CAP_SPIRAM);
    if (rom == NULL || esp_partition_read(p, 0, rom, ROM_SIZE) != ESP_OK) {
        ESP_LOGE(TAG, "cannot load the ROM");
        heap_caps_free(rom);
        return NULL;
    }
    /* umac knows how to patch one ROM only (its disc driver hooks in there). */
    if (rom_patch(rom) != 0) {
        ESP_LOGE(TAG, "'%s' is not a Mac Plus v3 ROM", MAC_ROM_PARTITION);
        heap_caps_free(rom);
        return NULL;
    }
    return rom;
}

typedef struct {
    int64_t window_start;  /* real time */
    uint64_t window_emu;   /* emulated time at window_start */
    uint32_t rows_sent;
    int64_t display_us;
} stats_t;

static void report(stats_t *s, int64_t now, uint64_t emu)
{
    const int64_t real = now - s->window_start;
    if (real < STATS_EVERY_US) return;
    ESP_LOGI(TAG, "speed %.2fx real time; panel: %u rows/s, %.1f%% of the time",
             (double)(emu - s->window_emu) / (double)real,
             (unsigned)((uint64_t)s->rows_sent * SECOND_US / (uint64_t)real),
             100.0 * (double)s->display_us / (double)real);
    *s = (stats_t){ .window_start = now, .window_emu = emu };
}

void app_main(void)
{
    if (!mac_display_init()) return;
    mac_input_init();

    uint8_t *rom = load_rom();
    uint8_t *ram = heap_caps_calloc(1, RAM_SIZE, MALLOC_CAP_SPIRAM);
    disc_descr_t discs[DISC_NUM_DRIVES] = { 0 };
    if (rom == NULL || ram == NULL || !mac_disk_open(&discs[0], DISK_OVERLAY_BYTES)) {
        ESP_LOGE(TAG, "cannot start the Mac");
        return;
    }
    umac_init(ram, rom, discs);
    const uint8_t *fb = ram + umac_get_fb_offset();
    ESP_LOGI(TAG, "Mac Plus: %d KB RAM, %dx%d screen", RAM_SIZE / 1024, DISP_WIDTH, DISP_HEIGHT);

    const int64_t tick_us = SECOND_US / configTICK_RATE_HZ;
    int64_t start = esp_timer_get_time();
    int64_t last_yield = start;
    uint64_t emu = 0, next_vsync = VSYNC_US, next_second = SECOND_US;
    uint32_t vsyncs = 0;
    stats_t stats = { .window_start = start };

    for (;;) {
        umac_loop();
        emu += LOOP_US;

        if (emu >= next_second) {
            umac_1hz_event();
            next_second += SECOND_US;
        }
        if (emu < next_vsync) continue;
        next_vsync += VSYNC_US;
        umac_vsync_event();

        /* Between umac_loop() calls the 68000 is stopped, so its memory can
         * be read and written without tearing. */
        mac_input_poll(ram);
        if (++vsyncs % DISPLAY_EVERY_VSYNCS == 0) {
            const int64_t t0 = esp_timer_get_time();
            const int rows = mac_display_update(fb);
            if (rows < 0) return;
            stats.rows_sent += (uint32_t)rows;
            stats.display_us += esp_timer_get_time() - t0;
        }

        /* Hold emulated time to real time. */
        int64_t now = esp_timer_get_time();
        const int64_t ahead = (int64_t)emu - (now - start);
        if (ahead >= tick_us) {
            vTaskDelay((TickType_t)(ahead / tick_us));
            last_yield = esp_timer_get_time();
        } else {
            if (ahead < -MAX_LAG_US) start += -ahead - MAX_LAG_US;
            if (now - last_yield >= YIELD_EVERY_US) {
                vTaskDelay(1);
                last_yield = esp_timer_get_time();
            }
        }
        report(&stats, esp_timer_get_time(), emu);
    }
}
