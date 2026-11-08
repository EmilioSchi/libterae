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
 * @file v5_pair_t.h
 * @brief UTF-8 String Pair.
 *
 * Holds a name and its value, each one a UTF-8 Encoded String, with the name
 * first.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 1.5.7, p.19.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */
#ifndef MQTT_V5_PAIR_T_H
#define MQTT_V5_PAIR_T_H

#include "base_utf8_t.h"

/** @brief Name and value of a UTF-8 String Pair. */
typedef struct {
    /** @brief Name, which comes first. */
    mqtt_utf8 name;
    /** @brief Value of that name. */
    mqtt_utf8 value;
} mqtt5_utf8_pair;

#endif /* MQTT_V5_PAIR_T_H */
