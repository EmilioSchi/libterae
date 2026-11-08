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
 * @file base_utf8_t.h
 * @brief UTF-8 Encoded String.
 *
 * Text travels as UTF-8, preceded by a length that counts the string's bytes.
 * The struct refers to the bytes and does not own them.
 *
 * Byte is uint8_t and Two Byte Integer is uint16_t.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 1.5.2, 1.5.4, p.16.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015,
 * 81 pages.
 * @li Cited: 1.5.2, 1.5.3, p.13.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 *
 * [V31] MQTT V3.1 Protocol Specification, IBM and Eurotech, 42 pages.
 * @li Cited: "strings are prefixed with two bytes", p.13.
 * @li Source: public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html
 */
#ifndef MQTT_BASE_UTF8_T_H
#define MQTT_BASE_UTF8_T_H

#include <stdint.h>

/** @brief Reference to a UTF-8 Encoded String. It does not own the bytes. */
typedef struct {
    /** @brief Number of bytes in data. */
    uint16_t length;
    /** @brief The string's UTF-8 bytes, if length > 0. */
    const uint8_t *data;
} mqtt_utf8;

#endif /* MQTT_BASE_UTF8_T_H */
