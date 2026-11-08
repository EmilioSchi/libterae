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
 * @file v31_protocol.h
 * @brief Protocol name and version.
 *
 * The protocol name, a UTF-encoded string, has exactly this capitalization. The
 * protocol version is an unsigned byte that identifies which revision of the
 * protocol the client uses.
 *
 * [V31] MQTT V3.1 Protocol Specification, IBM and Eurotech, 42 pages.
 * @li Cited: "Protocol name", "Protocol version", p.8.
 * @li Source: public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html
 */
#ifndef MQTT_V31_PROTOCOL_H
#define MQTT_V31_PROTOCOL_H

/** @brief Protocol name. */
#define MQTT31_PROTOCOL_NAME        "MQIsdp"
/** @brief Number of bytes in the protocol name. */
#define MQTT31_PROTOCOL_NAME_LENGTH 6U
/** @brief Protocol version for this revision of the protocol. */
#define MQTT31_PROTOCOL_VERSION     3U

#endif /* MQTT_V31_PROTOCOL_H */
