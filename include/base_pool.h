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
 * @file base_pool.h
 * @brief A pool of blocks of one size, shared by the rings of many connections.
 *
 * The bytes come from the caller at start-up, and nothing is allocated after.
 * Each block has an entry in an array of links: a free block's entry points to
 * the next free one, and a block in a ring points to the next block of that
 * ring. Taking and returning a block are each a few steps.
 *
 * The pool doesn't know who holds what. Whoever holds blocks keeps the count,
 * and asks mqtt_pool_grant how many more a rule lets it take: a holder may take
 * blocks up to a cap, and only while more blocks are free than the rule keeps
 * back. The rule keeps back fewer blocks while the holder has few, which is
 * how a reserve stays for connections that have just started to need any.
 */
#ifndef MQTT_BASE_POOL_H
#define MQTT_BASE_POOL_H

#include <stdbool.h>
#include <stdint.h>

#include "base_status.h"

/** @brief The index that stands for no block. */
#define MQTT_POOL_NONE 0xFFFFFFFFU
/** @brief The largest block, in bytes. */
#define MQTT_POOL_BLOCK_MAX 4096U

/** @brief The state of a pool. */
typedef struct {
    /** @brief The bytes of the blocks, block_count times block_size. */
    uint8_t *bytes;
    /** @brief One link for each block. */
    uint32_t *next;
    /** @brief Number of bytes in a block. */
    uint32_t block_size;
    /** @brief Number of blocks. */
    uint32_t block_count;
    /** @brief The first free block, or MQTT_POOL_NONE. */
    uint32_t free_head;
    /** @brief Number of free blocks. */
    uint32_t free_count;
    /** @brief The fewest free blocks there have been since start-up. */
    uint32_t lowest_free;
} mqtt_pool;

/** @brief How many blocks a holder may take. */
typedef struct {
    /** @brief The most blocks it may hold. */
    uint32_t cap;
    /** @brief The number of blocks up to which floor_low applies. */
    uint32_t soft;
    /** @brief Free blocks to leave while it holds fewer than soft. */
    uint32_t floor_low;
    /** @brief Free blocks to leave once it holds soft or more. */
    uint32_t floor_high;
} mqtt_pool_rule;

/**
 * @brief Set up a pool, with every block free.
 * @param[out] pool The pool.
 * @param[in] bytes The bytes of the blocks: block_count times block_size.
 * @param[in] next One link for each block.
 * @param[in] block_size Number of bytes in a block, 1 to MQTT_POOL_BLOCK_MAX.
 * @param[in] block_count Number of blocks, at least 1 and less than
 * MQTT_POOL_NONE, with all the bytes fitting in 32-bit offsets.
 * @return MQTT_OK or MQTT_INVALID_ARGUMENT.
 */
mqtt_status mqtt_pool_init(mqtt_pool *pool, uint8_t *bytes, uint32_t *next,
                           uint32_t block_size, uint32_t block_count);

/**
 * @brief Take a free block.
 * @param[in,out] pool The pool.
 * @return The block, or MQTT_POOL_NONE if none is free. Its link is
 * MQTT_POOL_NONE.
 */
uint32_t mqtt_pool_take(mqtt_pool *pool);

/**
 * @brief Return a block.
 *
 * MQTT_POOL_NONE and a value out of range are ignored. Returning a block that
 * is already free breaks the pool, which nothing here can see.
 *
 * @param[in,out] pool The pool.
 * @param[in] block The block.
 */
void mqtt_pool_give(mqtt_pool *pool, uint32_t block);

/**
 * @brief Count the blocks a holder may take now.
 *
 * Each block it would take must leave at least the rule's floor free:
 * floor_low for the blocks that keep it under soft, and floor_high for the
 * rest. A floor of zero lets it take the last free block.
 *
 * @param[in] pool The pool.
 * @param[in] held Number of blocks the holder has.
 * @param[in] rule The rule.
 * @return The number of blocks, which may be zero.
 */
uint32_t mqtt_pool_grant(const mqtt_pool *pool, uint32_t held,
                         const mqtt_pool_rule *rule);

/**
 * @brief Mark the free blocks, and check the list they are in.
 * @param[in] pool The pool.
 * @param[in,out] marks One byte for each block: zero for a block not marked
 * yet. Each free block's becomes one.
 * @return true if the list holds free_count blocks, each in range and none
 * marked before.
 */
bool mqtt_pool_audit(const mqtt_pool *pool, uint8_t *marks);

#endif /* MQTT_BASE_POOL_H */
