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
 * @file v5_protocol.h
 * @brief Protocol Name and Protocol Version.
 *
 * The Protocol Name, a UTF-8 Encoded String, has exactly this capitalization.
 * Later versions of the specification will keep its text, position and length
 * the same. The Protocol Version is an unsigned byte that identifies which
 * revision of the protocol is in use.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 3.1.2.1, p.30; 3.1.2.2, p.31.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */
#ifndef MQTT_V5_PROTOCOL_H
#define MQTT_V5_PROTOCOL_H

/** @brief Protocol Name. */
#define MQTT5_PROTOCOL_NAME        "MQTT"
/** @brief Number of bytes in the Protocol Name. */
#define MQTT5_PROTOCOL_NAME_LENGTH 4U
/** @brief Protocol Version for this revision of the protocol. */
#define MQTT5_PROTOCOL_VERSION     5U

#endif /* MQTT_V5_PROTOCOL_H */
