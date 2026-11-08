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
 * @file base_topic_check.c
 * @brief Checking and matching Topic Names and Topic Filters.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 4.7, p.102-105. 4.8.2, p.105-107.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */

#include <stddef.h>

#include "base_string.h"
#include "base_topic.h"
#include "base_topic_check.h"
#include "base_utf8_check.h"
#include "base_wire.h"
#include "v5_share.h"
#include "v5_topic.h"

/**
 * @brief Find the end of a topic level.
 * @param[in] data The bytes of a Topic Name or Topic Filter.
 * @param[in] length Number of bytes in data.
 * @param[in] start Index where the level starts.
 * @return Index of the next separator, or length if there is none.
 */
static uint32_t topic_level_end(const uint8_t *data, uint32_t length,
                                uint32_t start)
{
    /** @brief Index being looked at. */
    uint32_t end = start;

    while ((end < length) &&
           ((uint32_t)data[end] != MQTT_TOPIC_LEVEL_SEPARATOR)) {
        end += 1U;
    }
    return end;
}

/**
 * @brief Decide whether a Topic Filter starts with the share prefix.
 * @param[in] filter The filter.
 * @return true if it does.
 */
static bool topic_has_share_prefix(const mqtt_utf8 *filter)
{
    /** @brief The answer. */
    bool found = false;

    if ((uint32_t)filter->length >= MQTT5_SHARE_PREFIX_LENGTH) {
        found = mqtt_utf8_equal_text(filter->data, MQTT5_SHARE_PREFIX_LENGTH,
                                     MQTT5_SHARE_PREFIX);
    }
    return found;
}

/**
 * @brief Check the levels of a Topic Filter.
 * @param[in] data The bytes of the filter.
 * @param[in] length Number of bytes in data.
 * @return true if every wildcard stands alone in its level and any multi-level
 * one is last.
 */
static bool topic_filter_levels_valid(const uint8_t *data, uint32_t length)
{
    /** @brief The answer so far. */
    bool valid = true;
    /** @brief Index where the current level starts. */
    uint32_t start = 0U;

    while (valid && (start <= length)) {
        /** @brief Index where the current level ends. */
        const uint32_t end = topic_level_end(data, length, start);
        /** @brief Index of the byte being looked at. */
        uint32_t index = start;

        while (valid && (index < end)) {
            /** @brief The byte being looked at. */
            const uint32_t byte = (uint32_t)data[index];

            if (byte == MQTT_MULTI_LEVEL_WILDCARD) {
                valid = ((end - start) == 1U) && (end == length);
            } else if (byte == MQTT_SINGLE_LEVEL_WILDCARD) {
                valid = ((end - start) == 1U);
            } else {
                /* Any other character is literal. */
            }
            index += 1U;
        }
        start = end + 1U;
    }
    return valid;
}

/**
 * @brief Compare a level of a filter and a level of a name.
 * @param[in] filter The filter.
 * @param[in] filter_start Index where the filter's level starts.
 * @param[in] filter_end Index where the filter's level ends.
 * @param[in] name The name.
 * @param[in] name_start Index where the name's level starts.
 * @param[in] name_end Index where the name's level ends.
 * @return true if the two have the same bytes.
 */
static bool topic_level_equal(const uint8_t *filter, uint32_t filter_start,
                              uint32_t filter_end, const uint8_t *name,
                              uint32_t name_start, uint32_t name_end)
{
    /** @brief Number of bytes in the filter's level. */
    const uint32_t length = filter_end - filter_start;
    /** @brief The answer so far. */
    bool equal = (length == (name_end - name_start));

    if (equal) {
        equal = mqtt_bytes_equal(&filter[filter_start], &name[name_start],
                                 length);
    }
    return equal;
}

/**
 * @brief Decide whether the characters of a topic are allowed.
 * @param[in] topic The topic.
 * @param[in] version The protocol in use. MQTT 3.1 only rules out the null
 * character in a topic.
 * @return true if they are.
 */
static bool topic_text_is_valid(const mqtt_utf8 *topic, mqtt_version version)
{
    /** @brief The answer. */
    bool valid = false;

    if (version != MQTT_VERSION_31) {
        valid = mqtt_utf8_is_valid(topic->data, (uint32_t)topic->length);
    } else if (mqtt_utf8_is_well_formed(topic->data, (uint32_t)topic->length)) {
        /** @brief Index of the byte being looked at. */
        uint32_t index = 0U;

        valid = true;
        while (valid && (index < (uint32_t)topic->length)) {
            valid = (topic->data[index] != 0U);
            index += 1U;
        }
    } else {
        /* It isn't UTF-8 at all. */
    }
    return valid;
}

