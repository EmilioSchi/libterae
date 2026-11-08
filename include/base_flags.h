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
 * @file base_flags.h
 * @brief PUBLISH flags in the Fixed Header.
 *
 * In a PUBLISH packet, DUP marks a repeated delivery, QoS gives the Quality of
 * Service, and RETAIN marks a retained message.
 *
 * [V31] also sets DUP on retries of PUBREL, SUBSCRIBE and UNSUBSCRIBE: see
 * v31_flags.h. [V311] and [V5] reserve those bits: see v311_flags.h and
 * v5_flags.h.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 2.1.3, Table 2-2, p.22; 3.3.1.1, p.53; 3.3.1.2, 3.3.1.3, p.54.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015,
 * 81 pages.
 * @li Cited: 2.2.2, Table 2.2, p.17; 3.3.1.1, p.33; 3.3.1.2, 3.3.1.3, p.34.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 *
 * [V31] MQTT V3.1 Protocol Specification, IBM and Eurotech, 42 pages.
 * @li Cited: "Flags", "DUP", "QoS", p.5; "RETAIN", p.6.
 * @li Source: public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html
 */
#ifndef MQTT_BASE_FLAGS_H
#define MQTT_BASE_FLAGS_H

/** @brief Byte 1, bit 3. */
#define MQTT_DUP      0x08U
/** @brief Byte 1, bits 2-1. */
#define MQTT_QOS_MASK 0x06U
/** @brief Position of the lowest QoS bit. */
#define MQTT_QOS_POS  1U
/** @brief Byte 1, bit 0. */
#define MQTT_RETAIN   0x01U

#endif /* MQTT_BASE_FLAGS_H */
