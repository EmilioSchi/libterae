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
 * @file base_server_store.c
 * @brief The MQTT Server: the journal of what outlasts a restart, and the
 * answers that wait for it.
 *
 * Each change to a Session the store holds is written as a record, through
 * store_append, at the moment it is made. The fixed fields of a record are
 * gathered in a few bytes of its own, and its strings and its message are
 * handed over where they already are, so a Payload is never copied.
 *
 * An answer that tells a Client something was kept goes in the connection's
 * ring at once, but no byte from it on is sent until the group of records
 * written before it is stored. MQTT 3.1 asks for a QoS 1 or QoS 2 message to
 * be logged before its PUBACK or PUBREC, and this extends that to every answer
 * a Client acts on: CONNACK, PUBACK, PUBREC, PUBREL, PUBCOMP, SUBACK and
 * UNSUBACK, and a QoS 2 PUBLISH, whose Packet Identifier has to be kept. A
 * loop closes the group of a round with mqtt_server_store_commit, so one sync
 * covers everything that came in the round.
 *
 * [V31] MQTT V3.1 Protocol Specification.
 * @li Cited: 2.2, 3.3, 4.1.
 * @li Source: public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015.
 * @li Cited: 3.1.2.4, 4.1, 4.3.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 3.1.2.5, p.32. 4.1, p.92-93. 4.3, p.94-96.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base_qos.h"
#include "base_server_internal.h"
#include "base_type.h"

/** @brief Half the values of a 32-bit number, for comparing sequences. */
#define JOURNAL_HALF 0x80000000U

/**
 * @brief Add a Session's Client Identifier.
 * @param[in,out] builder The record.
 * @param[in] server The server.
 * @param[in] session The Session.
 */
static void journal_identifier(mqtt_journal_builder *builder,
                               const mqtt_server *server, uint32_t session);

/**
 * @brief Finish a record and hand it to store_append. A refusal breaks the
 * journal.
 * @param[in,out] server The server.
 * @param[in,out] builder The record.
 * @param[in] kind The kind of record.
 * @param[in] durability What it has to outlast.
 * @return true if it was written.
 */
static bool journal_emit(mqtt_server *server, mqtt_journal_builder *builder,
                         uint8_t kind, mqtt_store_durability durability);

/**
 * @brief Break the journal: nothing more is written, and every answer waits
 * until a snapshot of a new generation is whole.
 * @param[in,out] server The server.
 */
static void journal_break(mqtt_server *server);

/**
 * @brief Find whether records are being written.
 * @param[in] server The server.
 * @return true while the journal runs.
 */
static bool journal_running(const mqtt_server *server);

/**
 * @brief Find whether the store holds a Session.
 * @param[in] server The server.
 * @param[in] session The Session.
 * @return true if records about it are written.
 */
static bool journal_kept(const mqtt_server *server, uint32_t session);

/**
 * @brief Find the time left before a moment.
 * @param[in] due The moment, or zero for none.
 * @param[in] now_ms The time.
 * @return Milliseconds left, or zero if it has passed or there is none.
 */
static uint64_t journal_left(uint64_t due, uint64_t now_ms);

/**
 * @brief Find the more demanding of two durabilities.
 * @param[in] first One.
 * @param[in] second The other.
 * @return The one that outlasts more.
 */
static mqtt_store_durability journal_most(mqtt_store_durability first,
                                          mqtt_store_durability second);

/**
 * @brief Make sure the journal segment holds a message before a record names
 * it, writing it if the segment doesn't yet.
 * @param[in,out] server The server.
 * @param[in] message The message.
 * @param[in] durability What the record that needs it has to outlast.
 * @return The message's store id, or zero if it could not be written.
 */
static uint64_t journal_message(mqtt_server *server, uint32_t message,
                                mqtt_store_durability durability);

/**
 * @brief Write a record that names a Session and nothing else.
 * @param[in,out] server The server.
 * @param[in] session The Session.
 * @param[in] kind The kind of record.
 */
static void journal_session_only(mqtt_server *server, uint32_t session,
                                 uint8_t kind);

/**
 * @brief Write a Session's Will Message.
 * @param[in,out] server The server.
 * @param[in] session The Session, which has one.
 */
static void journal_will(mqtt_server *server, uint32_t session);

