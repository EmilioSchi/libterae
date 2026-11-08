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
 * @file base_server_restore.c
 * @brief The MQTT Server: putting back what a store holds.
 *
 * A restore reads every place of the store twice. The first time it checks
 * each header and each record's CRC, and from what it finds chooses the chain
 * to apply: the newest snapshot copy whose journal segments follow on from it,
 * then those segments. The second time it applies them, checking each CRC
 * again, as the medium could have changed in between. A message is read
 * straight into its block of the arena, so no buffer has to hold a Payload.
 *
 * The records name messages and deliveries by store ids, which a table the
 * caller gives turns back into indexes. The snapshot's entries go in as they
 * come and are put in order once it ends, and the journal's come in order, as
 * the counter of ids only goes up.
 *
 * Times are on one line through the segments: each segment starts where the
 * one before it last committed, and a record's time is that of the last
 * COMMIT before it. What is left of a timer at the restore is what was left at
 * the end of the journal, less the time the store says has passed since.
 *
 * Anything wrong sets the server up again, empty, so a restore is all or
 * nothing.
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015.
 * @li Cited: 3.1.2.4, 3.1.2.5, 4.1, 4.4.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 3.1.2.5, p.32-33. 3.1.2.11.2, p.35. 4.1, p.92-93. 4.4, p.96.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base_qos.h"
#include "base_server_internal.h"
#include "base_string.h"
#include "base_topic_check.h"
#include "base_type.h"
#include "v5_prop.h"
#include "v5_properties.h"
#include "v5_session.h"

/** @brief Number of bytes of a MESSAGE record before its Topic Name. */
#define RESTORE_MESSAGE_FIELDS 28U

/** @brief The value a timer started at the end of the journal holds. */
#define RESTORE_AT_END UINT64_MAX

/** @brief The most time a record may say is left, so sums can't overflow. */
#define RESTORE_LEFT_MAX 0x3FFFFFFFFFFFFFFFU

/**
 * @brief Number of bytes at the start of the buffer that the end of a restore
 * uses as a bitmap of Packet Identifiers, one bit for each of the 65,536.
 */
#define RESTORE_IDENTIFIER_BITMAP (65536U / 8U)

#if MQTT_SERVER_RESTORE_MINIMUM < RESTORE_IDENTIFIER_BITMAP
#error "MQTT_SERVER_RESTORE_MINIMUM must hold the bitmap of Packet Identifiers"
#endif

/**
 * @brief Read bytes of the place being applied, all of them.
 * @param[in,out] restore The restore.
 * @param[in] offset Offset of the first byte.
 * @param[out] data Where they go.
 * @param[in] length Number of bytes.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED if the medium fails or gives fewer.
 */
static mqtt_status restore_read(mqtt_restore *restore, uint64_t offset,
                                uint8_t *data, uint32_t length);

/**
 * @brief Swap two entries of map.
 * @param[in,out] map The table.
 * @param[in] first One.
 * @param[in] second The other.
 */
static void restore_swap(mqtt_server_restore_entry *map, uint32_t first,
                         uint32_t second);

/**
 * @brief Move an entry of a heap down to its place.
 * @param[in,out] map The table.
 * @param[in] root The entry.
 * @param[in] count Number of entries in the heap.
 */
static void restore_sift(mqtt_server_restore_entry *map, uint32_t root,
                         uint32_t count);

/**
 * @brief Drop the entries of map that name nothing.
 * @param[in,out] restore The restore, whose map is in order.
 */
static void restore_compact(mqtt_restore *restore);

/**
 * @brief Read the bytes of a MESSAGE record whose message the restore holds
 * already, for its CRC.
 * @param[in,out] restore The restore.
 * @param[in] offset Offset of its content after the fixed fields.
 * @param[in] length Number of bytes there.
 * @param[in,out] crc The CRC.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status restore_message_again(mqtt_restore *restore,
                                         uint64_t offset, uint32_t length,
                                         uint16_t *crc);

/**
 * @brief Check the Topic Name and the Properties of a message read back.
 * @param[in] bytes The Topic Name, the Properties and the Payload.
 * @param[in] topic_length Number of bytes of Topic Name.
 * @param[in] properties_length Number of bytes of Properties.
 * @return true if a PUBLISH could carry them.
 */
static bool restore_message_valid(const uint8_t *bytes, uint32_t topic_length,
                                  uint32_t properties_length);

/**
 * @brief Read a message's bytes into a new block of the arena, and make it a
 * message.
 * @param[in,out] restore The restore.
 * @param[in] fields The record's fixed fields.
 * @param[in] offset Offset of the bytes.
 * @param[in] total Number of bytes.
 * @param[in,out] crc The CRC.
 * @param[out] message The message.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status restore_message_new(mqtt_restore *restore,
                                       const uint8_t *fields, uint64_t offset,
                                       uint32_t total, uint16_t *crc,
                                       uint32_t *message);

/**
 * @brief Compare the CRC that ends a record with the one computed.
 * @param[in] tail The record's last two bytes.
 * @param[in] crc The CRC computed.
 * @return true if they are the same.
 */
