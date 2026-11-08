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
 * @file platform_store_flash.c
 * @brief A store kept in a region of flash.
 *
 * The region is cut into places of whole erase blocks, one for each snapshot
 * copy and two for the journal, all the same size. A place is read whole: what
 * follows its last record is erased, 0xFF, which is how a restore finds where
 * its records end. Starting a journal segment or a snapshot copy erases its
 * place first, so a place is only ever programmed where it is erased, from its
 * start on.
 *
 * Where a place's records end is known only for a place begun since the store
 * was opened. That is all a server or a client needs: a restore reads what
 * every place holds, and the snapshot after it begins a new segment before any
 * record is added. A record that can't be programmed whole leaves its place
 * full, as flash can't take back what it programmed, so the journal breaks and
 * the snapshot that mends it erases the place.
 *
 * Flash keeps no time, so the downtime a restore is told is 0. Setting a store
 * aside asks the board to keep what the region holds elsewhere, then erases it.
 */

#include "base_platform.h"
#include "base_string.h"
#include "platform_flash.h"

/** @brief The number of places of a store, at most. */
#define FLASH_PLACES (MQTT_PLATFORM_STORE_COPIES_MAX + 2U)

/** @brief Where a place's records end, when it isn't known. */
#define FLASH_UNKNOWN 0xFFFFFFFFU

/** @brief Number of bytes a copy is written from another in. */
#define FLASH_COPY_CHUNK 256U

/** @brief The longest name of a region, with its end. */
#define FLASH_NAME_MAX 64U

/** @brief One store that may be open. */
typedef struct {
    /** @brief true while it is open. */
    bool open;
    /** @brief Number of snapshot copies. */
    uint32_t copies;
    /** @brief The name of the region, with its end. */
    char where[FLASH_NAME_MAX];
    /** @brief The region of flash. */
    uint32_t region;
    /** @brief Number of bytes of each place. */
    uint32_t place_size;
    /** @brief Where the records of each place end, or FLASH_UNKNOWN. */
    uint32_t used[FLASH_PLACES];
    /** @brief The journal place records go to, or FLASH_UNKNOWN. */
    uint32_t journal;
    /** @brief true once a COMMIT asks for the journal to be synced. */
    bool sync_needed;
} flash_entry;

/** @brief The stores that may be open. */
static flash_entry flash_table[MQTT_PLATFORM_STORES];

/**
 * @brief Find an open store.
 * @param[in] store The store.
 * @return Its entry, or NULL if it isn't open.
 */
static flash_entry *flash_find(int32_t store);

/**
 * @brief Find whether a store of a name is open.
 * @param[in] where The name.
 * @param[in] length Number of bytes of it.
 * @return true if it is.
 */
static bool flash_named(const char *where, uint32_t length);

/**
 * @brief Find the index of a place among a store's places.
 * @param[in] entry The store.
 * @param[in] part The part.
 * @param[in] place The copy, or the journal place.
 * @return The index, or FLASH_UNKNOWN if there is no such place.
 */
static uint32_t flash_index(const flash_entry *entry, mqtt_store_part part,
                            uint32_t place);

/**
 * @brief Erase a place, and program a segment header at its start.
 * @param[in,out] entry The store.
 * @param[in] index The place.
 * @param[in] header The MQTT_STORE_HEADER_SIZE bytes of the header.
 * @return MQTT_OK, or MQTT_INVALID_STATE if the flash failed.
 */
static mqtt_status flash_restart(flash_entry *entry, uint32_t index,
                                 const uint8_t *header);

/**
 * @brief Program a record at the end of a place's records. If it can't be
 * programmed whole, or doesn't fit, the place counts as full.
 * @param[in,out] entry The store.
 * @param[in] index The place.
 * @param[in] record The record.
 * @return MQTT_OK, or MQTT_INVALID_STATE.
 */
static mqtt_status flash_add(flash_entry *entry, uint32_t index,
                             const mqtt_store_record *record);

/**
 * @brief Write one copy of the snapshot from the first: erase it, then program
 * what the first holds.
 * @param[in] entry The store.
 * @param[in] copy The copy.
 * @return MQTT_OK, or MQTT_INVALID_STATE if the flash failed.
 */
static mqtt_status flash_duplicate(const flash_entry *entry, uint32_t copy);

static flash_entry *flash_find(int32_t store)
{
    /** @brief The answer. */
    flash_entry *entry = NULL;

    if ((store >= 0) && ((uint32_t)store < MQTT_PLATFORM_STORES) &&
        flash_table[store].open) {
        entry = &flash_table[store];
    }
    return entry;
}

