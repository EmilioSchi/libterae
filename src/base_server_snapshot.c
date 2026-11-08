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
 * @file base_server_snapshot.c
 * @brief The MQTT Server: the snapshot of what outlasts a restart, with which
 * the journal starts again.
 *
 * A snapshot begins with a new journal segment, and the two together are what
 * a restore reads: the snapshot first, then the segment. The snapshot is
 * handed out a record at a time while the server goes on, so what it holds of
 * each thing is from the moment it got there. That is enough, as each record
 * says what something is, and whatever changed after the segment began is in
 * the segment too.
 *
 * When the snapshot begins, everything it will hold is given a store id, and
 * the counter of ids is noted. The snapshot holds only what is named below
 * that, as anything named later was made after the segment began, and the
 * segment holds it. The messages come first, so every record after them can
 * name them, then the retained messages, then each Session with its
 * subscriptions, its Will Message, its deliveries and the QoS 2 flows its
 * Client has in progress, then END.
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015.
 * @li Cited: 3.1.2.4, 4.1.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 4.1, p.92-93. 4.8.2, p.101-103.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base_qos.h"
#include "base_server_internal.h"
#include "base_string.h"
#include "base_topic.h"
#include "v5_share.h"

/** @brief Step of a Session: its SESSION record. */
#define SNAPSHOT_SESSION 0U

/** @brief Step of a Session: its subscriptions. */
#define SNAPSHOT_SUBSCRIPTIONS 1U

/** @brief Step of a Session: its Will Message. */
#define SNAPSHOT_WILL 2U

/** @brief Step of a Session: the DELIVERY of a message in flight. */
#define SNAPSHOT_INFLIGHT 3U

/** @brief Step of a Session: the SENT of that message. */
#define SNAPSHOT_SENT 4U

/** @brief Step of a Session: the RELEASED of that message. */
#define SNAPSHOT_RELEASED 5U

/** @brief Step of a Session: the messages waiting. */
#define SNAPSHOT_QUEUED 6U

/** @brief Step of a Session: the QoS 2 flows its Client has in progress. */
#define SNAPSHOT_INCOMING 7U

/**
 * @brief Find whether the snapshot holds what a store id names.
 * @param[in] server The server.
 * @param[in] store_id The store id.
 * @return true if it was named before the snapshot began.
 */
static bool snapshot_holds(const mqtt_server *server, uint64_t store_id);

/**
 * @brief Find whether the snapshot holds a delivery.
 * @param[in] server The server.
 * @param[in] delivery The delivery.
 * @return true if it does.
 */
static bool snapshot_delivery(const mqtt_server *server, uint32_t delivery);

/**
 * @brief Give a store id to a message that has none.
 * @param[in,out] server The server.
 * @param[in] message The message, or MQTT_SERVER_NONE.
 */
static void snapshot_name_message(mqtt_server *server, uint32_t message);

/**
 * @brief Give a store id to each QoS 1 and QoS 2 delivery of a list, and to
 * its message, that has none.
 * @param[in,out] server The server.
 * @param[in] head The first delivery of the list.
 */
static void snapshot_name_list(mqtt_server *server, uint32_t head);

/**
 * @brief Move the walk on from the messages to where the next record is.
 * @param[in,out] server The server.
 * @return true once the walk is at a message to give, or past the messages.
 */
static bool snapshot_settle_messages(mqtt_server *server);

/**
 * @brief Move the walk on through the retained messages.
 * @param[in,out] server The server.
 * @return true once the walk is at one to give, or past them.
 */
static bool snapshot_settle_retained(mqtt_server *server);

/**
 * @brief Move the walk on within a Session's deliveries in flight or waiting.
 * @param[in,out] server The server.
 * @param[in] entry The Session.
 * @return true once the walk is at a delivery to give.
 */
static bool snapshot_settle_list(mqtt_server *server,
                                 const mqtt_session *entry);

/**
 * @brief Write the record of a delivery where a Session's walk is: its
 * DELIVERY, SENT or RELEASED.
 * @param[in,out] server The server.
 * @param[in,out] builder The record.
 * @return The kind of record.
 */
