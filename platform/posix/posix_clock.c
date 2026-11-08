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
 * @file posix_clock.c
 * @brief The clock, sleeping, the page size and the program's output.
 */

#include <poll.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "base_platform.h"

/**
 * @brief Read the clock that only goes forward if the system has one, or else
 * the one that can be set.
 * @param[out] now The time.
 */
static void platform_clock(struct timespec *now)
{
#if defined(CLOCK_MONOTONIC)
    if (clock_gettime(CLOCK_MONOTONIC, now) != 0) {
        (void)clock_gettime(CLOCK_REALTIME, now);
    }
#else
    (void)clock_gettime(CLOCK_REALTIME, now);
#endif
}

void mqtt_platform_console_write(const char *text)
{
    /**
     * @brief Number of bytes written, or -1. A failure could only be told on
     * the console itself.
     */
    const ssize_t written = write(2, text, strlen(text));

    (void)written;
}

void mqtt_platform_output_write(const uint8_t *data, uint32_t length)
{
    /**
     * @brief Number of bytes written, or -1. What was not written is lost:
     * there is nowhere to tell.
     */
    const ssize_t written = write(1, data, (size_t)length);

    (void)written;
}

uint64_t mqtt_platform_time_ms(void)
{
    /** @brief The time now. */
    struct timespec now = {0};

    platform_clock(&now);
    return ((uint64_t)now.tv_sec * 1000U) + ((uint64_t)now.tv_nsec / 1000000U);
}

uint32_t mqtt_platform_page_size(void)
{
    /** @brief What the system says. */
    const long size = sysconf(_SC_PAGESIZE);

    return ((size > 0L) && (size <= 0x7FFFFFFFL)) ? (uint32_t)size : 0U;
}

uint64_t mqtt_platform_time_us(void)
{
    /** @brief The time now. */
    struct timespec now = {0};

    platform_clock(&now);
    return ((uint64_t)now.tv_sec * 1000000U) + ((uint64_t)now.tv_nsec / 1000U);
}

void mqtt_platform_sleep_ms(uint32_t milliseconds)
{
    (void)poll(NULL, 0U, (int)milliseconds);
}
