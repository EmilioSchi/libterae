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
 * @file base_page.c
 * @brief Runs of whole pages.
 *
 * Only the first and the last page of a run say anything about it. The first
 * page of a free run is marked free, with the run's length, and so is the last
 * one when the run is longer than a page. The first page of a run in use is
 * marked used, with its length, and its last page is marked as inside a run.
 * A run's neighbours are always the last page of the run before it and the
 * first page of the run after it, so what the pages between hold doesn't
 * matter.
 */

#include <stddef.h>

#include "base_page.h"

/** @brief The bits of the state word that hold the state. */
#define PAGE_STATE_MASK 0xFFU
/** @brief Number of bits the owner's tag is shifted by in the state word. */
#define PAGE_TAG_SHIFT 8U
/** @brief The largest tag. */
#define PAGE_TAG_MAX 0xFFFFFFU
/** @brief State of the first page of a free run. */
#define PAGE_FREE 1U
/** @brief State of the last page of a free run longer than a page. */
#define PAGE_FREE_END 2U
/** @brief State of the first page of a run in use. */
#define PAGE_USED 3U
/** @brief State of the last page of a run in use longer than a page. */
#define PAGE_INSIDE 4U

/**
 * @brief Find a word of a page's metadata.
 * @param[in] runs The runs.
 * @param[in] page The page.
 * @param[in] word Which word.
 * @return The word.
 */
static uint32_t *page_word(const mqtt_runs *runs, uint32_t page, uint32_t word)
{
    return &runs->meta[(page * MQTT_PAGE_META_WORDS) + word];
}

/**
 * @brief Find a page's state.
 * @param[in] runs The runs.
 * @param[in] page The page.
 * @return The state.
 */
static uint32_t page_state(const mqtt_runs *runs, uint32_t page)
{
    return *page_word(runs, page, MQTT_PAGE_WORD_STATE) & PAGE_STATE_MASK;
}

/**
 * @brief Find which list holds free runs of a length: the power of two at or
 * below it.
 * @param[in] length Number of pages, at least one.
 * @return The list.
 */
static uint32_t page_list(uint32_t length)
{
    /** @brief The list. */
    uint32_t list = 0U;
    /** @brief What is left of the length. */
    uint32_t rest = length;

    while (rest > 1U) {
        rest >>= 1U;
        list += 1U;
    }
    return list;
}

/**
 * @brief Mark a free run and put it at the front of its list.
 * @param[in,out] runs The runs.
 * @param[in] first The first page.
 * @param[in] length Number of pages, at least one.
 */
static void page_push(mqtt_runs *runs, uint32_t first, uint32_t length)
{
    /** @brief The list. */
    const uint32_t list = page_list(length);
    /** @brief The run that was first on it. */
    const uint32_t head = runs->free_heads[list];

    *page_word(runs, first, MQTT_PAGE_WORD_STATE) = PAGE_FREE;
    *page_word(runs, first, MQTT_PAGE_WORD_LENGTH) = length;
    if (length > 1U) {
        /** @brief The last page. */
        const uint32_t last = (first + length) - 1U;

        *page_word(runs, last, MQTT_PAGE_WORD_STATE) = PAGE_FREE_END;
        *page_word(runs, last, MQTT_PAGE_WORD_LENGTH) = length;
    }
    *page_word(runs, first, MQTT_PAGE_WORD_NEXT) = head;
    *page_word(runs, first, MQTT_PAGE_WORD_PREVIOUS) = MQTT_PAGE_NONE;
    if (head != MQTT_PAGE_NONE) {
        *page_word(runs, head, MQTT_PAGE_WORD_PREVIOUS) = first;
    }
    runs->free_heads[list] = first;
}

/**
 * @brief Take a free run off its list.
 * @param[in,out] runs The runs.
 * @param[in] first The first page of the run.
 */
static void page_unlink(mqtt_runs *runs, uint32_t first)
{
    /** @brief The run after it. */
    const uint32_t next = *page_word(runs, first, MQTT_PAGE_WORD_NEXT);
    /** @brief The run before it. */
    const uint32_t previous = *page_word(runs, first, MQTT_PAGE_WORD_PREVIOUS);

    if (previous == MQTT_PAGE_NONE) {
        runs->free_heads[page_list(
            *page_word(runs, first, MQTT_PAGE_WORD_LENGTH))] = next;
    } else {
        *page_word(runs, previous, MQTT_PAGE_WORD_NEXT) = next;
    }
    if (next != MQTT_PAGE_NONE) {
        *page_word(runs, next, MQTT_PAGE_WORD_PREVIOUS) = previous;
    }
}

/**
 * @brief Find a free run at least as long as asked for.
 * @param[in] runs The runs.
 * @param[in] length Number of pages, at least one.
 * @return Its first page, or MQTT_PAGE_NONE.
 */
