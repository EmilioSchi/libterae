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
 * @file base_connect.h
 * @brief Connect Flags byte in the CONNECT Variable Header.
 *
 * Bit 1 differs by version: see MQTT31_CLEAN_SESSION, MQTT311_CLEAN_SESSION and
 * MQTT5_CLEAN_START.
 *
 * A Password without a User Name is invalid in [V311] and [V31], and allowed in
 * [V5].
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 3.1.2.3, 3.1.2.5, p.31-32; 3.1.2.6, 3.1.2.7, 3.1.2.8, 3.1.2.9,
 * p.33.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015,
 * 81 pages.
 * @li Cited: 3.1.2.3, p.24; 3.1.2.5, 3.1.2.6, 3.1.2.7, p.26; 3.1.2.8, 3.1.2.9,
 * p.27.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 *
 * [V31] MQTT V3.1 Protocol Specification, IBM and Eurotech, 42 pages.
 * @li Cited: "Connect flags", p.8; "Will flag", "Will QoS", p.9;
 * "Will Retain flag", "User name and password flags", p.10.
 * @li Source: public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html
 */
#ifndef MQTT_BASE_CONNECT_H
#define MQTT_BASE_CONNECT_H

/** @brief User Name Flag: bit 7. */
#define MQTT_USER_NAME_FLAG   0x80U
/** @brief Password Flag: bit 6. */
#define MQTT_PASSWORD_FLAG    0x40U
/** @brief Will Retain: bit 5. */
#define MQTT_WILL_RETAIN      0x20U
/** @brief Will QoS: bits 4 and 3. */
#define MQTT_WILL_QOS_MASK    0x18U
/** @brief Position of the lowest Will QoS bit. */
#define MQTT_WILL_QOS_POS     3U
/** @brief Will Flag: bit 2. */
#define MQTT_WILL_FLAG        0x04U
/** @brief Reserved: bit 0. */
#define MQTT_CONNECT_RESERVED 0x01U

#endif /* MQTT_BASE_CONNECT_H */