/**
 * @brief Write a record that names a store id and nothing else.
 * @param[in,out] server The server.
 * @param[in] store_id The store id.
 * @param[in] kind The kind of record.
 * @param[in] durability What it has to outlast.
 */
static void journal_id_only(mqtt_server *server, uint64_t store_id,
                            uint8_t kind, mqtt_store_durability durability);

/**
 * @brief Find whether a group is stored, from the low 32 bits of its number.
 * @param[in] server The server.
 * @param[in] sequence The low 32 bits.
 * @return true if it is, and no gap in the journal still holds it back.
 */
static bool journal_done(const mqtt_server *server, uint32_t sequence);

/**
 * @brief Let go every connection whose answers waited for groups now stored,
 * and take off the list those that hold nothing.
 * @param[in,out] server The server.
 */
static void journal_release(mqtt_server *server);

void mqtt_journal_session_fields(mqtt_journal_builder *builder,
                                 const mqtt_server *server, uint32_t session)
{
    /** @brief The Session. */
    const mqtt_session *entry = &server->storage.sessions[session];
    /** @brief Its flags. */
    uint32_t flags = 0U;

    if (entry->connection == MQTT_SERVER_NONE) {
        flags |= MQTT_STORE_SESSION_AWAY;
    }
    if (entry->will_due_ms != 0U) {
        flags |= MQTT_STORE_SESSION_WILL_TIMER;
    }
    mqtt_journal_integer(builder, (uint64_t)entry->version, 1U);
    mqtt_journal_integer(builder, (uint64_t)flags, 1U);
    mqtt_journal_integer(builder, (uint64_t)entry->expiry_s, 4U);
    mqtt_journal_integer(builder,
                         journal_left(entry->expiry_due_ms, server->now_ms),
                         8U);
    mqtt_journal_integer(builder,
                         journal_left(entry->will_due_ms, server->now_ms), 8U);
    journal_identifier(builder, server, session);
}

void mqtt_journal_will_fields(mqtt_journal_builder *builder,
                              const mqtt_server *server, uint32_t session)
{
    /** @brief The Session. */
    const mqtt_session *entry = &server->storage.sessions[session];

    mqtt_journal_integer(
        builder, server->storage.messages[entry->will_message].store_id, 8U);
    mqtt_journal_integer(builder, (uint64_t)entry->will_delay_s, 4U);
    mqtt_journal_integer(builder,
                         entry->will_has_expiry ? MQTT_STORE_WILL_EXPIRY : 0U,
                         1U);
    mqtt_journal_integer(builder, (uint64_t)entry->will_expiry_s, 4U);
    journal_identifier(builder, server, session);
}

void mqtt_journal_message_fields(mqtt_journal_builder *builder,
                                 const mqtt_server *server, uint32_t message)
{
    /** @brief The message. */
    const mqtt_message *entry = &server->storage.messages[message];
    /** @brief Its flags. */
    uint32_t flags = entry->retain ? MQTT_STORE_RETAIN : 0U;

    if (entry->expiry_ms != 0U) {
        flags |= MQTT_STORE_MESSAGE_EXPIRES;
    }
    mqtt_journal_integer(builder, entry->store_id, 8U);
    mqtt_journal_integer(builder, (uint64_t)entry->qos, 1U);
    mqtt_journal_integer(builder, (uint64_t)flags, 1U);
    mqtt_journal_integer(builder,
                         journal_left(entry->expiry_ms, server->now_ms), 8U);
    mqtt_journal_integer(builder, (uint64_t)entry->topic_length, 2U);
    mqtt_journal_integer(builder, (uint64_t)entry->properties_length, 4U);
    mqtt_journal_integer(builder, (uint64_t)entry->payload_length, 4U);
    mqtt_journal_span(builder,
                      mqtt_page_arena_bytes(&server->arena, entry->block),
                      (uint32_t)entry->topic_length + entry->properties_length +
                          entry->payload_length);
}

void mqtt_journal_delivery_fields(mqtt_journal_builder *builder,
                                  const mqtt_server *server, uint32_t session,
                                  uint32_t delivery)
{
    /** @brief The delivery. */
    const mqtt_delivery *entry = &server->storage.deliveries[delivery];

    mqtt_journal_integer(builder, entry->store_id, 8U);
    mqtt_journal_integer(builder,
                         (entry->message == MQTT_SERVER_NONE) ?
                             0U :
                             server->storage.messages[entry->message].store_id,
                         8U);
    mqtt_journal_integer(builder, (uint64_t)entry->qos, 1U);
    mqtt_journal_integer(builder, entry->retain ? MQTT_STORE_RETAIN : 0U, 1U);
    mqtt_journal_integer(builder, (uint64_t)entry->subscription_identifier,
                         4U);
    journal_identifier(builder, server, session);
}

