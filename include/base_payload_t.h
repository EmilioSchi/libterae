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
 * @file base_payload_t.h
 * @brief PUBLISH Payload.
 *
 * The Payload is the Application Message: the bytes from the end of the
 * Variable Header to the end of the packet. It has no length field of its own,
 * so its length is worked out from the Remaining Length and can need more than
 * the 16 bits of mqtt_binary. It can be empty. The struct refers to the bytes
 * and does not own them.
 *
 * A Will Message is not a Payload of this kind: it has a 16-bit length, as in
 * base_binary_t.h.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 3.3.3, p.60.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015,
 * 81 pages.
 * @li Cited: 3.3.3, p.36.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 *
 * [V31] MQTT V3.1 Protocol Specification, IBM and Eurotech, 42 pages.
 * @li Cited: "Payload", p.21.
 * @li Source: public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html
 */
#ifndef MQTT_BASE_PAYLOAD_T_H
#define MQTT_BASE_PAYLOAD_T_H

#include <stdint.h>

/** @brief Reference to a PUBLISH Payload. It does not own the bytes. */
typedef struct {
    /** @brief Number of bytes in data. */
    uint32_t length;
    /** @brief The Payload's bytes, if length > 0. */
    const uint8_t *data;
} mqtt_payload;

#endif /* MQTT_BASE_PAYLOAD_T_H */
