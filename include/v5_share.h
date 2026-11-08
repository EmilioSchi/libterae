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
 * @file v5_share.h
 * @brief Shared Subscription Topic Filter prefix.
 *
 * The Topic Filter of a Shared Subscription MUST begin with this prefix and
 * MUST include a ShareName of one or more characters.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 4.8.2, p.101.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */
#ifndef MQTT_V5_SHARE_H
#define MQTT_V5_SHARE_H

/** @brief Prefix of a Shared Subscription Topic Filter. */
#define MQTT5_SHARE_PREFIX        "$share/"
/** @brief Number of bytes in the prefix. */
#define MQTT5_SHARE_PREFIX_LENGTH 7U

#endif /* MQTT_V5_SHARE_H */
