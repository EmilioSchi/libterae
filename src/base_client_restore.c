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
 * @file base_client_restore.c
 * @brief The MQTT Client: putting back what a store holds.
 *
 * Every place is read and checked first, and the chain to apply is chosen as
 * the server's is. Then each record of the chain is read again, its CRC
 * checked again, as the medium could have changed in between, and applied. A
 * packet is read straight into its block of the arena, and checked as if the
 * Server had sent it: it has to decode, and to agree with the fields beside
 * it.
 *
 * At the end, what is held has to be what a Client can hold: nothing sent or
 * received without a Session. Anything wrong sets the client up again, empty,
 * so a restore is all or nothing.
 *
 * [V31] MQTT V3.1 Protocol Specification.
 * @li Cited: 3.3, 3.8, 3.10, 4.1.
 * @li Source: public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015.
 * @li Cited: 3.3, 4.1, 4.4.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 3.3, p.53-64. 4.1, p.92-93. 4.4, p.96.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base_client_internal.h"
#include "base_qos.h"
#include "base_store_internal.h"
#include "base_string.h"
#include "base_type.h"
#include "v5_prop.h"

/** @brief Number of bytes of a CLIENT_SESSION record before its identifier. */
#define RESTORE_SESSION_FIELDS 4U

/** @brief Number of bytes of a CLIENT_MESSAGE record before its packet. */
#define RESTORE_MESSAGE_FIELDS 7U

/** @brief Number of bytes of a record that names a Packet Identifier. */
#define RESTORE_MARK_FIELDS 2U

/** @brief The flags of a SUBSCRIBE or UNSUBSCRIBE packet. */
#define RESTORE_REQUEST_FLAGS 0x02U

/** @brief A restore under way. */
typedef struct {
    /** @brief The client. */
    mqtt_client *client;
    /** @brief The store. */
    const mqtt_store_source *source;
    /** @brief Room for the reads. */
    uint8_t *buffer;
    /** @brief Number of bytes in buffer. */
    uint32_t size;
    /** @brief What was wrong. */
    mqtt_store_fault *fault;
    /** @brief The part being applied. */
    mqtt_store_part part;
    /** @brief The place being applied. */
    uint32_t place;
    /** @brief Its generation. */
    uint64_t generation;
    /** @brief Offset of the record being applied. */
    uint64_t offset;
    /** @brief Its kind. */
    uint8_t kind;
    /** @brief true while a snapshot is applied, false for a journal. */
    bool snapshot;
} client_restore;

/**
 * @brief Note a fault where the restore is, unless one is noted already.
 * @param[in,out] restore The restore.
 * @param[in] kind What was wrong.
 * @return MQTT_STORE_DAMAGED.
 */
static mqtt_status reload_fail(client_restore *restore,
                               mqtt_store_fault_kind kind);

/**
 * @brief Read bytes of the place being applied, all of them, and add them to
 * a CRC.
 * @param[in,out] restore The restore.
 * @param[in] offset Offset of the first byte.
 * @param[out] data Where they go.
 * @param[in] length Number of bytes.
 * @param[in,out] crc The CRC.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED if the medium fails or gives fewer.
 */
static mqtt_status reload_read(client_restore *restore, uint64_t offset,
                               uint8_t *data, uint32_t length, uint16_t *crc);

/**
 * @brief Add bytes of the place being applied to a CRC, reading them through
 * the buffer, as nothing keeps them.
 * @param[in,out] restore The restore.
 * @param[in] offset Offset of the first byte.
 * @param[in] length Number of bytes.
 * @param[in,out] crc The CRC.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status reload_pass(client_restore *restore, uint64_t offset,
                               uint32_t length, uint16_t *crc);

/**
 * @brief Read a 2-byte integer in network byte order.
 * @param[in] bytes The bytes.
 * @return The integer.
 */
static uint16_t reload_u16(const uint8_t *bytes);

/**
 * @brief Read a record's content into an arena block, as packets and the
 * Client Identifier are.
 * @param[in,out] restore The restore.
 * @param[in] offset Offset of the bytes.
 * @param[in] length Number of bytes, which may be zero.
 * @param[in,out] crc The CRC.
 * @param[out] block The block, or MQTT_ARENA_NONE for none.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED, also when there is no room.
 */
static mqtt_status reload_take(client_restore *restore, uint64_t offset,
                               uint32_t length, uint16_t *crc,
                               uint32_t *block);

/**
 * @brief Check a record's CRC against the one at its end.
 * @param[in,out] restore The restore.
 * @param[in] offset Offset of the CRC.
 * @param[in] crc The CRC of the record.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status reload_tail(client_restore *restore, uint64_t offset,
                               uint16_t crc);

/**
 * @brief Find whether a Packet Identifier is held by a message or a packet
 * waiting for its answer.
 * @param[in] client The client.
 * @param[in] packet_identifier The Packet Identifier.
 * @return true if it is.
 */
