/*
 * mac_frame.c - see mac_frame.h.
 *
 * Every pixel is a function of its position, worked out in layers from the
 * screen outwards: tube, recess, bevel, case. The badge goes in the case
 * below the screen, inside the panel's safe area (12 px in from the edge).
 *
 * The layers are bands of distance from the screen's core: the screen
 * shrunk by SCREEN_RADIUS on every side. So the tube is a rounded rectangle
 * of that radius, and every edge around it is rounded concentrically.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mac_frame.h"

#include <stdbool.h>
#include <stddef.h>

#include "mac_display.h"

/* System 7 rounds the corners of its desktop with a circle of radius 8: from
 * the corner inwards it paints 5, 3, 2, 1 and 1 pixels black. */
#define SCREEN_RADIUS 8

/* Layers around the screen, in pixels. */
#define RECESS 4   /* dark surround the tube sits in */
#define BEVEL  2   /* sunken step from the case down into the recess */
#define EDGE   10  /* the case darkens towards the panel edge, like a curved shell */

/* The badge row, below the screen. */
#define BADGE_Y      (MAC_SCREEN_Y + MAC_SCREEN_H + RECESS + BEVEL + 2)
#define STRIPE_H     2
#define STRIPES      6
#define STRIPES_X    (MAC_SCREEN_X + 12)
#define STRIPES_W    14
#define NAME_X       (STRIPES_X + STRIPES_W + 6)
#define NAME_Y       (BADGE_Y + 2)
#define LED_X        (MAC_SCREEN_X + MAC_SCREEN_W - 20)
#define LED_Y        (BADGE_Y + 4)
#define LED_W        8
#define LED_H        4

typedef struct {
    uint8_t r, g, b;
} rgb_t;

/* Apple II beige, lit from above. */
static const rgb_t CASE_TOP = { 0xe4, 0xd9, 0xbd };
static const rgb_t CASE_BOTTOM = { 0xc9, 0xbb, 0x99 };
static const rgb_t BEVEL_SHADOW = { 0x96, 0x88, 0x68 };
static const rgb_t BEVEL_LIGHT = { 0xf5, 0xed, 0xd6 };
static const rgb_t RECESS_COLOUR = { 0x2a, 0x26, 0x21 };
static const rgb_t INK = { 0x5e, 0x52, 0x3c };       /* engraved lettering */
static const rgb_t INK_LIGHT = { 0xf2, 0xe9, 0xd0 }; /* its lower edge catching the light */
static const rgb_t LED_ON = { 0x4c, 0xd9, 0x64 };
static const rgb_t LED_RIM = { 0x2e, 0x7d, 0x3a };

/* The six-colour rainbow, top to bottom. */
static const rgb_t STRIPE[STRIPES] = {
    { 0x61, 0xbb, 0x46 }, { 0xfd, 0xb8, 0x27 }, { 0xf5, 0x82, 0x1f },
    { 0xe0, 0x3a, 0x3e }, { 0x96, 0x3d, 0x97 }, { 0x00, 0x9d, 0xdc },
};

/* "Mosaintosh" in 5x7, one string of five columns per row. */
#define GLYPH_W 5
#define GLYPH_H 7
#define GLYPH_ADVANCE (GLYPH_W + 1)
typedef struct {
    char c;
    const char *rows[GLYPH_H];
} glyph_t;

static const glyph_t GLYPHS[] = {
    { 'M', { "#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#" } },
    { 'o', { ".....", ".....", ".###.", "#...#", "#...#", "#...#", ".###." } },
    { 's', { ".....", ".....", ".####", "#....", ".###.", "....#", "####." } },
    { 'a', { ".....", ".....", ".###.", "....#", ".####", "#...#", ".####" } },
    { 'i', { "..#..", ".....", ".##..", "..#..", "..#..", "..#..", ".###." } },
    { 'n', { ".....", ".....", "####.", "#...#", "#...#", "#...#", "#...#" } },
    { 't', { "..#..", "..#..", ".###.", "..#..", "..#..", "..#..", "...##" } },
    { 'h', { "#....", "#....", "####.", "#...#", "#...#", "#...#", "#...#" } },
};
static const char NAME[] = "Mosaintosh";
#define NAME_LEN ((int)sizeof NAME - 1)

