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
 * @file v311_subscribe.h
 * @brief Requested QoS in the SUBSCRIBE payload.
 *
 * The Requested QoS byte uses only its low 2 bits; the other 6 are reserved. If
 * a reserved payload bit is set, or the QoS is anything other than 0, 1 or 2,
 * the Server MUST regard this as a malformed SUBSCRIBE packet and close its
 * Network Connection.
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015,
 * 81 pages.
 * @li Cited: 3.8.3, p.41.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 */
#ifndef MQTT_V311_SUBSCRIBE_H
#define MQTT_V311_SUBSCRIBE_H

/** @brief QoS: bits 1-0. */
#define MQTT311_REQUESTED_QOS_MASK     0x03U
/** @brief Reserved: bits 7-2. */
#define MQTT311_REQUESTED_QOS_RESERVED 0xFCU

#endif /* MQTT_V311_SUBSCRIBE_H */
