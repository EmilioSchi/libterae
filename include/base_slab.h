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
 * @file base_slab.h
 * @brief Slots of one size, grouped by page, handed out so that the slots in
 * use stay in as few pages as they can.
 *
 * A slab only counts slots, and the caller keeps the objects, in an array of
 * their own type or in the bytes of a page. The slots of page p are numbered
 * from p times a power of two, the smallest at least page_slots, so finding a
 * slot's page takes a shift and no division. With page_slots a power of two,
 * the numbers have no gaps, and can index an array.
 * Each page has a bitmap of its slots in use and a count of them, in the words
 * base_page.h sets out. The pages that have both free slots and slots in use
 * are on one list, and a slot is taken from the page first on it, so a page
 * that empties stays empty for as long as others have room. A page whose last
 * slot is given back goes to a list of empty pages, or back to the slab's
 * owner, which can then use it for something else.
 *
 * Reference: J. Bonwick, The Slab Allocator, USENIX Summer Technical
 * Conference, 1994.
 */
#ifndef MQTT_BASE_SLAB_H
#define MQTT_BASE_SLAB_H

#include <stdbool.h>
#include <stdint.h>

#include "base_page.h"
#include "base_status.h"

/** @brief The slot number that stands for no slot. */
#define MQTT_SLAB_NONE 0xFFFFFFFFU
/** @brief The most slots in a page: one for each bit of its bitmap. */
#define MQTT_SLAB_PAGE_SLOTS_MAX (MQTT_PAGE_SIZE / 16U)

/** @brief The state of a slab. */
typedef struct {
    /** @brief MQTT_PAGE_META_WORDS words for each page. */
    uint32_t *meta;
    /** @brief Number of slots in a page. */
    uint32_t page_slots;
    /** @brief The power of two the slot numbers of a page start at. */
    uint32_t page_shift;
    /**
     * @brief The slot numbers below which slots exist, which may end part of
     * the way into a page.
     */
    uint32_t slot_count;
    /** @brief The first page with free slots and slots in use. */
    uint32_t partial;
    /** @brief The first page with every slot free, if they are kept. */
    uint32_t empty;
    /** @brief Number of slots in use. */
    uint32_t used;
    /** @brief Number of pages with a slot in use. */
    uint32_t pages_used;
    /** @brief true to keep empty pages, false to give them to the owner. */
    bool keep_empty;
} mqtt_slab;

/**
 * @brief Set up a slab with no pages.
 * @param[out] slab The slab, which is left as it was if page_slots is refused.
 * @param[in] meta MQTT_PAGE_META_WORDS words for each page it may be given.
 * @param[in] page_slots Number of slots in a page, from 1 to
 * MQTT_SLAB_PAGE_SLOTS_MAX.
 * @param[in] slot_count The slot numbers below which slots exist, in all the
 * pages it may be given.
 * @param[in] keep_empty true to keep a page whose slots are all free.
 * @return MQTT_OK, or MQTT_INVALID_ARGUMENT if page_slots is out of range.
 */
mqtt_status mqtt_slab_init(mqtt_slab *slab, uint32_t *meta,
                           uint32_t page_slots, uint32_t slot_count,
                           bool keep_empty);

/**
 * @brief Set up a slab that owns every page its slots need, all of them empty
 * and kept. A slab of objects of one type in an array of their own, whose
 * indexes are the slot numbers.
 * @param[out] slab The slab, which is left as it was if page_slots is refused.
 * @param[in] meta MQTT_PAGE_META_WORDS words for each page.
 * @param[in] page_slots Number of slots in a page: a power of two up to
 * MQTT_SLAB_PAGE_SLOTS_MAX.
 * @param[in] slot_count Number of slots.
 * @return MQTT_OK, or MQTT_INVALID_ARGUMENT if page_slots is not such a
 * power of two.
 */
mqtt_status mqtt_slab_init_all(mqtt_slab *slab, uint32_t *meta,
                               uint32_t page_slots, uint32_t slot_count);

/**
 * @brief Find how many pages a number of slots needs.
 * @param[in] page_slots Number of slots in a page: a power of two.
 * @param[in] slot_count Number of slots.
 * @return Number of pages.
 */
uint32_t mqtt_slab_pages(uint32_t page_slots, uint32_t slot_count);

/**
 * @brief Give a slab a page with every slot free.
 * @param[in,out] slab The slab.
 * @param[in] page The page, which it doesn't have.
 */
void mqtt_slab_add_page(mqtt_slab *slab, uint32_t page);

/**
 * @brief Take a free slot.
 * @param[in,out] slab The slab.
 * @return The slot, or MQTT_SLAB_NONE if no page it has has one.
 */
uint32_t mqtt_slab_take(mqtt_slab *slab);

/**
 * @brief Find whether a slab has no free slot in any page it has.
 * @param[in] slab The slab.
 * @return true if a take would find none.
 */
bool mqtt_slab_full(const mqtt_slab *slab);

/**
 * @brief Give a slot back. A slot not in use is ignored.
 * @param[in,out] slab The slab.
 * @param[in] slot The slot.
 * @return true if its page is now empty and the slab has let it go, which
 * happens only when the slab doesn't keep empty pages.
 */
bool mqtt_slab_give(mqtt_slab *slab, uint32_t slot);

/**
 * @brief Find whether a slot is in use.
 * @param[in] slab The slab.
 * @param[in] slot The slot, which may be out of range.
 * @return true if it is.
 */
bool mqtt_slab_used(const mqtt_slab *slab, uint32_t slot);

/**
 * @brief Find the next slot in use, for a slab that owns all its pages.
 * @param[in] slab The slab.
 * @param[in] from The slot to look from.
 * @return The first slot in use at or after it, or MQTT_SLAB_NONE.
 */
uint32_t mqtt_slab_next(const mqtt_slab *slab, uint32_t from);

#endif /* MQTT_BASE_SLAB_H */
