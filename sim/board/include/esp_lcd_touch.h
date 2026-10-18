/*
 * Host stand-in for espressif/esp_lcd_touch's esp_lcd_touch.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef struct esp_lcd_touch_s *esp_lcd_touch_handle_t;

typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t strength;
} esp_lcd_touch_point_data_t;

/* Reads the controller into the driver's buffer. Nothing new is visible to
 * esp_lcd_touch_get_data() until this has been called. */
esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t tp);

/* Copies points out of the driver's buffer and consumes them. Leaves
 * *point_cnt untouched when there is nothing to report, as the real one does. */
esp_err_t esp_lcd_touch_get_data(esp_lcd_touch_handle_t tp, esp_lcd_touch_point_data_t *data,
                                 uint8_t *point_cnt, uint8_t max_point_cnt);
