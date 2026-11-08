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
 * @file base_slab.c
 * @brief Slots of one size, grouped by page.
 *
 * A page's count is the number of its slots in use. A last page that is short
 * has fewer slots than the others: the bits of the slots past slot_count are
 * set from the start, and so are the bits past page_slots in the last word of
 * a bitmap, so a search for a free slot never finds them. They are not
 * counted.
 */

#include <stddef.h>

#include "base_slab.h"

/** @brief Number of bits in a word of a bitmap. */
#define SLAB_WORD_BITS 32U
/** @brief A word with every bit set. */
#define SLAB_WORD_FULL 0xFFFFFFFFU
/** @brief One, as a word, to shift into a power of two. */
#define SLAB_ONE ((uint32_t)1U)

/**
 * @brief Find a word of a page's metadata.
 * @param[in] slab The slab.
 * @param[in] page The page.
 * @param[in] word Which word.
 * @return The word.
 */
static uint32_t *slab_word(const mqtt_slab *slab, uint32_t page, uint32_t word)
{
    return &slab->meta[(page * MQTT_PAGE_META_WORDS) + word];
}

/**
 * @brief Find the index of the one bit set in a word.
 * @param[in] bit A word with exactly one bit set.
 * @return The bit's index, from 0.
 */
static uint32_t slab_bit_index(uint32_t bit)
{
    /** @brief The index. */
    uint32_t index = 0U;
    /** @brief The word, shifted as the search narrows. */
    uint32_t rest = bit;

    if ((rest & 0xFFFFU) == 0U) {
        index += 16U;
        rest >>= 16U;
    }
    if ((rest & 0xFFU) == 0U) {
        index += 8U;
        rest >>= 8U;
    }
    if ((rest & 0xFU) == 0U) {
        index += 4U;
        rest >>= 4U;
    }
    if ((rest & 0x3U) == 0U) {
        index += 2U;
        rest >>= 2U;
    }
    if ((rest & 0x1U) == 0U) {
        index += 1U;
    }
    return index;
}

/**
 * @brief Find the smallest power of two that is at least a number of slots.
 * @param[in] page_slots Number of slots in a page, at least one.
 * @return The power. It stops at 31, the most a word can be shifted by, for a
 * number above any power of two a word holds.
 */
static uint32_t slab_shift(uint32_t page_slots)
{
    /** @brief The power. */
    uint32_t shift = 0U;

    while ((shift < (SLAB_WORD_BITS - 1U)) &&
           ((SLAB_ONE << shift) < page_slots)) {
        shift += 1U;
    }
    return shift;
}

/**
 * @brief Find how many slots of a page exist.
 * @param[in] slab The slab.
 * @param[in] page The page.
 * @return page_slots, or fewer for a last page that is short.
 */
static uint32_t slab_usable(const mqtt_slab *slab, uint32_t page)
{
    /** @brief Number of slots from the page's first to the end. */
    const uint32_t left = slab->slot_count - (page << slab->page_shift);

    return (left < slab->page_slots) ? left : slab->page_slots;
}

/**
 * @brief Put a page at the front of a list.
 * @param[in,out] slab The slab.
 * @param[in,out] head The list's first page.
 * @param[in] page The page.
 */
static void slab_push(const mqtt_slab *slab, uint32_t *head, uint32_t page)
{
    *slab_word(slab, page, MQTT_PAGE_WORD_NEXT) = *head;
    *slab_word(slab, page, MQTT_PAGE_WORD_PREVIOUS) = MQTT_PAGE_NONE;
    if (*head != MQTT_PAGE_NONE) {
        *slab_word(slab, *head, MQTT_PAGE_WORD_PREVIOUS) = page;
    }
    *head = page;
}

/**
 * @brief Take a page off a list.
 * @param[in,out] slab The slab.
 * @param[in,out] head The list's first page.
 * @param[in] page The page, which is on the list.
 */
