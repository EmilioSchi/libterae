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
 * @file base_packet_id.h
 * @brief Packet Identifier.
 *
 * A 16-bit Packet Identifier, never 0, appears in SUBSCRIBE and UNSUBSCRIBE
 * packets, and in PUBLISH packets with QoS above 0. [V31] names it Message ID
 * and also treats 0 as invalid.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 1.5.2, p.16; 2.2.1, p.23-24.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015,
 * 81 pages.
 * @li Cited: 2.3.1, p.20.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 *
 * [V31] MQTT V3.1 Protocol Specification, IBM and Eurotech, 42 pages.
 * @li Cited: "The message identifier is present", p.13.
 * @li Source: public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html
 */
#ifndef MQTT_BASE_PACKET_ID_H
#define MQTT_BASE_PACKET_ID_H

/** @brief Lowest valid value. */
#define MQTT_PACKET_ID_MIN 1U
/** @brief Highest value that fits in the 16 bits of the field. */
#define MQTT_PACKET_ID_MAX 65535U

#endif /* MQTT_BASE_PACKET_ID_H */
