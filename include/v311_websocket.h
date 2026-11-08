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
 * @file v311_websocket.h
 * @brief WebSocket subprotocol.
 *
 * A Client MUST offer this name among its WebSocket Sub Protocols, and the
 * Server MUST select and return this same name. Every MQTT Control Packet MUST
 * travel in WebSocket binary data frames.
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015,
 * 81 pages.
 * @li Cited: 6 Using WebSocket as a network transport, p.65.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 */
#ifndef MQTT_V311_WEBSOCKET_H
#define MQTT_V311_WEBSOCKET_H

/** @brief WebSocket subprotocol name. */
#define MQTT311_WEBSOCKET_SUBPROTOCOL "mqtt"

#endif /* MQTT_V311_WEBSOCKET_H */
