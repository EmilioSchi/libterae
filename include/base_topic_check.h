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
 * @file base_topic_check.h
 * @brief Checking and matching Topic Names and Topic Filters.
 *
 * A Topic Name has no wildcard characters. In a Topic Filter a wildcard takes a
 * whole level, and the multi-level wildcard can only be the last level. A
 * filter that starts with a wildcard never matches a Topic Name that starts
 * with the dollar character.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 4.7, p.102-105. 4.8.2, p.105-107.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015.
 * @li Cited: 4.7.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 */
#ifndef MQTT_BASE_TOPIC_CHECK_H
#define MQTT_BASE_TOPIC_CHECK_H

#include <stdbool.h>
#include <stdint.h>

#include "base_status.h"
#include "base_utf8_t.h"
#include "base_version.h"

/**
 * @brief Decide whether a string can be a Topic Name.
 * @param[in] topic_name The string, with characters not yet checked.
 * @param[in] version The protocol in use, as MQTT 3.1 refuses fewer characters.
 * @return MQTT_OK, MQTT_TOPIC_NAME_INVALID, MQTT_MALFORMED_PACKET if it
 * isn't valid UTF-8, or MQTT_INVALID_ARGUMENT if topic_name is NULL.
 */
mqtt_status mqtt_topic_name_check(const mqtt_utf8 *topic_name,
                                  mqtt_version version);

/**
 * @brief Decide whether a string can be a Topic Filter.
 * @param[in] topic_filter The string, with characters not yet checked.
 * @param[in] version The protocol in use. Only MQTT 5.0 gives the share prefix
 * a meaning.
 * @return MQTT_OK, MQTT_TOPIC_FILTER_INVALID, or MQTT_INVALID_ARGUMENT if
 * topic_filter is NULL.
 */
mqtt_status mqtt_topic_filter_check(const mqtt_utf8 *topic_filter,
                                    mqtt_version version);

/**
 * @brief Decide whether a Topic Filter matches a Topic Name.
 * @param[in] topic_filter A Topic Filter that has passed
 * mqtt_topic_filter_check.
 * @param[in] topic_name A Topic Name that has passed mqtt_topic_name_check.
 * @return true if it matches, false if either is NULL.
 */
bool mqtt_topic_matches(const mqtt_utf8 *topic_filter,
                        const mqtt_utf8 *topic_name);

/**
 * @brief Split a Shared Subscription into its two parts.
 * @param[in] topic_filter A Topic Filter that passed mqtt_topic_filter_check
 * for MQTT 5.0.
 * @param[out] share_name The ShareName, if the filter is shared.
 * @param[out] inner_filter The filter after the ShareName, if the filter is
 * shared.
 * @return true if the filter is a Shared Subscription.
 */
bool mqtt_topic_share_split(const mqtt_utf8 *topic_filter,
                            mqtt_utf8 *share_name, mqtt_utf8 *inner_filter);

#endif /* MQTT_BASE_TOPIC_CHECK_H */