static bool reload_in_use(const mqtt_client *client,
                          uint16_t packet_identifier);

/**
 * @brief Apply a CLIENT_SESSION record.
 * @param[in,out] restore The restore.
 * @param[in] fields Its fields before the identifier.
 * @param[in] block The identifier, or MQTT_ARENA_NONE.
 * @param[in] length Number of bytes of the identifier.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status reload_session(client_restore *restore,
                                  const uint8_t *fields, uint32_t block,
                                  uint32_t length);

/**
 * @brief Check that a message's PUBLISH packet decodes, and agrees with its
 * record.
 * @param[in] client The client.
 * @param[in] fields The record's fields before the packet.
 * @param[in] bytes The packet.
 * @param[in] length Number of bytes.
 * @return true if it does.
 */
static bool reload_publish_valid(const mqtt_client *client,
                                 const uint8_t *fields, const uint8_t *bytes,
                                 uint32_t length);

/**
 * @brief Apply a CLIENT_MESSAGE record.
 * @param[in,out] restore The restore.
 * @param[in] fields Its fields before the packet.
 * @param[in] block The packet, or MQTT_ARENA_NONE.
 * @param[in] length Number of bytes of the packet.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status reload_message(client_restore *restore,
                                  const uint8_t *fields, uint32_t block,
                                  uint32_t length);

/**
 * @brief Check that a SUBSCRIBE or UNSUBSCRIBE packet decodes with MQTT 3.1,
 * with a Packet Identifier.
 * @param[in] bytes The packet.
 * @param[in] length Number of bytes.
 * @param[in] packet_identifier The Packet Identifier it must have.
 * @return true if it does.
 */
static bool reload_request_valid(const uint8_t *bytes, uint32_t length,
                                 uint16_t packet_identifier);

/**
 * @brief Apply a CLIENT_PENDING record.
 * @param[in,out] restore The restore.
 * @param[in] packet_identifier Its Packet Identifier.
 * @param[in] block The packet, or MQTT_ARENA_NONE.
 * @param[in] length Number of bytes of the packet.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status reload_pending(client_restore *restore,
                                  uint16_t packet_identifier, uint32_t block,
                                  uint32_t length);

/**
 * @brief Apply a record about a message that names its Packet Identifier.
 * @param[in,out] restore The restore.
 * @param[in] packet_identifier The Packet Identifier.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status reload_mark_message(client_restore *restore,
                                       uint16_t packet_identifier);

/**
 * @brief Apply a record about a QoS 2 Packet Identifier received or a packet
 * waiting for its answer.
 * @param[in,out] restore The restore.
 * @param[in] packet_identifier The Packet Identifier.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status reload_mark_other(client_restore *restore,
                                     uint16_t packet_identifier);

/**
 * @brief Read and apply a record whose content goes to the arena in part.
 * @param[in,out] restore The restore, with the record's offset and kind.
 * @param[in] length Number of bytes of content.
 * @param[in] fields Number of bytes before the part that goes to the arena.
 * @param[in,out] crc The CRC of the frame.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status reload_large(client_restore *restore, uint32_t length,
                                uint32_t fields, uint16_t *crc);

/**
 * @brief Read and apply one record.
 * @param[in,out] restore The restore, with the record's offset.
 * @param[out] length Number of bytes of its content.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status reload_record(client_restore *restore, uint32_t *length);

/**
 * @brief Apply the records of one place, up to where they may be applied.
 * @param[in,out] restore The restore.
 * @param[in] part The part.
 * @param[in] place The place.
 * @param[in] found What its scan found.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status reload_place(client_restore *restore, mqtt_store_part part,
                                uint32_t place, const mqtt_store_place *found);

/**
 * @brief Check what was put back, and leave the client offline with it.
 * @param[in,out] restore The restore.
 * @param[in] last What the scan of the last segment applied found.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status reload_finish(client_restore *restore,
                                 const mqtt_store_scan *last);

/**
 * @brief Set the client up again, empty, with its store.
 * @param[in,out] client The client.
 */
static void reload_reset(mqtt_client *client);

/**
 * @brief Find whether a client holds nothing yet, as a restore needs.
 * @param[in] client The client.
 * @return true if it has a store, has written nothing and holds nothing.
 */
static bool reload_fresh(const mqtt_client *client);

