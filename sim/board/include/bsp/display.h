/*
 * Host stand-in for esp-mosaico-bsp's bsp/display.h (non-LVGL build).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_types.h"

#define BSP_LCD_H_RES          480
#define BSP_LCD_V_RES          480
#define BSP_LCD_X_GAP          0
#define BSP_LCD_Y_GAP          0
#define BSP_LCD_BITS_PER_PIXEL 16

typedef enum {
    BSP_DISPLAY_ROTATE_0 = 0,
    BSP_DISPLAY_ROTATE_90 = 90,
    BSP_DISPLAY_ROTATE_180 = 180,
    BSP_DISPLAY_ROTATE_270 = 270,
} bsp_display_rotation_t;

#define BSP_LCD_ROTATION_DEFAULT BSP_DISPLAY_ROTATE_0

typedef struct {
    bsp_display_rotation_t rotation;
} bsp_display_config_t;

#define BSP_DISPLAY_DEFAULT_CONFIG() { .rotation = BSP_LCD_ROTATION_DEFAULT }

esp_err_t bsp_display_new(const bsp_display_config_t *config, esp_lcd_panel_handle_t *ret_panel);
esp_lcd_panel_handle_t bsp_display_get_panel(void);
esp_lcd_panel_io_handle_t bsp_display_get_panel_io(void);
esp_err_t bsp_touch_new(bsp_display_rotation_t rotation, esp_lcd_touch_handle_t *ret_touch);