void mqtt_journal_subscribe_fields(mqtt_journal_builder *builder,
                                   const mqtt_server *server, uint32_t session,
                                   uint8_t options, uint32_t identifier,
                                   bool shared)
{
    mqtt_journal_integer(builder, (uint64_t)options, 1U);
    mqtt_journal_integer(builder, shared ? MQTT_STORE_SHARED : 0U, 1U);
    mqtt_journal_integer(builder, (uint64_t)identifier, 4U);
    journal_identifier(builder, server, session);
}

static void journal_identifier(mqtt_journal_builder *builder,
                               const mqtt_server *server, uint32_t session)
{
    /** @brief The Client Identifier. */
    const mqtt_utf8 identifier = mqtt_session_identifier(server, session);

    mqtt_journal_string(builder, &identifier);
}

static bool journal_emit(mqtt_server *server, mqtt_journal_builder *builder,
                         uint8_t kind, mqtt_store_durability durability)
{
    /** @brief The journal. */
    mqtt_server_store *store = &server->store;
    /** @brief true once it is written. */
    bool written = false;

    if (mqtt_journal_finish(builder, kind)) {
        /** @brief What store_append is given. */
        mqtt_store_record record;

        record.runs = builder->runs;
        record.run_count = builder->count;
        record.length = builder->length;
        record.sequence = store->closed + 1U;
        record.durability = durability;
        record.kind = kind;
        if (server->config.store_append(server->config.user_tag, &record) ==
            MQTT_OK) {
            written = true;
            server->statistics.records_written += 1U;
            store->journal_bytes += (uint64_t)builder->length;
            store->group_durability =
                journal_most(store->group_durability, durability);
            if (kind != (uint8_t)MQTT_STORE_RECORD_COMMIT) {
                store->open = true;
                mqtt_snapshot_changed(server);
            }
        } else {
            server->statistics.records_refused += 1U;
            journal_break(server);
        }
    }
    return written;
}

static void journal_break(mqtt_server *server)
{
    server->store.state = MQTT_SERVER_STORE_BROKEN;
    /* What the open group held is lost: the snapshot that mends the journal
     * holds it instead. */
    server->store.open = false;
    server->store.group_durability = MQTT_STORE_MEMORY;
}

static bool journal_running(const mqtt_server *server)
{
    return server->store.state == MQTT_SERVER_STORE_RUNNING;
}

static bool journal_kept(const mqtt_server *server, uint32_t session)
{
    return journal_running(server) &&
           server->storage.sessions[session].stored;
}

static uint64_t journal_left(uint64_t due, uint64_t now_ms)
{
    return (due > now_ms) ? (due - now_ms) : 0U;
}

static mqtt_store_durability journal_most(mqtt_store_durability first,
                                          mqtt_store_durability second)
{
    /** @brief The answer. */
    mqtt_store_durability most = MQTT_STORE_MEMORY;

    if ((first == MQTT_STORE_SYNCED) || (second == MQTT_STORE_SYNCED)) {
        most = MQTT_STORE_SYNCED;
    } else if ((first == MQTT_STORE_WRITTEN) ||
               (second == MQTT_STORE_WRITTEN)) {
        most = MQTT_STORE_WRITTEN;
    } else {
        /* Both are kept in memory only. */
    }
    return most;
}

