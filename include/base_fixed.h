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
 * @file base_fixed.h
 * @brief Fixed Header.
 *
 * Every packet starts with a Fixed Header. In its first byte, bits 7-4 give the
 * MQTT Control Packet type and bits 3-0 hold flags whose meaning depends on
 * that type. The Remaining Length comes next: see base_remaining.h.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 1.5.5, p.18; 2.1.1, 2.1.2, p.21; 2.1.3, p.22; 2.1.4, p.23.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015,
 * 81 pages.
 * @li Cited: 2.2, 2.2.1, p.16; 2.2.2, p.17; 2.2.3, p.18.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 *
 * [V31] MQTT V3.1 Protocol Specification, IBM and Eurotech, 42 pages.
 * @li Cited: "Fixed header", "Message Type", p.4; "Flags", p.5;
 * "Remaining Length", p.6-7.
 * @li Source: public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html
 */
#ifndef MQTT_BASE_FIXED_H
#define MQTT_BASE_FIXED_H

/** @brief Position of the MQTT Control Packet type: bits 7-4. */
#define MQTT_TYPE_POS           4U
/** @brief Flags: bits 3-0. */
#define MQTT_FLAGS_MASK         0x0FU
/**
 * @brief Longest Fixed Header, in bytes: byte 1 and the longest Remaining
 * Length.
 */
#define MQTT_FIXED_HEADER_BYTES 5U

#endif /* MQTT_BASE_FIXED_H */