static bool restore_tail_matches(const uint8_t *tail, uint16_t crc);

/**
 * @brief Read and check the fixed fields of a MESSAGE record into the buffer,
 * after its frame.
 * @param[in,out] restore The restore.
 * @param[in] length Number of bytes of content.
 * @param[out] total Number of bytes of Topic Name, Properties and Payload.
 * @param[in,out] crc The CRC.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status restore_message_head(mqtt_restore *restore, uint32_t length,
                                        uint32_t *total, uint16_t *crc);

/**
 * @brief Read the bytes of a MESSAGE record, into a new message or for one the
 * restore holds, and check them.
 * @param[in,out] restore The restore, with the fixed fields in its buffer.
 * @param[in] total Number of bytes.
 * @param[in,out] crc The CRC.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status restore_message_body(mqtt_restore *restore, uint32_t total,
                                        uint16_t *crc);

/**
 * @brief Apply a MESSAGE record.
 * @param[in,out] restore The restore.
 * @param[in] length Number of bytes of content.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status restore_message(mqtt_restore *restore, uint32_t length);

/**
 * @brief Apply one record.
 * @param[in,out] restore The restore, with the record's offset.
 * @param[out] length Number of bytes of the record's content.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status restore_record(mqtt_restore *restore, uint32_t *length);

/**
 * @brief Apply the records of a place, up to an offset.
 * @param[in,out] restore The restore, with the place set.
 * @param[in] end Offset past the last record to apply.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status restore_records(mqtt_restore *restore, uint64_t end);

/**
 * @brief Apply one place of a chain.
 * @param[in,out] restore The restore.
 * @param[in] part The part.
 * @param[in] place The place.
 * @param[in] found What its scan found.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status restore_apply_place(mqtt_restore *restore,
                                       mqtt_store_part part, uint32_t place,
                                       const mqtt_store_place *found);

/**
 * @brief Turn a deadline on the store's line into a time of the server.
 * @param[in] deadline The deadline, one above it, or zero for none.
 * @param[in] end Where the line stands now: its end and the time passed.
 * @param[in] now_ms The time.
 * @return The time it is due, at least 1, or zero for none.
 */
static uint64_t restore_due(uint64_t deadline, uint64_t end, uint64_t now_ms);

/**
 * @brief Find the Packet Identifier of a delivery.
 * @param[in] server The server.
 * @param[in] delivery The delivery.
 * @return Its Packet Identifier.
 */
static uint16_t restore_identifier_of(const mqtt_server *server,
                                      uint32_t delivery);

/**
 * @brief Find what a Session's messages in flight leave to its Packet
 * Identifiers, and check them.
 * @param[in,out] restore The restore, whose buffer holds a bit for each
 * Packet Identifier, all clear.
 * @param[in] session The Session.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status restore_identifiers(mqtt_restore *restore,
                                       uint32_t session);

/**
 * @brief Finish a Session: its Packet Identifiers and its timers.
 * @param[in,out] restore The restore.
 * @param[in] session The Session.
 * @param[in] end Where the line stands now.
 * @param[in] now_ms The time.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status restore_finish_session(mqtt_restore *restore,
                                          uint32_t session, uint64_t end,
                                          uint64_t now_ms);

/**
 * @brief Finish the restore once every record is applied.
 * @param[in,out] restore The restore.
 * @param[in] now_ms The time.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status restore_finish(mqtt_restore *restore, uint64_t now_ms);

/**
 * @brief Set up a restore.
 * @param[out] restore The restore.
 * @param[in,out] server The server.
 * @param[in] source The store.
 * @param[out] map The table of store ids.
 * @param[in] map_count Number of entries in map.
 * @param[out] buffer Room for the reads.
 * @param[in] size Number of bytes in buffer.
 * @param[out] fault Where a fault is described.
 */
static void restore_start(mqtt_restore *restore, mqtt_server *server,
                          const mqtt_store_source *source,
                          mqtt_server_restore_entry *map, uint32_t map_count,
                          uint8_t *buffer, uint32_t size,
                          mqtt_store_fault *fault);

/**
 * @brief Apply the places of a chain, and leave the journal where its last
 * segment ended.
 * @param[in,out] restore The restore.
 * @param[in] copies What each snapshot copy holds.
 * @param[in] journals What each journal place holds.
 * @param[in] chain The places.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status restore_chain_apply(mqtt_restore *restore,
                                       const mqtt_store_place *copies,
                                       const mqtt_store_place *journals,
                                       const mqtt_store_chain *chain);

/**
 * @brief Set the server up again as mqtt_server_init left it.
 * @param[in,out] server The server.
 */
static void restore_reset(mqtt_server *server);

/**
 * @brief Find whether a server holds nothing yet, as a restore needs.
 * @param[in] server The server.
 * @return true if it has a store, has written nothing and holds nothing.
 */
static bool restore_fresh(const mqtt_server *server);

mqtt_status mqtt_restore_fail(mqtt_restore *restore,
                              mqtt_store_fault_kind kind)
{
    if (restore->fault->kind == MQTT_STORE_FAULT_NONE) {
        restore->fault->kind = kind;
        restore->fault->part = restore->part;
        restore->fault->place = restore->place;
        restore->fault->generation = restore->generation;
        restore->fault->offset = restore->offset;
        restore->fault->record_kind = restore->kind;
    }
    return MQTT_STORE_DAMAGED;
}