mqtt_status mqtt_topic_name_check(const mqtt_utf8 *topic_name,
                                  mqtt_version version)
{
    /** @brief The answer. */
    mqtt_status status = MQTT_OK;

    if (topic_name == NULL) {
        status = MQTT_INVALID_ARGUMENT;
    } else if (topic_name->length == 0U) {
        status = MQTT_TOPIC_NAME_INVALID;
    } else if (!topic_text_is_valid(topic_name, version)) {
        /* Ill-formed text makes the whole packet malformed: [MQTT-1.5.4-1]. */
        status = MQTT_MALFORMED_PACKET;
    } else {
        /** @brief Index of the byte being looked at. */
        uint32_t index = 0U;

        while ((status == MQTT_OK) && (index < (uint32_t)topic_name->length)) {
            /** @brief The byte being looked at. */
            const uint32_t byte = (uint32_t)topic_name->data[index];

            if ((byte == MQTT_MULTI_LEVEL_WILDCARD) ||
                (byte == MQTT_SINGLE_LEVEL_WILDCARD)) {
                status = MQTT_TOPIC_NAME_INVALID;
            } else {
                /* Any other character is allowed. */
            }
            index += 1U;
        }
    }
    return status;
}

mqtt_status mqtt_topic_filter_check(const mqtt_utf8 *topic_filter,
                                    mqtt_version version)
{
    /** @brief The answer. */
    mqtt_status status = MQTT_OK;

    if (topic_filter == NULL) {
        status = MQTT_INVALID_ARGUMENT;
    } else if ((topic_filter->length == 0U) ||
        (!topic_text_is_valid(topic_filter, version))) {
        status = MQTT_TOPIC_FILTER_INVALID;
    } else if (!topic_filter_levels_valid(topic_filter->data,
                                          (uint32_t)topic_filter->length)) {
        status = MQTT_TOPIC_FILTER_INVALID;
    } else if (version == MQTT_VERSION_5) {
        /** @brief The ShareName, if the filter is shared. */
        mqtt_utf8 share_name = {0U, NULL};
        /** @brief The filter after the ShareName, if the filter is shared. */
        mqtt_utf8 inner_filter = {0U, NULL};

        if (topic_has_share_prefix(topic_filter)) {
            if (!mqtt_topic_share_split(topic_filter, &share_name,
                                        &inner_filter)) {
                status = MQTT_TOPIC_FILTER_INVALID;
            }
        }
    } else {
        /* Earlier versions give the share prefix no meaning. */
    }
    return status;
}

/**
 * @brief Do what mqtt_topic_matches does, for arguments known to be valid.
 * @param[in] topic_filter A Topic Filter that has passed
 * mqtt_topic_filter_check.
 * @param[in] topic_name A Topic Name that has passed mqtt_topic_name_check.
 * @return true if it matches.
 */
