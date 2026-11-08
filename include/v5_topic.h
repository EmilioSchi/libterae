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
 * @file v5_topic.h
 * @brief Topics beginning with $.
 *
 * A Topic Filter whose first character is a multi-level or single-level
 * wildcard MUST NOT be matched by the Server against a Topic Name whose first
 * character is $.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 4.7.2, p.99.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */
#ifndef MQTT_V5_TOPIC_H
#define MQTT_V5_TOPIC_H

/** @brief Dollar sign, U+0024. */
#define MQTT5_TOPIC_DOLLAR 0x24U

#endif /* MQTT_V5_TOPIC_H */