uint32_t mqtt_restore_find(const mqtt_restore *restore, uint64_t id,
                           bool message)
{
    /** @brief The first entry that may hold it. */
    uint32_t low = 0U;
    /** @brief One past the last. */
    uint32_t high = restore->map_sorted;
    /** @brief The answer. */
    uint32_t found = MQTT_SERVER_NONE;

    while (low < high) {
        /** @brief The entry in the middle. */
        const uint32_t middle = low + ((high - low) / 2U);

        if (restore->map[middle].id < id) {
            low = middle + 1U;
        } else {
            high = middle;
        }
    }
    if ((low < restore->map_sorted) && (restore->map[low].id == id)) {
        found = low;
    }
    /* What the snapshot added is not in order yet. */
    low = restore->map_sorted;
    while ((found == MQTT_SERVER_NONE) && (low < restore->map_used)) {
        if (restore->map[low].id == id) {
            found = low;
        }
        low += 1U;
    }
    if (found != MQTT_SERVER_NONE) {
        /** @brief true if the entry names a message. */
        const bool named = (restore->map[found].session == MQTT_SERVER_NONE);

        if ((restore->map[found].index == MQTT_SERVER_NONE) ||
            (message ? (!named) : named)) {
            found = MQTT_SERVER_NONE;
        }
    }
    return found;
}

mqtt_status mqtt_restore_add(mqtt_restore *restore, uint64_t id,
                             uint32_t index, uint32_t session)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief Where it goes. */
    uint32_t place = restore->map_used;

    if ((restore->map_used == restore->map_count) && (!restore->snapshot)) {
        restore_compact(restore);
        place = restore->map_used;
    }
    if (restore->map_used == restore->map_count) {
        status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_ROOM);
    } else if (restore->snapshot) {
        /* Put in order when the snapshot ends. */
        restore->map_used += 1U;
    } else {
        /* The journal names them in order, nearly always: the entries after
         * one that doesn't move up. */
        while ((place > 0U) && (restore->map[place - 1U].id > id)) {
            restore->map[place] = restore->map[place - 1U];
            place -= 1U;
        }
        if ((place > 0U) && (restore->map[place - 1U].id == id)) {
            status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_CONTENT);
        }
        restore->map_used += 1U;
        restore->map_sorted = restore->map_used;
    }
    if (status == MQTT_OK) {
        restore->map[place].id = id;
        restore->map[place].index = index;
        restore->map[place].session = session;
    }
    return status;
}

void mqtt_restore_forget(mqtt_restore *restore, uint32_t entry)
{
    if (entry < restore->map_used) {
        restore->map[entry].index = MQTT_SERVER_NONE;
    }
}

static void restore_swap(mqtt_server_restore_entry *map, uint32_t first,
                         uint32_t second)
{
    /** @brief The first entry, kept while it is replaced. */
    const mqtt_server_restore_entry kept = map[first];

    map[first] = map[second];
    map[second] = kept;
}

static void restore_sift(mqtt_server_restore_entry *map, uint32_t root,
                         uint32_t count)
{
    /** @brief The entry being moved down. */
    uint32_t current = root;
    /** @brief true once it is in its place. */
    bool placed = false;

    while (!placed) {
        /** @brief Its first child. */
        const uint32_t child = (2U * current) + 1U;

        if (child >= count) {
            placed = true;
        } else {
            /** @brief The larger of its children. */
            uint32_t larger = child;

            if (((child + 1U) < count) &&
                (map[child + 1U].id > map[child].id)) {
                larger = child + 1U;
            }
            if (map[larger].id > map[current].id) {
                restore_swap(map, current, larger);
                current = larger;
            } else {
                placed = true;
            }
        }
    }
}

mqtt_status mqtt_restore_sort(mqtt_restore *restore)
{
    /** @brief Number of entries. */
    const uint32_t count = restore->map_used;
    /** @brief Index of the entry being placed. */
    uint32_t index = count / 2U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    while (index > 0U) {
        index -= 1U;
        restore_sift(restore->map, index, count);
    }
    index = count;
    while (index > 1U) {
        index -= 1U;
        restore_swap(restore->map, 0U, index);
        restore_sift(restore->map, 0U, index);
    }
    restore->map_sorted = count;
    index = 1U;
    while ((status == MQTT_OK) && (index < count)) {
        if (restore->map[index].id == restore->map[index - 1U].id) {
            status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_CONTENT);
        }
        index += 1U;
    }
    return status;
}

static void restore_compact(mqtt_restore *restore)
{
    /** @brief Index of the entry being looked at. */
    uint32_t from = 0U;
    /** @brief Number of entries kept. */
    uint32_t kept = 0U;

    while (from < restore->map_used) {
        if (restore->map[from].index != MQTT_SERVER_NONE) {
            restore->map[kept] = restore->map[from];
            kept += 1U;
        }
        from += 1U;
    }
    restore->map_used = kept;
    restore->map_sorted = kept;
}