static uint8_t snapshot_delivery_record(mqtt_server *server,
                                        mqtt_journal_builder *builder);

/**
 * @brief Move the walk on within a Session, or to the next one, until it is
 * where a record is.
 * @param[in,out] server The server.
 * @return true once the walk is at a record, or past the Sessions.
 */
static bool snapshot_settle_session(mqtt_server *server);

/**
 * @brief Move the walk on until it is where a record is.
 * @param[in,out] server The server.
 */
static void snapshot_settle(mqtt_server *server);

/**
 * @brief Write a Shared Subscription's or a subscription's Topic Filter.
 * @param[in,out] builder The record.
 * @param[in] server The server.
 * @param[in] subscription The subscription.
 */
static void snapshot_filter(mqtt_journal_builder *builder,
                            const mqtt_server *server, uint32_t subscription);

/**
 * @brief Write the record where a Session's walk is, and move past it.
 * @param[in,out] server The server.
 * @param[in,out] builder The record.
 * @return The kind of record.
 */
static uint8_t snapshot_session_record(mqtt_server *server,
                                       mqtt_journal_builder *builder);

/**
 * @brief Write the record where the walk is, and move past it.
 * @param[in,out] server The server.
 * @param[in,out] builder The record.
 * @return The kind of record.
 */
static uint8_t snapshot_record(mqtt_server *server,
                               mqtt_journal_builder *builder);

static bool snapshot_holds(const mqtt_server *server, uint64_t store_id)
{
    return (store_id != 0U) && (store_id < server->store.walk_end);
}

static bool snapshot_delivery(const mqtt_server *server, uint32_t delivery)
{
    /** @brief The delivery. */
    const mqtt_delivery *entry = &server->storage.deliveries[delivery];

    return (entry->qos != MQTT_QOS_0) &&
           snapshot_holds(server, entry->store_id);
}

static void snapshot_name_message(mqtt_server *server, uint32_t message)
{
    if (message != MQTT_SERVER_NONE) {
        /** @brief The message. */
        mqtt_message *entry = &server->storage.messages[message];

        if (entry->store_id == 0U) {
            entry->store_id = server->store.next_id;
            server->store.next_id += 1U;
        }
    }
}

static void snapshot_name_list(mqtt_server *server, uint32_t head)
{
    /** @brief The delivery being named. */
    uint32_t index = head;

    while (index != MQTT_SERVER_NONE) {
        /** @brief The delivery. */
        mqtt_delivery *entry = &server->storage.deliveries[index];

        if (entry->qos != MQTT_QOS_0) {
            if (entry->store_id == 0U) {
                entry->store_id = server->store.next_id;
                server->store.next_id += 1U;
            }
            snapshot_name_message(server, entry->message);
        }
        index = entry->next;
    }
}

void mqtt_snapshot_prepare(mqtt_server *server)
{
    /** @brief The journal. */
    mqtt_server_store *store = &server->store;
    /** @brief Index of the Session or node being named. */
    uint32_t index = 0U;

    while (index < server->storage.session_count) {
        /** @brief The Session. */
        mqtt_session *entry = &server->storage.sessions[index];

        entry->stored = entry->in_use && (entry->expiry_s != 0U);
        if (entry->stored) {
            snapshot_name_list(server, entry->inflight_head);
            snapshot_name_list(server, entry->queued_head);
            snapshot_name_message(server, entry->will_message);
        }
        index += 1U;
    }
    index = mqtt_tree_next(&server->tree, 0U);
    while (index != MQTT_TREE_NONE) {
        snapshot_name_message(server, server->tree.nodes[index].retained);
        index = mqtt_tree_next(&server->tree, index + 1U);
    }
    store->walk_end = store->next_id;
    store->phase = MQTT_SERVER_SNAPSHOT_MESSAGES;
    store->snapshot_records = 0U;
    store->snapshot_index = 0U;
    store->snapshot_item = MQTT_SERVER_NONE;
    store->snapshot_step = SNAPSHOT_SESSION;
    snapshot_settle(server);
}

