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
 * @file base_bool.h
 * @brief Markers that say whether memory has been set, without clearing it.
 *
 * The library doesn't zero memory before it uses it, so memory it reuses still
 * holds whatever was there, and memory it was given may hold anything. A plain
 * bool can't tell set from leftover: any byte that isn't zero reads as true.
 *
 * A marker has two valid values, each the complement of the other. Zeroed
 * memory, leftovers and a single changed bit all read as neither, so the code
 * can refuse an object it never set up, or one that something overwrote.
 *
 * A mask holds one bit for each of up to 64 things that may be set, and a
 * second word worked out from the first. Clearing it is two writes, however
 * much memory the things it marks take. A mask of zeros is valid and empty, so
 * a struct set to zero marks nothing. Leftovers, memory filled with 0xA5 or
 * 0xFF, and a changed bit don't match, and such a mask reads as having nothing
 * set.
 */
#ifndef MQTT_BASE_BOOL_H
#define MQTT_BASE_BOOL_H

#include <stdbool.h>
#include <stdint.h>

/** @brief The value of a marker that is set. */
#define MQTT_BOOL_SET 0x6D51A7E9U
/** @brief The value of a marker that is not set: MQTT_BOOL_SET inverted. */
#define MQTT_BOOL_UNSET 0x92AE5816U

/**
 * @brief A marker: MQTT_BOOL_SET, MQTT_BOOL_UNSET, or, if it was never
 * written or was overwritten, any other value.
 */
typedef uint32_t mqtt_bool;

/** @brief A set of up to 64 bits, kept with a check of them. */
typedef struct {
    /** @brief One bit for each thing that is set. */
    uint64_t bits;
    /**
     * @brief Zero if bits is zero. Otherwise bits rotated left by one, with a
     * constant mixed in, so that no fill of one byte matches.
     */
    uint64_t check;
} mqtt_bool_mask;

/**
 * @brief Report whether a marker is set.
 * @param[in] marker The marker.
 * @return true only if it is exactly MQTT_BOOL_SET.
 */
bool mqtt_bool_is_set(mqtt_bool marker);

/**
 * @brief Make a mask with nothing set.
 * @param[out] mask The mask.
 */
void mqtt_bool_mask_clear(mqtt_bool_mask *mask);

/**
 * @brief Set one bit of a mask. A mask that isn't valid is left as it is, so
 * it stays refused.
 * @param[in,out] mask The mask.
 * @param[in] bit The bit, from 0 to 63. Only its lowest 6 bits are used.
 */
void mqtt_bool_mask_mark(mqtt_bool_mask *mask, uint32_t bit);

/**
 * @brief Report whether one bit of a mask is set.
 * @param[in] mask The mask.
 * @param[in] bit The bit, from 0 to 63. Only its lowest 6 bits are used.
 * @return true if the mask is valid and the bit is set.
 */
bool mqtt_bool_mask_has(const mqtt_bool_mask *mask, uint32_t bit);

/**
 * @brief Report whether any of some bits of a mask are set.
 * @param[in] mask The mask.
 * @param[in] bits The bits to look at.
 * @return true if the mask is valid and one of them is set.
 */
bool mqtt_bool_mask_any(const mqtt_bool_mask *mask, uint64_t bits);

/**
 * @brief Report whether a mask's two words match.
 * @param[in] mask The mask.
 * @return true if check is what bits gives.
 */
bool mqtt_bool_mask_is_valid(const mqtt_bool_mask *mask);

#endif /* MQTT_BASE_BOOL_H */
