/*
 * Host stand-in for ESP-IDF's esp_log.h. Lines keep the device format
 * ("W (1234) tag: message") and every warning and error is counted, so a test
 * can fail on a log line the same way a reviewer would on the device.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

void board_log(char level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

uint32_t esp_log_timestamp(void);

#define ESP_LOGE(tag, fmt, ...) board_log('E', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) board_log('W', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) board_log('I', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) ((void)0)
#define ESP_LOGV(tag, fmt, ...) ((void)0)
