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
 * @file win32_system.c
 * @brief The clock, sleeping, the page size, the program's output, and
 * stopping, for Windows.
 *
 * Windows has no signals to stop a program with. The console sends a control
 * event instead, on a thread of its own, and the handler does what a signal
 * handler does elsewhere: it notes the stop and sends a byte on a pair of
 * loopback sockets, so that a wait ends at once.
 */

#include <stddef.h>
#include <string.h>
#include <winsock2.h>
#include <windows.h>

#include "base_platform.h"
#include "win32_internal.h"

/** @brief Microseconds in a second. */
#define WIN32_SECOND_US 1000000U
/** @brief Number of 100-nanosecond units in a microsecond. */
#define WIN32_UNITS_PER_US 10U

/** @brief Number of stops asked for, which another thread may change. */
static volatile LONG win32_stops = 0;
/** @brief The pair of sockets that wakes a wait: the end read, then written. */
static int32_t win32_wake[2] = {MQTT_SOCKET_NONE, MQTT_SOCKET_NONE};

/**
 * @brief Write bytes to one of the program's outputs.
 * @param[in] which STD_OUTPUT_HANDLE or STD_ERROR_HANDLE.
 * @param[in] data The bytes.
 * @param[in] length Number of bytes.
 */
static void win32_output(DWORD which, const void *data, uint32_t length)
{
    /** @brief Number of bytes written. */
    DWORD written = 0U;

    (void)WriteFile(GetStdHandle(which), data, (DWORD)length, &written, NULL);
}

void mqtt_platform_console_write(const char *text)
{
    win32_output(STD_ERROR_HANDLE, text, (uint32_t)strlen(text));
}

void mqtt_platform_output_write(const uint8_t *data, uint32_t length)
{
    win32_output(STD_OUTPUT_HANDLE, data, length);
}

uint64_t mqtt_platform_time_us(void)
{
    /** @brief Ticks of the counter in a second, which never changes. */
    static LARGE_INTEGER frequency = {{0U, 0}};
    /** @brief The counter now. */
    LARGE_INTEGER now;
    /** @brief The answer. */
    uint64_t micros = 0U;

    if (frequency.QuadPart == 0) {
        (void)QueryPerformanceFrequency(&frequency);
    }
    if ((frequency.QuadPart > 0) && QueryPerformanceCounter(&now)) {
        /** @brief The ticks, and the ticks per second. */
        const uint64_t ticks = (uint64_t)now.QuadPart;
        /** @brief Ticks of the counter in a second. */
        const uint64_t rate = (uint64_t)frequency.QuadPart;

        micros = ((ticks / rate) * WIN32_SECOND_US) +
                 (((ticks % rate) * WIN32_SECOND_US) / rate);
    }
    return micros;
}

uint64_t mqtt_platform_time_ms(void)
{
    return mqtt_platform_time_us() / 1000U;
}

uint32_t mqtt_platform_page_size(void)
{
    /** @brief What the system says of itself. */
    SYSTEM_INFO information;

    GetSystemInfo(&information);
    return (uint32_t)information.dwPageSize;
}

void mqtt_platform_sleep_ms(uint32_t milliseconds)
{
    Sleep((DWORD)milliseconds);
}

/** @brief Open the pair of sockets that wakes a wait, the first time. */
static void win32_prepare_wake(void)
{
    if (win32_wake[0] == MQTT_SOCKET_NONE) {
        if (mqtt_platform_socket_pair(&win32_wake[0], &win32_wake[1]) !=
            MQTT_OK) {
            win32_wake[0] = MQTT_SOCKET_NONE;
            win32_wake[1] = MQTT_SOCKET_NONE;
        }
    }
}

/** @brief Note that the program is to stop, and wake the wait. */
static void win32_note_stop(void)
{
    /** @brief The byte sent. */
    const uint8_t byte = 1U;
    /** @brief Number of bytes taken. */
    uint32_t count = 0U;

    (void)InterlockedExchange(&win32_stops, 1);
    if (win32_wake[1] != MQTT_SOCKET_NONE) {
        (void)mqtt_platform_write(win32_wake[1], &byte, 1U, &count);
    }
}

/**
 * @brief Take a console's control event: Ctrl+C, Ctrl+Break, or the console
 * closing.
 * @param[in] kind The event.
 * @return TRUE, as the event is handled.
 */
static BOOL WINAPI win32_on_control(DWORD kind)
{
    (void)kind;
    win32_note_stop();
    return TRUE;
}

bool mqtt_platform_stop_requested(void)
{
    /** @brief true once the console's events are being watched. */
    static bool watching = false;

    win32_prepare_wake();
    if (!watching) {
        watching = true;
        (void)SetConsoleCtrlHandler(&win32_on_control, TRUE);
    }
    return InterlockedCompareExchange(&win32_stops, 0, 0) != 0;
}

void mqtt_platform_request_stop(void)
{
    win32_prepare_wake();
    win32_note_stop();
}

int32_t mqtt_platform_wake_handle(void)
{
    win32_prepare_wake();
    return win32_wake[0];
}

void mqtt_platform_wake_drain(void)
{
    /** @brief Where the bytes go. */
    uint8_t bytes[16];
    /** @brief Number of bytes read. */
    uint32_t count = 0U;

    if (win32_wake[0] != MQTT_SOCKET_NONE) {
        while (mqtt_platform_read(win32_wake[0], bytes, sizeof(bytes),
                                  &count) == MQTT_OK) {
            /* Each byte only says a stop was asked for. */
        }
    }
}

/**
 * @brief Read a time Windows gives in 100-nanosecond units.
 * @param[in] time The time.
 * @return Microseconds.
 */
static uint64_t win32_micros(const FILETIME *time)
{
    return ((((uint64_t)time->dwHighDateTime) << 32U) |
            (uint64_t)time->dwLowDateTime) /
           WIN32_UNITS_PER_US;
}

uint64_t mqtt_platform_cpu_us(void)
{
    /** @brief When the process started, and ended, which aren't used. */
    FILETIME created;
    /** @brief When it ended. */
    FILETIME ended;
    /** @brief Time in the kernel. */
    FILETIME kernel;
    /** @brief Time in the program. */
    FILETIME user;
    /** @brief The answer. */
    uint64_t micros = 0U;

    if (GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel,
                        &user)) {
        micros = win32_micros(&kernel) + win32_micros(&user);
    }
    return micros;
}
