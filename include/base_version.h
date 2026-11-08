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
 * @file base_version.h
 * @brief Protocol versions.
 *
 * Each value is the number the version carries in the CONNECT packet: Protocol
 * Version in MQTT 3.1 and in MQTT 5.0, Protocol Level in MQTT 3.1.1.
 *
 * [V31] MQTT V3.1 Protocol Specification.
 * @li Cited: 3.1, Protocol version number.
 * @li Source: public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015.
 * @li Cited: 3.1.2.2.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 3.1.2.2, p.42.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */
#ifndef MQTT_BASE_VERSION_H
#define MQTT_BASE_VERSION_H

/** @brief A protocol version. */
typedef enum {
    /** @brief MQTT 3.1. */
    MQTT_VERSION_31 = 3,
    /** @brief MQTT 3.1.1. */
    MQTT_VERSION_311 = 4,
    /** @brief MQTT 5.0. */
    MQTT_VERSION_5 = 5
} mqtt_version;

#endif /* MQTT_BASE_VERSION_H */
