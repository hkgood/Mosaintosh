/*
 * board_check.c - boot Mosaintosh on the simulated Mosaico board and use it.
 *
 * main/ is compiled unchanged against Mosaintosh's board simulator (fake
 * ESP-IDF, CO5300 panel, CST9220 touch, flash partitions from files). Time
 * is virtual, so a minute and a half of Mac time takes about a second.
 *
 * The script:
 *   1. boot to the Finder; tap the empty desktop;
 *   2. press on the Apple menu, slide to the first item, let go;
 *   3. wait for "About This Macintosh" to open.
 *
 * Pass means: whenever the firmware has just refreshed the panel, the panel
 * shows exactly the Mac's framebuffer, but for the screen's rounded corners,
 * which must cut off just what System 7 paints black; the frame is intact;
 * the Finder's menu bar is up; the
 * About window opened (the screen below the menu bar changed); and the
 * firmware logged no error or warning and never sent a bad window.
 * Screenshots of the panel go to build/check/.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "esp_timer.h"
#include "mac_display.h"
#include "mac_frame.h"
#include "umac.h"

/* Virtual times, in seconds of Mac time. */
#define T_PARK    72.0
#define T_DESKTOP 75.0
#define T_SLIDE   76.0
#define T_RELEASE 77.0
#define T_ABOUT   81.0
#define T_GIVE_UP 90.0

/* Panel coordinates. After booting the cursor rests on the Apple menu, so it
 * is first parked on the empty desktop: otherwise a click delivered before
 * the cursor moves would still hit the menu and go unnoticed. The Apple menu
 * is then pressed on the frame above it, which must reach the menu bar. */
#define PARK_X 240
#define PARK_Y 300
#define APPLE_X (MAC_SCREEN_X + 18)
#define APPLE_Y 14
#define ABOUT_X (MAC_SCREEN_X + 70)
#define ABOUT_Y (MAC_SCREEN_Y + 29)

/* The Finder's menu bar ends in a black line at Mac y = 19. */
#define MENU_LINE_Y 19

#define WALL_TIMEOUT_S 120

typedef enum {
    STEP_BOOT,
    STEP_PARKING,  /* finger on the desktop */
    STEP_PARKED,
    STEP_MENU,     /* finger on the Apple menu title */
    STEP_ITEM,     /* finger on About This Macintosh */
    STEP_ABOUT,    /* lifted; waiting for the window */
} step_t;

typedef struct {
    int failures;
    uint32_t checked;
    uint32_t stale_runs;    /* panel wrong on two hooks in a row */
    bool was_stale;
    step_t step;
    uint16_t desktop[MAC_SCREEN_W * MAC_SCREEN_H];  /* the screen area after booting */
} scenario_t;

static uint16_t s_panel[PANEL_W * PANEL_H];
static uint16_t s_frame[PANEL_W * PANEL_H];  /* what the frame should look like */

static const uint8_t *mac_fb(void)
{
    return ram_get_base() + umac_get_fb_offset();
}

static uint16_t mac_pixel(const uint8_t *fb, int x, int y)
{
    const uint8_t b = fb[y * MAC_SCREEN_STRIDE + x / 8];
    return (b >> (7 - x % 8)) & 1 ? 0x0000 : 0xffff;
}

/* The panel at Mac screen coordinates. */
static uint16_t screen_px(int x, int y)
{
    return s_panel[(MAC_SCREEN_Y + y) * PANEL_W + MAC_SCREEN_X + x];
}

/* True where panel pixel (x, y) shows the Mac: inside the screen's window and
 * not cut off by the frame's rounded corners. */
static bool on_tube(int x, int y)
{
    return x >= MAC_SCREEN_X && x < MAC_SCREEN_X + MAC_SCREEN_W && y >= MAC_SCREEN_Y &&
           y < MAC_SCREEN_Y + MAC_SCREEN_H && s_frame[y * PANEL_W + x] == 0x0000;
}

/* Pixels where the tube and the Mac's framebuffer disagree. */
static uint32_t screen_mismatches(void)
{
    const uint8_t *fb = mac_fb();
    uint32_t bad = 0;
    for (int y = 0; y < MAC_SCREEN_H; y++) {
        for (int x = 0; x < MAC_SCREEN_W; x++) {
            if (on_tube(MAC_SCREEN_X + x, MAC_SCREEN_Y + y)) bad += screen_px(x, y) != mac_pixel(fb, x, y);
        }
    }
    return bad;
}

