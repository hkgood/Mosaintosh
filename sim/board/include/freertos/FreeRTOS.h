/*
 * Host stand-in for FreeRTOS.h as ESP-IDF ships it.
 *
 * The tick rate is the firmware's CONFIG_FREERTOS_HZ, passed in by the build,
 * so pdMS_TO_TICKS() rounds exactly as it does on the device -- at 100 Hz a
 * 16 ms delay is one 10 ms tick, and the simulator has to show that.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#ifndef CONFIG_FREERTOS_HZ
#error "CONFIG_FREERTOS_HZ must match the firmware's sdkconfig"
#endif

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;

#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  pdTRUE
#define pdFAIL  pdFALSE

#define configTICK_RATE_HZ CONFIG_FREERTOS_HZ
#define portMAX_DELAY      ((TickType_t)0xffffffffu)
#define portTICK_PERIOD_MS (1000 / configTICK_RATE_HZ)

#define pdMS_TO_TICKS(ms) \
    ((TickType_t)(((uint64_t)(ms) * (uint64_t)configTICK_RATE_HZ) / 1000u))