void mqtt_snapshot_changed(mqtt_server *server)
{
    if ((server->store.phase == MQTT_SERVER_SNAPSHOT_SESSIONS) &&
        (server->store.snapshot_step != SNAPSHOT_SESSION)) {
        server->store.snapshot_step = SNAPSHOT_SESSION;
        server->store.snapshot_item = MQTT_SERVER_NONE;
    }
}

static bool snapshot_settle_messages(mqtt_server *server)
{
    /** @brief The journal. */
    mqtt_server_store *store = &server->store;
    /** @brief The next message in use. */
    const uint32_t index =
        mqtt_slab_next(&server->message_slots, store->snapshot_index);
    /** @brief true once the walk is where a record is. */
    bool settled = true;

    if (index == MQTT_SLAB_NONE) {
        store->phase = MQTT_SERVER_SNAPSHOT_RETAINED;
        store->snapshot_index = 0U;
        settled = false;
    } else if (!snapshot_holds(server,
                               server->storage.messages[index].store_id)) {
        store->snapshot_index = index + 1U;
        settled = false;
    } else {
        store->snapshot_index = index;
    }
    return settled;
}

static bool snapshot_settle_retained(mqtt_server *server)
{
    /** @brief The journal. */
    mqtt_server_store *store = &server->store;
    /** @brief The next node in use. */
    const uint32_t node = mqtt_tree_next(&server->tree, store->snapshot_index);
    /** @brief true once the walk is where a record is. */
    bool settled = true;

    if (node == MQTT_TREE_NONE) {
        store->phase = MQTT_SERVER_SNAPSHOT_SESSIONS;
        store->snapshot_index = 0U;
        store->snapshot_step = SNAPSHOT_SESSION;
        settled = false;
    } else {
        /** @brief The retained message there. */
        const uint32_t retained = server->tree.nodes[node].retained;

        store->snapshot_index = node;
        if ((retained == MQTT_SERVER_NONE) ||
            !snapshot_holds(server,
                            server->storage.messages[retained].store_id)) {
            store->snapshot_index = node + 1U;
            settled = false;
        }
    }
    return settled;
}

static bool snapshot_settle_list(mqtt_server *server,
                                 const mqtt_session *entry)
{
    /** @brief The journal. */
    mqtt_server_store *store = &server->store;
    /** @brief The item the walk is at. */
    const uint32_t item = store->snapshot_item;
    /** @brief true once the walk is where a record is. */
    bool settled = true;

    if (item == MQTT_SERVER_NONE) {
        store->snapshot_step = (store->snapshot_step == SNAPSHOT_INFLIGHT) ?
                                   SNAPSHOT_QUEUED :
                                   SNAPSHOT_INCOMING;
        store->snapshot_item = (store->snapshot_step == SNAPSHOT_QUEUED) ?
                                   entry->queued_head :
                                   entry->incoming_head;
        settled = false;
    } else if (!snapshot_delivery(server, item)) {
        store->snapshot_item = server->storage.deliveries[item].next;
        settled = false;
    } else {
        /* A delivery to give. */
    }
    return settled;
}

static bool snapshot_settle_session(mqtt_server *server)
{
    /** @brief The journal. */
    mqtt_server_store *store = &server->store;
    /** @brief The Session the walk is at. */
    const mqtt_session *entry = NULL;
    /** @brief The item the walk is at. */
    const uint32_t item = store->snapshot_item;
    /** @brief true once the walk is where a record is. */
    bool settled = true;

    if (store->snapshot_index >= server->storage.session_count) {
        store->phase = MQTT_SERVER_SNAPSHOT_END;
    } else {
        entry = &server->storage.sessions[store->snapshot_index];
    }
    if (entry == NULL) {
        /* Past the Sessions. */
    } else if (!(entry->in_use && entry->stored)) {
        store->snapshot_index += 1U;
        store->snapshot_step = SNAPSHOT_SESSION;
        settled = false;
    } else if (store->snapshot_step == SNAPSHOT_SUBSCRIPTIONS) {
        if (item == MQTT_SERVER_NONE) {
            store->snapshot_step = SNAPSHOT_WILL;
            settled = false;
        }
    } else if (store->snapshot_step == SNAPSHOT_WILL) {
        if ((entry->will_message == MQTT_SERVER_NONE) ||
            !snapshot_holds(
                server,
                server->storage.messages[entry->will_message].store_id)) {
            store->snapshot_step = SNAPSHOT_INFLIGHT;
            store->snapshot_item = entry->inflight_head;
            settled = false;
        }
    } else if ((store->snapshot_step == SNAPSHOT_INFLIGHT) ||
               (store->snapshot_step == SNAPSHOT_QUEUED)) {
        settled = snapshot_settle_list(server, entry);
    } else if (store->snapshot_step == SNAPSHOT_RELEASED) {
        if (!server->storage.deliveries[item].released) {
            store->snapshot_step = SNAPSHOT_INFLIGHT;
            store->snapshot_item = server->storage.deliveries[item].next;
            settled = false;
        }
    } else if ((store->snapshot_step == SNAPSHOT_INCOMING) &&
               (item == MQTT_SERVER_NONE)) {
        store->snapshot_index += 1U;
        store->snapshot_step = SNAPSHOT_SESSION;
        settled = false;
    } else {
        /* SESSION, SENT, and a flow in progress always give a record. */
    }
    return settled;
}

