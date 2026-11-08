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
 * @file base_page_arena.c
 * @brief Blocks of bytes of any size, from pages.
 *
 * Every page in use is a run of the arena's runs. A page split into slots is a
 * run of one page whose tag is its size class plus one, and a run of pages that
 * holds one large block has the tag zero. From 32 bytes, the size classes go up
 * by half a power of two at a time, so a block larger than 32 bytes wastes at
 * most a third of its slot. Each class is a power of two, or three times one,
 * so dividing by it takes a shift and at most a division by 3, which the
 * compiler does without a call even where the processor can't divide.
 */

#include <stddef.h>

#include "base_page_arena.h"
#include "base_string.h"

/** @brief The size class of a block that is a run of whole pages. */
#define PAGE_ARENA_LARGE 0xFFFFFFFFU

/** @brief What a handle names, once checked. */
typedef struct {
    /** @brief The first page of the run that holds it. */
    uint32_t page;
    /** @brief Its size class, or PAGE_ARENA_LARGE for a run of pages. */
    uint32_t size_class;
    /** @brief Its slot in the slab of its class, for a size class. */
    uint32_t slot;
    /** @brief Number of bytes it holds, or zero if it names no block. */
    uint32_t size;
} page_arena_block;

/** @brief Number of bytes in a slot of each size class. */
static const uint32_t page_arena_sizes[MQTT_PAGE_ARENA_CLASSES] = {
    16U, 32U, 48U, 64U, 96U, 128U, 192U, 256U, 384U, 512U, 768U, 1024U,
    1536U, 2048U, 3072U, 4096U, 6144U, 8192U, 12288U, 16384U, 24576U,
    32768U};

/**
 * @brief Divide by the size of a class.
 * @param[in] value The number to divide.
 * @param[in] size_class The size class.
 * @return The quotient, rounded down.
 */
static uint32_t page_arena_divide(uint32_t value, uint32_t size_class)
{
    /** @brief The power of two in each size class. */
    static const uint32_t shifts[MQTT_PAGE_ARENA_CLASSES] = {
        4U, 5U, 4U, 6U, 5U, 7U, 6U, 8U, 7U, 9U, 8U,
        10U, 9U, 11U, 10U, 12U, 11U, 13U, 12U, 14U, 13U, 15U};
    /** @brief true for each size class three times its power of two. */
    static const bool thirds[MQTT_PAGE_ARENA_CLASSES] = {
        false, false, true, false, true, false, true, false,
        true, false, true, false, true, false, true, false,
        true, false, true, false, true, false};
    /** @brief The number divided by the class's power of two. */
    const uint32_t shifted = value >> shifts[size_class];

    return thirds[size_class] ? (shifted / 3U) : shifted;
}

/**
 * @brief Find the block a handle names.
 * @param[in] arena The arena.
 * @param[in] block The handle.
 * @return What it names, with a size of zero if it is not a block in use.
 */
static page_arena_block page_arena_find(const mqtt_page_arena *arena,
                                        uint32_t block)
{
    /** @brief The answer. */
    page_arena_block found = {MQTT_PAGE_NONE, PAGE_ARENA_LARGE, MQTT_SLAB_NONE,
                              0U};
    /** @brief The page. */
    const uint32_t page = block / MQTT_PAGE_SIZE;
    /** @brief The offset in the page. */
    const uint32_t offset = block % MQTT_PAGE_SIZE;
    /** @brief Length of the run the page starts, or zero. */
    const uint32_t length = ((arena != NULL) && (page < arena->page_count)) ?
                                mqtt_runs_length(&arena->runs, page) :
                                0U;

    if (length > 0U) {
        /** @brief The run's tag. */
        const uint32_t tag = mqtt_runs_tag(&arena->runs, page);

        if ((tag == 0U) && (offset == 0U)) {
            found.page = page;
            found.size = length * MQTT_PAGE_SIZE;
        } else if ((tag > 0U) && (tag <= arena->class_count)) {
            /** @brief The size class. */
            const uint32_t size_class = tag - 1U;
            /** @brief Number of bytes in its slots. */
            const uint32_t size = page_arena_sizes[size_class];
            /** @brief The slab of the class. */
            const mqtt_slab *slab = &arena->classes[size_class];
            /** @brief The slot's index in its page. */
            const uint32_t index = page_arena_divide(offset, size_class);
            /** @brief The slot, if the offset starts one. */
            const uint32_t slot = (page << slab->page_shift) + index;

            if (((index * size) == offset) && mqtt_slab_used(slab, slot)) {
                found.page = page;
                found.size_class = size_class;
                found.slot = slot;
                found.size = size;
            }
        } else {
            /* Not the start of a block. */
        }
    }
    return found;
}

mqtt_status mqtt_page_arena_init(mqtt_page_arena *arena, uint8_t *bytes,
                                 uint32_t *meta, uint32_t page_count)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if ((arena == NULL) || (bytes == NULL) || (meta == NULL) ||
        (page_count == 0U) || (page_count > MQTT_PAGE_MAX_PAGES)) {
        status = MQTT_INVALID_ARGUMENT;
    } else {
        /** @brief Index of the size class being set up. */
        uint32_t size_class = 0U;

        arena->bytes = bytes;
        arena->page_count = page_count;
        arena->bytes_used = 0U;
        arena->class_count = 0U;
        status = mqtt_runs_init(&arena->runs, meta, page_count);
        while ((status == MQTT_OK) &&
               (size_class < MQTT_PAGE_ARENA_CLASSES) &&
               (page_arena_sizes[size_class] <= (MQTT_PAGE_SIZE / 2U))) {
            /** @brief The slab of the class. */
            mqtt_slab *slab = &arena->classes[size_class];

            status = mqtt_slab_init(
                slab, meta, page_arena_divide(MQTT_PAGE_SIZE, size_class), 0U,
                false);
            if (status == MQTT_OK) {
                /* The slots of each page start at a power of two. */
                slab->slot_count = page_count << slab->page_shift;
                arena->class_count += 1U;
                size_class += 1U;
            }
        }
    }
    return status;
}

