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
 * @file base_client_id.h
 * @brief Client Identifier length.
 *
 * In [V5] and [V311] a Server MUST accept a ClientID of 1 to 23 bytes once
 * encoded in UTF-8, and MAY accept longer ones. In [V31] the Client ID has 1 to
 * 23 characters, and the server refuses anything longer.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 3.1.3.1, p.40.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015,
 * 81 pages.
 * @li Cited: 3.1.3.1, p.29.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 *
 * [V31] MQTT V3.1 Protocol Specification, IBM and Eurotech, 42 pages.
 * @li Cited: "Client Identifier", p.16.
 * @li Source: public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html
 */
#ifndef MQTT_BASE_CLIENT_ID_H
#define MQTT_BASE_CLIENT_ID_H

/** @brief Lower end of the Client Identifier length range. */
#define MQTT_CLIENT_ID_MIN 1U
/** @brief Upper end of the Client Identifier length range. */
#define MQTT_CLIENT_ID_MAX 23U

#endif /* MQTT_BASE_CLIENT_ID_H */
