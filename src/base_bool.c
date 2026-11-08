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
 * @file base_bool.c
 * @brief Markers that say whether memory has been set, without clearing it.
 */

#include <stddef.h>

#include "base_bool.h"

/**
 * @brief Mixed into a mask's check, so that a word of one repeated byte, which
 * rotates to itself or near it, doesn't match: the fractional part of the
 * golden ratio.
 */
#define BOOL_MASK_KEY 0x9E3779B97F4A7C15U

/**
 * @brief Work out the check of a mask's bits.
 * @param[in] bits The bits.
 * @return Zero for no bits, otherwise bits rotated left by one, with
 * BOOL_MASK_KEY mixed in.
 */
static uint64_t bool_mask_check(uint64_t bits)
{
    /** @brief The answer. */
    uint64_t check = 0U;

    if (bits != 0U) {
        check = ((bits << 1U) | (bits >> 63U)) ^ (uint64_t)BOOL_MASK_KEY;
    }
    return check;
}

bool mqtt_bool_is_set(mqtt_bool marker)
{
    return marker == MQTT_BOOL_SET;
}

void mqtt_bool_mask_clear(mqtt_bool_mask *mask)
{
    if (mask != NULL) {
        mask->bits = 0U;
        mask->check = 0U;
    }
}

void mqtt_bool_mask_mark(mqtt_bool_mask *mask, uint32_t bit)
{
    if (mqtt_bool_mask_is_valid(mask)) {
        mask->bits |= (uint64_t)1U << (bit & 63U);
        mask->check = bool_mask_check(mask->bits);
    }
}

bool mqtt_bool_mask_has(const mqtt_bool_mask *mask, uint32_t bit)
{
    return mqtt_bool_mask_any(mask, (uint64_t)1U << (bit & 63U));
}

bool mqtt_bool_mask_any(const mqtt_bool_mask *mask, uint64_t bits)
{
    return mqtt_bool_mask_is_valid(mask) && ((mask->bits & bits) != 0U);
}

bool mqtt_bool_mask_is_valid(const mqtt_bool_mask *mask)
{
    return (mask != NULL) && (mask->check == bool_mask_check(mask->bits));
}