/**
 * @brief Apply the places of a chain, then finish.
 * @param[in,out] restore The restore.
 * @param[in] copies What each snapshot copy holds.
 * @param[in] journals What each journal place holds.
 * @param[in] chain The places.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status reload_chain(client_restore *restore,
                                const mqtt_store_place *copies,
                                const mqtt_store_place *journals,
                                const mqtt_store_chain *chain);

mqtt_status mqtt_client_restore(mqtt_client *client,
                                const mqtt_store_source *source,
                                uint8_t *buffer, uint32_t size,
                                mqtt_store_fault *fault)
{
    /** @brief What each copy holds. */
    mqtt_store_place copies[MQTT_CLIENT_RESTORE_COPIES];
    /** @brief What each journal place holds. */
    mqtt_store_place journals[2];
    /** @brief The places to apply. */
    mqtt_store_chain chain = {MQTT_STORE_PLACE_NONE, MQTT_STORE_PLACE_NONE,
                              MQTT_STORE_PLACE_NONE};
    /** @brief The restore. */
    client_restore restore;
    /** @brief The outcome. */
    mqtt_status status = MQTT_INVALID_ARGUMENT;

    if ((fault != NULL) && mqtt_client_ready(client) && (source != NULL) &&
        (source->read != NULL) && (source->copies > 0U) &&
        (source->copies <= MQTT_CLIENT_RESTORE_COPIES) && (buffer != NULL) &&
        (size >= MQTT_CLIENT_RESTORE_MINIMUM) &&
        (client->store.state != MQTT_CLIENT_STORE_OFF)) {
        status = reload_fresh(client) ? MQTT_OK : MQTT_INVALID_STATE;
    }
    if (status == MQTT_OK) {
        fault->kind = MQTT_STORE_FAULT_NONE;
        restore.client = client;
        restore.source = source;
        restore.buffer = buffer;
        restore.size = size;
        restore.fault = fault;
        restore.part = MQTT_STORE_JOURNAL;
        restore.place = 0U;
        restore.generation = 0U;
        restore.offset = 0U;
        restore.kind = 0U;
        restore.snapshot = false;
        status = mqtt_store_choose(source, (uint8_t)MQTT_STORE_CLIENT_SNAPSHOT,
                                   (uint8_t)MQTT_STORE_CLIENT_JOURNAL, buffer,
                                   size, copies, journals, &chain, fault);
    }
    if (status == MQTT_OK) {
        status = reload_chain(&restore, copies, journals, &chain);
    }
    if (status == MQTT_NOT_FOUND) {
        /* The store holds nothing, so the client starts as it is. */
        status = MQTT_OK;
    } else if (status == MQTT_STORE_DAMAGED) {
        reload_reset(client);
    } else {
        /* Done, or a wrong argument or state, which changes nothing. */
    }
    return status;
}

static mqtt_status reload_fail(client_restore *restore,
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

static mqtt_status reload_read(client_restore *restore, uint64_t offset,
                               uint8_t *data, uint32_t length, uint16_t *crc)
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
        status = reload_fail(restore, MQTT_STORE_FAULT_MEDIUM);
    } else {
        *crc = mqtt_store_crc16(*crc, data, length);
    }
    return status;
}

static mqtt_status reload_pass(client_restore *restore, uint64_t offset,
                               uint32_t length, uint16_t *crc)
{
    /** @brief Number of bytes read so far. */
    uint32_t done = 0U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    while ((status == MQTT_OK) && (done < length)) {
        /** @brief Number of bytes to read now. */
        const uint32_t chunk = ((length - done) < restore->size) ?
                                   (length - done) :
                                   restore->size;

        status = reload_read(restore, offset + (uint64_t)done,
                             restore->buffer, chunk, crc);
        done += chunk;
    }
    return status;
}

static uint16_t reload_u16(const uint8_t *bytes)
{
    return (uint16_t)(((uint32_t)bytes[0] << 8U) | (uint32_t)bytes[1]);
}

static mqtt_status reload_take(client_restore *restore, uint64_t offset,
                               uint32_t length, uint16_t *crc,
                               uint32_t *block)
{
    /** @brief The client. */
    mqtt_client *client = restore->client;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    *block = MQTT_ARENA_NONE;
    if (length > 0U) {
        *block = mqtt_arena_take(&client->arena, length);
        if (*block == MQTT_ARENA_NONE) {
            status = reload_fail(restore, MQTT_STORE_FAULT_ROOM);
        } else {
            status = reload_read(restore, offset,
                                 mqtt_arena_bytes(&client->arena, *block),
                                 length, crc);
        }
    }
    return status;
}

static mqtt_status reload_tail(client_restore *restore, uint64_t offset,
                               uint16_t crc)
{
    /** @brief The CRC at the end of the record. */
    uint8_t tail[MQTT_STORE_FRAME_TAIL];
    /** @brief The CRC of the tail itself, which isn't used. */
    uint16_t unused = 0U;
    /** @brief The outcome. */
    mqtt_status status = reload_read(restore, offset, tail,
                                     MQTT_STORE_FRAME_TAIL, &unused);

    if ((status == MQTT_OK) && (reload_u16(tail) != crc)) {
        status = reload_fail(restore, MQTT_STORE_FAULT_RECORD);
    }
    return status;
}

