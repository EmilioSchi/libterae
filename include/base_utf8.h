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
 * @file base_utf8.h
 * @brief UTF-8 Encoded String limits.
 *
 * A 16-bit length comes before every string, so a UTF-8 Encoded String cannot
 * be longer than 65,535 bytes. [V31] mentions only the 16-bit length: see
 * v31_topic.h for its topic limit. RFC 3629 limits the characters that UTF-8
 * can encode.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 1.5.4, p.16-17.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015,
 * 81 pages.
 * @li Cited: 1.5.3, p.13.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 *
 * [V31] MQTT V3.1 Protocol Specification, IBM and Eurotech, 42 pages.
 * @li Cited: "strings are prefixed with two bytes", p.13.
 * @li Source: public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html
 *
 * [RFC3629] UTF-8, a transformation format of ISO 10646, November 2003, 14
 * pages.
 * @li Cited: 3, p.4.
 * @li Source: www.rfc-editor.org/info/rfc3629
 */
#ifndef MQTT_BASE_UTF8_H
#define MQTT_BASE_UTF8_H

/** @brief Bytes. */
#define MQTT_UTF8_MAX            65535U
/** @brief Highest code point that UTF-8 can encode, U+10FFFF. */
#define MQTT_UTF8_CODE_POINT_MAX 0x10FFFFU

#endif /* MQTT_BASE_UTF8_H */
