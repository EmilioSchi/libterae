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
 * @file base_utf8_check.h
 * @brief Checking the characters of a UTF-8 Encoded String.
 *
 * A receiver must reject bytes that are not well-formed UTF-8, which includes
 * code points in the surrogate range, and must reject the null character. A
 * BOM-like U+FEFF stays in the string.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 1.5.4, p.18.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015.
 * @li Cited: 1.5.3.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 *
 * [RFC3629] UTF-8, a transformation format of ISO 10646.
 * @li Cited: 3, 4.
 * @li Source: www.rfc-editor.org/rfc/rfc3629
 */
#ifndef MQTT_BASE_UTF8_CHECK_H
#define MQTT_BASE_UTF8_CHECK_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Decide whether bytes may appear in a UTF-8 Encoded String.
 * @param[in] data The bytes.
 * @param[in] length Number of bytes in data.
 * @return true if they are well-formed, and hold no null character and none of
 * the control characters and non-characters a sender should not use, which a
 * receiver may refuse.
 */
bool mqtt_utf8_is_valid(const uint8_t *data, uint32_t length);

/**
 * @brief Decide whether bytes are well-formed UTF-8 of any character, which is
 * all MQTT 3.1 asks of a string other than a topic.
 * @param[in] data The bytes.
 * @param[in] length Number of bytes in data.
 * @return true if they are.
 */
bool mqtt_utf8_is_well_formed(const uint8_t *data, uint32_t length);

/**
 * @brief Count the characters in a UTF-8 Encoded String.
 * @param[in] data The bytes of a string that has been found valid.
 * @param[in] length Number of bytes in data.
 * @return Number of characters.
 */
uint32_t mqtt_utf8_character_count(const uint8_t *data, uint32_t length);

#endif /* MQTT_BASE_UTF8_CHECK_H */
