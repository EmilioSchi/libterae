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
 * @file linux_random.c
 * @brief Random bytes, from the kernel's generator through getrandom.
 *
 * getrandom needs no file, so it works where /dev is missing. With no flags it
 * waits only until the generator has been seeded, early in a boot. A request
 * longer than 256 bytes may be cut short by a signal, and the rest is then
 * asked for again.
 */

#include <errno.h>
#include <stddef.h>
#include <sys/random.h>

#include "base_platform.h"

/** @brief The most bytes one call asks for, which a ssize_t always holds. */
#define RANDOM_CALL_MAX 32768U

mqtt_status mqtt_platform_random_bytes(uint8_t *data, uint32_t length)
{
    /** @brief Number of bytes given so far. */
    uint32_t done = 0U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    while ((status == MQTT_OK) && (done < length)) {
        /** @brief Number of bytes to ask for. */
        const uint32_t want = ((length - done) < RANDOM_CALL_MAX) ?
                                  (length - done) :
                                  RANDOM_CALL_MAX;
        /** @brief Number of bytes given, or -1. */
        const ssize_t got = getrandom(&data[done], (size_t)want, 0U);

        if (got > 0) {
            done += (uint32_t)got;
        } else if ((got < 0) && (errno == EINTR)) {
            /* A signal came before any byte did, so it is asked again. */
        } else {
            status = MQTT_INVALID_STATE;
        }
    }
    return status;
}