static uint64_t journal_message(mqtt_server *server, uint32_t message,
                                mqtt_store_durability durability)
{
    /** @brief The message. */
    mqtt_message *entry = &server->storage.messages[message];
    /** @brief The low 32 bits of the segment's generation. */
    const uint32_t generation =
        (uint32_t)(server->store.generation & 0xFFFFFFFFU);
    /** @brief The store id to hand back. */
    uint64_t store_id = 0U;

    if (!journal_running(server)) {
        /* Nothing is written. */
    } else if ((entry->store_id != 0U) &&
               (entry->store_generation == generation)) {
        store_id = entry->store_id;
    } else {
        /** @brief The fixed fields. */
        uint8_t bytes[MQTT_JOURNAL_FIELDS];
        /** @brief The runs. */
        mqtt_run runs[MQTT_SERVER_STORE_RUNS];
        /** @brief The record. */
        mqtt_journal_builder builder;
        /** @brief true if the id is new, so the counter moves on if written. */
        const bool fresh = (entry->store_id == 0U);

        if (fresh) {
            entry->store_id = server->store.next_id;
        }
        mqtt_journal_start(&builder, bytes, MQTT_JOURNAL_FIELDS, runs);
        mqtt_journal_message_fields(&builder, server, message);
        if (journal_emit(server, &builder, (uint8_t)MQTT_STORE_RECORD_MESSAGE,
                         durability)) {
            entry->store_generation = generation;
            store_id = entry->store_id;
            if (fresh) {
                server->store.next_id += 1U;
            }
        } else if (fresh) {
            entry->store_id = 0U;
        } else {
            /* It keeps the name it had. */
        }
    }
    return store_id;
}

static void journal_session_only(mqtt_server *server, uint32_t session,
                                 uint8_t kind)
{
    /** @brief The fixed fields. */
    uint8_t bytes[MQTT_JOURNAL_FIELDS];
    /** @brief The runs. */
    mqtt_run runs[MQTT_SERVER_STORE_RUNS];
    /** @brief The record. */
    mqtt_journal_builder builder;

    mqtt_journal_start(&builder, bytes, MQTT_JOURNAL_FIELDS, runs);
    journal_identifier(&builder, server, session);
    (void)journal_emit(server, &builder, kind, server->config.store_sessions);
}

static void journal_will(mqtt_server *server, uint32_t session)
{
    /** @brief The Session. */
    const mqtt_session *entry = &server->storage.sessions[session];

    if (journal_message(server, entry->will_message,
                        server->config.store_sessions) != 0U) {
        /** @brief The fixed fields. */
        uint8_t bytes[MQTT_JOURNAL_FIELDS];
        /** @brief The runs. */
        mqtt_run runs[MQTT_SERVER_STORE_RUNS];
        /** @brief The record. */
        mqtt_journal_builder builder;

        mqtt_journal_start(&builder, bytes, MQTT_JOURNAL_FIELDS, runs);
        mqtt_journal_will_fields(&builder, server, session);
        (void)journal_emit(server, &builder, (uint8_t)MQTT_STORE_RECORD_WILL,
                           server->config.store_sessions);
    }
}

static void journal_id_only(mqtt_server *server, uint64_t store_id,
                            uint8_t kind, mqtt_store_durability durability)
{
    /** @brief The fixed fields. */
    uint8_t bytes[MQTT_JOURNAL_FIELDS];
    /** @brief The runs. */
    mqtt_run runs[MQTT_SERVER_STORE_RUNS];
    /** @brief The record. */
    mqtt_journal_builder builder;

    mqtt_journal_start(&builder, bytes, MQTT_JOURNAL_FIELDS, runs);
    mqtt_journal_integer(&builder, store_id, 8U);
    (void)journal_emit(server, &builder, kind, durability);
}

static bool journal_done(const mqtt_server *server, uint32_t sequence)
{
    /** @brief The low 32 bits of the last group stored. */
    const uint32_t stored = (uint32_t)(server->store.stored & 0xFFFFFFFFU);

    return journal_running(server) && (!server->store.recovering) &&
           ((stored - sequence) < JOURNAL_HALF);
}

static void journal_release(mqtt_server *server)
{
    /** @brief The connection before the one being looked at. */
    uint32_t previous = MQTT_SERVER_NONE;
    /** @brief The connection being looked at. */
    uint32_t index = server->store.held_head;

    while (index != MQTT_SERVER_NONE) {
        /** @brief The connection. */
        mqtt_connection *entry = &server->storage.connections[index];
        /** @brief The connection after it. */
        const uint32_t next = entry->next_held;
        /** @brief true if it still waits. */
        const bool waits = (entry->state != MQTT_CONNECTION_FREE) &&
                           (entry->hold_bytes != MQTT_SERVER_NONE) &&
                           (!journal_done(server, entry->hold_sequence));

        if (waits) {
            previous = index;
        } else {
            if ((entry->state != MQTT_CONNECTION_FREE) &&
                (entry->hold_bytes != MQTT_SERVER_NONE)) {
                entry->hold_bytes = MQTT_SERVER_NONE;
                mqtt_route_mark_pending(server, index);
            }
            if (previous == MQTT_SERVER_NONE) {
                server->store.held_head = next;
            } else {
                server->storage.connections[previous].next_held = next;
            }
            entry->held_listed = false;
            entry->next_held = MQTT_SERVER_NONE;
        }
        index = next;
    }
}

