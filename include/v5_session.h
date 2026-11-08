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
 * @file v5_session.h
 * @brief Session Expiry Interval.
 *
 * A Four Byte Integer, held in a uint32_t, in seconds. With a value of 0, or
 * with no value at all, the Session finishes when the Network Connection
 * closes.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 1.5.3, p.16; 3.1.2.11.2, p.35.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */
#ifndef MQTT_V5_SESSION_H
#define MQTT_V5_SESSION_H

/** @brief The Session does not expire. */
#define MQTT5_SESSION_EXPIRY_NEVER 0xFFFFFFFFU

#endif /* MQTT_V5_SESSION_H */
