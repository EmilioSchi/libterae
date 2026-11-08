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
 * @file base_pool.c
 * @brief A pool of blocks of one size, shared by the rings of many connections.
 *
 * The free blocks are a stack linked through the array of links, so the block
 * returned last is the next one taken, while it is likely still in the cache.
 */

#include <stddef.h>

#include "base_pool.h"

mqtt_status mqtt_pool_init(mqtt_pool *pool, uint8_t *bytes, uint32_t *next,
                           uint32_t block_size, uint32_t block_count)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_INVALID_ARGUMENT;

    if ((pool != NULL) && (bytes != NULL) && (next != NULL) &&
        (block_size > 0U) && (block_size <= MQTT_POOL_BLOCK_MAX) &&
        (block_count > 0U) && (block_count < MQTT_POOL_NONE) &&
        (((uint64_t)block_size * (uint64_t)block_count) <=
         (uint64_t)UINT32_MAX)) {
        /** @brief Index of the block being linked. */
        uint32_t index = 0U;

        while (index < block_count) {
            next[index] =
                ((index + 1U) < block_count) ? (index + 1U) : MQTT_POOL_NONE;
            index += 1U;
        }
        pool->bytes = bytes;
        pool->next = next;
        pool->block_size = block_size;
        pool->block_count = block_count;
        pool->free_head = 0U;
        pool->free_count = block_count;
        pool->lowest_free = block_count;
        status = MQTT_OK;
    }
    return status;
}

/**
 * @brief Do what mqtt_pool_take does, for arguments known to be valid.
 * @param[in,out] pool The pool.
 * @return The block, or MQTT_POOL_NONE if none is free. Its link is
 * MQTT_POOL_NONE.
 */
static uint32_t pool_take_body(mqtt_pool *pool)
{
    /** @brief The block. */
    const uint32_t block = pool->free_head;

    if (block != MQTT_POOL_NONE) {
        pool->free_head = pool->next[block];
        pool->next[block] = MQTT_POOL_NONE;
        pool->free_count -= 1U;
        if (pool->free_count < pool->lowest_free) {
            pool->lowest_free = pool->free_count;
        }
    }
    return block;
}

uint32_t mqtt_pool_take(mqtt_pool *pool)
{
    /** @brief The answer. */
    uint32_t answer = MQTT_POOL_NONE;

    if (pool != NULL) {
        answer = pool_take_body(pool);
    }
    return answer;
}

void mqtt_pool_give(mqtt_pool *pool, uint32_t block)
{
    if ((pool != NULL) && (block < pool->block_count)) {
        pool->next[block] = pool->free_head;
        pool->free_head = block;
        pool->free_count += 1U;
    }
}

/**
 * @brief Do what mqtt_pool_grant does, for arguments known to be valid.
 * @param[in] pool The pool.
 * @param[in] held Number of blocks the holder has.
 * @param[in] rule The rule.
 * @return The number of blocks, which may be zero.
 */
static uint32_t pool_grant_body(const mqtt_pool *pool, uint32_t held,
                                const mqtt_pool_rule *rule)
{
    /** @brief Number of blocks the cap still allows. */
    const uint32_t left = (rule->cap > held) ? (rule->cap - held) : 0U;
    /** @brief Number of blocks the holder is under soft. */
    const uint32_t under = (rule->soft > held) ? (rule->soft - held) : 0U;
    /** @brief Number of blocks that keep the holder under soft. */
    const uint32_t below = (under < left) ? under : left;
    /** @brief Number of free blocks. */
    const uint32_t free_count = pool->free_count;
    /** @brief Number of blocks floor_low lets go. */
    const uint32_t low = (free_count > rule->floor_low) ?
                             (free_count - rule->floor_low) :
                             0U;
    /** @brief Number of blocks granted under soft. */
    const uint32_t first = (below < low) ? below : low;
    /** @brief Number of blocks granted from soft up. */
    uint32_t second = 0U;

    /* Blocks from soft up only come once every block below it has. */
    if (first == below) {
        /** @brief Number of blocks still free after the first ones. */
        const uint32_t rest = free_count - first;
        /** @brief Number of blocks floor_high lets go. */
        const uint32_t high =
            (rest > rule->floor_high) ? (rest - rule->floor_high) : 0U;

        second = ((left - first) < high) ? (left - first) : high;
    }
    return first + second;
}

uint32_t mqtt_pool_grant(const mqtt_pool *pool, uint32_t held,
                         const mqtt_pool_rule *rule)
{
    /** @brief The answer. */
    uint32_t answer = 0U;

    if ((pool != NULL) && (rule != NULL)) {
        answer = pool_grant_body(pool, held, rule);
    }
    return answer;
}

/**
 * @brief Mark a block, unless it is out of range or marked already.
 * @param[in] pool The pool.
 * @param[in,out] marks One byte for each block.
 * @param[in] block The block.
 * @return true if it was marked here.
 */
static bool pool_mark(const mqtt_pool *pool, uint8_t *marks, uint32_t block)
{
    /** @brief The answer. */
    bool marked = false;

    if ((block < pool->block_count) && (marks[block] == 0U)) {
        marks[block] = 1U;
        marked = true;
    }
    return marked;
}

/**
 * @brief Do what mqtt_pool_audit does, for arguments known to be valid.
 * @param[in] pool The pool.
 * @param[in,out] marks One byte for each block: zero for a block not marked
 * yet. Each free block's becomes one.
 * @return true if the list holds free_count blocks, each in range and none
 * marked before.
 */
static bool pool_audit_body(const mqtt_pool *pool, uint8_t *marks)
{
    /** @brief The block being looked at. */
    uint32_t block = pool->free_head;
    /** @brief Number of blocks found. */
    uint32_t found = 0U;
    /** @brief true while the list looks right. */
    bool sound = (pool->free_count <= pool->block_count) &&
                 (pool->lowest_free <= pool->free_count);

    /* A block marked twice ends the walk, so a loop in the list ends it too. */
    while (sound && (block != MQTT_POOL_NONE)) {
        sound = pool_mark(pool, marks, block);
        if (sound) {
            found += 1U;
            block = pool->next[block];
        }
    }
    return sound && (found == pool->free_count);
}

bool mqtt_pool_audit(const mqtt_pool *pool, uint8_t *marks)
{
    /** @brief The answer. */
    bool answer = false;

    if ((pool != NULL) && (marks != NULL)) {
        answer = pool_audit_body(pool, marks);
    }
    return answer;
}
