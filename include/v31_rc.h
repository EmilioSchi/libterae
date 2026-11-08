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
 * @file v31_rc.h
 * @brief Connect return codes.
 *
 * An unsigned return code, one byte long, in byte 2 of the CONNACK Variable
 * header. Values 6-255 are reserved for future use.
 *
 * [V31] MQTT V3.1 Protocol Specification, IBM and Eurotech, 42 pages.
 * @li Cited: "Connect return code", p.11-12.
 * @li Source: public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html
 */
#ifndef MQTT_V31_RC_H
#define MQTT_V31_RC_H

/** @brief Connection Accepted. */
#define MQTT31_RC_ACCEPTED                      0U
/** @brief Refused: unacceptable protocol version. */
#define MQTT31_RC_UNACCEPTABLE_PROTOCOL_VERSION 1U
/** @brief Refused: identifier rejected. */
#define MQTT31_RC_IDENTIFIER_REJECTED           2U
/** @brief Refused: server unavailable. */
#define MQTT31_RC_SERVER_UNAVAILABLE            3U
/** @brief Refused: bad user name or password. */
#define MQTT31_RC_BAD_USER_NAME_OR_PASSWORD     4U
/** @brief Refused: not authorized. */
#define MQTT31_RC_NOT_AUTHORIZED                5U
/** @brief Lowest of the reserved values. */
#define MQTT31_RC_RESERVED_MIN                  6U

#endif /* MQTT_V31_RC_H */