static void snapshot_settle(mqtt_server *server)
{
    /** @brief true once the walk is where a record is. */
    bool settled = false;

    while (!settled) {
        switch (server->store.phase) {
        case MQTT_SERVER_SNAPSHOT_MESSAGES:
            settled = snapshot_settle_messages(server);
            break;
        case MQTT_SERVER_SNAPSHOT_RETAINED:
            settled = snapshot_settle_retained(server);
            break;
        case MQTT_SERVER_SNAPSHOT_SESSIONS:
            settled = snapshot_settle_session(server);
            break;
        default:
            /* END, or no snapshot. */
            settled = true;
            break;
        }
    }
}

static void snapshot_filter(mqtt_journal_builder *builder,
                            const mqtt_server *server, uint32_t subscription)
{
    /** @brief The bytes that start a Shared Subscription's filter. */
    static const uint8_t share[MQTT5_SHARE_PREFIX_LENGTH] = {
        0x24U, 0x73U, 0x68U, 0x61U, 0x72U, 0x65U, 0x2FU};
    /** @brief The subscription. */
    const mqtt_subscription *item =
        &server->storage.subscriptions[subscription];
    /** @brief The group, if it is shared. */
    const mqtt_share_group *group = (item->group == MQTT_SERVER_NONE) ?
                                        NULL :
                                        &server->storage.groups[item->group];
    /** @brief Number of bytes before the filter in the tree. */
    const uint32_t prefix =
        (group == NULL) ?
            0U :
            (MQTT5_SHARE_PREFIX_LENGTH + (uint32_t)group->name_length + 1U);
    /** @brief Number of bytes of the filter in the tree. */
    const uint32_t path = mqtt_tree_path_length(&server->tree, item->node);
    /** @brief Number of bytes in all. */
    const uint32_t total = prefix + path;

    mqtt_journal_integer(builder, (uint64_t)total, 2U);
    if ((total > (uint32_t)UINT16_MAX) ||
        (total > (builder->capacity - builder->used))) {
        builder->failed = true;
    } else {
        /** @brief Where the filter goes. */
        uint8_t *room = mqtt_journal_room(builder, total);

        if (group != NULL) {
            mqtt_bytes_put(room, share, MQTT5_SHARE_PREFIX_LENGTH);
            mqtt_bytes_put(&room[MQTT5_SHARE_PREFIX_LENGTH],
                           mqtt_page_arena_bytes(&server->arena,
                                                 group->name_block),
                           (uint32_t)group->name_length);
            room[prefix - 1U] = (uint8_t)MQTT_TOPIC_LEVEL_SEPARATOR;
        }
        (void)mqtt_tree_path_copy(&server->tree, item->node, &room[prefix],
                                  path);
    }
}

