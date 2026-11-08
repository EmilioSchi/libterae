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
 * @file v311_remaining.h
 * @brief Remaining Length of packets that have only one size.
 *
 * CONNECT, PUBLISH, SUBSCRIBE, SUBACK and UNSUBSCRIBE vary in size, so they
 * aren't listed. The encoding of this field is in base_remaining.h.
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015,
 * 81 pages.
 * @li Cited: 3.2.1, p.31; 3.4.1, p.37; 3.5.1, p.38; 3.6.1, p.38-39; 3.7.1,
 * p.39; 3.11.1, p.47; 3.12.1, 3.13.1, p.48; 3.14.1, p.49.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 */
#ifndef MQTT_V311_REMAINING_H
#define MQTT_V311_REMAINING_H

/** @brief Connect Acknowledge Flags and Connect Return code. */
#define MQTT311_CONNACK_REMAINING_LENGTH    2U
/** @brief Packet Identifier. */
#define MQTT311_PUBACK_REMAINING_LENGTH     2U
/** @brief Packet Identifier. */
#define MQTT311_PUBREC_REMAINING_LENGTH     2U
/** @brief Packet Identifier. */
#define MQTT311_PUBREL_REMAINING_LENGTH     2U
/** @brief Packet Identifier. */
#define MQTT311_PUBCOMP_REMAINING_LENGTH    2U
/** @brief Packet Identifier. */
#define MQTT311_UNSUBACK_REMAINING_LENGTH   2U
/** @brief No variable header or payload. */
#define MQTT311_PINGREQ_REMAINING_LENGTH    0U
/** @brief No variable header or payload. */
#define MQTT311_PINGRESP_REMAINING_LENGTH   0U
/** @brief No variable header or payload. */
#define MQTT311_DISCONNECT_REMAINING_LENGTH 0U

#endif /* MQTT_V311_REMAINING_H */