static bool flash_named(const char *where, uint32_t length)
{
    /** @brief Index of the entry being looked at. */
    uint32_t index = 0U;
    /** @brief The answer. */
    bool named = false;

    while ((!named) && (index < MQTT_PLATFORM_STORES)) {
        /* The names' ends are compared too, so a longer one isn't taken. */
        named = flash_table[index].open &&
                mqtt_bytes_equal((const uint8_t *)flash_table[index].where,
                                 (const uint8_t *)where, length + 1U);
        index += 1U;
    }
    return named;
}

static uint32_t flash_index(const flash_entry *entry, mqtt_store_part part,
                            uint32_t place)
{
    /** @brief The answer. */
    uint32_t index = FLASH_UNKNOWN;

    if ((part == MQTT_STORE_SNAPSHOT) && (place < entry->copies)) {
        index = place;
    } else if ((part == MQTT_STORE_JOURNAL) && (place < 2U)) {
        index = entry->copies + place;
    } else {
        /* No such place. */
    }
    return index;
}

static mqtt_status flash_restart(flash_entry *entry, uint32_t index,
                                 const uint8_t *header)
{
    /** @brief Where the place starts in the region. */
    const uint32_t start = index * entry->place_size;
    /** @brief The outcome. */
    mqtt_status status = mqtt_platform_flash_erase(entry->region, start,
                                                   entry->place_size);

    /* Until it is whole, nothing more goes into it. */
    entry->used[index] = entry->place_size;
    if (status == MQTT_OK) {
        status = mqtt_platform_flash_program(entry->region, start, header,
                                             MQTT_STORE_HEADER_SIZE);
    }
    if (status == MQTT_OK) {
        entry->used[index] = MQTT_STORE_HEADER_SIZE;
    }
    return status;
}

static mqtt_status flash_add(flash_entry *entry, uint32_t index,
                             const mqtt_store_record *record)
{
    /** @brief Where the place's records end. */
    const uint32_t used = entry->used[index];
    /** @brief Index of the run being programmed. */
    uint32_t run = 0U;
    /** @brief Where the next run goes, from the place's start. */
    uint32_t at = used;
    /** @brief The outcome. */
    mqtt_status status = ((used != FLASH_UNKNOWN) &&
                          (record->length <= (entry->place_size - used))) ?
                             MQTT_OK :
                             MQTT_INVALID_STATE;

    while ((status == MQTT_OK) && (run < record->run_count)) {
        /** @brief The run. */
        const mqtt_run *piece = &record->runs[run];

        status = mqtt_platform_flash_program(
            entry->region, (index * entry->place_size) + at, piece->data,
            piece->length);
        at += piece->length;
        run += 1U;
    }
    /* What was programmed of a record that failed can't be taken back. */
    entry->used[index] = (status == MQTT_OK) ? at : entry->place_size;
    return status;
}

static mqtt_status flash_duplicate(const flash_entry *entry, uint32_t copy)
{
    /** @brief A part of the first copy. */
    static uint8_t chunk[FLASH_COPY_CHUNK];
    /** @brief Number of bytes to write. */
    const uint32_t length = entry->used[0];
    /** @brief Number of bytes written so far. */
    uint32_t done = 0U;
    /** @brief The outcome. */
    mqtt_status status =
        (length == FLASH_UNKNOWN) ?
            MQTT_INVALID_STATE :
            mqtt_platform_flash_erase(entry->region,
                                      copy * entry->place_size,
                                      entry->place_size);

    while ((status == MQTT_OK) && (done < length)) {
        /** @brief Number of bytes now. */
        const uint32_t take = ((length - done) < FLASH_COPY_CHUNK) ?
                                  (length - done) :
                                  FLASH_COPY_CHUNK;

        status = mqtt_platform_flash_read(entry->region, done, chunk, take);
        if (status == MQTT_OK) {
            status = mqtt_platform_flash_program(
                entry->region, (copy * entry->place_size) + done, chunk, take);
        }
        done += take;
    }
    if (status == MQTT_OK) {
        status = mqtt_platform_flash_sync(entry->region);
    }
    return status;
}