bool mqtt_journal_config_valid(const mqtt_server_config *config)
{
    /** @brief The two durabilities. */
    const mqtt_store_durability both[2] = {config->store_sessions,
                                           config->store_messages};
    /** @brief The answer. */
    bool valid = true;
    /** @brief Index of the durability being checked. */
    uint32_t index = 0U;

    while (index < 2U) {
        if ((both[index] != MQTT_STORE_MEMORY) &&
            (both[index] != MQTT_STORE_WRITTEN) &&
            (both[index] != MQTT_STORE_SYNCED)) {
            valid = false;
        }
        index += 1U;
    }
    return valid;
}

void mqtt_journal_init(mqtt_server *server)
{
    /** @brief The journal. */
    mqtt_server_store *store = &server->store;

    store->state = ((server->config.store_append != NULL) &&
                    (server->config.store_sessions != MQTT_STORE_MEMORY)) ?
                       MQTT_SERVER_STORE_FRESH :
                       MQTT_SERVER_STORE_OFF;
    store->generation = 0U;
    store->segment_base = 0U;
    store->closed = 0U;
    store->stored = 0U;
    store->segment_start_ms = 0U;
    store->next_id = 1U;
    store->journal_bytes = 0U;
    store->held_head = MQTT_SERVER_NONE;
    store->group_durability = MQTT_STORE_MEMORY;
    store->open = false;
    store->snapshot_whole = true;
    store->recovering = false;
    store->phase = MQTT_SERVER_SNAPSHOT_IDLE;
    store->walk_end = 1U;
    store->snapshot_records = 0U;
    store->snapshot_index = 0U;
    store->snapshot_item = MQTT_SERVER_NONE;
    store->snapshot_step = 0U;
}

void mqtt_journal_session(mqtt_server *server, uint32_t session)
{
    /** @brief The Session. */
    mqtt_session *entry = &server->storage.sessions[session];

    if (!journal_running(server)) {
        /* Nothing is written. */
    } else if (entry->expiry_s != 0U) {
        /** @brief The fixed fields. */
        uint8_t bytes[MQTT_JOURNAL_FIELDS];
        /** @brief The runs. */
        mqtt_run runs[MQTT_SERVER_STORE_RUNS];
        /** @brief The record. */
        mqtt_journal_builder builder;

        mqtt_journal_start(&builder, bytes, MQTT_JOURNAL_FIELDS, runs);
        mqtt_journal_session_fields(&builder, server, session);
        if (journal_emit(server, &builder, (uint8_t)MQTT_STORE_RECORD_SESSION,
                         server->config.store_sessions)) {
            entry->stored = true;
        }
        if (entry->stored && (entry->will_message != MQTT_SERVER_NONE)) {
            journal_will(server, session);
        }
    } else if (entry->stored) {
        mqtt_journal_session_remove(server, session);
    } else {
        /* It ends with its connection, so the store never holds it. */
    }
}

void mqtt_journal_session_remove(mqtt_server *server, uint32_t session)
{
    if (journal_kept(server, session)) {
        journal_session_only(server, session,
                             (uint8_t)MQTT_STORE_RECORD_SESSION_REMOVE);
    }
    server->storage.sessions[session].stored = false;
}

void mqtt_journal_subscribe(mqtt_server *server, uint32_t session,
                            const mqtt_utf8 *filter, uint8_t options,
                            uint32_t identifier, bool shared)
{
    if (journal_kept(server, session)) {
        /** @brief The fixed fields. */
        uint8_t bytes[MQTT_JOURNAL_FIELDS];
        /** @brief The runs. */
        mqtt_run runs[MQTT_SERVER_STORE_RUNS];
        /** @brief The record. */
        mqtt_journal_builder builder;

        mqtt_journal_start(&builder, bytes, MQTT_JOURNAL_FIELDS, runs);
        mqtt_journal_subscribe_fields(&builder, server, session, options,
                                      identifier, shared);
        mqtt_journal_string(&builder, filter);
        (void)journal_emit(server, &builder,
                           (uint8_t)MQTT_STORE_RECORD_SUBSCRIBE,
                           server->config.store_sessions);
    }
}