static bool topic_matches_body(const mqtt_utf8 *topic_filter,
                               const mqtt_utf8 *topic_name)
{
    /** @brief Number of bytes in the filter. */
    const uint32_t filter_length = (uint32_t)topic_filter->length;
    /** @brief Number of bytes in the name. */
    const uint32_t name_length = (uint32_t)topic_name->length;
    /** @brief Index where the filter's current level starts. */
    uint32_t filter_start = 0U;
    /** @brief Index where the name's current level starts. */
    uint32_t name_start = 0U;
    /** @brief The answer, once decided. */
    bool matched = false;
    /** @brief true once the answer is decided. */
    bool done = (filter_length == 0U) || (name_length == 0U);

    /* A filter that starts with a wildcard does not match a name that starts
     * with a dollar. */
    if ((!done) && ((uint32_t)topic_name->data[0] == MQTT5_TOPIC_DOLLAR) &&
        (((uint32_t)topic_filter->data[0] == MQTT_MULTI_LEVEL_WILDCARD) ||
         ((uint32_t)topic_filter->data[0] == MQTT_SINGLE_LEVEL_WILDCARD))) {
        done = true;
    }
    while (!done) {
        /** @brief Index where the filter's current level ends. */
        const uint32_t filter_end =
            topic_level_end(topic_filter->data, filter_length, filter_start);
        /** @brief true if the filter's level is one byte long. */
        const bool single = ((filter_end - filter_start) == 1U);

        if (single && ((uint32_t)topic_filter->data[filter_start] ==
                       MQTT_MULTI_LEVEL_WILDCARD)) {
            /* The wildcard is last, and it stands for the parent as well as any
             * level below. */
            matched = true;
            done = true;
        } else if (name_start > name_length) {
            /* The name has no more levels. Only a last level of # could still
             * match. */
            matched = false;
            done = true;
        } else {
            /** @brief Index where the name's current level ends. */
            const uint32_t name_end =
                topic_level_end(topic_name->data, name_length, name_start);

            if ((!(single && ((uint32_t)topic_filter->data[filter_start] ==
                              MQTT_SINGLE_LEVEL_WILDCARD))) &&
                (!topic_level_equal(topic_filter->data, filter_start,
                                    filter_end, topic_name->data, name_start,
                                    name_end))) {
                done = true;
            } else {
                filter_start = filter_end + 1U;
                name_start = name_end + 1U;
                if (filter_start > filter_length) {
                    matched = (name_start > name_length);
                    done = true;
                }
            }
        }
    }
    return matched;
}

bool mqtt_topic_matches(const mqtt_utf8 *topic_filter,
                        const mqtt_utf8 *topic_name)
{
    /** @brief The answer. */
    bool answer = false;

    if ((topic_filter != NULL) && (topic_name != NULL)) {
        answer = topic_matches_body(topic_filter, topic_name);
    }
    return answer;
}

/**
 * @brief Do what mqtt_topic_share_split does, for arguments known to be valid.
 * @param[in] topic_filter A Topic Filter that passed mqtt_topic_filter_check
 * for MQTT 5.0.
 * @param[out] share_name The ShareName, if the filter is shared.
 * @param[out] inner_filter The filter after the ShareName, if the filter is
 * shared.
 * @return true if the filter is a Shared Subscription.
 */
static bool topic_share_split_body(const mqtt_utf8 *topic_filter,
                                   mqtt_utf8 *share_name,
                                   mqtt_utf8 *inner_filter)
{
    /** @brief The answer. */
    bool shared = false;
    /** @brief Number of bytes in the filter. */
    const uint32_t length = (uint32_t)topic_filter->length;

    if ((length > MQTT5_SHARE_PREFIX_LENGTH) &&
        topic_has_share_prefix(topic_filter)) {
        /** @brief Index of the separator after the ShareName. */
        const uint32_t end = topic_level_end(topic_filter->data, length,
                                             MQTT5_SHARE_PREFIX_LENGTH);
        /** @brief Index of the byte being looked at. */
        uint32_t index = MQTT5_SHARE_PREFIX_LENGTH;
        /** @brief true if the ShareName has no wildcard. */
        bool name_valid = (end > MQTT5_SHARE_PREFIX_LENGTH) && (end < length) &&
                          ((end + 1U) < length);

        while (name_valid && (index < end)) {
            /** @brief The byte being looked at. */
            const uint32_t byte = (uint32_t)topic_filter->data[index];

            name_valid = (byte != MQTT_MULTI_LEVEL_WILDCARD) &&
                         (byte != MQTT_SINGLE_LEVEL_WILDCARD);
            index += 1U;
        }
        if (name_valid) {
            share_name->data = &topic_filter->data[MQTT5_SHARE_PREFIX_LENGTH];
            share_name->length = (uint16_t)(end - MQTT5_SHARE_PREFIX_LENGTH);
            inner_filter->data = &topic_filter->data[end + 1U];
            inner_filter->length = (uint16_t)(length - (end + 1U));
            shared = true;
        }
    }
    return shared;
}

bool mqtt_topic_share_split(const mqtt_utf8 *topic_filter,
                            mqtt_utf8 *share_name, mqtt_utf8 *inner_filter)
{
    /** @brief The answer. */
    bool answer = false;

    if ((topic_filter != NULL) && (share_name != NULL) &&
        (inner_filter != NULL)) {
        answer = topic_share_split_body(topic_filter, share_name, inner_filter);
    }
    return answer;
}
