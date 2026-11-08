/*
 * Copyright (c) 2026 Emilio Schinina' <emilioschi@gmail.com>
 *
 * This file is part of libterae.
 *
 * libterae is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * libterae is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with libterae.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file freertos_system.c
 * @brief The clock, sleeping, the program's output, and stopping, for
 * FreeRTOS.
 *
 * The clock is the kernel's tick count, which wraps, extended to 64 bits as
 * long as it is read at least once each time round. A program on FreeRTOS has
 * no signals, so it stops only when a task asks it to with
 * mqtt_platform_request_stop, which sends a byte on a pair of loopback sockets,
 * so that a wait ends at once. Pages, a process to copy and its processor time
 * aren't something the kernel has, so those calls say so.
 */

#include <string.h>

#include <FreeRTOS.h>
#include <task.h>

#include "base_platform.h"
#include "freertos_internal.h"

/** @brief Microseconds in a millisecond. */
#define FREERTOS_MS_US 1000U

/** @brief true once a stop has been asked for. */
static volatile bool freertos_stopping = false;
/** @brief The pair of sockets that wakes a wait: the end read, then written. */
static int32_t freertos_wake[2] = {MQTT_SOCKET_NONE, MQTT_SOCKET_NONE};

void mqtt_platform_console_write(const char *text)
{
    freertos_board_write(true, (const uint8_t *)text, (uint32_t)strlen(text));
}

void mqtt_platform_output_write(const uint8_t *data, uint32_t length)
{
    freertos_board_write(false, data, length);
}

uint64_t mqtt_platform_time_ms(void)
{
    /** @brief The tick count the last time it was read. */
    static TickType_t last = 0U;
    /** @brief The times it has wrapped, as the high bits of the count. */
    static uint64_t wraps = 0U;
    /** @brief The count now. */
    TickType_t now = 0U;
    /** @brief The count in 64 bits. */
    uint64_t ticks = 0U;

    taskENTER_CRITICAL();
    now = xTaskGetTickCount();
    if (now < last) {
        wraps += (uint64_t)portMAX_DELAY + 1U;
    }
    last = now;
    ticks = wraps + (uint64_t)now;
    taskEXIT_CRITICAL();
    return (ticks * 1000U) / (uint64_t)configTICK_RATE_HZ;
}

uint64_t mqtt_platform_time_us(void)
{
    return mqtt_platform_time_ms() * FREERTOS_MS_US;
}

uint32_t mqtt_platform_page_size(void)
{
    return 0U;
}

void mqtt_platform_sleep_ms(uint32_t milliseconds)
{
    vTaskDelay(freertos_ticks(milliseconds));
}

/** @brief Open the pair of sockets that wakes a wait, the first time. */
static void freertos_prepare_wake(void)
{
    if (freertos_wake[0] == MQTT_SOCKET_NONE) {
        if (mqtt_platform_socket_pair(&freertos_wake[0], &freertos_wake[1]) !=
            MQTT_OK) {
            freertos_wake[0] = MQTT_SOCKET_NONE;
            freertos_wake[1] = MQTT_SOCKET_NONE;
        }
    }
}

bool mqtt_platform_stop_requested(void)
{
    freertos_prepare_wake();
    return freertos_stopping;
}

void mqtt_platform_request_stop(void)
{
    /** @brief The byte sent. */
    const uint8_t byte = 1U;
    /** @brief Number of bytes taken. */
    uint32_t count = 0U;

    freertos_prepare_wake();
    freertos_stopping = true;
    if (freertos_wake[1] != MQTT_SOCKET_NONE) {
        (void)mqtt_platform_write(freertos_wake[1], &byte, 1U, &count);
    }
}

int32_t mqtt_platform_wake_handle(void)
{
    freertos_prepare_wake();
    return freertos_wake[0];
}

void mqtt_platform_wake_drain(void)
{
    /** @brief Where the bytes go. */
    uint8_t bytes[16];
    /** @brief Number of bytes read. */
    uint32_t count = 0U;

    if (freertos_wake[0] != MQTT_SOCKET_NONE) {
        while (mqtt_platform_read(freertos_wake[0], bytes, sizeof(bytes),
                                  &count) == MQTT_OK) {
            /* Each byte only says a stop was asked for. */
        }
    }
}

uint64_t mqtt_platform_cpu_us(void)
{
    return 0U;
}