void mqtt_journal_unsubscribe(mqtt_server *server, uint32_t session,
                              const mqtt_utf8 *filter, bool shared)
{
    if (journal_kept(server, session)) {
        /** @brief The fixed fields. */
        uint8_t bytes[MQTT_JOURNAL_FIELDS];
        /** @brief The runs. */
        mqtt_run runs[MQTT_SERVER_STORE_RUNS];
        /** @brief The record. */
        mqtt_journal_builder builder;

        mqtt_journal_start(&builder, bytes, MQTT_JOURNAL_FIELDS, runs);
        mqtt_journal_integer(&builder, shared ? MQTT_STORE_SHARED : 0U, 1U);
        journal_identifier(&builder, server, session);
        mqtt_journal_string(&builder, filter);
        (void)journal_emit(server, &builder,
                           (uint8_t)MQTT_STORE_RECORD_UNSUBSCRIBE,
                           server->config.store_sessions);
    }
}

void mqtt_journal_will_remove(mqtt_server *server, uint32_t session)
{
    if (journal_kept(server, session)) {
        journal_session_only(server, session,
                             (uint8_t)MQTT_STORE_RECORD_WILL_REMOVE);
    }
}

void mqtt_journal_message_gone(mqtt_server *server, uint32_t message)
{
    /** @brief The message. */
    mqtt_message *entry = &server->storage.messages[message];

    if ((entry->store_id != 0U) && journal_running(server)) {
        journal_id_only(server, entry->store_id,
                        (uint8_t)MQTT_STORE_RECORD_MESSAGE_GONE,
                        journal_most(server->config.store_sessions,
                                     server->config.store_messages));
    }
    entry->store_id = 0U;
}

void mqtt_journal_message_expiry(mqtt_server *server, uint32_t message)
{
    /** @brief The message. */
    const mqtt_message *entry = &server->storage.messages[message];
    /** @brief The low 32 bits of the segment's generation. */
    const uint32_t generation =
        (uint32_t)(server->store.generation & 0xFFFFFFFFU);

    /* A message this segment hasn't written yet is written whole, with its
     * expiry, the next time a record names it. */
    if ((entry->store_id != 0U) && (entry->store_generation == generation) &&
        journal_running(server)) {
        /** @brief The fixed fields. */
        uint8_t bytes[MQTT_JOURNAL_FIELDS];
        /** @brief The runs. */
        mqtt_run runs[MQTT_SERVER_STORE_RUNS];
        /** @brief The record. */
        mqtt_journal_builder builder;

        mqtt_journal_start(&builder, bytes, MQTT_JOURNAL_FIELDS, runs);
        mqtt_journal_integer(&builder, entry->store_id, 8U);
        mqtt_journal_integer(&builder,
                             journal_left(entry->expiry_ms, server->now_ms),
                             8U);
        (void)journal_emit(server, &builder,
                           (uint8_t)MQTT_STORE_RECORD_MESSAGE_EXPIRY,
                           journal_most(server->config.store_sessions,
                                        server->config.store_messages));
    }
}

void mqtt_journal_retained(mqtt_server *server, uint32_t message)
{
    if (journal_running(server) &&
        (server->config.store_messages != MQTT_STORE_MEMORY)) {
        /** @brief The message's store id. */
        const uint64_t store_id =
            journal_message(server, message, server->config.store_messages);

        if (store_id != 0U) {
            journal_id_only(server, store_id,
                            (uint8_t)MQTT_STORE_RECORD_RETAINED,
                            server->config.store_messages);
        }
    }
}

void mqtt_journal_retained_remove(mqtt_server *server,
                                  const mqtt_utf8 *topic_name)
{
    if (journal_running(server) &&
        (server->config.store_messages != MQTT_STORE_MEMORY)) {
        /** @brief The fixed fields. */
        uint8_t bytes[MQTT_JOURNAL_FIELDS];
        /** @brief The runs. */
        mqtt_run runs[MQTT_SERVER_STORE_RUNS];
        /** @brief The record. */
        mqtt_journal_builder builder;

        mqtt_journal_start(&builder, bytes, MQTT_JOURNAL_FIELDS, runs);
        mqtt_journal_string(&builder, topic_name);
        (void)journal_emit(server, &builder,
                           (uint8_t)MQTT_STORE_RECORD_RETAINED_REMOVE,
                           server->config.store_messages);
    }
}

