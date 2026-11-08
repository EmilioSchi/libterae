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
 * @file v311_rc.h
 * @brief Connect Return codes.
 *
 * A one byte unsigned value in byte 2 of the CONNACK variable header. Values
 * 6-255 are reserved for future use.
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015,
 * 81 pages.
 * @li Cited: 3.2.2.3, Table 3.1, p.32-33.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 */
#ifndef MQTT_V311_RC_H
#define MQTT_V311_RC_H

/** @brief Connection accepted. */
#define MQTT311_RC_ACCEPTED                      0x00U
/** @brief Refused, unacceptable protocol version. */
#define MQTT311_RC_UNACCEPTABLE_PROTOCOL_VERSION 0x01U
/** @brief Refused, identifier rejected. */
#define MQTT311_RC_IDENTIFIER_REJECTED           0x02U
/** @brief Refused, Server unavailable. */
#define MQTT311_RC_SERVER_UNAVAILABLE            0x03U
/** @brief Refused, bad user name or password. */
#define MQTT311_RC_BAD_USER_NAME_OR_PASSWORD     0x04U
/** @brief Refused, not authorized. */
#define MQTT311_RC_NOT_AUTHORIZED                0x05U
/** @brief Lowest of the reserved values. */
#define MQTT311_RC_RESERVED_MIN                  0x06U

#endif /* MQTT_V311_RC_H */
