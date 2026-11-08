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
 * @file base_page.h
 * @brief Pages, the unit every arena of the server is cut into, and runs of
 * whole pages.
 *
 * A page is MQTT_PAGE_SIZE bytes, which should be the size of a page of the
 * operating system the library is built for. The build sets it: the Makefile
 * picks the page size of the system it runs on, and anything else that builds
 * the library defines MQTT_PAGE_SIZE itself, or gets 4 KiB. Each arena keeps a
 * few words of metadata for each of its pages, in an array of its own, so an
 * arena works on page numbers and never converts a pointer.
 *
 * A set of runs hands out runs of consecutive pages. Free runs wait on lists by
 * the power of two below their length, a request takes the first run long
 * enough from the list that can hold it, and the rest of that run is given
 * back. A run given back joins the free runs on either side, which the first
 * and last page of each free run make a single step.
 *
 * The page size changes what the caller has to provide, so a library and the
 * program using it must be built with the same one.
 *
 * Reference: D. Knuth, The Art of Computer Programming volume 1, section 2.5,
 * for boundary tags.
 */
#ifndef MQTT_BASE_PAGE_H
#define MQTT_BASE_PAGE_H

#include <stdint.h>

#include "base_status.h"

#if !defined(MQTT_PAGE_SIZE)
/**
 * @brief Number of bytes in a page: 4 KiB, as on most systems, unless the
 * build sets another power of two from 4096 to 65536.
 */
#define MQTT_PAGE_SIZE 4096U
#endif

#if (MQTT_PAGE_SIZE < 4096U) || (MQTT_PAGE_SIZE > 65536U)
#error "MQTT_PAGE_SIZE must be from 4096 to 65536"
#endif
#if (MQTT_PAGE_SIZE & (MQTT_PAGE_SIZE - 1U)) != 0U
#error "MQTT_PAGE_SIZE must be a power of two"
#endif

/** @brief The page number that stands for no page. */
#define MQTT_PAGE_NONE 0xFFFFFFFFU
/** @brief The word of a page's metadata that holds its state and tag. */
#define MQTT_PAGE_WORD_STATE 0U
/** @brief The word that holds the length of the run a page starts. */
#define MQTT_PAGE_WORD_LENGTH 1U
/** @brief The word that links a page to the next one on a list. */
#define MQTT_PAGE_WORD_NEXT 2U
/** @brief The word that links a page to the one before it on a list. */
#define MQTT_PAGE_WORD_PREVIOUS 3U
/** @brief The word left for the owner of a page to count with. */
#define MQTT_PAGE_WORD_COUNT 4U
/** @brief The first word of the bitmap of a page split into slots. */
#define MQTT_PAGE_WORD_BITMAP 5U
/**
 * @brief Number of words of metadata for each page: enough for a bitmap of
 * slots of 16 bytes.
 */
#define MQTT_PAGE_META_WORDS (MQTT_PAGE_WORD_BITMAP + (MQTT_PAGE_SIZE / 512U))
/**
 * @brief The most pages in a set of runs, so that a byte offset into them
 * fits in 32 bits with room for MQTT_PAGE_NONE.
 */
#define MQTT_PAGE_MAX_PAGES ((0xFFFFFFFFU / MQTT_PAGE_SIZE) - 1U)
/** @brief Number of free lists, one for each power of two of a length. */
#define MQTT_PAGE_RUN_LISTS 32U

/** @brief The state of a set of runs of pages. */
typedef struct {
    /** @brief MQTT_PAGE_META_WORDS words for each page. */
    uint32_t *meta;
    /** @brief Number of pages. */
    uint32_t page_count;
    /** @brief Number of pages in free runs. */
    uint32_t pages_free;
    /** @brief The first free run on each list. */
    uint32_t free_heads[MQTT_PAGE_RUN_LISTS];
} mqtt_runs;

/**
 * @brief Set up a set of runs, with every page free.
 * @param[out] runs The runs, which are left as they were if page_count is
 * refused.
 * @param[in] meta MQTT_PAGE_META_WORDS words for each page.
 * @param[in] page_count Number of pages, at most MQTT_PAGE_MAX_PAGES. It may
 * be zero, for a set that hands out nothing.
 * @return MQTT_OK, or MQTT_INVALID_ARGUMENT if page_count is above
 * MQTT_PAGE_MAX_PAGES.
 */
mqtt_status mqtt_runs_init(mqtt_runs *runs, uint32_t *meta,
                           uint32_t page_count);

/**
 * @brief Take a run of pages.
 * @param[in,out] runs The runs.
 * @param[in] length Number of pages, at least one.
 * @return The first page, or MQTT_PAGE_NONE if no free run is that long.
 */
uint32_t mqtt_runs_take(mqtt_runs *runs, uint32_t length);

/**
 * @brief Give a run back. Nothing happens unless the page starts a run in use.
 * @param[in,out] runs The runs.
 * @param[in] first The first page of the run.
 */
void mqtt_runs_give(mqtt_runs *runs, uint32_t first);

/**
 * @brief Find the length of a run in use.
 * @param[in] runs The runs.
 * @param[in] first The first page of the run.
 * @return Number of pages, or zero if the page doesn't start a run in use.
 */
uint32_t mqtt_runs_length(const mqtt_runs *runs, uint32_t first);

/**
 * @brief Find the tag the owner of a run in use gave it.
 * @param[in] runs The runs.
 * @param[in] first The first page of the run, which is in use.
 * @return The tag, from 0 to 0xFFFFFF.
 */
uint32_t mqtt_runs_tag(const mqtt_runs *runs, uint32_t first);

/**
 * @brief Give a run in use a tag of the owner's, which starts as zero. The tag
 * is in the metadata, so the set itself doesn't change.
 * @param[in] runs The runs.
 * @param[in] first The first page of the run, which is in use.
 * @param[in] tag The tag, from 0 to 0xFFFFFF.
 */
void mqtt_runs_set_tag(const mqtt_runs *runs, uint32_t first, uint32_t tag);

#endif /* MQTT_BASE_PAGE_H */
