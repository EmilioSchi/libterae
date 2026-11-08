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
 * @file posix_random.c
 * @brief Random bytes, from /dev/urandom.
 *
 * POSIX.1-2008 has no call that gives random bytes, and names no file that
 * does. Every system this folder is built for has /dev/urandom, which gives
 * bytes from the kernel's generator, so this is the one thing here that POSIX
 * doesn't promise. The file is opened for each call and closed after it, so
 * nothing stays open between calls, or in a program the process starts.
 */

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <unistd.h>

#include "base_platform.h"

/** @brief The most bytes one read asks for, which a ssize_t always holds. */
#define RANDOM_READ_MAX 32768U

mqtt_status mqtt_platform_random_bytes(uint8_t *data, uint32_t length)
{
    /** @brief The file, or -1 if it could not be opened. */
    const int file = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    /** @brief Number of bytes read so far. */
    uint32_t done = 0U;
    /** @brief The outcome. */
    mqtt_status status = (file >= 0) ? MQTT_OK : MQTT_INVALID_STATE;

    while ((status == MQTT_OK) && (done < length)) {
        /** @brief Number of bytes to ask for. */
        const uint32_t want = ((length - done) < RANDOM_READ_MAX) ?
                                  (length - done) :
                                  RANDOM_READ_MAX;
        /** @brief Number of bytes read, or -1. */
        const ssize_t got = read(file, &data[done], (size_t)want);

        if (got > 0) {
            done += (uint32_t)got;
        } else if ((got < 0) && (errno == EINTR)) {
            /* A signal came before any byte did, so it is asked again. */
        } else {
            status = MQTT_INVALID_STATE;
        }
    }
    if (file >= 0) {
        (void)close(file);
    }
    return status;
}