static bool reload_in_use(const mqtt_client *client,
                          uint16_t packet_identifier)
{
    /** @brief The answer. */
    bool used = (mqtt_client_message_find(client, packet_identifier) !=
                 MQTT_CLIENT_NONE);
    /** @brief Index of the entry being looked at. */
    uint32_t index = 0U;

    while ((!used) && (index < client->storage.pending_count)) {
        used = (client->storage.pending[index].packet_identifier ==
                packet_identifier);
        index += 1U;
    }
    return used;
}

static mqtt_status reload_session(client_restore *restore,
                                  const uint8_t *fields, uint32_t block,
                                  uint32_t length)
{
    /** @brief The client. */
    mqtt_client *client = restore->client;
    /** @brief The version. */
    const uint32_t version = (uint32_t)fields[0];
    /** @brief The flags. */
    const uint32_t flags = (uint32_t)fields[1];
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (((version != (uint32_t)MQTT_VERSION_31) &&
         (version != (uint32_t)MQTT_VERSION_311) &&
         (version != (uint32_t)MQTT_VERSION_5)) ||
        ((flags & ~MQTT_STORE_CLIENT_EXISTS) != 0U) ||
        ((client->list_head != MQTT_CLIENT_NONE) &&
         (version != (uint32_t)client->version))) {
        /* What is held is encoded for its version, which can't change. */
        mqtt_arena_give(&client->arena, block);
        status = reload_fail(restore, MQTT_STORE_FAULT_CONTENT);
    } else {
        /** @brief Index of a packet waiting for its answer. */
        uint32_t index = 0U;

        if (version == (uint32_t)MQTT_VERSION_31) {
            client->version = MQTT_VERSION_31;
        } else if (version == (uint32_t)MQTT_VERSION_311) {
            client->version = MQTT_VERSION_311;
        } else {
            client->version = MQTT_VERSION_5;
        }
        client->version_known = true;
        client->session_exists = ((flags & MQTT_STORE_CLIENT_EXISTS) != 0U);
        if (!client->session_exists) {
            client->incoming_used = 0U;
        }
        /* Only MQTT 3.1 keeps these from one connection to the next. */
        while ((index < client->storage.pending_count) &&
               ((!client->session_exists) ||
                (client->version != MQTT_VERSION_31))) {
            mqtt_arena_give(&client->arena,
                            client->storage.pending[index].block);
            client->storage.pending[index].block = MQTT_ARENA_NONE;
            client->storage.pending[index].packet_identifier = 0U;
            index += 1U;
        }
        mqtt_arena_give(&client->arena, client->store.identifier_block);
        client->store.identifier_block = block;
        client->store.identifier_length = (uint16_t)length;
    }
    return status;
}

static bool reload_publish_valid(const mqtt_client *client,
                                 const uint8_t *fields, const uint8_t *bytes,
                                 uint32_t length)
{
    /** @brief The Fixed Header. */
    mqtt_fixed_header header;
    /** @brief The packet. */
    mqtt_publish packet;
    /** @brief true if the record says RETAIN is set. */
    const bool retain = (((uint32_t)fields[4] & MQTT_STORE_RETAIN) != 0U);
    /** @brief The answer. */
    bool valid = (mqtt_fixed_header_decode(bytes, length, &header) == MQTT_OK);

    if (valid) {
        valid = (header.type == MQTT_PUBLISH) &&
                ((header.size + header.remaining_length) == length) &&
                (mqtt_fixed_header_check(&header, client->version) == MQTT_OK);
    }
    if (valid) {
        valid = (mqtt_publish_decode(&header, &bytes[header.size],
                                     client->version, &packet) == MQTT_OK);
    }
    if (valid) {
        /** @brief The Topic Alias the packet carries, or zero. */
        uint32_t alias = 0U;

        if (mqtt5_properties_has(&packet.properties, MQTT5_PROP_TOPIC_ALIAS)) {
            alias = (uint32_t)packet.properties.topic_alias;
        }
        valid = (packet.packet_identifier == reload_u16(fields)) &&
                ((uint32_t)packet.qos == (uint32_t)fields[3]) &&
                (packet.retain == retain) &&
                (packet.topic_name.length > 0U) &&
                (alias == (uint32_t)reload_u16(&fields[5]));
    }
    return valid;
}

