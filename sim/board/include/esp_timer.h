/*
 * Host stand-in for ESP-IDF's esp_timer.h: microseconds since boot, on the
 * board simulator's clock (virtual in headless runs, wall time in the window).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

int64_t esp_timer_get_time(void);