static uint8_t snapshot_session_record(mqtt_server *server,
                                       mqtt_journal_builder *builder)
{
    /** @brief The journal. */
    mqtt_server_store *store = &server->store;
    /** @brief The Session. */
    const uint32_t session = store->snapshot_index;
    /** @brief The item. */
    const uint32_t item = store->snapshot_item;
    /** @brief The kind of record. */
    uint8_t kind = (uint8_t)MQTT_STORE_RECORD_SESSION;

    if (store->snapshot_step == SNAPSHOT_SESSION) {
        mqtt_journal_session_fields(builder, server, session);
        store->snapshot_step = SNAPSHOT_SUBSCRIPTIONS;
        store->snapshot_item = server->storage.sessions[session].subscriptions;
    } else if (store->snapshot_step == SNAPSHOT_SUBSCRIPTIONS) {
        /** @brief The subscription. */
        const mqtt_subscription *entry = &server->storage.subscriptions[item];

        kind = (uint8_t)MQTT_STORE_RECORD_SUBSCRIBE;
        mqtt_journal_subscribe_fields(builder, server, session, entry->options,
                                      entry->identifier,
                                      entry->group != MQTT_SERVER_NONE);
        snapshot_filter(builder, server, item);
        store->snapshot_item = entry->next_of_session;
    } else if (store->snapshot_step == SNAPSHOT_WILL) {
        kind = (uint8_t)MQTT_STORE_RECORD_WILL;
        mqtt_journal_will_fields(builder, server, session);
        store->snapshot_step = SNAPSHOT_INFLIGHT;
        store->snapshot_item = server->storage.sessions[session].inflight_head;
    } else if (store->snapshot_step != SNAPSHOT_INCOMING) {
        kind = snapshot_delivery_record(server, builder);
    } else {
        /** @brief The Client Identifier. */
        const mqtt_utf8 identifier = mqtt_session_identifier(server, session);

        kind = (uint8_t)MQTT_STORE_RECORD_INCOMING;
        mqtt_journal_integer(
            builder,
            (uint64_t)server->storage.deliveries[item].packet_identifier, 2U);
        mqtt_journal_string(builder, &identifier);
        store->snapshot_item = server->storage.deliveries[item].next;
    }
    return kind;
}

static uint8_t snapshot_delivery_record(mqtt_server *server,
                                        mqtt_journal_builder *builder)
{
    /** @brief The journal. */
    mqtt_server_store *store = &server->store;
    /** @brief The delivery. */
    const mqtt_delivery *entry =
        &server->storage.deliveries[store->snapshot_item];
    /** @brief The kind of record. */
    uint8_t kind = (uint8_t)MQTT_STORE_RECORD_DELIVERY;

    if (store->snapshot_step == SNAPSHOT_SENT) {
        kind = (uint8_t)MQTT_STORE_RECORD_SENT;
        mqtt_journal_integer(builder, entry->store_id, 8U);
        mqtt_journal_integer(builder, (uint64_t)entry->packet_identifier, 2U);
        store->snapshot_step = SNAPSHOT_RELEASED;
    } else if (store->snapshot_step == SNAPSHOT_RELEASED) {
        kind = (uint8_t)MQTT_STORE_RECORD_RELEASED;
        mqtt_journal_integer(builder, entry->store_id, 8U);
        store->snapshot_step = SNAPSHOT_INFLIGHT;
        store->snapshot_item = entry->next;
    } else {
        mqtt_journal_delivery_fields(builder, server, store->snapshot_index,
                                     store->snapshot_item);
        if (store->snapshot_step == SNAPSHOT_INFLIGHT) {
            store->snapshot_step = SNAPSHOT_SENT;
        } else {
            store->snapshot_item = entry->next;
        }
    }
    return kind;
}