static mqtt_status reload_message(client_restore *restore,
                                  const uint8_t *fields, uint32_t block,
                                  uint32_t length)
{
    /** @brief The client. */
    mqtt_client *client = restore->client;
    /** @brief The Packet Identifier. */
    const uint16_t identifier = reload_u16(fields);
    /** @brief The stage. */
    const uint32_t stage = (uint32_t)fields[2];
    /** @brief The QoS. */
    const uint32_t qos = (uint32_t)fields[3];
    /** @brief true if the record has what its stage needs. */
    bool valid = client->version_known && (identifier != 0U) &&
                 ((qos == MQTT_QOS_1) || (qos == MQTT_QOS_2)) &&
                 (((uint32_t)fields[4] & ~MQTT_STORE_RETAIN) == 0U) &&
                 (stage <= (uint32_t)MQTT_CLIENT_MESSAGE_RELEASED) &&
                 (restore->snapshot ||
                  (stage == (uint32_t)MQTT_CLIENT_MESSAGE_QUEUED));
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (valid && (stage == (uint32_t)MQTT_CLIENT_MESSAGE_RELEASED)) {
        valid = (qos == MQTT_QOS_2) && (length == 0U);
    } else if (valid && (length > 0U)) {
        valid = reload_publish_valid(
            client, fields, mqtt_arena_bytes(&client->arena, block), length);
    } else {
        /* Refused already, or a message waiting with no packet. */
        valid = false;
    }
    if (valid) {
        valid = !reload_in_use(client, identifier);
    }
    if (!valid) {
        mqtt_arena_give(&client->arena, block);
        status = reload_fail(restore, MQTT_STORE_FAULT_CONTENT);
    } else if (client->free_message == MQTT_CLIENT_NONE) {
        mqtt_arena_give(&client->arena, block);
        status = reload_fail(restore, MQTT_STORE_FAULT_ROOM);
    } else {
        /** @brief The message. */
        mqtt_client_message *entry =
            &client->storage.messages[mqtt_client_message_add(
                client, block, length, identifier)];

        if (stage == (uint32_t)MQTT_CLIENT_MESSAGE_QUEUED) {
            entry->state = MQTT_CLIENT_MESSAGE_QUEUED;
        } else if (stage == (uint32_t)MQTT_CLIENT_MESSAGE_SENT) {
            entry->state = MQTT_CLIENT_MESSAGE_SENT;
        } else {
            entry->state = MQTT_CLIENT_MESSAGE_RELEASED;
        }
        entry->qos = (uint8_t)qos;
        entry->retain = (((uint32_t)fields[4] & MQTT_STORE_RETAIN) != 0U);
        entry->topic_alias = reload_u16(&fields[5]);
    }
    return status;
}

static bool reload_request_valid(const uint8_t *bytes, uint32_t length,
                                 uint16_t packet_identifier)
{
    /** @brief The Fixed Header. */
    mqtt_fixed_header header;
    /** @brief The packet. */
    mqtt_subscribe packet;
    /** @brief The answer. */
    bool valid = (mqtt_fixed_header_decode(bytes, length, &header) == MQTT_OK);

    mqtt_subscribe_clear(&packet);
    if (valid) {
        valid = ((header.type == MQTT_SUBSCRIBE) ||
                 (header.type == MQTT_UNSUBSCRIBE)) &&
                ((uint32_t)header.flags == RESTORE_REQUEST_FLAGS) &&
                ((header.size + header.remaining_length) == length);
    }
    if (valid && (header.type == MQTT_SUBSCRIBE)) {
        valid = (mqtt_subscribe_decode(&bytes[header.size],
                                       header.remaining_length,
                                       MQTT_VERSION_31, &packet) == MQTT_OK);
    } else if (valid) {
        valid = (mqtt_unsubscribe_decode(&bytes[header.size],
                                         header.remaining_length,
                                         MQTT_VERSION_31, &packet) == MQTT_OK);
    } else {
        /* Refused already. */
    }
    return valid && (packet.packet_identifier == packet_identifier);
}

static mqtt_status reload_pending(client_restore *restore,
                                  uint16_t packet_identifier, uint32_t block,
                                  uint32_t length)
{
    /** @brief The client. */
    mqtt_client *client = restore->client;
    /** @brief Index of the entry being looked at. */
    uint32_t index = 0U;
    /** @brief true if the record holds what it may. */
    bool valid = client->session_exists &&
                 (client->version == MQTT_VERSION_31) &&
                 (packet_identifier != 0U);
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (valid) {
        valid = !reload_in_use(client, packet_identifier);
    }
    if (valid && (block != MQTT_ARENA_NONE)) {
        valid = reload_request_valid(mqtt_arena_bytes(&client->arena, block),
                                     length, packet_identifier);
    }
    while ((index < client->storage.pending_count) &&
           (client->storage.pending[index].packet_identifier != 0U)) {
        index += 1U;
    }
    if (!valid) {
        mqtt_arena_give(&client->arena, block);
        status = reload_fail(restore, MQTT_STORE_FAULT_CONTENT);
    } else if (index >= client->storage.pending_count) {
        mqtt_arena_give(&client->arena, block);
        status = reload_fail(restore, MQTT_STORE_FAULT_ROOM);
    } else {
        client->storage.pending[index].packet_identifier = packet_identifier;
        client->storage.pending[index].block = block;
        client->storage.pending[index].length = length;
    }
    return status;
}