static mqtt_status restore_read(mqtt_restore *restore, uint64_t offset,
                                uint8_t *data, uint32_t length)
{
    /** @brief Number of bytes read. */
    uint32_t count = 0U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (length > 0U) {
        status = restore->source->read(restore->source->user_tag,
                                       restore->part, restore->place, offset,
                                       data, length, &count);
    }
    if ((status != MQTT_OK) || (count != length)) {
        status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_MEDIUM);
    }
    return status;
}

static mqtt_status restore_message_again(mqtt_restore *restore,
                                         uint64_t offset, uint32_t length,
                                         uint16_t *crc)
{
    /** @brief Offset of the next read. */
    uint64_t at = offset;
    /** @brief Number of bytes still to read. */
    uint32_t left = length;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    while ((status == MQTT_OK) && (left > 0U)) {
        /** @brief Number of bytes this read takes. */
        const uint32_t take = (left < restore->size) ? left : restore->size;

        status = restore_read(restore, at, restore->buffer, take);
        if (status == MQTT_OK) {
            *crc = mqtt_store_crc16(*crc, restore->buffer, take);
        }
        left -= take;
        at += (uint64_t)take;
    }
    return status;
}

static bool restore_message_valid(const uint8_t *bytes, uint32_t topic_length,
                                  uint32_t properties_length)
{
    /** @brief The Topic Name. */
    const mqtt_utf8 topic_name = {(uint16_t)topic_length, bytes};
    /** @brief The Properties, decoded. */
    mqtt5_properties properties;
    /** @brief The answer. */
    bool valid = (topic_length > 0U) &&
                 (mqtt_topic_name_check(&topic_name, MQTT_VERSION_31) ==
                  MQTT_OK);

    if (valid && (properties_length > 0U)) {
        valid = (mqtt5_properties_decode_block(&bytes[topic_length],
                                               properties_length, MQTT_PUBLISH,
                                               &properties) == MQTT_OK) &&
                (!mqtt5_properties_has(&properties, MQTT5_PROP_TOPIC_ALIAS)) &&
                (!mqtt5_properties_has(&properties,
                                       MQTT5_PROP_SUBSCRIPTION_IDENTIFIER)) &&
                (!mqtt5_properties_has(&properties,
                                       MQTT5_PROP_MESSAGE_EXPIRY_INTERVAL));
    }
    return valid;
}

static mqtt_status restore_message_new(mqtt_restore *restore,
                                       const uint8_t *fields, uint64_t offset,
                                       uint32_t total, uint16_t *crc,
                                       uint32_t *message)
{
    /** @brief The server. */
    mqtt_server *server = restore->server;
    /** @brief The message. */
    const uint32_t index = mqtt_slab_take(&server->message_slots);
    /** @brief Its block. */
    uint32_t block = MQTT_PAGE_ARENA_NONE;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    *message = index;
    if (index != MQTT_SLAB_NONE) {
        block = mqtt_page_arena_take(&server->arena, total);
    }
    if (block == MQTT_PAGE_ARENA_NONE) {
        if (index != MQTT_SLAB_NONE) {
            (void)mqtt_slab_give(&server->message_slots, index);
        }
        status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_ROOM);
    } else {
        /** @brief The message. */
        mqtt_message *entry = &server->storage.messages[index];
        /** @brief Its bytes. */
        uint8_t *bytes = mqtt_page_arena_bytes(&server->arena, block);
        /** @brief The fields, read. */
        mqtt_reader reader = {NULL, 0U, 0U, MQTT_OK};

        mqtt_reader_init(&reader, fields, RESTORE_MESSAGE_FIELDS);
        entry->block = block;
        entry->references = 1U;
        entry->store_id = mqtt_store_get_u64(fields);
        reader.position = 8U;
        entry->qos = mqtt_read_byte(&reader);
        entry->retain = ((mqtt_read_byte(&reader) & MQTT_STORE_RETAIN) != 0U);
        reader.position = 18U;
        entry->topic_length = mqtt_read_two_byte_integer(&reader);
        entry->properties_length = mqtt_read_four_byte_integer(&reader);
        entry->payload_length = mqtt_read_four_byte_integer(&reader);
        entry->expiry_ms = 0U;
        entry->store_generation = 0U;
        status = restore_read(restore, offset, bytes, total);
        if (status == MQTT_OK) {
            *crc = mqtt_store_crc16(*crc, bytes, total);
            status = mqtt_restore_add(restore, entry->store_id, index,
                                      MQTT_SERVER_NONE);
        }
        if (status != MQTT_OK) {
            /* Nothing else holds it yet. */
            mqtt_route_release(server, index);
        }
    }
    return status;
}

static bool restore_tail_matches(const uint8_t *tail, uint16_t crc)
{
    return (((uint32_t)tail[0] << 8U) | (uint32_t)tail[1]) == (uint32_t)crc;
}