/* Pixels off the tube that are not the frame. */
static uint32_t frame_damage(void)
{
    uint32_t bad = 0;
    for (int y = 0; y < PANEL_H; y++) {
        for (int x = 0; x < PANEL_W; x++) {
            if (!on_tube(x, y)) bad += s_panel[y * PANEL_W + x] != s_frame[y * PANEL_W + x];
        }
    }
    return bad;
}

/*
 * On the Finder's desktop, pixels where the frame's corners and System 7's
 * disagree. Every pixel the frame cuts off must be one the Mac paints black.
 * And in the menu bar's top rows, white at its ends but for the corners, the
 * Mac's black must stop where the cut does: on the first pixel of the tube.
 */
#define CORNER_ROWS 10
static uint32_t corner_misfit(void)
{
    const uint8_t *fb = mac_fb();
    uint32_t bad = 0;
    for (int y = 0; y < MAC_SCREEN_H; y++) {
        for (int x = 0; x < MAC_SCREEN_W; x++) {
            if (!on_tube(MAC_SCREEN_X + x, MAC_SCREEN_Y + y)) bad += mac_pixel(fb, x, y) != 0x0000;
        }
    }
    for (int y = 0; y < CORNER_ROWS; y++) {
        int cut = 0;
        while (cut < MAC_SCREEN_W / 2 && !on_tube(MAC_SCREEN_X + cut, MAC_SCREEN_Y + y)) cut++;
        bad += mac_pixel(fb, cut, y) != 0xffff;
        bad += mac_pixel(fb, MAC_SCREEN_W - 1 - cut, y) != 0xffff;
    }
    return bad;
}

