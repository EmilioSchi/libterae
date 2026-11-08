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
 * @file v31_remaining.h
 * @brief Remaining Length of messages that have only one size.
 *
 * CONNECT, PUBLISH, SUBSCRIBE, SUBACK and UNSUBSCRIBE vary in size, so they
 * aren't listed. The encoding of this field is in base_remaining.h.
 *
 * [V31] MQTT V3.1 Protocol Specification, IBM and Eurotech, 42 pages.
 * @li Cited: "CONNACK", p.18; "PUBACK", p.22; "PUBREC", p.23; "PUBREL", p.24;
 * "PUBCOMP", p.26; "UNSUBACK", p.33; "PINGREQ", p.34; "PINGRESP", "DISCONNECT",
 * p.35.
 * @li Source: public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html
 */
#ifndef MQTT_V31_REMAINING_H
#define MQTT_V31_REMAINING_H

/** @brief Reserved byte and return code. */
#define MQTT31_CONNACK_REMAINING_LENGTH    2U
/** @brief Message ID. */
#define MQTT31_PUBACK_REMAINING_LENGTH     2U
/** @brief Message ID. */
#define MQTT31_PUBREC_REMAINING_LENGTH     2U
/** @brief Message ID. */
#define MQTT31_PUBREL_REMAINING_LENGTH     2U
/** @brief Message ID. */
#define MQTT31_PUBCOMP_REMAINING_LENGTH    2U
/** @brief Message ID. */
#define MQTT31_UNSUBACK_REMAINING_LENGTH   2U
/** @brief No variable header or payload. */
#define MQTT31_PINGREQ_REMAINING_LENGTH    0U
/** @brief No variable header or payload. */
#define MQTT31_PINGRESP_REMAINING_LENGTH   0U
/** @brief No variable header or payload. */
#define MQTT31_DISCONNECT_REMAINING_LENGTH 0U

#endif /* MQTT_V31_REMAINING_H */
