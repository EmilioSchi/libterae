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
 * @file v311_suback.h
 * @brief SUBACK return codes.
 *
 * Only these return codes are allowed. All other values are reserved, and a
 * sender MUST NOT use them.
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015,
 * 81 pages.
 * @li Cited: 3.9.3, p.44.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 */
#ifndef MQTT_V311_SUBACK_H
#define MQTT_V311_SUBACK_H

/** @brief Success - Maximum QoS 0. */
#define MQTT311_RC_MAXIMUM_QOS_0 0x00U
/** @brief Success - Maximum QoS 1. */
#define MQTT311_RC_MAXIMUM_QOS_1 0x01U
/** @brief Success - Maximum QoS 2. */
#define MQTT311_RC_MAXIMUM_QOS_2 0x02U
/** @brief Failure. */
#define MQTT311_RC_FAILURE       0x80U

#endif /* MQTT_V311_SUBACK_H */
