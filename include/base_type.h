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
 * @file base_type.h
 * @brief MQTT Control Packet types.
 *
 * Stored as a 4-bit unsigned number in bits 7-4 of byte 1.
 *
 * Value 15 differs by version: see v31_type.h, v311_type.h and v5_type.h.
 * DISCONNECT flows from Client to Server in [V311] and [V31], and in both
 * directions in [V5]. [V31] calls these Message Types.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 2.1.2, Table 2-1, p.21-22.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015,
 * 81 pages.
 * @li Cited: 2.2.1, Table 2.1, p.16-17.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 *
 * [V31] MQTT V3.1 Protocol Specification, IBM and Eurotech, 42 pages.
 * @li Cited: "Message Type", p.4-5.
 * @li Source: public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html
 */
#ifndef MQTT_BASE_TYPE_H
#define MQTT_BASE_TYPE_H

/** @brief Reserved. */
#define MQTT_RESERVED    0U
/** @brief Connection request. */
#define MQTT_CONNECT     1U
/** @brief Connect acknowledgment. */
#define MQTT_CONNACK     2U
/** @brief Publish message. */
#define MQTT_PUBLISH     3U
/** @brief Publish acknowledgment (QoS 1). */
#define MQTT_PUBACK      4U
/** @brief Publish received (step 1 of the QoS 2 exchange). */
#define MQTT_PUBREC      5U
/** @brief Publish release (step 2 of the QoS 2 exchange). */
#define MQTT_PUBREL      6U
/** @brief Publish complete (step 3 of the QoS 2 exchange). */
#define MQTT_PUBCOMP     7U
/** @brief Subscribe request. */
#define MQTT_SUBSCRIBE   8U
/** @brief Subscribe acknowledgment. */
#define MQTT_SUBACK      9U
/** @brief Unsubscribe request. */
#define MQTT_UNSUBSCRIBE 10U
/** @brief Unsubscribe acknowledgment. */
#define MQTT_UNSUBACK    11U
/** @brief PING request. */
#define MQTT_PINGREQ     12U
/** @brief PING response. */
#define MQTT_PINGRESP    13U
/** @brief Disconnect notification. */
#define MQTT_DISCONNECT  14U

#endif /* MQTT_BASE_TYPE_H */