mqtt_status mqtt_platform_store_open(const char *where, uint32_t copies,
                                     int32_t *store)
{
    /** @brief Index of the entry being looked at. */
    uint32_t index = 0U;
    /** @brief Number of bytes of the name. */
    uint32_t length = 0U;
    /** @brief The region. */
    uint32_t region = 0U;
    /** @brief Number of bytes in it. */
    uint32_t size = 0U;
    /** @brief Number of bytes of an erase block. */
    uint32_t block = 0U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_INVALID_ARGUMENT;

    *store = MQTT_STORE_NONE;
    if (!mqtt_text_length(where, FLASH_NAME_MAX - 1U, &length)) {
        length = 0U;
    }
    if ((length > 0U) && (copies >= MQTT_PLATFORM_STORE_COPIES_MIN) &&
        (copies <= MQTT_PLATFORM_STORE_COPIES_MAX)) {
        status = MQTT_QUOTA_EXCEEDED;
        while ((index < MQTT_PLATFORM_STORES) && flash_table[index].open) {
            index += 1U;
        }
    }
    if ((status == MQTT_QUOTA_EXCEEDED) && (index < MQTT_PLATFORM_STORES)) {
        /* Two stores in one region would write over each other. */
        status = flash_named(where, length) ?
                     MQTT_INVALID_STATE :
                     mqtt_platform_flash_region(where, &region, &size, &block);
    }
    if ((status == MQTT_OK) &&
        ((block == 0U) || ((size / block) < (copies + 2U)))) {
        /* Too small for a place of a block each. */
        status = MQTT_INVALID_STATE;
    }
    if (status == MQTT_OK) {
        /** @brief The entry. */
        flash_entry *entry = &flash_table[index];
        /** @brief Index of a place. */
        uint32_t place = 0U;

        entry->copies = copies;
        mqtt_bytes_put((uint8_t *)entry->where, (const uint8_t *)where,
                       length + 1U);
        entry->region = region;
        entry->place_size = ((size / block) / (copies + 2U)) * block;
        while (place < FLASH_PLACES) {
            entry->used[place] = FLASH_UNKNOWN;
            place += 1U;
        }
        entry->journal = FLASH_UNKNOWN;
        entry->sync_needed = false;
        entry->open = true;
        *store = (int32_t)index;
    }
    return status;
}

void mqtt_platform_store_close(int32_t store)
{
    /** @brief The store. */
    flash_entry *entry = flash_find(store);

    if (entry != NULL) {
        entry->open = false;
    }
}

uint32_t mqtt_platform_store_copies(int32_t store)
{
    /** @brief The store. */
    const flash_entry *entry = flash_find(store);

    return (entry == NULL) ? 0U : entry->copies;
}

mqtt_status mqtt_platform_store_append(uint32_t user_tag,
                                       const mqtt_store_record *record)
{
    /** @brief The store. */
    flash_entry *entry = flash_find((int32_t)user_tag);
    /** @brief The outcome. */
    mqtt_status status = MQTT_INVALID_STATE;

    if ((entry != NULL) && (entry->journal != FLASH_UNKNOWN)) {
        status = flash_add(entry, entry->copies + entry->journal, record);
    }
    if ((status == MQTT_OK) &&
        (record->kind == (uint8_t)MQTT_STORE_RECORD_COMMIT) &&
        (record->durability == MQTT_STORE_SYNCED)) {
        entry->sync_needed = true;
    }
    return status;
}

mqtt_status mqtt_platform_store_commit(int32_t store)
{
    /** @brief The store. */
    flash_entry *entry = flash_find(store);
    /** @brief The outcome. */
    mqtt_status status = (entry == NULL) ? MQTT_INVALID_STATE : MQTT_OK;

    if ((status == MQTT_OK) && entry->sync_needed) {
        status = mqtt_platform_flash_sync(entry->region);
        entry->sync_needed = false;
    }
    return status;
}

mqtt_status mqtt_platform_store_read(uint32_t user_tag, mqtt_store_part part,
                                     uint32_t place, uint64_t offset,
                                     uint8_t *data, uint32_t capacity,
                                     uint32_t *count)
{
    /** @brief The store. */
    const flash_entry *entry = flash_find((int32_t)user_tag);
    /** @brief The place, among the store's. */
    uint32_t index = FLASH_UNKNOWN;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    *count = 0U;
    if (entry == NULL) {
        status = MQTT_INVALID_STATE;
    } else {
        index = flash_index(entry, part, place);
    }
    if ((index != FLASH_UNKNOWN) && (offset < (uint64_t)entry->place_size)) {
        /** @brief Number of bytes after the offset. */
        const uint32_t left = entry->place_size - (uint32_t)offset;
        /** @brief Number of bytes read. */
        const uint32_t take = (left < capacity) ? left : capacity;

        status = mqtt_platform_flash_read(
            entry->region, (index * entry->place_size) + (uint32_t)offset,
            data, take);
        if (status == MQTT_OK) {
            *count = take;
        }
    }
    return status;
}