static uint32_t page_find(const mqtt_runs *runs, uint32_t length)
{
    /** @brief The list that may hold a run long enough. */
    uint32_t list = page_list(length);
    /** @brief The run being looked at. */
    uint32_t run = runs->free_heads[list];
    /** @brief The run found. */
    uint32_t found = MQTT_PAGE_NONE;

    while ((found == MQTT_PAGE_NONE) && (run != MQTT_PAGE_NONE)) {
        if (*page_word(runs, run, MQTT_PAGE_WORD_LENGTH) >= length) {
            found = run;
        } else {
            run = *page_word(runs, run, MQTT_PAGE_WORD_NEXT);
        }
    }
    /* Every run on a later list is longer than any on this one. */
    list += 1U;
    while ((found == MQTT_PAGE_NONE) && (list < MQTT_PAGE_RUN_LISTS)) {
        found = runs->free_heads[list];
        list += 1U;
    }
    return found;
}

mqtt_status mqtt_runs_init(mqtt_runs *runs, uint32_t *meta,
                           uint32_t page_count)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_INVALID_ARGUMENT;

    /* Past this, a byte offset into the pages no longer fits 32 bits. */
    if ((runs != NULL) && ((meta != NULL) || (page_count == 0U)) &&
        (page_count <= MQTT_PAGE_MAX_PAGES)) {
        /** @brief Index of the list or page being cleared. */
        uint32_t index = 0U;

        runs->meta = meta;
        runs->page_count = page_count;
        runs->pages_free = page_count;
        while (index < MQTT_PAGE_RUN_LISTS) {
            runs->free_heads[index] = MQTT_PAGE_NONE;
            index += 1U;
        }
        /* No page may look as if it starts a run in use, whatever the
         * memory held, or giving back a page inside a run could free it. */
        index = 0U;
        while (index < page_count) {
            *page_word(runs, index, MQTT_PAGE_WORD_STATE) = PAGE_INSIDE;
            index += 1U;
        }
        if (page_count > 0U) {
            page_push(runs, 0U, page_count);
        }
        status = MQTT_OK;
    }
    return status;
}

uint32_t mqtt_runs_take(mqtt_runs *runs, uint32_t length)
{
    /** @brief The run handed back. */
    uint32_t first = MQTT_PAGE_NONE;

    if ((runs != NULL) && (length > 0U) && (length <= runs->pages_free)) {
        first = page_find(runs, length);
    }
    if (first != MQTT_PAGE_NONE) {
        /** @brief Length of the free run. */
        const uint32_t have = *page_word(runs, first, MQTT_PAGE_WORD_LENGTH);

        page_unlink(runs, first);
        if (have > length) {
            page_push(runs, first + length, have - length);
        }
        *page_word(runs, first, MQTT_PAGE_WORD_STATE) = PAGE_USED;
        *page_word(runs, first, MQTT_PAGE_WORD_LENGTH) = length;
        if (length > 1U) {
            *page_word(runs, (first + length) - 1U, MQTT_PAGE_WORD_STATE) =
                PAGE_INSIDE;
        }
        runs->pages_free -= length;
    }
    return first;
}

void mqtt_runs_give(mqtt_runs *runs, uint32_t first)
{
    if ((runs != NULL) && (first < runs->page_count) &&
        (page_state(runs, first) == PAGE_USED)) {
        /** @brief Length of the run. */
        const uint32_t length = *page_word(runs, first, MQTT_PAGE_WORD_LENGTH);
        /** @brief The first page after it. */
        const uint32_t after = first + length;
        /** @brief The first page of the free run it becomes part of. */
        uint32_t start = first;
        /** @brief Length of that free run. */
        uint32_t total = length;

        runs->pages_free += length;
        *page_word(runs, first, MQTT_PAGE_WORD_STATE) = PAGE_INSIDE;
        if ((first > 0U) && (page_state(runs, first - 1U) == PAGE_FREE)) {
            start = first - 1U;
        } else if ((first > 0U) &&
                   (page_state(runs, first - 1U) == PAGE_FREE_END)) {
            start = first -
                    *page_word(runs, first - 1U, MQTT_PAGE_WORD_LENGTH);
        } else {
            /* The run before is in use, or there is none. */
        }
        if (start != first) {
            total += *page_word(runs, start, MQTT_PAGE_WORD_LENGTH);
            page_unlink(runs, start);
        }
        if ((after < runs->page_count) &&
            (page_state(runs, after) == PAGE_FREE)) {
            total += *page_word(runs, after, MQTT_PAGE_WORD_LENGTH);
            page_unlink(runs, after);
        }
        page_push(runs, start, total);
    }
}

uint32_t mqtt_runs_length(const mqtt_runs *runs, uint32_t first)
{
    /** @brief The answer. */
    uint32_t length = 0U;

    if ((runs != NULL) && (first < runs->page_count) &&
        (page_state(runs, first) == PAGE_USED)) {
        length = *page_word(runs, first, MQTT_PAGE_WORD_LENGTH);
    }
    return length;
}

uint32_t mqtt_runs_tag(const mqtt_runs *runs, uint32_t first)
{
    return (runs != NULL) ?
               (*page_word(runs, first, MQTT_PAGE_WORD_STATE) >>
                PAGE_TAG_SHIFT) :
               0U;
}

void mqtt_runs_set_tag(const mqtt_runs *runs, uint32_t first, uint32_t tag)
{
    if (runs != NULL) {
        *page_word(runs, first, MQTT_PAGE_WORD_STATE) =
            ((tag & PAGE_TAG_MAX) << PAGE_TAG_SHIFT) | PAGE_USED;
    }
}