static uint8_t snapshot_record(mqtt_server *server,
                               mqtt_journal_builder *builder)
{
    /** @brief The journal. */
    mqtt_server_store *store = &server->store;
    /** @brief The kind of record. */
    uint8_t kind = (uint8_t)MQTT_STORE_RECORD_END;

    if (store->phase == MQTT_SERVER_SNAPSHOT_MESSAGES) {
        kind = (uint8_t)MQTT_STORE_RECORD_MESSAGE;
        mqtt_journal_message_fields(builder, server, store->snapshot_index);
        store->snapshot_index += 1U;
    } else if (store->phase == MQTT_SERVER_SNAPSHOT_RETAINED) {
        /** @brief The retained message. */
        const uint32_t message =
            server->tree.nodes[store->snapshot_index].retained;

        kind = (uint8_t)MQTT_STORE_RECORD_RETAINED;
        mqtt_journal_integer(builder,
                             server->storage.messages[message].store_id, 8U);
        store->snapshot_index += 1U;
    } else if (store->phase == MQTT_SERVER_SNAPSHOT_SESSIONS) {
        kind = snapshot_session_record(server, builder);
    } else {
        mqtt_journal_integer(builder, store->snapshot_records, 8U);
        mqtt_journal_integer(builder, store->next_id, 8U);
        store->phase = MQTT_SERVER_SNAPSHOT_OVER;
    }
    if (kind != (uint8_t)MQTT_STORE_RECORD_END) {
        store->snapshot_records += 1U;
    }
    snapshot_settle(server);
    return kind;
}

/**
 * @brief Do what mqtt_server_snapshot_begin does, for arguments known to be
 * valid.
 * @param[in,out] server The server.
 * @param[in] now_ms The time.
 * @param[out] snapshot_header Where the MQTT_STORE_HEADER_SIZE bytes of the
 * snapshot's header go.
 * @param[out] journal_header Where the MQTT_STORE_HEADER_SIZE bytes of the
 * journal segment's header go.
 * @param[out] new_segment true if a new journal segment starts.
 * @return MQTT_OK; MQTT_INVALID_ARGUMENT if the server isn't set up or keeps
 * nothing; or MQTT_INVALID_STATE if a group is still open or a snapshot is
 * under way.
 */
static mqtt_status server_snapshot_begin_body(mqtt_server *server,
                                              uint64_t now_ms,
                                              uint8_t *snapshot_header,
                                              uint8_t *journal_header,
                                              bool *new_segment)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_INVALID_ARGUMENT;

    *new_segment = false;
    if (!mqtt_server_ready(server) ||
        (server->store.state == MQTT_SERVER_STORE_OFF)) {
        /* There is no journal. */
    } else if (server->store.open ||
               (server->store.phase != MQTT_SERVER_SNAPSHOT_IDLE)) {
        status = MQTT_INVALID_STATE;
    } else {
        /** @brief The journal. */
        mqtt_server_store *store = &server->store;
        /** @brief The fields of the headers. */
        mqtt_store_header header;

        server->now_ms = now_ms;
        *new_segment = store->snapshot_whole ||
                       (store->state == MQTT_SERVER_STORE_BROKEN);
        if (*new_segment) {
            store->recovering = (store->state == MQTT_SERVER_STORE_BROKEN);
            store->generation += 1U;
            store->segment_base = store->closed;
            store->segment_start_ms = now_ms;
            store->journal_bytes = 0U;
        }
        store->state = MQTT_SERVER_STORE_RUNNING;
        store->snapshot_whole = false;
        header.generation = store->generation;
        header.base = store->segment_base;
        header.kind = (uint8_t)MQTT_STORE_SERVER_SNAPSHOT;
        header.major = (uint8_t)MQTT_STORE_FORMAT_MAJOR;
        header.minor = (uint8_t)MQTT_STORE_FORMAT_MINOR;
        header.flags = 0U;
        mqtt_store_header_encode(&header, snapshot_header);
        header.kind = (uint8_t)MQTT_STORE_SERVER_JOURNAL;
        header.flags = store->recovering ? (uint8_t)MQTT_STORE_FLAG_RECOVERY :
                                           0U;
        mqtt_store_header_encode(&header, journal_header);
        mqtt_snapshot_prepare(server);
        status = MQTT_OK;
    }
    return status;
}

mqtt_status mqtt_server_snapshot_begin(mqtt_server *server, uint64_t now_ms,
                                       uint8_t *snapshot_header,
                                       uint8_t *journal_header,
                                       bool *new_segment)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if ((snapshot_header != NULL) && (journal_header != NULL) &&
        (new_segment != NULL)) {
        answer = server_snapshot_begin_body(server, now_ms, snapshot_header,
                                            journal_header, new_segment);
    }
    return answer;
}

