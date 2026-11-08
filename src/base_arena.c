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
 * @file base_arena.c
 * @brief A buddy allocator over caller-owned bytes.
 *
 * Each free block keeps its list links in its own first eight bytes. Each unit
 * has a state byte: for the first unit of a block, it says the block's size and
 * whether it is free. For any other unit it is ARENA_STATE_INSIDE, which is how
 * mqtt_arena_give recognises a handle it should ignore.
 */

#include <stddef.h>

#include "base_arena.h"
#include "base_string.h"

/** @brief Added to a block's order in its state byte when it is free. */
#define ARENA_STATE_FREE 0x40U
/** @brief Added to a block's order in its state byte when it is in use. */
#define ARENA_STATE_USED 0x80U
/** @brief The state byte of a unit that is not the first of a block. */
#define ARENA_STATE_INSIDE 0U
/** @brief The bits of a state byte that hold the order. */
#define ARENA_STATE_ORDER 0x3FU

/**
 * @brief Count the units in a block.
 * @param[in] order The block's size: 2 to this power of units.
 * @return The number of units.
 */
static uint32_t arena_units(uint32_t order)
{
    return (uint32_t)1U << order;
}

/**
 * @brief Tell whether a handle is the first unit of a block in use.
 * @param[in] arena The arena, or NULL.
 * @param[in] block The handle.
 * @return true if it is.
 */
static bool arena_in_use(const mqtt_arena *arena, uint32_t block)
{
    return (arena != NULL) && (block < arena->unit_count) &&
           (((uint32_t)arena->states[block] & ARENA_STATE_USED) != 0U);
}

/**
 * @brief Read a link stored in a free block.
 * @param[in] arena The arena.
 * @param[in] block The block.
 * @param[in] offset 0U for the next block, 4U for the previous one.
 * @return The link.
 */
static uint32_t arena_link_get(const mqtt_arena *arena, uint32_t block,
                               uint32_t offset)
{
    /** @brief The bytes of the link. */
    const uint8_t *bytes = &arena->bytes[(block * MQTT_ARENA_UNIT) + offset];

    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) | ((uint32_t)bytes[3] << 24U);
}

/**
 * @brief Write a link stored in a free block.
 * @param[in,out] arena The arena.
 * @param[in] block The block.
 * @param[in] offset 0U for the next block, 4U for the previous one.
 * @param[in] value The link.
 */
static void arena_link_set(const mqtt_arena *arena, uint32_t block,
                           uint32_t offset, uint32_t value)
{
    /** @brief The bytes of the link. */
    uint8_t *bytes = &arena->bytes[(block * MQTT_ARENA_UNIT) + offset];

    bytes[0] = (uint8_t)(value & 0xFFU);
    bytes[1] = (uint8_t)((value >> 8U) & 0xFFU);
    bytes[2] = (uint8_t)((value >> 16U) & 0xFFU);
    bytes[3] = (uint8_t)(value >> 24U);
}

/**
 * @brief Put a block on the free list of its size.
 * @param[in,out] arena The arena.
 * @param[in] block The block.
 * @param[in] order The block's size: 2 to this power of units.
 */
static void arena_list_push(mqtt_arena *arena, uint32_t block, uint32_t order)
{
    /** @brief The block that is first now. */
    const uint32_t head = arena->free_heads[order];

    arena_link_set(arena, block, 0U, head);
    arena_link_set(arena, block, 4U, MQTT_ARENA_NONE);
    if (head != MQTT_ARENA_NONE) {
        arena_link_set(arena, head, 4U, block);
    }
    arena->free_heads[order] = block;
    arena->states[block] = (uint8_t)(ARENA_STATE_FREE | order);
}

/**
 * @brief Take a block off the free list of its size.
 * @param[in,out] arena The arena.
 * @param[in] block The block.
 * @param[in] order The block's size: 2 to this power of units.
 */
static void arena_list_remove(mqtt_arena *arena, uint32_t block, uint32_t order)
{
    /** @brief The block after it. */
    const uint32_t next = arena_link_get(arena, block, 0U);
    /** @brief The block before it. */
    const uint32_t previous = arena_link_get(arena, block, 4U);

    if (previous == MQTT_ARENA_NONE) {
        arena->free_heads[order] = next;
    } else {
        arena_link_set(arena, previous, 0U, next);
    }
    if (next != MQTT_ARENA_NONE) {
        arena_link_set(arena, next, 4U, previous);
    }
}

mqtt_status mqtt_arena_init(mqtt_arena *arena, uint8_t *bytes, uint8_t *states,
                            uint32_t unit_count)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if ((arena == NULL) || (bytes == NULL) || (states == NULL) ||
        (unit_count == 0U) || (unit_count > MQTT_ARENA_MAX_UNITS)) {
        status = MQTT_INVALID_ARGUMENT;
    } else {
        /** @brief Index of the size being set up. */
        uint32_t order = 0U;
        /** @brief Index of the state byte being cleared. */
        uint32_t unit = 0U;
        /** @brief The next unit not yet on a free list. */
        uint32_t block = 0U;

        arena->bytes = bytes;
        arena->states = states;
        arena->unit_count = unit_count;
        arena->units_used = 0U;
        while (order < MQTT_ARENA_ORDERS) {
            arena->free_heads[order] = MQTT_ARENA_NONE;
            order += 1U;
        }
        while (unit < unit_count) {
            states[unit] = ARENA_STATE_INSIDE;
            unit += 1U;
        }
        while (block < unit_count) {
            /** @brief The largest size that fits and is aligned here. */
            uint32_t size_order = 0U;

            while (((size_order + 1U) < MQTT_ARENA_ORDERS) &&
                   ((block & (arena_units(size_order + 1U) - 1U)) == 0U) &&
                   ((block + arena_units(size_order + 1U)) <= unit_count)) {
                size_order += 1U;
            }
            arena_list_push(arena, block, size_order);
            block += arena_units(size_order);
        }
    }
    return status;
}

