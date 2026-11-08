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
 * @file base_string.c
 * @brief Copying, comparing, wiping and measuring bytes, with no C library.
 */

#include <stddef.h>

#include "base_string.h"

mqtt_status mqtt_bytes_copy(uint8_t *destination, uint32_t capacity,
                            const uint8_t *source, uint32_t length)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if ((length > capacity) ||
        ((length > 0U) && ((destination == NULL) || (source == NULL)))) {
        status = MQTT_INVALID_ARGUMENT;
    } else {
        mqtt_bytes_put(destination, source, length);
    }
    return status;
}

void mqtt_bytes_put(uint8_t *destination, const uint8_t *source,
                    uint32_t length)
{
#if defined(MQTT_PORT_MEMORY)
    /* memcpy may not be given NULL, even for no bytes. */
    if ((length > 0U) && (destination != NULL) && (source != NULL)) {
        (void)mqtt_port_memcpy(destination, source, (size_t)length);
    }
#else
    /** @brief Index of the byte being copied. */
    uint32_t index = 0U;
    /** @brief Number of bytes to copy: none to or from NULL. */
    const uint32_t count =
        ((destination != NULL) && (source != NULL)) ? length : 0U;

    while (index < count) {
        destination[index] = source[index];
        index += 1U;
    }
#endif
}

bool mqtt_bytes_equal(const uint8_t *left, const uint8_t *right,
                      uint32_t length)
{
    /** @brief The answer. */
    bool equal = true;

    if ((length > 0U) && ((left == NULL) || (right == NULL))) {
        equal = false;
    } else {
        /** @brief Every bit that differs, from any byte. */
        uint32_t difference = 0U;
        /** @brief Index of the byte being compared. */
        uint32_t index = 0U;

        while (index < length) {
            difference |= (uint32_t)left[index] ^ (uint32_t)right[index];
            index += 1U;
        }
        equal = (difference == 0U);
    }
    return equal;
}

void mqtt_bytes_wipe(uint8_t *destination, uint32_t length)
{
    /** @brief The bytes, written through volatile so that each write stays. */
    volatile uint8_t *const target = destination;

    if (target != NULL) {
        /** @brief Index of the byte being wiped. */
        uint32_t index = 0U;

        while (index < length) {
            target[index] = 0U;
            index += 1U;
        }
    }
}

bool mqtt_text_length(const char *text, uint32_t limit, uint32_t *length)
{
    /** @brief The answer. */
    bool found = false;

    if (length != NULL) {
        /** @brief Index of the byte being looked at. */
        uint32_t index = 0U;

        if (text != NULL) {
            while ((index < limit) && (!found)) {
                if (text[index] == '\0') {
                    found = true;
                } else {
                    index += 1U;
                }
            }
        }
        *length = found ? index : 0U;
    }
    return found;
}