static uint16_t to_wire(rgb_t c)
{
    const uint16_t v = (uint16_t)(((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3));
    return (uint16_t)((v << 8) | (v >> 8));
}

static rgb_t mix(rgb_t a, rgb_t b, int num, int den)
{
    return (rgb_t){
        (uint8_t)(a.r + (b.r - a.r) * num / den),
        (uint8_t)(a.g + (b.g - a.g) * num / den),
        (uint8_t)(a.b + (b.b - a.b) * num / den),
    };
}

static int min_int(int a, int b)
{
    return a < b ? a : b;
}

/* True where the name's pixel (x, y) is inked. */
static bool name_ink(int x, int y)
{
    const int dx = x - NAME_X, dy = y - NAME_Y;
    if (dx < 0 || dy < 0 || dy >= GLYPH_H || dx >= NAME_LEN * GLYPH_ADVANCE) return false;
    const int col = dx % GLYPH_ADVANCE;
    if (col >= GLYPH_W) return false;
    const char c = NAME[dx / GLYPH_ADVANCE];
    for (size_t i = 0; i < sizeof GLYPHS / sizeof GLYPHS[0]; i++) {
        if (GLYPHS[i].c == c) return GLYPHS[i].rows[dy][col] == '#';
    }
    return false;
}

/* The case at (x, y): the badge, or beige shaded by height and edge. */
static rgb_t case_pixel(int x, int y)
{
    if (x >= STRIPES_X && x < STRIPES_X + STRIPES_W && y >= BADGE_Y &&
        y < BADGE_Y + STRIPES * STRIPE_H) {
        return STRIPE[(y - BADGE_Y) / STRIPE_H];
    }
    if (x >= LED_X && x < LED_X + LED_W && y >= LED_Y && y < LED_Y + LED_H) {
        const bool rim = x == LED_X || x == LED_X + LED_W - 1 || y == LED_Y || y == LED_Y + LED_H - 1;
        return rim ? LED_RIM : LED_ON;
    }
    if (name_ink(x, y)) return INK;
    if (name_ink(x, y - 1) || name_ink(x - 1, y - 1)) return INK_LIGHT;

    rgb_t c = mix(CASE_TOP, CASE_BOTTOM, y, PANEL_H - 1);
    const int edge = min_int(min_int(x, PANEL_W - 1 - x), min_int(y, PANEL_H - 1 - y));
    if (edge < EDGE) c = mix(c, (rgb_t){ 0, 0, 0 }, EDGE - edge, EDGE * 6);
    return c;
}

static int clamp_int(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/*
 * The offset of panel pixel (x, y)'s centre from the nearest point of the
 * screen's core, in half pixels so that it stays whole.
 */
static void core_offset(int x, int y, int *dx, int *dy)
{
    const int cx = 2 * x + 1, cy = 2 * y + 1;
    *dx = cx - clamp_int(cx, 2 * (MAC_SCREEN_X + SCREEN_RADIUS),
                         2 * (MAC_SCREEN_X + MAC_SCREEN_W - SCREEN_RADIUS));
    *dy = cy - clamp_int(cy, 2 * (MAC_SCREEN_Y + SCREEN_RADIUS),
                         2 * (MAC_SCREEN_Y + MAC_SCREEN_H - SCREEN_RADIUS));
}

/* True if that offset is at most `reach` pixels. */
static bool within(int dx, int dy, int reach)
{
    return dx * dx + dy * dy <= 4 * reach * reach;
}

static uint16_t frame_pixel(int x, int y)
{
    int dx, dy;
    core_offset(x, y, &dx, &dy);

    if (within(dx, dy, SCREEN_RADIUS)) return 0x0000;  /* the tube, off */
    if (within(dx, dy, SCREEN_RADIUS + RECESS)) return to_wire(RECESS_COLOUR);
    if (within(dx, dy, SCREEN_RADIUS + RECESS + BEVEL)) {
        /* Light from the top left: the walls facing down and right are lit,
         * which splits the top-right and bottom-left corners on the diagonal. */
        return to_wire(dx + dy > 0 ? BEVEL_LIGHT : BEVEL_SHADOW);
    }
    return to_wire(case_pixel(x, y));
}

void mac_frame_round_corners(uint16_t *row, int y)
{
    if (y >= SCREEN_RADIUS && y < MAC_SCREEN_H - SCREEN_RADIUS) return;

    /* The corners are mirror images, so the cut at the left end, measured
     * along this row, is also the cut at the right end. */
    const uint16_t recess = to_wire(RECESS_COLOUR);
    for (int x = 0; x < SCREEN_RADIUS; x++) {
        int dx, dy;
        core_offset(MAC_SCREEN_X + x, MAC_SCREEN_Y + y, &dx, &dy);
        if (within(dx, dy, SCREEN_RADIUS)) break;
        row[x] = recess;
        row[MAC_SCREEN_W - 1 - x] = recess;
    }
}

void mac_frame_rows(uint16_t *dst, int y0, int h)
{
    for (int y = y0; y < y0 + h; y++) {
        for (int x = 0; x < PANEL_W; x++) *dst++ = frame_pixel(x, y);
    }
}
