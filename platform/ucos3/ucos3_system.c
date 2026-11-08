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
 * @file ucos3_system.c
 * @brief The clock, sleeping, the program's output, and stopping, for
 * uC/OS-III.
 *
 * The clock is the kernel's tick count, which wraps, extended to 64 bits as
 * long as it is read at least once each time round. uC/OS-III has no signals,
 * so the program stops only when a task asks it to with
 * mqtt_platform_request_stop, which sends a byte on a pair of loopback sockets,
 * so that a wait ends at once.
 */

#include <string.h>

#include <cpu.h>
#include <os.h>

#include "base_platform.h"
#include "ucos3_internal.h"

/** @brief Microseconds in a millisecond. */
#define UCOS3_MS_US 1000U

/** @brief true once a stop has been asked for. */
static volatile bool ucos3_stopping = false;
/** @brief The pair of sockets that wakes a wait: the end read, then written. */
static int32_t ucos3_wake[2] = {MQTT_SOCKET_NONE, MQTT_SOCKET_NONE};

OS_TICK ucos3_ticks(uint32_t milliseconds)
{
    /** @brief The ticks, rounded up. */
    const uint64_t ticks =
        (((uint64_t)milliseconds * (uint64_t)OSCfg_TickRate_Hz) + 999U) / 1000U;

    return (ticks < 0xFFFFFFFFU) ? (OS_TICK)ticks : (OS_TICK)0xFFFFFFFFU;
}

void mqtt_platform_console_write(const char *text)
{
    ucos3_board_write(true, (const uint8_t *)text, (uint32_t)strlen(text));
}

void mqtt_platform_output_write(const uint8_t *data, uint32_t length)
{
    ucos3_board_write(false, data, length);
}

uint64_t mqtt_platform_time_ms(void)
{
    /** @brief The tick count the last time it was read. */
    static OS_TICK last = 0U;
    /** @brief The times it has wrapped, as the high bits of the count. */
    static uint64_t wraps = 0U;
    /** @brief What the kernel said. */
    OS_ERR error = OS_ERR_NONE;
    /** @brief The count in 64 bits. */
    uint64_t ticks = 0U;
    /* The saved interrupt state, which the macro declares. */
    CPU_SR_ALLOC();

    CPU_CRITICAL_ENTER();
    {
        /** @brief The count now. */
        const OS_TICK now = OSTimeGet(&error);

        if (now < last) {
            wraps += (uint64_t)0x100000000U;
        }
        last = now;
        ticks = wraps + (uint64_t)now;
    }
    CPU_CRITICAL_EXIT();
    return (ticks * 1000U) / (uint64_t)OSCfg_TickRate_Hz;
}

uint64_t mqtt_platform_time_us(void)
{
    return mqtt_platform_time_ms() * UCOS3_MS_US;
}

uint32_t mqtt_platform_page_size(void)
{
    return 0U;
}

void mqtt_platform_sleep_ms(uint32_t milliseconds)
{
    /** @brief What the kernel said. */
    OS_ERR error = OS_ERR_NONE;
    /** @brief How long, at least one tick, so that the others can run. */
    const OS_TICK ticks = ucos3_ticks(milliseconds);

    OSTimeDly((ticks > 0U) ? ticks : 1U, OS_OPT_TIME_DLY, &error);
}

/** @brief Open the pair of sockets that wakes a wait, the first time. */
static void ucos3_prepare_wake(void)
{
    if (ucos3_wake[0] == MQTT_SOCKET_NONE) {
        if (mqtt_platform_socket_pair(&ucos3_wake[0], &ucos3_wake[1]) !=
            MQTT_OK) {
            ucos3_wake[0] = MQTT_SOCKET_NONE;
            ucos3_wake[1] = MQTT_SOCKET_NONE;
        }
    }
}

bool mqtt_platform_stop_requested(void)
{
    ucos3_prepare_wake();
    return ucos3_stopping;
}

void mqtt_platform_request_stop(void)
{
    /** @brief The byte sent. */
    const uint8_t byte = 1U;
    /** @brief Number of bytes taken. */
    uint32_t count = 0U;

    ucos3_prepare_wake();
    ucos3_stopping = true;
    if (ucos3_wake[1] != MQTT_SOCKET_NONE) {
        (void)mqtt_platform_write(ucos3_wake[1], &byte, 1U, &count);
    }
}

int32_t mqtt_platform_wake_handle(void)
{
    ucos3_prepare_wake();
    return ucos3_wake[0];
}

void mqtt_platform_wake_drain(void)
{
    /** @brief Where the bytes go. */
    uint8_t bytes[16];
    /** @brief Number of bytes read. */
    uint32_t count = 0U;

    if (ucos3_wake[0] != MQTT_SOCKET_NONE) {
        while (mqtt_platform_read(ucos3_wake[0], bytes, sizeof(bytes),
                                  &count) == MQTT_OK) {
            /* Each byte only says a stop was asked for. */
        }
    }
}

uint64_t mqtt_platform_cpu_us(void)
{
    return 0U;
}