static void slab_unlink(const mqtt_slab *slab, uint32_t *head, uint32_t page)
{
    /** @brief The page after it. */
    const uint32_t next = *slab_word(slab, page, MQTT_PAGE_WORD_NEXT);
    /** @brief The page before it. */
    const uint32_t previous = *slab_word(slab, page, MQTT_PAGE_WORD_PREVIOUS);

    if (previous == MQTT_PAGE_NONE) {
        *head = next;
    } else {
        *slab_word(slab, previous, MQTT_PAGE_WORD_NEXT) = next;
    }
    if (next != MQTT_PAGE_NONE) {
        *slab_word(slab, next, MQTT_PAGE_WORD_PREVIOUS) = previous;
    }
}

mqtt_status mqtt_slab_init(mqtt_slab *slab, uint32_t *meta,
                           uint32_t page_slots, uint32_t slot_count,
                           bool keep_empty)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_INVALID_ARGUMENT;

    /* A page's metadata has a bit for no more slots than this. */
    if ((slab != NULL) && (meta != NULL) && (page_slots > 0U) &&
        (page_slots <= MQTT_SLAB_PAGE_SLOTS_MAX)) {
        slab->meta = meta;
        slab->page_slots = page_slots;
        slab->page_shift = slab_shift(page_slots);
        slab->slot_count = slot_count;
        slab->partial = MQTT_PAGE_NONE;
        slab->empty = MQTT_PAGE_NONE;
        slab->used = 0U;
        slab->pages_used = 0U;
        slab->keep_empty = keep_empty;
        status = MQTT_OK;
    }
    return status;
}

uint32_t mqtt_slab_pages(uint32_t page_slots, uint32_t slot_count)
{
    /** @brief The power of two of a page's slots. */
    const uint32_t shift = slab_shift(page_slots);

    return (slot_count >> shift) +
           (((slot_count & ((SLAB_ONE << shift) - 1U)) != 0U) ? 1U : 0U);
}

mqtt_status mqtt_slab_init_all(mqtt_slab *slab, uint32_t *meta,
                               uint32_t page_slots, uint32_t slot_count)
{
    /** @brief The page being added, from the last, so the first is first. */
    uint32_t page = mqtt_slab_pages(page_slots, slot_count);
    /** @brief The outcome. */
    mqtt_status status = MQTT_INVALID_ARGUMENT;

    /* The slot numbers are the indexes of an array, which only a power of two
     * of slots to a page fills with no gaps. */
    if ((page_slots & (page_slots - 1U)) == 0U) {
        status = mqtt_slab_init(slab, meta, page_slots, slot_count, true);
    }
    while ((status == MQTT_OK) && (page > 0U)) {
        page -= 1U;
        mqtt_slab_add_page(slab, page);
    }
    return status;
}

/**
 * @brief Do what mqtt_slab_add_page does, for arguments known to be valid.
 * @param[in,out] slab The slab.
 * @param[in] page The page, which it doesn't have.
 */
static void slab_add_page_body(mqtt_slab *slab, uint32_t page)
{
    /** @brief Number of words in the bitmap. */
    const uint32_t words =
        (slab->page_slots + (SLAB_WORD_BITS - 1U)) / SLAB_WORD_BITS;
    /** @brief Number of slots of the page that exist. */
    const uint32_t usable = slab_usable(slab, page);
    /** @brief Index of the word or bit being set. */
    uint32_t index = 0U;

    while (index < words) {
        *slab_word(slab, page, MQTT_PAGE_WORD_BITMAP + index) = 0U;
        index += 1U;
    }
    index = usable;
    while (index < (words * SLAB_WORD_BITS)) {
        /** @brief The word of the bit. */
        uint32_t *word = slab_word(
            slab, page, MQTT_PAGE_WORD_BITMAP + (index / SLAB_WORD_BITS));

        *word |= 1U << (index % SLAB_WORD_BITS);
        index += 1U;
    }
    *slab_word(slab, page, MQTT_PAGE_WORD_COUNT) = 0U;
    if (slab->keep_empty) {
        slab_push(slab, &slab->empty, page);
    } else {
        slab_push(slab, &slab->partial, page);
    }
}