/* The whole panel, in colour, as the eye would see it. */
static void save_ppm(const char *name)
{
    char path[256];
    snprintf(path, sizeof path, "build/check/%s.ppm", name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("cannot write %s\n", path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", PANEL_W, PANEL_H);
    for (int i = 0; i < PANEL_W * PANEL_H; i++) {
        const uint16_t v = board_wire_to_rgb565(s_panel[i]);
        const uint8_t r = (uint8_t)((v >> 11) & 0x1f), g = (uint8_t)((v >> 5) & 0x3f),
                      b = (uint8_t)(v & 0x1f);
        fputc((r << 3) | (r >> 2), f);
        fputc((g << 2) | (g >> 4), f);
        fputc((b << 3) | (b >> 2), f);
    }
    fclose(f);
    printf("  saved %s\n", path);
}

static void fail(scenario_t *s, const char *what)
{
    printf("FAIL: %s\n", what);
    s->failures++;
}

static void finish(scenario_t *s)
{
    const board_stats_t st = board_stats();

    /* Black across the width of the menu bar's bottom line, but for where
     * the cursor (16 px wide, white outline) may cross it. */
    uint32_t line = 0;
    for (int x = 0; x < MAC_SCREEN_W; x++) line += s->desktop[MENU_LINE_Y * MAC_SCREEN_W + x] == 0;
    if (line < MAC_SCREEN_W - 16) fail(s, "no Finder menu bar after booting");

    uint32_t changed = 0;
    for (int y = MENU_LINE_Y + 1; y < MAC_SCREEN_H; y++) {
        for (int x = 0; x < MAC_SCREEN_W; x++) {
            changed += screen_px(x, y) != s->desktop[y * MAC_SCREEN_W + x];
        }
    }
    printf("  %u px changed below the menu bar after choosing About\n", changed);
    if (changed < 5000) fail(s, "the About window did not open (is touch reaching the Mac?)");

    const uint32_t damage = frame_damage();
    if (damage) {
        printf("FAIL: %u px of the frame were drawn over\n", damage);
        s->failures++;
    }

    if (s->stale_runs) {
        printf("FAIL: %u times the panel stayed different from the framebuffer\n", s->stale_runs);
        s->failures++;
    }
    if (st.window_violations || st.rejected_transfers) {
        printf("FAIL: %u bad panel windows, %u rejected transfers\n", st.window_violations,
               st.rejected_transfers);
        s->failures++;
    }
    if (st.errors || st.warnings) {
        printf("FAIL: firmware logged %u errors and %u warnings\n", st.errors, st.warnings);
        s->failures++;
    }

    printf("board: %u hooks checked; %u transfers (%llu px), %u touch reads; "
           "%u errors, %u warnings; stack peak %u of %d B\n",
           s->checked, st.transfers, (unsigned long long)st.pixels, st.touch_reads, st.errors,
           st.warnings, st.stack_peak, CONFIG_ESP_MAIN_TASK_STACK_SIZE);
    printf("%s\n", s->failures ? "board check FAILED" : "board check passed");
    fflush(stdout);
    exit(s->failures ? 1 : 0);
}

/*
 * Runs at each vTaskDelay(), which the firmware reaches once per retrace.
 * Every other retrace it has just refreshed the panel, so a panel that
 * differs from the framebuffer twice running was not refreshed properly.
 */
static void on_frame(uint32_t frame, void *ctx)
{
    (void)frame;
    scenario_t *s = ctx;
    const double t = (double)esp_timer_get_time() / 1e6;

    board_panel_snapshot(s_panel);
    const bool stale = screen_mismatches() != 0;
    if (stale && s->was_stale && s->stale_runs++ == 0) {
        printf("  t=%.2f s: panel still differs from the framebuffer\n", t);
    }
    s->was_stale = stale;
    s->checked++;

    if (t > T_GIVE_UP) {
        printf("FAIL: t=%.1f s: stuck at step %d (the panel never settled?)\n", t, s->step);
        s->failures++;
        finish(s);
    }

    switch (s->step) {
    case STEP_BOOT:
        if (t < T_PARK) break;
        board_touch_set(true, PARK_X, PARK_Y);
        s->step = STEP_PARKING;
        break;
    case STEP_PARKING:
        if (t < T_PARK + 0.5) break;
        board_touch_set(false, PARK_X, PARK_Y);
        s->step = STEP_PARKED;
        break;
    case STEP_PARKED:
        if (t < T_DESKTOP || stale) break;
        printf("t=%.1f s: desktop; pressing the Apple menu\n", t);
        save_ppm("1_desktop");
        const uint32_t misfit = corner_misfit();
        if (misfit) {
            printf("FAIL: %u px where the frame's corners and System 7's differ\n", misfit);
            s->failures++;
        }
        for (int y = 0; y < MAC_SCREEN_H; y++) {
            for (int x = 0; x < MAC_SCREEN_W; x++) s->desktop[y * MAC_SCREEN_W + x] = screen_px(x, y);
        }
        board_touch_set(true, APPLE_X, APPLE_Y);
        s->step = STEP_MENU;
        break;
    case STEP_MENU:
        if (t < T_SLIDE) break;
        save_ppm("2_menu");
        board_touch_set(true, ABOUT_X, ABOUT_Y);
        s->step = STEP_ITEM;
        break;
    case STEP_ITEM:
        if (t < T_RELEASE) break;
        board_touch_set(false, ABOUT_X, ABOUT_Y);
        s->step = STEP_ABOUT;
        break;
    case STEP_ABOUT:
        if (t < T_ABOUT || stale) break;
        printf("t=%.1f s: after choosing About This Macintosh\n", t);
        save_ppm("3_about");
        finish(s);
        break;
    }
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s rom.bin disk.img\n", argv[0]);
        return 2;
    }
    static scenario_t scenario;
    mac_frame_rows(s_frame, 0, PANEL_H);
    board_config_t cfg = {
        .clock = BOARD_CLOCK_VIRTUAL,
        .frame_hook = on_frame,
        .hook_ctx = &scenario,
        .max_echoed_problems = 10,
        .echo_info = true,
        .partitions = {
            { .label = "macrom", .subtype = 0x41, .path = argv[1], .size = 0x20000 },
            /* Bigger than the image, as on the board: the rest is erased flash. */
            { .label = "macdisk", .subtype = 0x42, .path = argv[2], .size = 0x9e0000 },
        },
        .partition_count = 2,
    };
    board_start(&cfg);

    for (int i = 0; i < WALL_TIMEOUT_S * 10; i++) {
        if (board_app_returned()) {
            printf("FAIL: app_main returned\nboard check FAILED\n");
            return 1;
        }
        nanosleep(&(struct timespec){ .tv_nsec = 100000000 }, NULL);
    }
    printf("FAIL: not done after %d s of wall time\nboard check FAILED\n", WALL_TIMEOUT_S);
    return 1;
}