void mqtt_journal_delivery(mqtt_server *server, uint32_t session,
                           uint32_t delivery)
{
    /** @brief The delivery. */
    mqtt_delivery *entry = &server->storage.deliveries[delivery];
    /** @brief The message's store id, once the segment holds it. */
    uint64_t message = 0U;

    if (journal_kept(server, session) && (entry->qos != MQTT_QOS_0) &&
        (server->config.store_messages != MQTT_STORE_MEMORY)) {
        message = journal_message(server, entry->message,
                                  server->config.store_messages);
    }
    if (message != 0U) {
        /** @brief The fixed fields. */
        uint8_t bytes[MQTT_JOURNAL_FIELDS];
        /** @brief The runs. */
        mqtt_run runs[MQTT_SERVER_STORE_RUNS];
        /** @brief The record. */
        mqtt_journal_builder builder;

        entry->store_id = server->store.next_id;
        mqtt_journal_start(&builder, bytes, MQTT_JOURNAL_FIELDS, runs);
        mqtt_journal_delivery_fields(&builder, server, session, delivery);
        if (journal_emit(server, &builder, (uint8_t)MQTT_STORE_RECORD_DELIVERY,
                         server->config.store_messages)) {
            server->store.next_id += 1U;
        } else {
            entry->store_id = 0U;
        }
    }
}

void mqtt_journal_sent(mqtt_server *server, const mqtt_delivery *delivery)
{
    if ((delivery->store_id != 0U) && journal_running(server)) {
        /** @brief The fixed fields. */
        uint8_t bytes[MQTT_JOURNAL_FIELDS];
        /** @brief The runs. */
        mqtt_run runs[MQTT_SERVER_STORE_RUNS];
        /** @brief The record. */
        mqtt_journal_builder builder;

        mqtt_journal_start(&builder, bytes, MQTT_JOURNAL_FIELDS, runs);
        mqtt_journal_integer(&builder, delivery->store_id, 8U);
        mqtt_journal_integer(&builder, (uint64_t)delivery->packet_identifier,
                             2U);
        (void)journal_emit(server, &builder, (uint8_t)MQTT_STORE_RECORD_SENT,
                           server->config.store_messages);
    }
}

void mqtt_journal_released(mqtt_server *server, uint32_t delivery)
{
    /** @brief The delivery. */
    const mqtt_delivery *entry = &server->storage.deliveries[delivery];

    if ((entry->store_id != 0U) && journal_running(server)) {
        journal_id_only(server, entry->store_id,
                        (uint8_t)MQTT_STORE_RECORD_RELEASED,
                        server->config.store_messages);
    }
}

void mqtt_journal_finished(mqtt_server *server, uint32_t delivery)
{
    /** @brief The delivery. */
    mqtt_delivery *entry = &server->storage.deliveries[delivery];

    if ((entry->store_id != 0U) && journal_running(server)) {
        journal_id_only(server, entry->store_id,
                        (uint8_t)MQTT_STORE_RECORD_FINISHED,
                        server->config.store_messages);
    }
    entry->store_id = 0U;
}

void mqtt_journal_incoming(mqtt_server *server, uint32_t session,
                           uint16_t packet_identifier, bool received)
{
    if (journal_kept(server, session) &&
        (server->config.store_messages != MQTT_STORE_MEMORY)) {
        /** @brief The fixed fields. */
        uint8_t bytes[MQTT_JOURNAL_FIELDS];
        /** @brief The runs. */
        mqtt_run runs[MQTT_SERVER_STORE_RUNS];
        /** @brief The record. */
        mqtt_journal_builder builder;

        mqtt_journal_start(&builder, bytes, MQTT_JOURNAL_FIELDS, runs);
        mqtt_journal_integer(&builder, (uint64_t)packet_identifier, 2U);
        journal_identifier(&builder, server, session);
        (void)journal_emit(server, &builder,
                           received ?
                               (uint8_t)MQTT_STORE_RECORD_INCOMING :
                               (uint8_t)MQTT_STORE_RECORD_INCOMING_REMOVE,
                           server->config.store_messages);
    }
}

