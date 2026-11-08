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
 * @file v311_flags.h
 * @brief Fixed Header flags other than PUBLISH.
 *
 * A flag bit shown as Reserved MUST have exactly the value given here. A
 * receiver that gets any other flags MUST close the Network Connection. PUBLISH
 * flags are in base_flags.h.
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015,
 * 81 pages.
 * @li Cited: 2.2.2, Table 2.2, p.17-18.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 */
#ifndef MQTT_V311_FLAGS_H
#define MQTT_V311_FLAGS_H

/** @brief All packets not listed below, other than PUBLISH. */
#define MQTT311_RESERVED_FLAGS    0x00U
/** @brief 0, 0, 1, 0. */
#define MQTT311_PUBREL_FLAGS      0x02U
/** @brief 0, 0, 1, 0. */
#define MQTT311_SUBSCRIBE_FLAGS   0x02U
/** @brief 0, 0, 1, 0. */
#define MQTT311_UNSUBSCRIBE_FLAGS 0x02U

#endif /* MQTT_V311_FLAGS_H */