/**
 * @brief Do what mqtt_arena_take does, for arguments known to be valid.
 * @param[in,out] arena The arena.
 * @param[in] size Number of bytes needed, which may be zero.
 * @return The handle of the block, or MQTT_ARENA_NONE if there is no block that
 * large.
 */
static uint32_t arena_take_body(mqtt_arena *arena, uint32_t size)
{
    /** @brief The block handed back. */
    uint32_t block = MQTT_ARENA_NONE;
    /** @brief The size needed, as a power of two of units. */
    uint32_t wanted = 0U;

    while (((wanted + 1U) < MQTT_ARENA_ORDERS) &&
           ((arena_units(wanted) * MQTT_ARENA_UNIT) < size)) {
        wanted += 1U;
    }
    if ((arena_units(wanted) * MQTT_ARENA_UNIT) >= size) {
        /** @brief The size of the free block being looked at. */
        uint32_t found = wanted;

        while ((found < MQTT_ARENA_ORDERS) &&
               (arena->free_heads[found] == MQTT_ARENA_NONE)) {
            found += 1U;
        }
        if (found < MQTT_ARENA_ORDERS) {
            block = arena->free_heads[found];
            arena_list_remove(arena, block, found);
            /* Split it until it is the size wanted, giving back the upper half
             * each time. */
            while (found > wanted) {
                found -= 1U;
                arena->states[block + arena_units(found)] = ARENA_STATE_INSIDE;
                arena_list_push(arena, block + arena_units(found), found);
            }
            arena->states[block] = (uint8_t)(ARENA_STATE_USED | wanted);
            arena->units_used += arena_units(wanted);
        }
    }
    return block;
}

uint32_t mqtt_arena_take(mqtt_arena *arena, uint32_t size)
{
    /** @brief The answer. */
    uint32_t answer = MQTT_ARENA_NONE;

    if (arena != NULL) {
        answer = arena_take_body(arena, size);
    }
    return answer;
}

void mqtt_arena_give(mqtt_arena *arena, uint32_t block)
{
    if (arena_in_use(arena, block)) {
        /** @brief The size of the block as it grows. */
        uint32_t order = (uint32_t)arena->states[block] & ARENA_STATE_ORDER;
        /** @brief The first unit of the block as it grows. */
        uint32_t start = block;
        /** @brief true while another merge may be possible. */
        bool merging = true;

        arena->units_used -= arena_units(order);
        arena->states[block] = ARENA_STATE_INSIDE;
        while (merging && ((order + 1U) < MQTT_ARENA_ORDERS)) {
            /** @brief The block's neighbour of the same size. */
            const uint32_t buddy = start ^ arena_units(order);

            if (((buddy + arena_units(order)) <= arena->unit_count) &&
                ((uint32_t)arena->states[buddy] ==
                 (ARENA_STATE_FREE | order))) {
                arena_list_remove(arena, buddy, order);
                arena->states[buddy] = ARENA_STATE_INSIDE;
                if (buddy < start) {
                    start = buddy;
                }
                order += 1U;
            } else {
                merging = false;
            }
        }
        arena_list_push(arena, start, order);
    }
}

uint8_t *mqtt_arena_bytes(const mqtt_arena *arena, uint32_t block)
{
    /** @brief The answer. */
    uint8_t *bytes = NULL;

    if (arena_in_use(arena, block)) {
        bytes = &arena->bytes[block * MQTT_ARENA_UNIT];
    }
    return bytes;
}

uint32_t mqtt_arena_size(const mqtt_arena *arena, uint32_t block)
{
    /** @brief The answer. */
    uint32_t size = 0U;

    if (arena_in_use(arena, block)) {
        size = arena_units((uint32_t)arena->states[block] & ARENA_STATE_ORDER) *
               MQTT_ARENA_UNIT;
    }
    return size;
}

mqtt_status mqtt_arena_write(const mqtt_arena *arena, uint32_t block,
                             const uint8_t *data, uint32_t length)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (length > 0U) {
        /** @brief Number of bytes the block holds. */
        const uint32_t size = mqtt_arena_size(arena, block);

        if (size == 0U) {
            status = MQTT_INVALID_ARGUMENT;
        } else {
            status =
                mqtt_bytes_copy(mqtt_arena_bytes(arena, block), size, data,
                                length);
        }
    }
    return status;
}

void mqtt_arena_wipe(const mqtt_arena *arena, uint32_t block)
{
    /** @brief Number of bytes the block holds. */
    const uint32_t size = mqtt_arena_size(arena, block);

    if (size > 0U) {
        mqtt_bytes_wipe(mqtt_arena_bytes(arena, block), size);
    }
}