void mqtt_journal_hold(mqtt_server *server, uint32_t connection)
{
    /** @brief The journal. */
    mqtt_server_store *store = &server->store;
    /** @brief true if every answer waits for a snapshot to be whole. */
    const bool gap = (store->state == MQTT_SERVER_STORE_BROKEN) ||
                     (journal_running(server) && store->recovering);

    if (gap || (journal_running(server) &&
                (store->open || (store->closed > store->stored)))) {
        /** @brief The connection. */
        mqtt_connection *entry = &server->storage.connections[connection];
        /** @brief The group that has to be stored first. */
        const uint64_t needed = (gap || store->open) ? (store->closed + 1U) :
                                                       store->closed;

        if (entry->hold_bytes == MQTT_SERVER_NONE) {
            entry->hold_bytes = entry->ring.used;
        }
        entry->hold_sequence = (uint32_t)(needed & 0xFFFFFFFFU);
        if (!entry->held_listed) {
            entry->next_held = store->held_head;
            store->held_head = connection;
            entry->held_listed = true;
        }
    }
}

bool mqtt_journal_holds(uint8_t first_byte)
{
    /** @brief The MQTT Control Packet type. */
    const uint32_t type = (uint32_t)first_byte >> 4U;

    return (type == MQTT_CONNACK) || (type == MQTT_PUBACK) ||
           (type == MQTT_PUBREC) || (type == MQTT_PUBREL) ||
           (type == MQTT_PUBCOMP) || (type == MQTT_SUBACK) ||
           (type == MQTT_UNSUBACK);
}

/**
 * @brief Do what mqtt_server_store_commit does, for arguments known to be
 * valid.
 * @param[in,out] server The server.
 * @param[in] now_ms The time.
 * @param[out] sequence The last group closed, which is the one to report.
 * @return MQTT_OK, also when there was nothing to close;
 * MQTT_INVALID_ARGUMENT if the server isn't set up or keeps nothing; or
 * MQTT_INVALID_STATE if the journal is broken, when sequence is the last group
 * closed before it broke.
 */
static mqtt_status server_store_commit_body(mqtt_server *server,
                                            uint64_t now_ms, uint64_t *sequence)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_INVALID_ARGUMENT;

    *sequence = 0U;
    if (mqtt_server_ready(server) &&
        (server->store.state != MQTT_SERVER_STORE_OFF)) {
        /** @brief The journal. */
        mqtt_server_store *store = &server->store;

        server->now_ms = now_ms;
        status = MQTT_OK;
        if (journal_running(server) && store->open) {
            /** @brief The fixed fields. */
            uint8_t bytes[MQTT_JOURNAL_FIELDS];
            /** @brief The runs. */
            mqtt_run runs[MQTT_SERVER_STORE_RUNS];
            /** @brief The record. */
            mqtt_journal_builder builder;

            mqtt_journal_start(&builder, bytes, MQTT_JOURNAL_FIELDS, runs);
            mqtt_journal_integer(
                &builder, journal_left(now_ms, store->segment_start_ms), 8U);
            if (journal_emit(server, &builder,
                             (uint8_t)MQTT_STORE_RECORD_COMMIT,
                             store->group_durability)) {
                store->closed += 1U;
                store->open = false;
                store->group_durability = MQTT_STORE_MEMORY;
            }
        }
        if (store->state == MQTT_SERVER_STORE_BROKEN) {
            status = MQTT_INVALID_STATE;
        }
        *sequence = store->closed;
    }
    return status;
}

mqtt_status mqtt_server_store_commit(mqtt_server *server, uint64_t now_ms,
                                     uint64_t *sequence)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if (sequence != NULL) {
        answer = server_store_commit_body(server, now_ms, sequence);
    }
    return answer;
}

void mqtt_server_stored(mqtt_server *server, uint64_t sequence)
{
    if (mqtt_server_ready(server) && journal_running(server)) {
        /** @brief The last group stored, which can't be one not closed. */
        const uint64_t stored = (sequence < server->store.closed) ?
                                    sequence :
                                    server->store.closed;

        if (stored > server->store.stored) {
            server->store.stored = stored;
        }
        journal_release(server);
    }
}

void mqtt_server_store_failed(mqtt_server *server)
{
    if (mqtt_server_ready(server) && journal_running(server)) {
        journal_break(server);
    }
}

bool mqtt_server_store_open(const mqtt_server *server)
{
    return mqtt_server_ready(server) && journal_running(server) &&
           server->store.open;
}

void mqtt_journal_release(mqtt_server *server)
{
    journal_release(server);
}
