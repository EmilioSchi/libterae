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
 * @file platform_parse.c
 * @brief Reading numbers and names from text, which needs nothing from the
 * system.
 */

#include "base_platform.h"

/**
 * @brief Find whether two strings are the same.
 * @param[in] text One, with its end.
 * @param[in] name The other, with its end.
 * @return true if they are.
 */
static bool parse_same(const char *text, const char *name);

static bool parse_same(const char *text, const char *name)
{
    /** @brief Index of the character being compared. */
    uint32_t index = 0U;

    while ((text[index] != '\0') && (text[index] == name[index])) {
        index += 1U;
    }
    return text[index] == name[index];
}

bool mqtt_platform_parse_u32(const char *text, uint32_t *value)
{
    /** @brief The number so far. */
    uint64_t total = 0U;
    /** @brief Index of the character being read. */
    uint32_t index = 0U;
    /** @brief The answer. */
    bool valid = (text[0] != '\0');

    while (valid && (text[index] != '\0')) {
        if ((text[index] >= '0') && (text[index] <= '9')) {
            /** @brief The value of this digit. */
            const uint32_t digit = (uint32_t)text[index] - (uint32_t)'0';

            total = (total * 10U) + (uint64_t)digit;
            valid = (total <= (uint64_t)UINT32_MAX);
        } else {
            valid = false;
        }
        index += 1U;
    }
    if (valid) {
        *value = (uint32_t)total;
    }
    return valid;
}

bool mqtt_platform_parse_durability(const char *text,
                                    mqtt_store_durability *durability)
{
    /** @brief The answer. */
    bool valid = true;

    if (parse_same(text, "memory")) {
        *durability = MQTT_STORE_MEMORY;
    } else if (parse_same(text, "written")) {
        *durability = MQTT_STORE_WRITTEN;
    } else if (parse_same(text, "synced")) {
        *durability = MQTT_STORE_SYNCED;
    } else {
        valid = false;
    }
    return valid;
}