void mqtt_slab_add_page(mqtt_slab *slab, uint32_t page)
{
    if (slab != NULL) {
        slab_add_page_body(slab, page);
    }
}

/**
 * @brief Do what mqtt_slab_take does, for arguments known to be valid.
 * @param[in,out] slab The slab.
 * @return The slot, or MQTT_SLAB_NONE if no page it has has one.
 */
static uint32_t slab_take_body(mqtt_slab *slab)
{
    /** @brief true if the page comes from the empty ones. */
    const bool was_empty = (slab->partial == MQTT_PAGE_NONE);
    /** @brief The page. */
    const uint32_t page = was_empty ? slab->empty : slab->partial;
    /** @brief The slot handed back. */
    uint32_t slot = MQTT_SLAB_NONE;

    if (page != MQTT_PAGE_NONE) {
        /** @brief Index of the word of the bitmap. */
        uint32_t index = 0U;
        /** @brief The word. */
        uint32_t *word = slab_word(slab, page, MQTT_PAGE_WORD_BITMAP);
        /** @brief The page's count. */
        uint32_t *count = slab_word(slab, page, MQTT_PAGE_WORD_COUNT);

        /* The page has a free slot, so a word with a bit clear is found. */
        while (*word == SLAB_WORD_FULL) {
            index += 1U;
            word = slab_word(slab, page, MQTT_PAGE_WORD_BITMAP + index);
        }
        {
            /** @brief The lowest bit clear, as the only bit set. */
            const uint32_t bit = (~*word) & (*word + 1U);

            *word |= bit;
            slot = (page << slab->page_shift) + (index * SLAB_WORD_BITS) +
                   slab_bit_index(bit);
        }
        *count += 1U;
        if (*count == 1U) {
            slab->pages_used += 1U;
        }
        if (was_empty) {
            slab_unlink(slab, &slab->empty, page);
            if (*count < slab_usable(slab, page)) {
                slab_push(slab, &slab->partial, page);
            }
        } else if (*count == slab_usable(slab, page)) {
            slab_unlink(slab, &slab->partial, page);
        } else {
            /* It stays first on the list, with room still. */
        }
        slab->used += 1U;
    }
    return slot;
}

uint32_t mqtt_slab_take(mqtt_slab *slab)
{
    /** @brief The answer. */
    uint32_t answer = MQTT_SLAB_NONE;

    if (slab != NULL) {
        answer = slab_take_body(slab);
    }
    return answer;
}

bool mqtt_slab_full(const mqtt_slab *slab)
{
    return (slab != NULL) && (slab->partial == MQTT_PAGE_NONE) &&
           (slab->empty == MQTT_PAGE_NONE);
}

bool mqtt_slab_give(mqtt_slab *slab, uint32_t slot)
{
    /** @brief true if the page has been let go. */
    bool released = false;

    if (mqtt_slab_used(slab, slot)) {
        /** @brief The page. */
        const uint32_t page = slot >> slab->page_shift;
        /** @brief The slot's index in the page. */
        const uint32_t index = slot & ((SLAB_ONE << slab->page_shift) - 1U);
        /** @brief The page's count. */
        uint32_t *count = slab_word(slab, page, MQTT_PAGE_WORD_COUNT);

        *slab_word(slab, page, MQTT_PAGE_WORD_BITMAP +
                                   (index / SLAB_WORD_BITS)) &=
            ~(1U << (index % SLAB_WORD_BITS));
        if (*count == slab_usable(slab, page)) {
            slab_push(slab, &slab->partial, page);
        }
        *count -= 1U;
        slab->used -= 1U;
        if (*count == 0U) {
            slab->pages_used -= 1U;
            slab_unlink(slab, &slab->partial, page);
            if (slab->keep_empty) {
                slab_push(slab, &slab->empty, page);
            } else {
                released = true;
            }
        }
    }
    return released;
}

