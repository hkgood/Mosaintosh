/*
 * mac_display.c - see mac_display.h.
 *
 * The Mac draws into its framebuffer whenever it likes and never says what
 * changed, so changes are found by comparing against a copy of what the
 * panel was last sent. A row is 52 bytes; comparing all 416 of them is far
 * cheaper than sending even one band, which costs a QSPI transfer.
 *
 * The frame is drawn once, at init; after that only the screen's window of
 * the panel is ever written, with the frame's rounded corners painted over
 * the ends of the rows that reach them.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mac_display.h"

#include <string.h>

#include "bsp/display.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mac_frame.h"

static const char *TAG = "mac_display";

/* CO5300 windows must start on an even row and span an even number of rows. */
#define ROW_ALIGN 2
#define BAND_H 32
/* Wide enough for full panel rows, which the frame needs. */
#define BAND_BYTES ((size_t)PANEL_W * BAND_H * sizeof(uint16_t))
#define TRANS_TIMEOUT_MS 1000

#define PIXEL_BLACK 0x0000
#define PIXEL_WHITE 0xffff

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_trans_done;
static uint16_t *s_band;   /* DMA-capable staging for one band */
static uint8_t *s_sent;    /* what the panel shows, in the Mac's layout */
static bool s_sent_valid;  /* false until the first full send */

static bool IRAM_ATTR on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                          esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)io;
    (void)edata;
    (void)user_ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_trans_done, &woken);
    return woken == pdTRUE;
}

/* Sends the band as panel window (x0, y0)-(x1, y1) and waits for it: the band
 * is reused, so each transfer must finish before the next fill. */
static bool send_band(int x0, int y0, int x1, int y1)
{
    if (esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x1, y1, s_band) != ESP_OK ||
        xSemaphoreTake(s_trans_done, pdMS_TO_TICKS(TRANS_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "sending panel rows %d..%d failed", y0, y1);
        return false;
    }
    return true;
}

/* Covers the whole panel, boot splash included. */
static bool draw_frame(void)
{
    for (int y = 0; y < PANEL_H; y += BAND_H) {
        const int h = y + BAND_H > PANEL_H ? PANEL_H - y : BAND_H;
        mac_frame_rows(s_band, y, h);
        if (!send_band(0, y, PANEL_W, y + h)) return false;
    }
    return true;
}

bool mac_display_init(void)
{
    s_band = heap_caps_malloc(BAND_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_sent = heap_caps_malloc(MAC_SCREEN_STRIDE * MAC_SCREEN_H, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_trans_done = xSemaphoreCreateBinary();
    if (s_band == NULL || s_sent == NULL || s_trans_done == NULL) {
        ESP_LOGE(TAG, "no memory for the transfer band and screen copy");
        return false;
    }

    const bsp_display_config_t cfg = BSP_DISPLAY_DEFAULT_CONFIG();
    if (bsp_display_new(&cfg, &s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "bsp_display_new failed");
        return false;
    }
    const esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = on_color_trans_done };
    if (esp_lcd_panel_io_register_event_callbacks(bsp_display_get_panel_io(), &cbs, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "cannot register the colour-transfer-done callback");
        return false;
    }
    if (!draw_frame()) return false;
    s_sent_valid = false;
    return true;
}

/* 1 bpp Mac rows [y0, y0 + h) into RGB565 in the band, corners rounded. */
static void expand_rows(const uint8_t *fb, int y0, int h)
{
    uint16_t *dst = s_band;
    for (int y = y0; y < y0 + h; y++) {
        uint16_t *row = dst;
        const uint8_t *src = fb + (size_t)y * MAC_SCREEN_STRIDE;
        for (int i = 0; i < MAC_SCREEN_STRIDE; i++) {
            const uint8_t b = src[i];
            for (int bit = 7; bit >= 0; bit--) {
                *dst++ = (b >> bit) & 1 ? PIXEL_BLACK : PIXEL_WHITE;
            }
        }
        mac_frame_round_corners(row, y);
    }
}

/* Mac rows [y0, y1), into the screen's window of the panel. */
static bool send_rows(const uint8_t *fb, int y0, int y1)
{
    for (int y = y0; y < y1; y += BAND_H) {
        const int h = y + BAND_H > y1 ? y1 - y : BAND_H;
        expand_rows(fb, y, h);
        if (!send_band(MAC_SCREEN_X, MAC_SCREEN_Y + y, MAC_SCREEN_X + MAC_SCREEN_W,
                       MAC_SCREEN_Y + y + h)) {
            return false;
        }
    }
    return true;
}

static bool rows_changed(const uint8_t *fb, int y)
{
    const size_t off = (size_t)y * MAC_SCREEN_STRIDE;
    return memcmp(fb + off, s_sent + off, MAC_SCREEN_STRIDE * ROW_ALIGN) != 0;
}

int mac_display_update(const uint8_t *fb)
{
    if (!s_sent_valid) {
        if (!send_rows(fb, 0, MAC_SCREEN_H)) return -1;
        memcpy(s_sent, fb, MAC_SCREEN_STRIDE * MAC_SCREEN_H);
        s_sent_valid = true;
        return MAC_SCREEN_H;
    }

    /*
     * Runs of changed row pairs go out as one window each. Each run is copied
     * into s_sent after it is sent, from the same bytes: the 68000 is not
     * running meanwhile, so the framebuffer cannot change underneath.
     */
    int sent = 0;
    for (int y = 0; y < MAC_SCREEN_H;) {
        if (!rows_changed(fb, y)) {
            y += ROW_ALIGN;
            continue;
        }
        int end = y + ROW_ALIGN;
        while (end < MAC_SCREEN_H && rows_changed(fb, end)) end += ROW_ALIGN;
        if (!send_rows(fb, y, end)) return -1;
        const size_t off = (size_t)y * MAC_SCREEN_STRIDE;
        memcpy(s_sent + off, fb + off, (size_t)(end - y) * MAC_SCREEN_STRIDE);
        sent += end - y;
        y = end;
    }
    return sent;
}
