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
 * @file v5_connack.h
 * @brief Connect Acknowledge Flags.
 *
 * Byte 1 of the CONNACK Variable Header. Bits 7-1 are reserved: they MUST all
 * be 0.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 3.2.2.1, 3.2.2.1.1, p.45.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */
#ifndef MQTT_V5_CONNACK_H
#define MQTT_V5_CONNACK_H

/** @brief Session Present: bit 0. */
#define MQTT5_SESSION_PRESENT  0x01U
/** @brief Reserved: bits 7-1. */
#define MQTT5_CONNACK_RESERVED 0xFEU

#endif /* MQTT_V5_CONNACK_H */