static mqtt_status reload_mark_message(client_restore *restore,
                                       uint16_t packet_identifier)
{
    /** @brief The client. */
    mqtt_client *client = restore->client;
    /** @brief The message it names. */
    const uint32_t index =
        mqtt_client_message_find(client, packet_identifier);
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if ((packet_identifier == 0U) || (index == MQTT_CLIENT_NONE)) {
        status = reload_fail(restore, MQTT_STORE_FAULT_CONTENT);
    } else {
        /** @brief The message. */
        mqtt_client_message *entry = &client->storage.messages[index];

        if (restore->kind == (uint8_t)MQTT_STORE_RECORD_CLIENT_FINISHED) {
            mqtt_client_message_remove(client, index);
        } else if ((restore->kind == (uint8_t)MQTT_STORE_RECORD_CLIENT_SENT) &&
                   (entry->state == MQTT_CLIENT_MESSAGE_QUEUED)) {
            entry->state = MQTT_CLIENT_MESSAGE_SENT;
        } else if ((restore->kind ==
                    (uint8_t)MQTT_STORE_RECORD_CLIENT_RELEASED) &&
                   (entry->state == MQTT_CLIENT_MESSAGE_SENT) &&
                   (entry->qos == MQTT_QOS_2)) {
            /* The bytes of the PUBLISH are no longer needed. */
            entry->state = MQTT_CLIENT_MESSAGE_RELEASED;
            mqtt_arena_give(&client->arena, entry->block);
            entry->block = MQTT_ARENA_NONE;
            entry->size = 0U;
        } else {
            status = reload_fail(restore, MQTT_STORE_FAULT_CONTENT);
        }
    }
    return status;
}

static mqtt_status reload_mark_other(client_restore *restore,
                                     uint16_t packet_identifier)
{
    /** @brief The client. */
    mqtt_client *client = restore->client;
    /** @brief Index of the entry being looked at. */
    uint32_t index = 0U;
    /** @brief The entry found, or MQTT_CLIENT_NONE. */
    uint32_t found = MQTT_CLIENT_NONE;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (restore->kind == (uint8_t)MQTT_STORE_RECORD_CLIENT_PENDING_DONE) {
        while ((found == MQTT_CLIENT_NONE) &&
               (index < client->storage.pending_count)) {
            if (client->storage.pending[index].packet_identifier ==
                packet_identifier) {
                found = index;
            }
            index += 1U;
        }
    } else {
        while ((found == MQTT_CLIENT_NONE) && (index < client->incoming_used)) {
            if (client->storage.incoming[index] == packet_identifier) {
                found = index;
            }
            index += 1U;
        }
    }
    if (packet_identifier == 0U) {
        status = reload_fail(restore, MQTT_STORE_FAULT_CONTENT);
    } else if (restore->kind == (uint8_t)MQTT_STORE_RECORD_CLIENT_INCOMING) {
        if ((found != MQTT_CLIENT_NONE) || (!client->session_exists)) {
            status = reload_fail(restore, MQTT_STORE_FAULT_CONTENT);
        } else if (client->incoming_used >= client->storage.incoming_count) {
            status = reload_fail(restore, MQTT_STORE_FAULT_ROOM);
        } else {
            client->storage.incoming[client->incoming_used] =
                packet_identifier;
            client->incoming_used += 1U;
        }
    } else if (found == MQTT_CLIENT_NONE) {
        status = reload_fail(restore, MQTT_STORE_FAULT_CONTENT);
    } else if (restore->kind ==
               (uint8_t)MQTT_STORE_RECORD_CLIENT_INCOMING_REMOVE) {
        client->incoming_used -= 1U;
        client->storage.incoming[found] =
            client->storage.incoming[client->incoming_used];
    } else {
        mqtt_arena_give(&client->arena, client->storage.pending[found].block);
        client->storage.pending[found].block = MQTT_ARENA_NONE;
        client->storage.pending[found].packet_identifier = 0U;
    }
    return status;
}

