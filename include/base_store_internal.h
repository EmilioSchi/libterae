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
 * @file base_store_internal.h
 * @brief What the server and the client share to write and read their stores.
 * It is not part of the library's interface.
 *
 * A record is put together from a few bytes of its own, which hold its frame
 * and its fixed fields, and runs that hand over strings and packets where they
 * already are, so nothing large is copied to write it.
 *
 * A restore first reads every place, and chooses what to apply: the newest
 * snapshot copy whose journal segments follow on from it, then those segments.
 * A segment of the generation after a snapshot follows on only if it starts
 * where the one before it ended, and one that starts after a gap only counts
 * with a snapshot of its own generation.
 */
#ifndef MQTT_BASE_STORE_INTERNAL_H
#define MQTT_BASE_STORE_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "base_run_t.h"
#include "base_status.h"
#include "base_store.h"
#include "base_utf8_t.h"

/** @brief Number of bytes a record's fixed fields take at most. */
#define MQTT_JOURNAL_FIELDS 64U

/** @brief Number of bytes a write that finds no room goes to instead. */
#define MQTT_JOURNAL_SINK 8U

/** @brief The index that stands for no place, in a chain. */
#define MQTT_STORE_PLACE_NONE 0xFFFFFFFFU

/**
 * @brief A record being put together: its fixed fields, and the strings and
 * the message it hands over where they are.
 */
typedef struct {
    /** @brief Where the fixed fields, the frame and the CRC go. */
    uint8_t *bytes;
    /** @brief Number of bytes bytes holds. */
    uint32_t capacity;
    /** @brief The record's bytes, in order. */
    mqtt_run *runs;
    /** @brief Where a write goes that finds no room, which fails the record. */
    uint8_t sink[MQTT_JOURNAL_SINK];
    /** @brief Number of bytes of bytes used. */
    uint32_t used;
    /** @brief Number of runs. */
    uint32_t count;
    /** @brief Number of bytes in all the runs. */
    uint32_t length;
    /** @brief true if the last run is in bytes, so that it can grow. */
    bool in_scratch;
    /** @brief true if something did not fit. */
    bool failed;
} mqtt_journal_builder;

/** @brief What a scan of one place found. */
typedef struct {
    /** @brief The outcome: MQTT_OK, MQTT_NOT_FOUND or MQTT_STORE_DAMAGED. */
    mqtt_status status;
    /** @brief What was found. */
    mqtt_store_scan scan;
    /** @brief What was wrong. */
    mqtt_store_fault fault;
} mqtt_store_place;

/** @brief The places a restore applies, in order. */
typedef struct {
    /**
     * @brief The snapshot copy, or MQTT_STORE_PLACE_NONE for the empty
     * snapshot of the first generation.
     */
    uint32_t copy;
    /** @brief The journal place of the snapshot's generation. */
    uint32_t first;
    /**
     * @brief The journal place of the generation after it, or
     * MQTT_STORE_PLACE_NONE.
     */
    uint32_t second;
} mqtt_store_chain;

/**
 * @brief Start a record, keeping room for its kind and length.
 * @param[out] builder The record.
 * @param[out] bytes Where its fixed fields go.
 * @param[in] capacity Number of bytes there.
 * @param[out] runs Room for MQTT_STORE_RECORD_RUNS runs.
 */
void mqtt_journal_start(mqtt_journal_builder *builder, uint8_t *bytes,
                        uint32_t capacity, mqtt_run *runs);

/**
 * @brief Take bytes of the fixed fields, at the end of the record.
 * @param[in,out] builder The record.
 * @param[in] count Number of bytes.
 * @return Where they go: if there is no room, the record fails, and the place
 * given holds MQTT_JOURNAL_SINK bytes.
 */
uint8_t *mqtt_journal_room(mqtt_journal_builder *builder, uint32_t count);

/**
 * @brief Add an integer, in network byte order.
 * @param[in,out] builder The record.
 * @param[in] value The integer.
 * @param[in] count Number of bytes, from 1 to 8.
 */
void mqtt_journal_integer(mqtt_journal_builder *builder, uint64_t value,
                          uint32_t count);

/**
 * @brief Add bytes from where they are, without copying them.
 * @param[in,out] builder The record.
 * @param[in] data The bytes, which stay where they are until it is written.
 * @param[in] length Number of bytes.
 */
void mqtt_journal_span(mqtt_journal_builder *builder, const uint8_t *data,
                       uint32_t length);

/**
 * @brief Add a UTF-8 Encoded String: its length, then its bytes.
 * @param[in,out] builder The record.
 * @param[in] text The string.
 */
void mqtt_journal_string(mqtt_journal_builder *builder, const mqtt_utf8 *text);

/**
 * @brief Write the kind and length of a record, then its CRC.
 * @param[in,out] builder The record, whose content is complete.
 * @param[in] kind The kind of record.
 * @return true if the record is whole, false if something did not fit.
 */
bool mqtt_journal_finish(mqtt_journal_builder *builder, uint8_t kind);

/**
 * @brief Read and check every place of a store, then choose the places to
 * apply.
 *
 * A damaged journal place is a fault. A damaged copy is not, as long as
 * another copy whose segments follow on from it is whole: the newest such is
 * taken. With no whole copy, the journal of the first generation alone is
 * enough, as its snapshot is of a server or a client that holds nothing.
 *
 * @param[in] source The store.
 * @param[in] snapshot_kind The kind of its snapshot segments.
 * @param[in] journal_kind The kind of its journal segments.
 * @param[out] buffer Room for the reads.
 * @param[in] size Number of bytes in buffer.
 * @param[out] copies What each of source->copies copies holds.
 * @param[out] journals What each of the two journal places holds.
 * @param[out] chain The places to apply.
 * @param[out] fault What was wrong, when the result is MQTT_STORE_DAMAGED.
 * @return MQTT_OK; MQTT_NOT_FOUND for a store with nothing in it; or
 * MQTT_STORE_DAMAGED.
 */
mqtt_status mqtt_store_choose(const mqtt_store_source *source,
                              uint8_t snapshot_kind, uint8_t journal_kind,
                              uint8_t *buffer, uint32_t size,
                              mqtt_store_place *copies,
                              mqtt_store_place *journals,
                              mqtt_store_chain *chain,
                              mqtt_store_fault *fault);

#endif /* MQTT_BASE_STORE_INTERNAL_H */
