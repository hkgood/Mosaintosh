/*
 * mac_input.c - see mac_input.h.
 *
 * A Mac mouse only reports movement, which a touch panel cannot give
 * sensibly: a finger lands somewhere, it does not travel there. So the
 * cursor is placed directly, the way Mini vMac does it, by writing the
 * position into the low-memory globals the cursor code reads on every
 * vertical retrace.
 *
 * The button must go down only after the cursor has moved, or the Mac would
 * click wherever the cursor used to be. A press therefore takes two polls:
 * move, then button.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mac_input.h"

#include "bsp/display.h"
#include "esp_err.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "mac_display.h"
#include "umac.h"

static const char *TAG = "mac_input";

/* Mac Plus low-memory globals (Inside Macintosh). Points are (v, h). */
#define LM_MTEMP      0x0828  /* where the mouse is, as the driver sees it */
#define LM_RAWMOUSE   0x082c  /* where the mouse is, unscaled */
#define LM_CRSRNEW    0x08ce  /* nonzero: the cursor must be redrawn */
#define LM_CRSRBUSY   0x08cd  /* nonzero: the cursor is being drawn now */
#define LM_CRSRCOUPLE 0x08cf  /* nonzero: the cursor follows the mouse */

typedef enum {
    TOUCH_UP,
    TOUCH_MOVED,  /* cursor placed; button goes down on the next poll */
    TOUCH_DOWN,
} touch_state_t;

static esp_lcd_touch_handle_t s_touch;
static touch_state_t s_state = TOUCH_UP;

/* Mac RAM is big-endian, as the 68000 sees it. */
static void put_word(uint8_t *ram, uint32_t addr, uint16_t v)
{
    ram[addr] = (uint8_t)(v >> 8);
    ram[addr + 1] = (uint8_t)v;
}

/* False if the cursor is mid-draw; the caller tries again next poll. */
static bool place_cursor(uint8_t *ram, int x, int y)
{
    if (ram[LM_CRSRBUSY]) return false;
    put_word(ram, LM_MTEMP, (uint16_t)y);
    put_word(ram, LM_MTEMP + 2, (uint16_t)x);
    put_word(ram, LM_RAWMOUSE, (uint16_t)y);
    put_word(ram, LM_RAWMOUSE + 2, (uint16_t)x);
    ram[LM_CRSRNEW] = ram[LM_CRSRCOUPLE];
    return true;
}

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

bool mac_input_init(void)
{
    const bsp_display_config_t cfg = BSP_DISPLAY_DEFAULT_CONFIG();
    if (bsp_touch_new(cfg.rotation, &s_touch) != ESP_OK) {
        ESP_LOGW(TAG, "no touch digitiser; the Mac runs without a mouse");
        s_touch = NULL;
        return false;
    }
    return true;
}

/* Polls the button stays down at least, so the OS's once-per-retrace look at
 * it cannot miss a short tap. */
#define MIN_HOLD_POLLS 2

static int s_held;

void mac_input_feed(uint8_t *ram, bool down, int x, int y)
{
    /* A finger on the frame means the nearest edge of the screen, which
     * makes the menu bar easy to hit: anywhere along the top of the frame. */
    x = clamp(x - MAC_SCREEN_X, 0, MAC_SCREEN_W - 1);
    y = clamp(y - MAC_SCREEN_Y, 0, MAC_SCREEN_H - 1);

    switch (s_state) {
    case TOUCH_UP:
        if (down && place_cursor(ram, x, y)) s_state = TOUCH_MOVED;
        break;
    case TOUCH_MOVED:
        /* Even if the finger has already lifted: a tap is still a click. */
        if (down) place_cursor(ram, x, y);
        umac_mouse(0, 0, 1);
        s_state = TOUCH_DOWN;
        s_held = 1;
        break;
    case TOUCH_DOWN:
        if (down) place_cursor(ram, x, y);
        if (s_held < MIN_HOLD_POLLS) {
            s_held++;
        } else if (!down) {
            umac_mouse(0, 0, 0);
            s_state = TOUCH_UP;
        }
        break;
    }
}

void mac_input_poll(uint8_t *ram)
{
    if (s_touch == NULL) return;

    /* get_data() only returns what read_data() fetched. A failed read says
     * nothing about the finger, so the state is left as it was. */
    if (esp_lcd_touch_read_data(s_touch) != ESP_OK) return;

    esp_lcd_touch_point_data_t pt = { 0 };
    uint8_t n = 0;
    const bool down = esp_lcd_touch_get_data(s_touch, &pt, &n, 1) == ESP_OK && n > 0;
    mac_input_feed(ram, down, pt.x, pt.y);
}