static mqtt_status reload_large(client_restore *restore, uint32_t length,
                                uint32_t fields, uint16_t *crc)
{
    /** @brief The record's content before the part in the arena. */
    uint8_t head[RESTORE_MESSAGE_FIELDS] = {0U, 0U, 0U, 0U, 0U, 0U, 0U};
    /** @brief Offset of the content. */
    const uint64_t at = restore->offset + MQTT_STORE_FRAME_HEAD;
    /** @brief The block of the part in the arena. */
    uint32_t block = MQTT_ARENA_NONE;
    /** @brief The outcome. */
    mqtt_status status = (length < fields) ?
                             reload_fail(restore, MQTT_STORE_FAULT_CONTENT) :
                             reload_read(restore, at, head, fields, crc);

    if ((status == MQTT_OK) &&
        (restore->kind == (uint8_t)MQTT_STORE_RECORD_CLIENT_SESSION) &&
        (((uint32_t)reload_u16(&head[2]) + RESTORE_SESSION_FIELDS) !=
         length)) {
        status = reload_fail(restore, MQTT_STORE_FAULT_CONTENT);
    }
    if (status == MQTT_OK) {
        status = reload_take(restore, at + (uint64_t)fields, length - fields,
                             crc, &block);
    }
    if (status == MQTT_OK) {
        status = reload_tail(restore, at + (uint64_t)length, *crc);
    }
    if (status != MQTT_OK) {
        mqtt_arena_give(&restore->client->arena, block);
    } else if (restore->kind == (uint8_t)MQTT_STORE_RECORD_CLIENT_SESSION) {
        status = reload_session(restore, head, block, length - fields);
    } else if (restore->kind == (uint8_t)MQTT_STORE_RECORD_CLIENT_MESSAGE) {
        status = reload_message(restore, head, block, length - fields);
    } else {
        status = reload_pending(restore, reload_u16(head), block,
                                length - fields);
    }
    return status;
}

static mqtt_status reload_record(client_restore *restore, uint32_t *length)
{
    /** @brief The frame, then the content of a small record. */
    uint8_t bytes[MQTT_STORE_END_SIZE];
    /** @brief The CRC of the record. */
    uint16_t crc = (uint16_t)MQTT_STORE_CRC_INIT;
    /** @brief The outcome. */
    mqtt_status status = reload_read(restore, restore->offset, bytes,
                                     MQTT_STORE_FRAME_HEAD, &crc);
    /** @brief Offset of the content. */
    const uint64_t at = restore->offset + MQTT_STORE_FRAME_HEAD;

    *length = 0U;
    if (status == MQTT_OK) {
        if (!mqtt_store_frame_check(bytes, &restore->kind, length)) {
            /* The scan found it right, so the medium has changed since. */
            status = reload_fail(restore, MQTT_STORE_FAULT_MEDIUM);
        }
    }
    /* The record's CRC leaves out the head's own. */
    crc = mqtt_store_frame_crc(bytes);
    if (status != MQTT_OK) {
        /* The medium failed, or changed. */
    } else if ((restore->kind == (uint8_t)MQTT_STORE_RECORD_CLIENT_SESSION) ||
               (restore->kind == (uint8_t)MQTT_STORE_RECORD_CLIENT_PENDING)) {
        status = reload_large(restore, *length,
                              (restore->kind ==
                                (uint8_t)MQTT_STORE_RECORD_CLIENT_SESSION) ?
                                   RESTORE_SESSION_FIELDS :
                                   RESTORE_MARK_FIELDS,
                              &crc);
    } else if (restore->kind == (uint8_t)MQTT_STORE_RECORD_CLIENT_MESSAGE) {
        status = reload_large(restore, *length, RESTORE_MESSAGE_FIELDS, &crc);
    } else if ((restore->kind == (uint8_t)MQTT_STORE_RECORD_COMMIT) ||
               (restore->kind == (uint8_t)MQTT_STORE_RECORD_END) ||
               ((restore->kind & MQTT_STORE_RECORD_OPTIONAL) != 0U)) {
        /* The scan checked these, and a record a later minor version adds
         * may be left out: only their CRC matters here. */
        status = reload_pass(restore, at, *length, &crc);
        if (status == MQTT_OK) {
            status = reload_tail(restore, at + (uint64_t)*length, crc);
        }
    } else if ((*length != RESTORE_MARK_FIELDS) ||
               (restore->kind < (uint8_t)MQTT_STORE_RECORD_CLIENT_SENT) ||
               (restore->kind >
                (uint8_t)MQTT_STORE_RECORD_CLIENT_PENDING_DONE)) {
        status = reload_fail(restore, MQTT_STORE_FAULT_CONTENT);
    } else {
        status = reload_read(restore, at, bytes, RESTORE_MARK_FIELDS, &crc);
        if (status == MQTT_OK) {
            status = reload_tail(restore, at + RESTORE_MARK_FIELDS, crc);
        }
        if (status != MQTT_OK) {
            /* The record is damaged, and nothing is applied. */
        } else if (restore->kind <=
                   (uint8_t)MQTT_STORE_RECORD_CLIENT_FINISHED) {
            status = reload_mark_message(restore, reload_u16(bytes));
        } else {
            status = reload_mark_other(restore, reload_u16(bytes));
        }
    }
    return status;
}