bool mqtt_slab_used(const mqtt_slab *slab, uint32_t slot)
{
    /** @brief The answer. */
    bool used = false;

    if ((slab != NULL) && (slot < slab->slot_count)) {
        /** @brief The slot's index in its page. */
        const uint32_t index = slot & ((SLAB_ONE << slab->page_shift) - 1U);

        /* An index past the page's slots is a gap between pages. */
        used = (index < slab->page_slots) &&
               (((*slab_word(slab, slot >> slab->page_shift,
                             MQTT_PAGE_WORD_BITMAP +
                                 (index / SLAB_WORD_BITS)) >>
                  (index % SLAB_WORD_BITS)) &
                 1U) != 0U);
    }
    return used;
}

/**
 * @brief Find the first slot in use in one page, at or after an index.
 * @param[in] slab The slab.
 * @param[in] page The page.
 * @param[in] from The index in the page to look from.
 * @return The index, or page_slots if there is none.
 */
static uint32_t slab_next_in_page(const mqtt_slab *slab, uint32_t page,
                                  uint32_t from)
{
    /** @brief Number of words in the bitmap. */
    const uint32_t words =
        (slab->page_slots + (SLAB_WORD_BITS - 1U)) / SLAB_WORD_BITS;
    /** @brief Index of the word. */
    uint32_t index = from / SLAB_WORD_BITS;
    /** @brief The bits of the first word to look at. */
    uint32_t mask = SLAB_WORD_FULL << (from % SLAB_WORD_BITS);
    /** @brief The index found. */
    uint32_t found = slab->page_slots;

    while ((found == slab->page_slots) && (index < words)) {
        /** @brief The bits in use, of those to look at. */
        const uint32_t bits =
            *slab_word(slab, page, MQTT_PAGE_WORD_BITMAP + index) & mask;

        if (bits != 0U) {
            /** @brief The index of the lowest of them. */
            const uint32_t bit =
                (index * SLAB_WORD_BITS) + slab_bit_index(bits & (~bits + 1U));

            /* A bit past the slots of the page ends the search too. */
            if (bit < slab->page_slots) {
                found = bit;
            }
            index = words;
        }
        mask = SLAB_WORD_FULL;
        index += 1U;
    }
    return found;
}

/**
 * @brief Do what mqtt_slab_next does, for arguments known to be valid.
 * @param[in] slab The slab.
 * @param[in] from The slot to look from.
 * @return The first slot in use at or after it, or MQTT_SLAB_NONE.
 */
static uint32_t slab_next_body(const mqtt_slab *slab, uint32_t from)
{
    /** @brief The slot found. */
    uint32_t found = MQTT_SLAB_NONE;
    /** @brief The slot to look from. */
    uint32_t slot = from;

    while ((found == MQTT_SLAB_NONE) && (slot < slab->slot_count)) {
        /** @brief The page. */
        const uint32_t page = slot >> slab->page_shift;
        /** @brief The page's first slot. */
        const uint32_t first = page << slab->page_shift;

        if (*slab_word(slab, page, MQTT_PAGE_WORD_COUNT) != 0U) {
            /** @brief Index in the page of the slot in use. */
            const uint32_t index = slab_next_in_page(slab, page, slot - first);

            if ((index < slab->page_slots) &&
                ((first + index) < slab->slot_count)) {
                found = first + index;
            }
        }
        slot = first + (SLAB_ONE << slab->page_shift);
    }
    return found;
}

uint32_t mqtt_slab_next(const mqtt_slab *slab, uint32_t from)
{
    /** @brief The answer. */
    uint32_t answer = MQTT_SLAB_NONE;

    if (slab != NULL) {
        answer = slab_next_body(slab, from);
    }
    return answer;
}