mqtt_status mqtt_platform_store_segment(int32_t store, const uint8_t *header)
{
    /** @brief The store. */
    flash_entry *entry = flash_find(store);
    /** @brief The outcome. */
    mqtt_status status = MQTT_INVALID_STATE;

    if (entry != NULL) {
        /** @brief The place its generation takes. */
        const uint32_t place = (uint32_t)(mqtt_store_get_u64(&header[8]) % 2U);

        /* Nothing goes to the journal until its new place is whole. */
        entry->journal = FLASH_UNKNOWN;
        status = flash_restart(entry, entry->copies + place, header);
        if (status == MQTT_OK) {
            status = mqtt_platform_flash_sync(entry->region);
        }
        if (status == MQTT_OK) {
            entry->journal = place;
            entry->sync_needed = false;
        }
    }
    return status;
}

mqtt_status mqtt_platform_store_copy_begin(int32_t store,
                                           const uint8_t *header)
{
    /** @brief The store. */
    flash_entry *entry = flash_find(store);

    return (entry == NULL) ? MQTT_INVALID_STATE :
                             flash_restart(entry, 0U, header);
}

mqtt_status mqtt_platform_store_copy_record(int32_t store,
                                            const mqtt_store_record *record)
{
    /** @brief The store. */
    flash_entry *entry = flash_find(store);

    return (entry == NULL) ? MQTT_INVALID_STATE :
                             flash_add(entry, 0U, record);
}

mqtt_status mqtt_platform_store_copy_end(int32_t store)
{
    /** @brief The store. */
    const flash_entry *entry = flash_find(store);

    return (entry == NULL) ? MQTT_INVALID_STATE :
                             mqtt_platform_flash_sync(entry->region);
}

mqtt_status mqtt_platform_store_copy_duplicate(int32_t store)
{
    /** @brief The store. */
    const flash_entry *entry = flash_find(store);
    /** @brief Index of the copy being written. */
    uint32_t copy = 1U;
    /** @brief The outcome. */
    mqtt_status status = (entry == NULL) ? MQTT_INVALID_STATE : MQTT_OK;

    while ((status == MQTT_OK) && (copy < entry->copies)) {
        status = flash_duplicate(entry, copy);
        copy += 1U;
    }
    return status;
}

mqtt_status mqtt_platform_store_set_aside(int32_t store, uint32_t *aside)
{
    /** @brief The store. */
    flash_entry *entry = flash_find(store);
    /** @brief The outcome. */
    mqtt_status status = (entry == NULL) ? MQTT_INVALID_STATE : MQTT_OK;

    *aside = 0U;
    if (status == MQTT_OK) {
        status = mqtt_platform_flash_set_aside(entry->region, aside);
    }
    if (status == MQTT_OK) {
        /** @brief Index of a place. */
        uint32_t place = 0U;

        status = mqtt_platform_flash_erase(
            entry->region, 0U, (entry->copies + 2U) * entry->place_size);
        while (place < FLASH_PLACES) {
            entry->used[place] = FLASH_UNKNOWN;
            place += 1U;
        }
        entry->journal = FLASH_UNKNOWN;
    }
    if (status != MQTT_OK) {
        *aside = 0U;
    }
    return status;
}

uint8_t mqtt_platform_store_erased(int32_t store)
{
    (void)store;
    return (uint8_t)MQTT_FLASH_ERASED;
}

uint64_t mqtt_platform_store_capacity(int32_t store)
{
    /** @brief The store. */
    const flash_entry *entry = flash_find(store);

    return (entry == NULL) ? 0U : (uint64_t)entry->place_size;
}

uint64_t mqtt_platform_store_downtime(int32_t store)
{
    /* Flash keeps no time. */
    (void)store;
    return 0U;
}

mqtt_status mqtt_platform_store_remove(const char *where)
{
    /** @brief Number of bytes of the name. */
    uint32_t length = 0U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_INVALID_ARGUMENT;

    if (mqtt_text_length(where, FLASH_NAME_MAX - 1U, &length) &&
        (length > 0U)) {
        /* A store that is open is never removed. */
        status = flash_named(where, length) ?
                     MQTT_INVALID_STATE :
                     mqtt_platform_flash_remove(where);
    }
    return status;
}