/**
 * @brief Do what mqtt_server_snapshot_next does, for arguments known to be
 * valid.
 * @param[in,out] server The server.
 * @param[out] space Where the record's fixed fields and a Topic Filter go,
 * valid until the next call.
 * @param[in] size Number of bytes in space, at least
 * MQTT_SERVER_SNAPSHOT_SPACE.
 * @param[out] record The record. Its runs are valid until the next call.
 * @param[out] between true if the next call starts a new message or Session.
 * @return MQTT_OK with a record; MQTT_NOT_FOUND once END has been given;
 * MQTT_BUFFER_TOO_SMALL if the record doesn't fit in space, when the snapshot
 * has to be aborted; or MQTT_INVALID_STATE if no snapshot is under way.
 */
static mqtt_status server_snapshot_next_body(mqtt_server *server,
                                             uint8_t *space, uint32_t size,
                                             mqtt_store_record *record,
                                             bool *between)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_INVALID_STATE;

    *between = true;
    if (!mqtt_server_ready(server) ||
        (server->store.phase == MQTT_SERVER_SNAPSHOT_IDLE)) {
        /* No snapshot is under way. */
    } else if (server->store.phase == MQTT_SERVER_SNAPSHOT_OVER) {
        status = MQTT_NOT_FOUND;
    } else if (size < MQTT_SERVER_SNAPSHOT_SPACE) {
        status = MQTT_BUFFER_TOO_SMALL;
    } else {
        /** @brief The journal. */
        mqtt_server_store *store = &server->store;
        /** @brief The record. */
        mqtt_journal_builder builder;
        /** @brief Its kind. */
        uint8_t kind = 0U;

        mqtt_journal_start(&builder, space, size, store->snapshot_runs);
        kind = snapshot_record(server, &builder);
        status = mqtt_journal_finish(&builder, kind) ? MQTT_OK :
                                                       MQTT_BUFFER_TOO_SMALL;
        record->runs = store->snapshot_runs;
        record->run_count = builder.count;
        record->length = builder.length;
        record->sequence = store->segment_base;
        record->durability = MQTT_STORE_SYNCED;
        record->kind = kind;
        *between = (store->phase != MQTT_SERVER_SNAPSHOT_SESSIONS) ||
                   (store->snapshot_step == SNAPSHOT_SESSION);
    }
    return status;
}

mqtt_status mqtt_server_snapshot_next(mqtt_server *server, uint8_t *space,
                                      uint32_t size, mqtt_store_record *record,
                                      bool *between)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if ((space != NULL) && (record != NULL) && (between != NULL)) {
        answer = server_snapshot_next_body(server, space, size, record,
                                           between);
    }
    return answer;
}

void mqtt_server_snapshot_done(mqtt_server *server)
{
    if (mqtt_server_ready(server) &&
        (server->store.phase == MQTT_SERVER_SNAPSHOT_OVER)) {
        server->store.phase = MQTT_SERVER_SNAPSHOT_IDLE;
        server->store.snapshot_whole = true;
        if (server->store.recovering) {
            /* The answers held over the gap wait for the group after it,
             * which the next commit closes even with nothing in it. */
            server->store.recovering = false;
            server->store.open = true;
            server->store.group_durability = server->config.store_sessions;
            mqtt_journal_release(server);
        }
    }
}

void mqtt_server_snapshot_abort(mqtt_server *server)
{
    if (mqtt_server_ready(server)) {
        server->store.phase = MQTT_SERVER_SNAPSHOT_IDLE;
    }
}

bool mqtt_server_snapshot_due(const mqtt_server *server)
{
    /** @brief The answer. */
    bool due = false;

    if (mqtt_server_ready(server) &&
        (server->store.phase == MQTT_SERVER_SNAPSHOT_IDLE)) {
        /** @brief The journal. */
        const mqtt_server_store *store = &server->store;

        due = (store->state == MQTT_SERVER_STORE_FRESH) ||
              (store->state == MQTT_SERVER_STORE_BROKEN) ||
              ((store->state == MQTT_SERVER_STORE_RUNNING) &&
               ((!store->snapshot_whole) ||
                (store->journal_bytes >= server->config.store_snapshot_bytes)));
    }
    return due;
}
