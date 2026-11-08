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
 * @file v311_protocol.h
 * @brief Protocol Name and Protocol Level.
 *
 * The Protocol Name, a UTF-8 string, has exactly this capitalization. Later
 * versions of the specification will keep its text, position and length the
 * same. The Protocol Level is an unsigned byte that identifies which revision
 * of the protocol is in use.
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015,
 * 81 pages.
 * @li Cited: 3.1.2.1, p.23; 3.1.2.2, p.24.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 */
#ifndef MQTT_V311_PROTOCOL_H
#define MQTT_V311_PROTOCOL_H

/** @brief Protocol Name. */
#define MQTT311_PROTOCOL_NAME        "MQTT"
/** @brief Number of bytes in the Protocol Name. */
#define MQTT311_PROTOCOL_NAME_LENGTH 4U
/** @brief Protocol Level for this revision of the protocol. */
#define MQTT311_PROTOCOL_LEVEL       4U

#endif /* MQTT_V311_PROTOCOL_H */