static mqtt_status reload_place(client_restore *restore, mqtt_store_part part,
                                uint32_t place, const mqtt_store_place *found)
{
    /** @brief Number of bytes of the record's content. */
    uint32_t length = 0U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    restore->part = part;
    restore->place = place;
    restore->generation = found->scan.header.generation;
    restore->snapshot = (part == MQTT_STORE_SNAPSHOT);
    restore->offset = MQTT_STORE_HEADER_SIZE;
    restore->kind = 0U;
    while ((status == MQTT_OK) && (restore->offset < found->scan.commit_end)) {
        status = reload_record(restore, &length);
        restore->offset += MQTT_STORE_FRAME_HEAD + (uint64_t)length +
                           MQTT_STORE_FRAME_TAIL;
    }
    if ((status == MQTT_OK) && (restore->offset != found->scan.commit_end)) {
        status = reload_fail(restore, MQTT_STORE_FAULT_MEDIUM);
    }
    return status;
}

static mqtt_status reload_finish(client_restore *restore,
                                 const mqtt_store_scan *last)
{
    /** @brief The client. */
    mqtt_client *client = restore->client;
    /** @brief The journal. */
    mqtt_client_store *store = &client->store;
    /** @brief The message being looked at. */
    uint32_t index = client->list_head;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    restore->kind = 0U;
    restore->offset = 0U;
    /* Without a Session, nothing sent or received is held: [MQTT-4.1.0-1]
     * keeps that state only while the Session lasts. */
    while ((status == MQTT_OK) && (index != MQTT_CLIENT_NONE)) {
        if ((!client->session_exists) &&
            (client->storage.messages[index].state !=
             MQTT_CLIENT_MESSAGE_QUEUED)) {
            status = reload_fail(restore, MQTT_STORE_FAULT_CONTENT);
        }
        index = client->storage.messages[index].next;
    }
    if ((status == MQTT_OK) && (!client->session_exists) &&
        (client->incoming_used > 0U)) {
        status = reload_fail(restore, MQTT_STORE_FAULT_CONTENT);
    }
    if (status == MQTT_OK) {
        client->send_cursor = client->list_head;
        client->release_cursor = MQTT_CLIENT_NONE;
        client->pending_cursor = client->storage.pending_count;
        store->generation = last->header.generation;
        store->closed = last->header.base + last->commits;
        store->stored = store->closed;
        store->segment_base = store->closed;
        store->snapshot_whole = true;
        store->session_due = false;
        store->written_version = client->version;
        store->written_exists = client->session_exists;
    }
    return status;
}

static void reload_reset(mqtt_client *client)
{
    /** @brief The memory, copied out before the client is set up. */
    mqtt_client_storage storage;
    /** @brief The store's configuration, copied out likewise. */
    mqtt_client_store_config config;

    mqtt_bytes_put((uint8_t *)&storage, (const uint8_t *)&client->storage,
                   (uint32_t)sizeof(storage));
    mqtt_bytes_put((uint8_t *)&config, (const uint8_t *)&client->store.config,
                   (uint32_t)sizeof(config));
    /* What init took once it takes again, so these can't fail. */
    if (mqtt_client_init(client, &storage) != MQTT_OK) {
        client->ready = MQTT_BOOL_UNSET;
    } else if (mqtt_client_store_init(client, &config) != MQTT_OK) {
        client->ready = MQTT_BOOL_UNSET;
    } else {
        /* Empty, with its store, as before the restore. */
    }
}

static bool reload_fresh(const mqtt_client *client)
{
    return (client->store.state == MQTT_CLIENT_STORE_FRESH) &&
           (client->store.generation == 0U) && (!client->version_known) &&
           (client->state == MQTT_CLIENT_OFFLINE) &&
           (client->list_head == MQTT_CLIENT_NONE) &&
           (client->incoming_used == 0U);
}

static mqtt_status reload_chain(client_restore *restore,
                                const mqtt_store_place *copies,
                                const mqtt_store_place *journals,
                                const mqtt_store_chain *chain)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief The last segment applied. */
    const uint32_t last = (chain->second != MQTT_STORE_PLACE_NONE) ?
                              chain->second :
                              chain->first;

    if (chain->copy != MQTT_STORE_PLACE_NONE) {
        status = reload_place(restore, MQTT_STORE_SNAPSHOT, chain->copy,
                              &copies[chain->copy]);
    }
    if (status == MQTT_OK) {
        status = reload_place(restore, MQTT_STORE_JOURNAL, chain->first,
                              &journals[chain->first]);
    }
    if ((status == MQTT_OK) && (chain->second != MQTT_STORE_PLACE_NONE)) {
        status = reload_place(restore, MQTT_STORE_JOURNAL, chain->second,
                              &journals[chain->second]);
    }
    if (status == MQTT_OK) {
        status = reload_finish(restore, &journals[last].scan);
    }
    return status;
}