/**
 * @brief Take a slot of a size class, giving the class a free page if it has
 * no room.
 * @param[in,out] arena The arena.
 * @param[in] size_class The size class.
 * @return The handle of the block, or MQTT_PAGE_ARENA_NONE.
 */
static uint32_t page_arena_take_slot(mqtt_page_arena *arena,
                                     uint32_t size_class)
{
    /** @brief The slab of the class. */
    mqtt_slab *slab = &arena->classes[size_class];
    /** @brief The slot. */
    uint32_t slot = mqtt_slab_take(slab);
    /** @brief The block. */
    uint32_t block = MQTT_PAGE_ARENA_NONE;

    if (slot == MQTT_SLAB_NONE) {
        /** @brief A free page for the class. */
        const uint32_t page = mqtt_runs_take(&arena->runs, 1U);

        if (page != MQTT_PAGE_NONE) {
            mqtt_runs_set_tag(&arena->runs, page, size_class + 1U);
            mqtt_slab_add_page(slab, page);
            slot = mqtt_slab_take(slab);
        }
    }
    if (slot != MQTT_SLAB_NONE) {
        block = ((slot >> slab->page_shift) * MQTT_PAGE_SIZE) +
                ((slot & (((uint32_t)1U << slab->page_shift) - 1U)) *
                 page_arena_sizes[size_class]);
        arena->bytes_used += page_arena_sizes[size_class];
    }
    return block;
}

/**
 * @brief Do what mqtt_page_arena_take does, for arguments known to be valid.
 * @param[in,out] arena The arena.
 * @param[in] size Number of bytes needed, which may be zero.
 * @return The handle of the block, or MQTT_PAGE_ARENA_NONE if there is no room.
 */
static uint32_t page_arena_take_body(mqtt_page_arena *arena, uint32_t size)
{
    /** @brief The smallest size class that holds the block. */
    uint32_t size_class = 0U;
    /** @brief The block handed back. */
    uint32_t block = MQTT_PAGE_ARENA_NONE;

    while ((size_class < arena->class_count) &&
           (page_arena_sizes[size_class] < size)) {
        size_class += 1U;
    }
    if (size_class < arena->class_count) {
        block = page_arena_take_slot(arena, size_class);
    } else {
        /** @brief Number of whole pages it needs. */
        const uint32_t pages = (size / MQTT_PAGE_SIZE) +
                               (((size % MQTT_PAGE_SIZE) != 0U) ? 1U : 0U);
        /** @brief Its first page. */
        const uint32_t first = mqtt_runs_take(&arena->runs, pages);

        if (first != MQTT_PAGE_NONE) {
            block = first * MQTT_PAGE_SIZE;
            arena->bytes_used += pages * MQTT_PAGE_SIZE;
        }
    }
    return block;
}

uint32_t mqtt_page_arena_take(mqtt_page_arena *arena, uint32_t size)
{
    /** @brief The answer. */
    uint32_t answer = MQTT_PAGE_ARENA_NONE;

    if (arena != NULL) {
        answer = page_arena_take_body(arena, size);
    }
    return answer;
}

void mqtt_page_arena_give(mqtt_page_arena *arena, uint32_t block)
{
    /** @brief The block. */
    const page_arena_block found = page_arena_find(arena, block);

    if (found.size > 0U) {
        arena->bytes_used -= found.size;
        if (found.size_class == PAGE_ARENA_LARGE) {
            mqtt_runs_give(&arena->runs, found.page);
        } else if (mqtt_slab_give(&arena->classes[found.size_class],
                                  found.slot)) {
            mqtt_runs_give(&arena->runs, found.page);
        } else {
            /* Its page still holds other blocks. */
        }
    }
}

uint8_t *mqtt_page_arena_bytes(const mqtt_page_arena *arena, uint32_t block)
{
    /** @brief The bytes. */
    uint8_t *bytes = NULL;

    if (arena != NULL) {
        bytes = arena->bytes;
        if (block < (arena->page_count * MQTT_PAGE_SIZE)) {
            bytes = &arena->bytes[block];
        }
    }
    return bytes;
}

uint32_t mqtt_page_arena_size(const mqtt_page_arena *arena, uint32_t block)
{
    return page_arena_find(arena, block).size;
}

mqtt_status mqtt_page_arena_write(const mqtt_page_arena *arena, uint32_t block,
                                  const uint8_t *data, uint32_t length)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (length > 0U) {
        /** @brief Number of bytes the block holds. */
        const uint32_t size = mqtt_page_arena_size(arena, block);

        if (size == 0U) {
            status = MQTT_INVALID_ARGUMENT;
        } else {
            status = mqtt_bytes_copy(mqtt_page_arena_bytes(arena, block), size,
                                     data, length);
        }
    }
    return status;
}

void mqtt_page_arena_wipe(const mqtt_page_arena *arena, uint32_t block)
{
    /** @brief Number of bytes the block holds. */
    const uint32_t size = mqtt_page_arena_size(arena, block);

    if (size > 0U) {
        mqtt_bytes_wipe(mqtt_page_arena_bytes(arena, block), size);
    }
}
