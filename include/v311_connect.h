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
 * @file v311_connect.h
 * @brief CONNECT specific to MQTT 3.1.1.
 *
 * With CleanSession 1, Client and Server MUST each throw away any earlier
 * Session and begin afresh. The new Session lasts only until the Network
 * Connection closes.
 *
 * The other Connect Flags are in base_connect.h.
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015,
 * 81 pages.
 * @li Cited: 3.1.1, p.23; 3.1.2.4, p.24.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 */
#ifndef MQTT_V311_CONNECT_H
#define MQTT_V311_CONNECT_H

/** @brief Length of the variable header in bytes. */
#define MQTT311_CONNECT_VARIABLE_HEADER 10U
/** @brief Clean Session: Connect Flags bit 1. */
#define MQTT311_CLEAN_SESSION           0x02U

#endif /* MQTT_V311_CONNECT_H */
