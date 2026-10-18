/*
 * Host stand-in for ESP-IDF's esp_lcd_panel_ops.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "esp_lcd_types.h"

/* x_end and y_end are exclusive, as in ESP-IDF. Like the CO5300 QSPI driver,
 * this only queues the transfer: color_data is read when it completes. */
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel, int x_start, int y_start,
                                    int x_end, int y_end, const void *color_data);
