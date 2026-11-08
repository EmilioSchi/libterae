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
 * @file base_topic.h
 * @brief Topic level separator and wildcards.
 *
 * Values are bytes, so they compare directly with data read from a packet.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 4.7.1.1, 4.7.1.2, p.98; 4.7.1.3, p.99.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015,
 * 81 pages.
 * @li Cited: 4.7.1.1, 4.7.1.2, 4.7.1.3, p.57.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 *
 * [V31] MQTT V3.1 Protocol Specification, IBM and Eurotech, 42 pages.
 * @li Cited: "Topic level separator", "Multi-level wildcard",
 * "Single-level wildcard", p.41.
 * @li Source: public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html
 */
#ifndef MQTT_BASE_TOPIC_H
#define MQTT_BASE_TOPIC_H

/** @brief Forward slash, U+002F. */
#define MQTT_TOPIC_LEVEL_SEPARATOR 0x2FU
/** @brief Number sign, U+0023. */
#define MQTT_MULTI_LEVEL_WILDCARD  0x23U
/** @brief Plus sign, U+002B. */
#define MQTT_SINGLE_LEVEL_WILDCARD 0x2BU

#endif /* MQTT_BASE_TOPIC_H */