static mqtt_status restore_message_head(mqtt_restore *restore, uint32_t length,
                                        uint32_t *total, uint16_t *crc)
{
    /** @brief The record's fixed fields. */
    const uint8_t *fields = &restore->buffer[MQTT_STORE_FRAME_HEAD];
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (length < RESTORE_MESSAGE_FIELDS) {
        status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_CONTENT);
    } else {
        status = restore_read(restore, restore->offset + MQTT_STORE_FRAME_HEAD,
                              &restore->buffer[MQTT_STORE_FRAME_HEAD],
                              RESTORE_MESSAGE_FIELDS);
    }
    if (status == MQTT_OK) {
        /** @brief The fields, read. */
        mqtt_reader reader = {NULL, 0U, 0U, MQTT_OK};
        /** @brief Number of bytes of Topic Name, Properties and Payload. */
        uint64_t sum = 0U;

        mqtt_reader_init(&reader, fields, RESTORE_MESSAGE_FIELDS);
        reader.position = 18U;
        sum = (uint64_t)mqtt_read_two_byte_integer(&reader);
        sum += (uint64_t)mqtt_read_four_byte_integer(&reader);
        sum += (uint64_t)mqtt_read_four_byte_integer(&reader);
        *crc = mqtt_store_crc16(mqtt_store_frame_crc(restore->buffer),
                                fields, RESTORE_MESSAGE_FIELDS);
        *total = (uint32_t)(sum & 0xFFFFFFFFU);
        if ((mqtt_store_get_u64(fields) == 0U) || (fields[8] > MQTT_QOS_2) ||
            ((fields[9] & ~(MQTT_STORE_RETAIN | MQTT_STORE_MESSAGE_EXPIRES)) !=
             0U) ||
            (sum != ((uint64_t)length - RESTORE_MESSAGE_FIELDS))) {
            status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_CONTENT);
        }
    }
    return status;
}

static mqtt_status restore_message_body(mqtt_restore *restore, uint32_t total,
                                        uint16_t *crc)
{
    /** @brief The record's fixed fields. */
    const uint8_t *fields = &restore->buffer[MQTT_STORE_FRAME_HEAD];
    /** @brief The message's entry in map, if it is there already. */
    const uint32_t found =
        mqtt_restore_find(restore, mqtt_store_get_u64(fields), true);
    /** @brief Offset of the bytes after the fixed fields. */
    const uint64_t bytes =
        restore->offset + MQTT_STORE_FRAME_HEAD + RESTORE_MESSAGE_FIELDS;
    /** @brief The milliseconds left before it expires. */
    const uint64_t left = mqtt_store_get_u64(&fields[10]);
    /** @brief true if it has a Message Expiry Interval. */
    const bool expires = ((fields[9] & MQTT_STORE_MESSAGE_EXPIRES) != 0U);
    /** @brief The message. */
    uint32_t message = MQTT_SERVER_NONE;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (found != MQTT_SERVER_NONE) {
        message = restore->map[found].index;
        status = restore_message_again(restore, bytes, total, crc);
    } else {
        status = restore_message_new(restore, fields, bytes, total, crc,
                                     &message);
    }
    if (status == MQTT_OK) {
        /** @brief The message. */
        mqtt_message *entry = &restore->server->storage.messages[message];
        /** @brief Its bytes. */
        const uint8_t *text =
            mqtt_page_arena_bytes(&restore->server->arena, entry->block);

        if (expires) {
            entry->expiry_ms =
                restore->clock +
                ((left < RESTORE_LEFT_MAX) ? left : RESTORE_LEFT_MAX) + 1U;
        }
        if (!restore_message_valid(text, (uint32_t)entry->topic_length,
                                   entry->properties_length)) {
            status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_CONTENT);
        }
    }
    return status;
}

static mqtt_status restore_message(mqtt_restore *restore, uint32_t length)
{
    /** @brief The CRC. */
    uint16_t crc = (uint16_t)MQTT_STORE_CRC_INIT;
    /** @brief Number of bytes of Topic Name, Properties and Payload. */
    uint32_t total = 0U;
    /** @brief The store id. */
    uint64_t id = 0U;
    /** @brief The outcome. */
    mqtt_status status = restore_message_head(restore, length, &total, &crc);

    if (status == MQTT_OK) {
        id = mqtt_store_get_u64(&restore->buffer[MQTT_STORE_FRAME_HEAD]);
        status = restore_message_body(restore, total, &crc);
    }
    if (status == MQTT_OK) {
        status = restore_read(restore,
                              restore->offset + MQTT_STORE_FRAME_HEAD +
                                  (uint64_t)length,
                              restore->buffer, MQTT_STORE_FRAME_TAIL);
    }
    if ((status == MQTT_OK) && !restore_tail_matches(restore->buffer, crc)) {
        status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_MEDIUM);
    }
    if ((status == MQTT_OK) && (id >= restore->next_id)) {
        restore->next_id = id + 1U;
    }
    return status;
}

