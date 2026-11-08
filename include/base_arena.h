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
 * @file base_arena.h
 * @brief A buddy allocator over caller-owned bytes.
 *
 * The library never calls an allocator of the system. The client's
 * variable-sized data, such as the packets it holds, comes from one block of
 * bytes the caller provides at start-up. The server's comes from pages, in
 * base_page_arena.h. Blocks are powers of two of MQTT_ARENA_UNIT
 * bytes. Taking and returning a block each take a number of steps bounded by
 * the number of block sizes, and running out is an ordinary result that the
 * caller handles.
 *
 * Reference: K. C. Knowlton, A Fast Storage Allocator, Communications of the
 * ACM 8(10), 1965.
 */
#ifndef MQTT_BASE_ARENA_H
#define MQTT_BASE_ARENA_H

#include <stdbool.h>
#include <stdint.h>

#include "base_status.h"

/** @brief Number of bytes in the smallest block. */
#define MQTT_ARENA_UNIT 64U
/** @brief Number of block sizes, which gives a largest block of 2 GiB. */
#define MQTT_ARENA_ORDERS 26U
/** @brief The handle that stands for no block. */
#define MQTT_ARENA_NONE 0xFFFFFFFFU
/**
 * @brief The most units in an arena: byte offsets are 32 bits, which gives 4
 * GiB.
 */
#define MQTT_ARENA_MAX_UNITS 67108864U

/** @brief The state of an arena. */
typedef struct {
    /** @brief The bytes handed out. */
    uint8_t *bytes;
    /** @brief For each unit, the state of the block that starts there. */
    uint8_t *states;
    /** @brief Number of units. */
    uint32_t unit_count;
    /** @brief Number of units in blocks handed out. */
    uint32_t units_used;
    /** @brief The first free block of each size. */
    uint32_t free_heads[MQTT_ARENA_ORDERS];
} mqtt_arena;

/**
 * @brief Set up an arena.
 * @param[out] arena The arena.
 * @param[in] bytes The bytes to hand out.
 * @param[in] states One byte for each MQTT_ARENA_UNIT bytes.
 * @param[in] unit_count Number of units, at most MQTT_ARENA_MAX_UNITS.
 * @return MQTT_OK or MQTT_INVALID_ARGUMENT.
 */
mqtt_status mqtt_arena_init(mqtt_arena *arena, uint8_t *bytes, uint8_t *states,
                            uint32_t unit_count);

/**
 * @brief Take a block.
 * @param[in,out] arena The arena.
 * @param[in] size Number of bytes needed, which may be zero.
 * @return The handle of the block, or MQTT_ARENA_NONE if there is no block that
 * large.
 */
uint32_t mqtt_arena_take(mqtt_arena *arena, uint32_t size);

/**
 * @brief Return a block.
 *
 * Nothing happens unless the handle is the start of a block in use, so
 * MQTT_ARENA_NONE, a handle already returned and a value out of range are all
 * ignored. An old handle can't be told apart once a new block in use starts at
 * the same unit: returning it frees that new block.
 *
 * @param[in,out] arena The arena.
 * @param[in] block The handle.
 */
void mqtt_arena_give(mqtt_arena *arena, uint32_t block);

/**
 * @brief Find the bytes of a block.
 * @param[in] arena The arena.
 * @param[in] block The handle.
 * @return The block's first byte, or NULL if the arena is NULL or the handle
 * isn't that of a block in use.
 */
uint8_t *mqtt_arena_bytes(const mqtt_arena *arena, uint32_t block);

/**
 * @brief Find how many bytes a block holds, which may be more than was asked
 * for.
 * @param[in] arena The arena.
 * @param[in] block The handle.
 * @return Number of bytes, or 0U if the handle is not a block in use.
 */
uint32_t mqtt_arena_size(const mqtt_arena *arena, uint32_t block);

/**
 * @brief Copy bytes into the start of a block.
 * @param[in] arena The arena.
 * @param[in] block The handle. It may be MQTT_ARENA_NONE if length is zero.
 * @param[in] data The bytes.
 * @param[in] length Number of bytes.
 * @return MQTT_OK, or MQTT_INVALID_ARGUMENT, with nothing copied, if the
 * handle is not a block in use that holds length bytes.
 */
mqtt_status mqtt_arena_write(const mqtt_arena *arena, uint32_t block,
                             const uint8_t *data, uint32_t length);

/**
 * @brief Overwrite every byte of a block with zeros, as mqtt_bytes_wipe does.
 * For a block that held a secret, before it is given back. A handle that is
 * not a block in use is ignored.
 * @param[in] arena The arena.
 * @param[in] block The handle.
 */
void mqtt_arena_wipe(const mqtt_arena *arena, uint32_t block);

#endif /* MQTT_BASE_ARENA_H */
