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
 * @file v5_flags.h
 * @brief Fixed Header flags other than PUBLISH.
 *
 * A flag bit shown as Reserved MUST have exactly the value given here. A packet
 * with any other flags is a Malformed Packet. PUBLISH flags are in
 * base_flags.h.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 2.1.3, Table 2-2, p.22-23.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */
#ifndef MQTT_V5_FLAGS_H
#define MQTT_V5_FLAGS_H

/** @brief All packets not listed below, other than PUBLISH. */
#define MQTT5_RESERVED_FLAGS    0x00U
/** @brief 0, 0, 1, 0. */
#define MQTT5_PUBREL_FLAGS      0x02U
/** @brief 0, 0, 1, 0. */
#define MQTT5_SUBSCRIBE_FLAGS   0x02U
/** @brief 0, 0, 1, 0. */
#define MQTT5_UNSUBSCRIBE_FLAGS 0x02U

#endif /* MQTT_V5_FLAGS_H */