static mqtt_status restore_record(mqtt_restore *restore, uint32_t *length)
{
    /** @brief The outcome. */
    mqtt_status status = restore_read(restore, restore->offset,
                                      restore->buffer, MQTT_STORE_FRAME_HEAD);

    *length = 0U;
    if (status == MQTT_OK) {
        if (!mqtt_store_frame_check(restore->buffer, &restore->kind, length)) {
            /* The scan found it right, so the medium has changed since. */
            status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_MEDIUM);
        }
    }
    if (status == MQTT_OK) {
        if (restore->snapshot && (!restore->messages_sorted) &&
            (restore->kind != (uint8_t)MQTT_STORE_RECORD_MESSAGE)) {
            /* The messages of a snapshot come first. */
            restore->messages_sorted = true;
            status = mqtt_restore_sort(restore);
        }
    }
    if (status != MQTT_OK) {
        /* The fault is described. */
    } else if (restore->kind == (uint8_t)MQTT_STORE_RECORD_MESSAGE) {
        status = restore_message(restore, *length);
    } else if (*length > (restore->size - MQTT_STORE_FRAME_HEAD -
                          MQTT_STORE_FRAME_TAIL)) {
        status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_ROOM);
    } else {
        status = restore_read(restore, restore->offset + MQTT_STORE_FRAME_HEAD,
                              &restore->buffer[MQTT_STORE_FRAME_HEAD],
                              *length + MQTT_STORE_FRAME_TAIL);
        if (status == MQTT_OK) {
            /** @brief The CRC of what was read. */
            const uint16_t crc =
                mqtt_store_crc16(mqtt_store_frame_crc(restore->buffer),
                                 &restore->buffer[MQTT_STORE_FRAME_HEAD],
                                 *length);

            if (!restore_tail_matches(
                    &restore->buffer[MQTT_STORE_FRAME_HEAD + *length], crc)) {
                status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_MEDIUM);
            }
        }
        if (status == MQTT_OK) {
            status = mqtt_restore_apply(
                restore, &restore->buffer[MQTT_STORE_FRAME_HEAD], *length);
        }
    }
    return status;
}

static mqtt_status restore_records(mqtt_restore *restore, uint64_t end)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    restore->offset = MQTT_STORE_HEADER_SIZE;
    while ((status == MQTT_OK) && (restore->offset < end)) {
        /** @brief Number of bytes of content. */
        uint32_t length = 0U;

        status = restore_record(restore, &length);
        restore->offset += MQTT_STORE_FRAME_HEAD + (uint64_t)length +
                           MQTT_STORE_FRAME_TAIL;
    }
    if ((status == MQTT_OK) && (restore->offset != end)) {
        status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_MEDIUM);
    }
    return status;
}

static mqtt_status restore_apply_place(mqtt_restore *restore,
                                       mqtt_store_part part, uint32_t place,
                                       const mqtt_store_place *found)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    restore->part = part;
    restore->place = place;
    restore->generation = found->scan.header.generation;
    restore->snapshot = (part == MQTT_STORE_SNAPSHOT);
    restore->messages_sorted = !restore->snapshot;
    restore->mark_session = MQTT_SERVER_NONE;
    restore->last_delivery = MQTT_SERVER_NONE;
    restore->clock = restore->segment_offset;
    status = restore_records(restore, found->scan.commit_end);
    if ((status == MQTT_OK) && restore->snapshot) {
        status = mqtt_restore_sort(restore);
    }
    if (status == MQTT_OK) {
        restore->snapshot = false;
        if (part == MQTT_STORE_JOURNAL) {
            restore->segment_offset += found->scan.elapsed_ms;
            restore->clock = restore->segment_offset;
        }
    }
    return status;
}

static uint64_t restore_due(uint64_t deadline, uint64_t end, uint64_t now_ms)
{
    /** @brief The answer. */
    uint64_t due = 0U;

    if (deadline != 0U) {
        due = now_ms + (((deadline - 1U) > end) ? ((deadline - 1U) - end) : 0U);
        if (due == 0U) {
            due = 1U;
        }
    }
    return due;
}

static uint16_t restore_identifier_of(const mqtt_server *server,
                                      uint32_t delivery)
{
    return server->storage.deliveries[delivery].packet_identifier;
}

static mqtt_status restore_identifiers(mqtt_restore *restore,
                                       uint32_t session)
{
    /** @brief The server. */
    mqtt_server *server = restore->server;
    /** @brief The Session. */
    mqtt_session *entry = &server->storage.sessions[session];
    /** @brief The delivery being looked at. */
    uint32_t index = entry->inflight_head;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    entry->identifier_boundary = MQTT_SERVER_NONE;
    entry->last_packet_identifier = 0U;
    while (index != MQTT_SERVER_NONE) {
        /** @brief The delivery. */
        const mqtt_delivery *item = &server->storage.deliveries[index];
        /** @brief Its Packet Identifier. */
        const uint32_t identifier = (uint32_t)item->packet_identifier;
        /** @brief Its bit. */
        const uint8_t bit = (uint8_t)(1U << (identifier & 7U));

        if ((restore->buffer[identifier >> 3U] & bit) != 0U) {
            status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_CONTENT);
        }
        restore->buffer[identifier >> 3U] |= bit;
        /* Values only go up after a wrap, so the last place they go down is
         * the last entry from before it. */
        if ((item->previous != MQTT_SERVER_NONE) &&
            (identifier < (uint32_t)restore_identifier_of(
                              server, item->previous))) {
            entry->identifier_boundary = item->previous;
        }
        if (item->released ? (item->message != MQTT_SERVER_NONE) :
                             (item->message == MQTT_SERVER_NONE)) {
            status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_CONTENT);
        }
        entry->last_packet_identifier = item->packet_identifier;
        index = item->next;
    }
    index = entry->inflight_head;
    while (index != MQTT_SERVER_NONE) {
        restore->buffer[(uint32_t)restore_identifier_of(server, index) >> 3U] =
            0U;
        index = server->storage.deliveries[index].next;
    }
    return status;
}

