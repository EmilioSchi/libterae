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
 * @file base_remaining.h
 * @brief Remaining Length.
 *
 * How many bytes of the current packet come after this field: the Variable
 * Header plus the Payload, not counting the bytes of this field itself. Each
 * encoded byte carries seven data bits in bits 6-0, and its top bit says
 * whether another byte follows.
 *
 * Decoders check the range before multiplier *= 128. [V311] still prints the
 * deleted decoder next to the corrected one, and the [V31] decoder has no range
 * check. [V5] also requires the minimum number of bytes; [V311] and [V31] have
 * no such rule.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 1.5.5, Table 1-1, p.18; 2.1.4, p.23.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015,
 * 81 pages.
 * @li Cited: 2.2.3, Table 2.4, p.18; "Malformed Remaining Length", p.19.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 *
 * [V31] MQTT V3.1 Protocol Specification, IBM and Eurotech, 42 pages.
 * @li Cited: "Remaining Length", p.6-7.
 * @li Source: public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html
 */
#ifndef MQTT_BASE_REMAINING_H
#define MQTT_BASE_REMAINING_H

/** @brief Longest encoding of this field, in bytes. */
#define MQTT_REMAINING_LENGTH_BYTES        4U
/** @brief 0xFF, 0xFF, 0xFF, 0x7F on the wire. */
#define MQTT_REMAINING_LENGTH_MAX          268435455U
/** @brief Continuation bit. */
#define MQTT_REMAINING_LENGTH_CONTINUATION 0x80U
/** @brief Seven data bits. */
#define MQTT_REMAINING_LENGTH_MASK         0x7FU

#endif /* MQTT_BASE_REMAINING_H */
