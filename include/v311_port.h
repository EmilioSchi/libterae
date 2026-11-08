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
 * @file v311_port.h
 * @brief TCP ports.
 *
 * IANA has registered TCP port 1883 for plain MQTT and TCP port 8883 for MQTT
 * over TLS.
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015,
 * 81 pages.
 * @li Cited: 4.2, p.52.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 */
#ifndef MQTT_V311_PORT_H
#define MQTT_V311_PORT_H

/** @brief Non TLS. */
#define MQTT311_PORT     1883U
/** @brief TLS. */
#define MQTT311_PORT_TLS 8883U

#endif /* MQTT_V311_PORT_H */