static mqtt_status restore_finish_session(mqtt_restore *restore,
                                          uint32_t session, uint64_t end,
                                          uint64_t now_ms)
{
    /** @brief The server. */
    mqtt_server *server = restore->server;
    /** @brief The Session. */
    mqtt_session *entry = &server->storage.sessions[session];
    /** @brief The time that has passed since the end of the journal. */
    const uint64_t downtime = restore->source->downtime_ms;
    /** @brief The delivery being looked at. */
    uint32_t index = entry->queued_head;
    /** @brief The outcome. */
    mqtt_status status = restore_identifiers(restore, session);

    while (index != MQTT_SERVER_NONE) {
        if (server->storage.deliveries[index].message == MQTT_SERVER_NONE) {
            status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_CONTENT);
        }
        index = server->storage.deliveries[index].next;
    }
    if (entry->expiry_due_ms == RESTORE_AT_END) {
        /** @brief The Session Expiry Interval, in milliseconds. */
        const uint64_t expiry = (uint64_t)entry->expiry_s * 1000U;

        entry->expiry_due_ms =
            (entry->expiry_s == MQTT5_SESSION_EXPIRY_NEVER) ?
                0U :
                (now_ms + ((expiry > downtime) ? (expiry - downtime) : 0U) +
                 1U);
    } else {
        entry->expiry_due_ms = restore_due(entry->expiry_due_ms, end, now_ms);
    }
    if (entry->will_message == MQTT_SERVER_NONE) {
        entry->will_due_ms = 0U;
    } else if ((entry->will_due_ms == RESTORE_AT_END) ||
               (entry->will_due_ms == 0U)) {
        /** @brief The Will Delay Interval, in milliseconds. */
        const uint64_t delay = (uint64_t)entry->will_delay_s * 1000U;

        entry->will_due_ms =
            now_ms + ((delay > downtime) ? (delay - downtime) : 0U) + 1U;
    } else {
        entry->will_due_ms = restore_due(entry->will_due_ms, end, now_ms);
    }
    return status;
}

static mqtt_status restore_finish(mqtt_restore *restore, uint64_t now_ms)
{
    /** @brief The server. */
    mqtt_server *server = restore->server;
    /** @brief Where the line stands now. */
    const uint64_t end = restore->clock + restore->source->downtime_ms;
    /** @brief Index of the entry, Session or message being finished. */
    uint32_t index = 0U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    /* Each message was held by the restore since its record. */
    while (index < restore->map_used) {
        if ((restore->map[index].index != MQTT_SERVER_NONE) &&
            (restore->map[index].session == MQTT_SERVER_NONE)) {
            mqtt_route_release(server, restore->map[index].index);
        }
        index += 1U;
    }
    /* The bitmap restore_identifiers marks each Session's Packet Identifiers
     * in starts empty, and each Session clears what it marked. */
    mqtt_bytes_wipe(restore->buffer, RESTORE_IDENTIFIER_BITMAP);
    index = 0U;
    while ((status == MQTT_OK) && (index < server->storage.session_count)) {
        if (server->storage.sessions[index].in_use) {
            status = restore_finish_session(restore, index, end, now_ms);
        }
        index += 1U;
    }
    index = mqtt_slab_next(&server->message_slots, 0U);
    while (index != MQTT_SLAB_NONE) {
        /** @brief The message. */
        mqtt_message *entry = &server->storage.messages[index];

        entry->expiry_ms = restore_due(entry->expiry_ms, end, now_ms);
        index = mqtt_slab_next(&server->message_slots, index + 1U);
    }
    return status;
}

static void restore_reset(mqtt_server *server)
{
    /** @brief The configuration, copied out before the server is set up. */
    mqtt_server_config config;
    /** @brief The memory, copied out likewise. */
    mqtt_server_storage storage;

    mqtt_bytes_put((uint8_t *)&config, (const uint8_t *)&server->config,
                   (uint32_t)sizeof(config));
    mqtt_bytes_put((uint8_t *)&storage, (const uint8_t *)&server->storage,
                   (uint32_t)sizeof(storage));
    /* What init took once it takes again, so this can't fail. */
    if (mqtt_server_init(server, &config, &storage) != MQTT_OK) {
        server->ready = MQTT_BOOL_UNSET;
    }
}

static bool restore_fresh(const mqtt_server *server)
{
    return (server->store.state == MQTT_SERVER_STORE_FRESH) &&
           (server->store.generation == 0U) &&
           (server->session_table_used == 0U) &&
           (mqtt_slab_next(&server->message_slots, 0U) == MQTT_SLAB_NONE) &&
           (mqtt_slab_next(&server->connection_slots, 0U) == MQTT_SLAB_NONE);
}

