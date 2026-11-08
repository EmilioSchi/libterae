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
 * @file macos_random.c
 * @brief Random bytes, from the kernel's generator through getentropy.
 *
 * getentropy needs no file, so it works in a sandbox with no /dev. It gives at
 * most 256 bytes a call, so a longer request takes several.
 */

#include <stddef.h>
#include <sys/random.h>

#include "base_platform.h"

/** @brief The most bytes getentropy gives in one call. */
#define RANDOM_CALL_MAX 256U

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

        if (getentropy(&data[done], (size_t)want) == 0) {
            done += want;
        } else {
            status = MQTT_INVALID_STATE;
        }
    }
    return status;
}
