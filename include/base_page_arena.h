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
 * @file base_page_arena.h
 * @brief Blocks of bytes of any size, from pages of caller-owned bytes.
 *
 * The server's variable-sized data, such as stored messages and the text of
 * Client Identifiers, comes from here. A block of at most half a page comes
 * from a page split into slots of one size class, the smallest that holds it,
 * and a page goes back to the free pages once its last slot is given back. A
 * larger block is a run of whole pages, so its bytes are in one place however
 * large it is. A block is named by the offset of its first byte.
 */
#ifndef MQTT_BASE_PAGE_ARENA_H
#define MQTT_BASE_PAGE_ARENA_H

#include <stdint.h>

#include "base_page.h"
#include "base_slab.h"
#include "base_status.h"

/** @brief The handle that stands for no block. */
#define MQTT_PAGE_ARENA_NONE 0xFFFFFFFFU
/** @brief Number of size classes, of which those up to half a page are used. */
#define MQTT_PAGE_ARENA_CLASSES 22U

/** @brief The state of an arena of pages. */
typedef struct {
    /** @brief The bytes, page_count times MQTT_PAGE_SIZE. */
    uint8_t *bytes;
    /** @brief Number of pages. */
    uint32_t page_count;
    /**
     * @brief Number of bytes in blocks handed out, as the size classes and
     * whole pages round them up.
     */
    uint32_t bytes_used;
    /** @brief Number of size classes in use. */
    uint32_t class_count;
    /** @brief The pages. */
    mqtt_runs runs;
    /** @brief The slots of each size class. */
    mqtt_slab classes[MQTT_PAGE_ARENA_CLASSES];
} mqtt_page_arena;

/**
 * @brief Set up an arena of pages.
 * @param[out] arena The arena.
 * @param[in] bytes The bytes to hand out: page_count times MQTT_PAGE_SIZE.
 * @param[in] meta MQTT_PAGE_META_WORDS words for each page.
 * @param[in] page_count Number of pages, from 1 to MQTT_PAGE_MAX_PAGES.
 * @return MQTT_OK or MQTT_INVALID_ARGUMENT.
 */
mqtt_status mqtt_page_arena_init(mqtt_page_arena *arena, uint8_t *bytes,
                                 uint32_t *meta, uint32_t page_count);

/**
 * @brief Take a block.
 * @param[in,out] arena The arena.
 * @param[in] size Number of bytes needed, which may be zero.
 * @return The handle of the block, or MQTT_PAGE_ARENA_NONE if there is no room.
 */
uint32_t mqtt_page_arena_take(mqtt_page_arena *arena, uint32_t size);

/**
 * @brief Return a block.
 *
 * Nothing happens unless the handle is the start of a block in use, so
 * MQTT_PAGE_ARENA_NONE, a handle already returned and a value out of range are
 * all ignored. An old handle can't be told apart once a new block in use starts
 * at the same byte: returning it frees that new block.
 *
 * @param[in,out] arena The arena.
 * @param[in] block The handle.
 */
void mqtt_page_arena_give(mqtt_page_arena *arena, uint32_t block);

/**
 * @brief Find the bytes of a block.
 * @param[in] arena The arena.
 * @param[in] block The handle. Anything out of range gives the arena's first
 * byte, for a caller that then reads no bytes.
 * @return The block's first byte.
 */
uint8_t *mqtt_page_arena_bytes(const mqtt_page_arena *arena, uint32_t block);

/**
 * @brief Find how many bytes a block holds, which may be more than was asked
 * for.
 * @param[in] arena The arena.
 * @param[in] block The handle.
 * @return Number of bytes, or 0U if the handle is not a block in use.
 */
uint32_t mqtt_page_arena_size(const mqtt_page_arena *arena, uint32_t block);

/**
 * @brief Copy bytes into the start of a block.
 * @param[in] arena The arena.
 * @param[in] block The handle. It may be MQTT_PAGE_ARENA_NONE if length is
 * zero.
 * @param[in] data The bytes.
 * @param[in] length Number of bytes.
 * @return MQTT_OK, or MQTT_INVALID_ARGUMENT, with nothing copied, if the
 * handle is not a block in use that holds length bytes.
 */
mqtt_status mqtt_page_arena_write(const mqtt_page_arena *arena, uint32_t block,
                                  const uint8_t *data, uint32_t length);

/**
 * @brief Overwrite every byte of a block with zeros, as mqtt_bytes_wipe does.
 * For a block that held a secret, before it is given back. A handle that is
 * not a block in use is ignored.
 * @param[in] arena The arena.
 * @param[in] block The handle.
 */
void mqtt_page_arena_wipe(const mqtt_page_arena *arena, uint32_t block);

#endif /* MQTT_BASE_PAGE_ARENA_H */
