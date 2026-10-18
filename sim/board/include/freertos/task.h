/*
 * Host stand-in for FreeRTOS task.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "freertos/FreeRTOS.h"

/* Blocks the calling task. In the board simulator this is also where queued
 * panel transfers complete and the per-frame hook runs. */
void vTaskDelay(TickType_t ticks);
