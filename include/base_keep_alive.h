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
 * @file base_keep_alive.h
 * @brief Keep Alive.
 *
 * A 16-bit time interval in seconds, sent in the CONNECT Variable Header. A
 * Client has to send a packet within each interval, using PINGREQ if it has
 * nothing else to send. A Server that receives nothing for one and a half
 * intervals closes the connection. A value of 0 turns this off.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 3.1.2.10, p.34.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015,
 * 81 pages.
 * @li Cited: 3.1.2.10, p.27-28.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 *
 * [V31] MQTT V3.1 Protocol Specification, IBM and Eurotech, 42 pages.
 * @li Cited: "Keep Alive timer", p.11.
 * @li Source: public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html
 */
#ifndef MQTT_BASE_KEEP_ALIVE_H
#define MQTT_BASE_KEEP_ALIVE_H

/** @brief Turns the mechanism off. */
#define MQTT_KEEP_ALIVE_OFF               0U
/** @brief Highest value, in seconds: 18 hours, 12 minutes and 15 seconds. */
#define MQTT_KEEP_ALIVE_MAX               65535U
/**
 * @brief A Server's limit is the Keep Alive times this, divided by the
 * denominator.
 */
#define MQTT_KEEP_ALIVE_GRACE_NUMERATOR   3U
/**
 * @brief Divides the Keep Alive after the numerator, which keeps the sum in
 * whole numbers.
 */
#define MQTT_KEEP_ALIVE_GRACE_DENOMINATOR 2U

#endif /* MQTT_BASE_KEEP_ALIVE_H */