static void restore_start(mqtt_restore *restore, mqtt_server *server,
                          const mqtt_store_source *source,
                          mqtt_server_restore_entry *map, uint32_t map_count,
                          uint8_t *buffer, uint32_t size,
                          mqtt_store_fault *fault)
{
    fault->kind = MQTT_STORE_FAULT_NONE;
    restore->server = server;
    restore->source = source;
    restore->map = map;
    restore->map_count = map_count;
    restore->map_used = 0U;
    restore->map_sorted = 0U;
    restore->buffer = buffer;
    restore->size = size;
    restore->fault = fault;
    restore->part = MQTT_STORE_JOURNAL;
    restore->place = 0U;
    restore->generation = 0U;
    restore->offset = 0U;
    restore->kind = 0U;
    restore->clock = 0U;
    restore->segment_offset = 0U;
    restore->next_id = 1U;
    restore->snapshot = false;
    restore->messages_sorted = true;
    restore->mark_session = MQTT_SERVER_NONE;
    restore->mark_map = 0U;
    restore->last_delivery = MQTT_SERVER_NONE;
}

static mqtt_status restore_chain_apply(mqtt_restore *restore,
                                       const mqtt_store_place *copies,
                                       const mqtt_store_place *journals,
                                       const mqtt_store_chain *chain)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (chain->copy != MQTT_STORE_PLACE_NONE) {
        status = restore_apply_place(restore, MQTT_STORE_SNAPSHOT, chain->copy,
                                     &copies[chain->copy]);
    }
    if (status == MQTT_OK) {
        status = restore_apply_place(restore, MQTT_STORE_JOURNAL, chain->first,
                                     &journals[chain->first]);
    }
    if ((status == MQTT_OK) && (chain->second != MQTT_STORE_PLACE_NONE)) {
        status = restore_apply_place(restore, MQTT_STORE_JOURNAL,
                                     chain->second, &journals[chain->second]);
    }
    if (status == MQTT_OK) {
        /** @brief The last segment applied. */
        const uint32_t place = (chain->second != MQTT_STORE_PLACE_NONE) ?
                                   chain->second :
                                   chain->first;
        /** @brief What its scan found. */
        const mqtt_store_scan *last = &journals[place].scan;
        /** @brief The journal. */
        mqtt_server_store *store = &restore->server->store;

        store->generation = last->header.generation;
        store->closed = last->header.base + last->commits;
        store->stored = store->closed;
        store->segment_base = store->closed;
        store->next_id = restore->next_id;
        store->snapshot_whole = true;
    }
    return status;
}

mqtt_status mqtt_server_restore(mqtt_server *server,
                                const mqtt_store_source *source,
                                mqtt_server_restore_entry *map,
                                uint32_t map_count, uint8_t *buffer,
                                uint32_t size, uint64_t now_ms,
                                mqtt_store_fault *fault)
{
    /** @brief What each copy holds. */
    mqtt_store_place copies[MQTT_SERVER_RESTORE_COPIES];
    /** @brief What each journal place holds. */
    mqtt_store_place journals[2];
    /** @brief The places to apply. */
    mqtt_store_chain chain = {MQTT_STORE_PLACE_NONE, MQTT_STORE_PLACE_NONE,
                              MQTT_STORE_PLACE_NONE};
    /** @brief The restore. */
    mqtt_restore restore;
    /** @brief The outcome. */
    mqtt_status status = MQTT_INVALID_ARGUMENT;

    if ((fault != NULL) && mqtt_server_ready(server) && (source != NULL) &&
        (source->read != NULL) && (source->copies > 0U) &&
        (source->copies <= MQTT_SERVER_RESTORE_COPIES) && (buffer != NULL) &&
        (size >= MQTT_SERVER_RESTORE_MINIMUM) &&
        ((map != NULL) || (map_count == 0U)) &&
        (server->store.state != MQTT_SERVER_STORE_OFF)) {
        status = restore_fresh(server) ? MQTT_OK : MQTT_INVALID_STATE;
    }
    if (status == MQTT_OK) {
        restore_start(&restore, server, source, map, map_count, buffer, size,
                      fault);
        server->now_ms = now_ms;
        status = mqtt_store_choose(source, (uint8_t)MQTT_STORE_SERVER_SNAPSHOT,
                                   (uint8_t)MQTT_STORE_SERVER_JOURNAL, buffer,
                                   size, copies, journals, &chain, fault);
    }
    if (status == MQTT_OK) {
        status = restore_chain_apply(&restore, copies, journals, &chain);
    }
    if (status == MQTT_OK) {
        status = restore_finish(&restore, now_ms);
    }
    if (status == MQTT_NOT_FOUND) {
        /* The store holds nothing, so the server starts as it is. */
        status = MQTT_OK;
    } else if (status == MQTT_STORE_DAMAGED) {
        restore_reset(server);
    } else {
        /* Done, or a wrong argument or state, which changes nothing. */
    }
    return status;
}
