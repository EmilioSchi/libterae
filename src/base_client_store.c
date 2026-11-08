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
 * @file base_client_store.c
 * @brief The MQTT Client: the journal of its Session State, and the answers
 * that wait for it.
 *
 * Each change to what the Client holds is written as a record, through the
 * store's append, at the moment it is made: a message held, sent, released or
 * finished, a QoS 2 Packet Identifier received or released, and with MQTT 3.1
 * a SUBSCRIBE or UNSUBSCRIBE packet waiting for its answer. A message's
 * PUBLISH packet is handed over where it is, in the arena.
 *
 * An answer that tells the Server something was kept goes in the ring at once,
 * but no byte from it on is sent until the records written before it are
 * stored. A QoS 2 PUBLISH waits for its own record, as its Packet Identifier
 * must not be given to another message after a restart; a PUBREL for the
 * message's release, or its PUBLISH would be sent again; a PUBREC for the
 * Packet Identifier received, so that a resend isn't taken for a new message;
 * and a PUBCOMP for its release. A message from the Server is given to the
 * application before its answer is sent, which is how MQTT 3.1 asks for it to
 * be logged before it is acknowledged when the application keeps it.
 *
 * A snapshot is small, as it holds only what this Client's memory holds, so it
 * is taken whole, while the Client isn't called.
 *
 * [V31] MQTT V3.1 Protocol Specification.
 * @li Cited: 3.3, 4.1.
 * @li Source: public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015.
 * @li Cited: 4.1, 4.3.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 4.1, p.92-93. 4.3, p.94-96.
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

/** @brief Number of bytes a record's frame and fixed fields take at most. */
#define CLIENT_JOURNAL_FIELDS MQTT_CLIENT_SNAPSHOT_SPACE

/** @brief The bits of a PUBLISH packet's first byte that hold its QoS. */
#define CLIENT_JOURNAL_QOS_BITS 0x06U

/** @brief Where the QoS starts in a PUBLISH packet's first byte. */
#define CLIENT_JOURNAL_QOS_SHIFT 1U

/**
 * @brief Find whether records are being written.
 * @param[in] client The client.
 * @return true while the journal runs.
 */
static bool client_journal_running(const mqtt_client *client);

/**
 * @brief Find whether records about messages are being written.
 * @param[in] client The client.
 * @return true if they are.
 */
static bool client_journal_keeps_messages(const mqtt_client *client);

/**
 * @brief Find the more demanding of two durabilities.
 * @param[in] first One.
 * @param[in] second The other.
 * @return The one that outlasts more.
 */
static mqtt_store_durability client_journal_most(mqtt_store_durability first,
                                                 mqtt_store_durability second);

/**
 * @brief Stop writing: what the open group held may be lost.
 * @param[in,out] client The client.
 */
static void client_journal_break(mqtt_client *client);

/**
 * @brief Finish a record and hand it to the store. A refusal breaks the
 * journal.
 * @param[in,out] client The client.
 * @param[in,out] builder The record.
 * @param[in] kind The kind of record.
 * @param[in] durability What it has to outlast.
 * @return true if it was written.
 */
static bool client_journal_emit(mqtt_client *client,
                                mqtt_journal_builder *builder, uint8_t kind,
                                mqtt_store_durability durability);

/**
 * @brief Add the fields of a CLIENT_SESSION record.
 * @param[in,out] builder The record.
 * @param[in] client The client.
 */
static void client_journal_session_fields(mqtt_journal_builder *builder,
                                          const mqtt_client *client);

/**
 * @brief Add the fields of a CLIENT_MESSAGE record.
 * @param[in,out] builder The record.
 * @param[in] client The client.
 * @param[in] entry The message.
 * @param[in] stage The stage to write.
 */
static void client_journal_message_fields(mqtt_journal_builder *builder,
                                          const mqtt_client *client,
                                          const mqtt_client_message *entry,
                                          mqtt_client_message_state stage);

/**
 * @brief Add the fields of a CLIENT_PENDING record.
 * @param[in,out] builder The record.
 * @param[in] client The client.
 * @param[in] entry The packet waiting for its answer.
 */
static void client_journal_pending_fields(mqtt_journal_builder *builder,
                                          const mqtt_client *client,
                                          const mqtt_client_pending *entry);

/**
 * @brief Make what goes into the ring from now on wait for the store, if
 * records are not stored yet.
 * @param[in,out] client The client.
 * @param[in] direct true if a packet sent straight from the arena goes next.
 */
static void client_journal_raise(mqtt_client *client, bool direct);

/**
 * @brief Find whether a packet tells the Server something was kept, so that
 * it waits for the records before it.
 * @param[in] client The client.
 * @param[in] first_byte The packet's first byte.
 * @return true if it does.
 */
static bool client_journal_holds(const mqtt_client *client, uint8_t first_byte);

/**
 * @brief Let the answers go whose records are stored.
 * @param[in,out] client The client.
 */
static void client_journal_release(mqtt_client *client);

/**
 * @brief Give the next record of the snapshot, from the stage the walk is at.
 * @param[in,out] client The client.
 * @param[in,out] builder The record.
 * @return Its kind, or zero if the stage has nothing more and the walk moved
 * on.
 */
static uint8_t client_snapshot_step(mqtt_client *client,
                                    mqtt_journal_builder *builder);

/**
 * @brief Give the next message held for the Server, if any is left.
 * @param[in,out] client The client.
 * @param[in,out] builder The record.
 * @return Its kind, or zero if none is left.
 */
static uint8_t client_snapshot_message(mqtt_client *client,
                                       mqtt_journal_builder *builder);

/**
 * @brief Give the next SUBSCRIBE or UNSUBSCRIBE packet waiting, if any is
 * left.
 * @param[in,out] client The client.
 * @param[in,out] builder The record.
 * @return Its kind, or zero if none is left.
 */
static uint8_t client_snapshot_pending(mqtt_client *client,
                                       mqtt_journal_builder *builder);

void mqtt_client_journal_init(mqtt_client *client)
{
    /** @brief The journal. */
    mqtt_client_store *store = &client->store;

    store->config.append = NULL;
    store->config.user_tag = 0U;
    store->config.sessions = MQTT_STORE_MEMORY;
    store->config.messages = MQTT_STORE_MEMORY;
    store->config.snapshot_bytes = 0U;
    store->state = MQTT_CLIENT_STORE_OFF;
    store->generation = 0U;
    store->segment_base = 0U;
    store->closed = 0U;
    store->stored = 0U;
    store->segment_start_ms = 0U;
    store->journal_bytes = 0U;
    store->group_durability = MQTT_STORE_MEMORY;
    store->open = false;
    store->snapshot_whole = true;
    store->recovering = false;
    store->phase = MQTT_CLIENT_SNAPSHOT_IDLE;
    store->snapshot_changed = false;
    store->snapshot_item = MQTT_CLIENT_NONE;
    store->snapshot_records = 0U;
    store->hold_bytes = MQTT_CLIENT_NONE;
    store->hold_sequence = 0U;
    store->hold_direct = false;
    store->identifier_block = MQTT_ARENA_NONE;
    store->identifier_length = 0U;
    store->session_due = false;
    store->written_version = MQTT_VERSION_311;
    store->written_exists = false;
}

mqtt_status mqtt_client_store_init(mqtt_client *client,
                                   const mqtt_client_store_config *config)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_INVALID_ARGUMENT;

    if ((!mqtt_client_ready(client)) || (config == NULL) ||
        (config->append == NULL)) {
        /* There is nothing to set up, or nowhere to write. */
    } else if (((config->sessions != MQTT_STORE_MEMORY) &&
                (config->sessions != MQTT_STORE_WRITTEN) &&
                (config->sessions != MQTT_STORE_SYNCED)) ||
               ((config->messages != MQTT_STORE_MEMORY) &&
                (config->messages != MQTT_STORE_WRITTEN) &&
                (config->messages != MQTT_STORE_SYNCED))) {
        /* Not a durability. */
    } else if ((client->store.config.append != NULL) ||
               (client->state != MQTT_CLIENT_OFFLINE) ||
               client->version_known ||
               (client->list_head != MQTT_CLIENT_NONE)) {
        status = MQTT_INVALID_STATE;
    } else {
        client->store.config.append = config->append;
        client->store.config.user_tag = config->user_tag;
        client->store.config.sessions = config->sessions;
        client->store.config.messages = config->messages;
        client->store.config.snapshot_bytes = config->snapshot_bytes;
        client->store.state = (config->sessions != MQTT_STORE_MEMORY) ?
                                  MQTT_CLIENT_STORE_FRESH :
                                  MQTT_CLIENT_STORE_OFF;
        status = MQTT_OK;
    }
    return status;
}

void mqtt_client_journal_identifier(mqtt_client *client,
                                    const mqtt_utf8 *identifier)
{
    /** @brief The journal. */
    mqtt_client_store *store = &client->store;
    /** @brief true if it is the one kept already. */
    bool same = (identifier->length == store->identifier_length);

    if (same && (identifier->length > 0U)) {
        same = mqtt_bytes_equal(
            identifier->data,
            mqtt_arena_bytes(&client->arena, store->identifier_block),
            (uint32_t)identifier->length);
    }
    if ((store->state != MQTT_CLIENT_STORE_OFF) && (!same)) {
        mqtt_arena_give(&client->arena, store->identifier_block);
        store->identifier_block = MQTT_ARENA_NONE;
        store->identifier_length = 0U;
        if (identifier->length > 0U) {
            store->identifier_block =
                mqtt_arena_take(&client->arena, (uint32_t)identifier->length);
        }
        /* With no room for it, it is kept as unknown, which only means the
         * application has to know it after a restart. */
        if (store->identifier_block != MQTT_ARENA_NONE) {
            mqtt_bytes_put(
                mqtt_arena_bytes(&client->arena, store->identifier_block),
                identifier->data, (uint32_t)identifier->length);
            store->identifier_length = identifier->length;
        }
        store->session_due = true;
    }
}

void mqtt_client_journal_session(mqtt_client *client)
{
    /** @brief The journal. */
    mqtt_client_store *store = &client->store;
    /** @brief true if what the Client knows of its Session has changed. */
    const bool changed = store->session_due ||
                         (store->written_version != client->version) ||
                         (store->written_exists != client->session_exists);

    if (changed && client_journal_running(client) && client->version_known) {
        /** @brief The fixed fields. */
        uint8_t bytes[CLIENT_JOURNAL_FIELDS];
        /** @brief The runs. */
        mqtt_run runs[MQTT_STORE_RECORD_RUNS];
        /** @brief The record. */
        mqtt_journal_builder builder;

        mqtt_journal_start(&builder, bytes, CLIENT_JOURNAL_FIELDS, runs);
        client_journal_session_fields(&builder, client);
        (void)client_journal_emit(client, &builder,
                                  (uint8_t)MQTT_STORE_RECORD_CLIENT_SESSION,
                                  store->config.sessions);
    }
    /* What isn't written now a snapshot holds. */
    store->session_due = false;
    store->written_version = client->version;
    store->written_exists = client->session_exists;
}

void mqtt_client_journal_message(mqtt_client *client, uint32_t index)
{
    /** @brief The message. */
    const mqtt_client_message *entry = &client->storage.messages[index];

    if (client_journal_keeps_messages(client) &&
        (entry->packet_identifier != 0U)) {
        /** @brief The fixed fields. */
        uint8_t bytes[CLIENT_JOURNAL_FIELDS];
        /** @brief The runs. */
        mqtt_run runs[MQTT_STORE_RECORD_RUNS];
        /** @brief The record. */
        mqtt_journal_builder builder;

        mqtt_journal_start(&builder, bytes, CLIENT_JOURNAL_FIELDS, runs);
        client_journal_message_fields(&builder, client, entry,
                                      MQTT_CLIENT_MESSAGE_QUEUED);
        (void)client_journal_emit(client, &builder,
                                  (uint8_t)MQTT_STORE_RECORD_CLIENT_MESSAGE,
                                  client->store.config.messages);
    }
}

void mqtt_client_journal_mark(mqtt_client *client, uint8_t kind,
                              uint16_t packet_identifier)
{
    /* Only MQTT 3.1 keeps SUBSCRIBE and UNSUBSCRIBE packets in the Session. */
    if (client_journal_keeps_messages(client) &&
        ((kind != (uint8_t)MQTT_STORE_RECORD_CLIENT_PENDING_DONE) ||
         (client->version == MQTT_VERSION_31))) {
        /** @brief The fixed fields. */
        uint8_t bytes[CLIENT_JOURNAL_FIELDS];
        /** @brief The runs. */
        mqtt_run runs[MQTT_STORE_RECORD_RUNS];
        /** @brief The record. */
        mqtt_journal_builder builder;

        mqtt_journal_start(&builder, bytes, CLIENT_JOURNAL_FIELDS, runs);
        mqtt_journal_integer(&builder, (uint64_t)packet_identifier, 2U);
        (void)client_journal_emit(client, &builder, kind,
                                  client->store.config.messages);
    }
}

void mqtt_client_journal_pending(mqtt_client *client,
                                 uint16_t packet_identifier,
                                 const uint8_t *bytes, uint32_t length)
{
    if (client_journal_keeps_messages(client) &&
        (client->version == MQTT_VERSION_31)) {
        /** @brief The fixed fields. */
        uint8_t fields[CLIENT_JOURNAL_FIELDS];
        /** @brief The runs. */
        mqtt_run runs[MQTT_STORE_RECORD_RUNS];
        /** @brief The record. */
        mqtt_journal_builder builder;

        mqtt_journal_start(&builder, fields, CLIENT_JOURNAL_FIELDS, runs);
        mqtt_journal_integer(&builder, (uint64_t)packet_identifier, 2U);
        mqtt_journal_span(&builder, bytes, length);
        (void)client_journal_emit(client, &builder,
                                  (uint8_t)MQTT_STORE_RECORD_CLIENT_PENDING,
                                  client->store.config.messages);
    }
}

void mqtt_client_journal_sending(mqtt_client *client,
                                 const mqtt_client_message *entry,
                                 bool direct)
{
    if ((entry->packet_identifier != 0U) &&
        (entry->state == MQTT_CLIENT_MESSAGE_QUEUED)) {
        mqtt_client_journal_mark(client, (uint8_t)MQTT_STORE_RECORD_CLIENT_SENT,
                                 entry->packet_identifier);
    }
    if (entry->qos == MQTT_QOS_2) {
        client_journal_raise(client, direct);
    }
}

void mqtt_client_journal_hold(mqtt_client *client, const uint8_t *bytes,
                              uint32_t length)
{
    if ((length > 0U) && client_journal_holds(client, bytes[0])) {
        client_journal_raise(client, false);
    }
}

uint32_t mqtt_client_journal_view(const mqtt_client *client, mqtt_run *runs,
                                  uint32_t max_runs)
{
    /** @brief The journal. */
    const mqtt_client_store *store = &client->store;
    /** @brief Number of runs. */
    uint32_t count = 0U;

    if (store->hold_bytes == MQTT_CLIENT_NONE) {
        count = mqtt_ring_transmit_view(&client->ring,
                                        client->direct_ring_bytes,
                                        client->direct_data,
                                        client->direct_left, runs, max_runs);
    } else {
        count = mqtt_ring_transmit_view_until(
            &client->ring, client->direct_ring_bytes,
            store->hold_direct ? NULL : client->direct_data,
            client->direct_left, store->hold_bytes, runs, max_runs);
    }
    return count;
}

void mqtt_client_journal_consumed(mqtt_client *client, uint32_t count)
{
    /** @brief The journal. */
    mqtt_client_store *store = &client->store;

    if (store->hold_bytes != MQTT_CLIENT_NONE) {
        store->hold_bytes -=
            (count < store->hold_bytes) ? count : store->hold_bytes;
    }
}

void mqtt_client_journal_lost(mqtt_client *client)
{
    client->store.hold_bytes = MQTT_CLIENT_NONE;
    client->store.hold_direct = false;
}

/**
 * @brief Do what mqtt_client_store_commit does, for arguments known to be
 * valid.
 * @param[in,out] client The client.
 * @param[in] now_ms The time.
 * @param[out] sequence The last group closed, which is the one to report.
 * @return MQTT_OK, also when there was nothing to close;
 * MQTT_INVALID_ARGUMENT if the client isn't set up or keeps nothing; or
 * MQTT_INVALID_STATE if the journal is broken.
 */
static mqtt_status client_store_commit_body(mqtt_client *client,
                                            uint64_t now_ms, uint64_t *sequence)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_INVALID_ARGUMENT;

    *sequence = 0U;
    if (mqtt_client_ready(client) &&
        (client->store.state != MQTT_CLIENT_STORE_OFF)) {
        /** @brief The journal. */
        mqtt_client_store *store = &client->store;

        status = MQTT_OK;
        if (client_journal_running(client) && store->open) {
            /** @brief The fixed fields. */
            uint8_t bytes[CLIENT_JOURNAL_FIELDS];
            /** @brief The runs. */
            mqtt_run runs[MQTT_STORE_RECORD_RUNS];
            /** @brief The record. */
            mqtt_journal_builder builder;

            mqtt_journal_start(&builder, bytes, CLIENT_JOURNAL_FIELDS, runs);
            mqtt_journal_integer(&builder,
                                 (now_ms > store->segment_start_ms) ?
                                     (now_ms - store->segment_start_ms) :
                                     0U,
                                 8U);
            if (client_journal_emit(client, &builder,
                                    (uint8_t)MQTT_STORE_RECORD_COMMIT,
                                    store->group_durability)) {
                store->closed += 1U;
                store->open = false;
                store->group_durability = MQTT_STORE_MEMORY;
            }
        }
        if (store->state == MQTT_CLIENT_STORE_BROKEN) {
            status = MQTT_INVALID_STATE;
        }
        *sequence = store->closed;
    }
    return status;
}

mqtt_status mqtt_client_store_commit(mqtt_client *client, uint64_t now_ms,
                                     uint64_t *sequence)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if (sequence != NULL) {
        answer = client_store_commit_body(client, now_ms, sequence);
    }
    return answer;
}

void mqtt_client_stored(mqtt_client *client, uint64_t sequence)
{
    if (mqtt_client_ready(client) && client_journal_running(client)) {
        /** @brief The last group stored, which can't be one not closed. */
        const uint64_t stored = (sequence < client->store.closed) ?
                                    sequence :
                                    client->store.closed;

        if (stored > client->store.stored) {
            client->store.stored = stored;
        }
        client_journal_release(client);
    }
}

void mqtt_client_store_failed(mqtt_client *client)
{
    if (mqtt_client_ready(client) && client_journal_running(client)) {
        client_journal_break(client);
    }
}

bool mqtt_client_store_open(const mqtt_client *client)
{
    return mqtt_client_ready(client) && client_journal_running(client) &&
           client->store.open;
}

bool mqtt_client_snapshot_due(const mqtt_client *client)
{
    /** @brief The answer. */
    bool due = false;

    if (mqtt_client_ready(client) &&
        (client->store.phase == MQTT_CLIENT_SNAPSHOT_IDLE)) {
        /** @brief The journal. */
        const mqtt_client_store *store = &client->store;

        due = (store->state == MQTT_CLIENT_STORE_FRESH) ||
              (store->state == MQTT_CLIENT_STORE_BROKEN) ||
              ((store->state == MQTT_CLIENT_STORE_RUNNING) &&
               ((!store->snapshot_whole) ||
                (store->journal_bytes >= store->config.snapshot_bytes)));
    }
    return due;
}

/**
 * @brief Do what mqtt_client_snapshot_begin does, for arguments known to be
 * valid.
 * @param[in,out] client The client.
 * @param[in] now_ms The time.
 * @param[out] snapshot_header Where the MQTT_STORE_HEADER_SIZE bytes of the
 * snapshot's header go.
 * @param[out] journal_header Where the MQTT_STORE_HEADER_SIZE bytes of the
 * journal segment's header go.
 * @param[out] new_segment true if a new journal segment starts.
 * @return MQTT_OK; MQTT_INVALID_ARGUMENT if the client isn't set up or keeps
 * nothing; or MQTT_INVALID_STATE if a group is still open or a snapshot is
 * under way.
 */
static mqtt_status client_snapshot_begin_body(mqtt_client *client,
                                              uint64_t now_ms,
                                              uint8_t *snapshot_header,
                                              uint8_t *journal_header,
                                              bool *new_segment)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_INVALID_ARGUMENT;

    *new_segment = false;
    if ((!mqtt_client_ready(client)) ||
        (client->store.state == MQTT_CLIENT_STORE_OFF)) {
        /* There is no journal. */
    } else if (client->store.open ||
               (client->store.phase != MQTT_CLIENT_SNAPSHOT_IDLE)) {
        status = MQTT_INVALID_STATE;
    } else {
        /** @brief The journal. */
        mqtt_client_store *store = &client->store;
        /** @brief The fields of the headers. */
        mqtt_store_header header;

        *new_segment = store->snapshot_whole ||
                       (store->state == MQTT_CLIENT_STORE_BROKEN);
        if (*new_segment) {
            store->recovering = (store->state == MQTT_CLIENT_STORE_BROKEN);
            store->generation += 1U;
            store->segment_base = store->closed;
            store->segment_start_ms = now_ms;
            store->journal_bytes = 0U;
        }
        store->state = MQTT_CLIENT_STORE_RUNNING;
        store->snapshot_whole = false;
        header.generation = store->generation;
        header.base = store->segment_base;
        header.kind = (uint8_t)MQTT_STORE_CLIENT_SNAPSHOT;
        header.major = (uint8_t)MQTT_STORE_FORMAT_MAJOR;
        header.minor = (uint8_t)MQTT_STORE_FORMAT_MINOR;
        header.flags = 0U;
        mqtt_store_header_encode(&header, snapshot_header);
        header.kind = (uint8_t)MQTT_STORE_CLIENT_JOURNAL;
        header.flags = store->recovering ? (uint8_t)MQTT_STORE_FLAG_RECOVERY :
                                           0U;
        mqtt_store_header_encode(&header, journal_header);
        store->phase = MQTT_CLIENT_SNAPSHOT_SESSION;
        store->snapshot_changed = false;
        store->snapshot_item = MQTT_CLIENT_NONE;
        store->snapshot_records = 0U;
        status = MQTT_OK;
    }
    return status;
}

mqtt_status mqtt_client_snapshot_begin(mqtt_client *client, uint64_t now_ms,
                                       uint8_t *snapshot_header,
                                       uint8_t *journal_header,
                                       bool *new_segment)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if ((snapshot_header != NULL) && (journal_header != NULL) &&
        (new_segment != NULL)) {
        answer = client_snapshot_begin_body(client, now_ms, snapshot_header,
                                            journal_header, new_segment);
    }
    return answer;
}

/**
 * @brief Do what mqtt_client_snapshot_next does, for arguments known to be
 * valid.
 * @param[in,out] client The client.
 * @param[out] space Where the record's frame and fixed fields go, valid until
 * the next call.
 * @param[in] size Number of bytes in space, at least
 * MQTT_CLIENT_SNAPSHOT_SPACE.
 * @param[out] record The record. Its runs are valid until the next call.
 * @return MQTT_OK with a record; MQTT_NOT_FOUND once END has been given;
 * MQTT_BUFFER_TOO_SMALL if space is too small; or MQTT_INVALID_STATE if no
 * snapshot is under way, or the client has changed since it began.
 */
static mqtt_status client_snapshot_next_body(mqtt_client *client,
                                             uint8_t *space, uint32_t size,
                                             mqtt_store_record *record)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_INVALID_STATE;

    if ((!mqtt_client_ready(client)) ||
        (client->store.phase == MQTT_CLIENT_SNAPSHOT_IDLE) ||
        client->store.snapshot_changed) {
        /* No snapshot is under way, or the one under way is out of date. */
    } else if (client->store.phase == MQTT_CLIENT_SNAPSHOT_OVER) {
        status = MQTT_NOT_FOUND;
    } else if (size < MQTT_CLIENT_SNAPSHOT_SPACE) {
        status = MQTT_BUFFER_TOO_SMALL;
    } else {
        /** @brief The journal. */
        mqtt_client_store *store = &client->store;
        /** @brief The record. */
        mqtt_journal_builder builder;
        /** @brief Its kind. */
        uint8_t kind = 0U;

        while (kind == 0U) {
            mqtt_journal_start(&builder, space, size, store->snapshot_runs);
            kind = client_snapshot_step(client, &builder);
        }
        status = mqtt_journal_finish(&builder, kind) ? MQTT_OK :
                                                       MQTT_BUFFER_TOO_SMALL;
        record->runs = store->snapshot_runs;
        record->run_count = builder.count;
        record->length = builder.length;
        record->sequence = store->segment_base;
        record->durability = MQTT_STORE_SYNCED;
        record->kind = kind;
        if (kind != (uint8_t)MQTT_STORE_RECORD_END) {
            store->snapshot_records += 1U;
        }
    }
    return status;
}

mqtt_status mqtt_client_snapshot_next(mqtt_client *client, uint8_t *space,
                                      uint32_t size, mqtt_store_record *record)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if ((space != NULL) && (record != NULL)) {
        answer = client_snapshot_next_body(client, space, size, record);
    }
    return answer;
}

void mqtt_client_snapshot_done(mqtt_client *client)
{
    if (mqtt_client_ready(client) &&
        (client->store.phase == MQTT_CLIENT_SNAPSHOT_OVER)) {
        client->store.phase = MQTT_CLIENT_SNAPSHOT_IDLE;
        client->store.snapshot_whole = true;
        if (client->store.recovering) {
            /* The answers held over the gap wait for the group after it,
             * which the next commit closes even with nothing in it. */
            client->store.recovering = false;
            client->store.open = true;
            client->store.group_durability = client->store.config.sessions;
            client_journal_release(client);
        }
    }
}

void mqtt_client_snapshot_abort(mqtt_client *client)
{
    if (mqtt_client_ready(client)) {
        client->store.phase = MQTT_CLIENT_SNAPSHOT_IDLE;
    }
}

/**
 * @brief Do what mqtt_client_store_identifier does, for arguments known to be
 * valid.
 * @param[in] client The client.
 * @param[out] identifier The identifier, valid until the next connection.
 * @return MQTT_OK, or MQTT_NOT_FOUND if none is known.
 */
static mqtt_status client_store_identifier_body(const mqtt_client *client,
                                                mqtt_utf8 *identifier)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_NOT_FOUND;

    identifier->data = NULL;
    identifier->length = 0U;
    if (mqtt_client_ready(client) &&
        (client->store.identifier_block != MQTT_ARENA_NONE)) {
        identifier->data = mqtt_arena_bytes(&client->arena,
                                            client->store.identifier_block);
        identifier->length = client->store.identifier_length;
        status = MQTT_OK;
    }
    return status;
}

mqtt_status mqtt_client_store_identifier(const mqtt_client *client,
                                         mqtt_utf8 *identifier)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if (identifier != NULL) {
        answer = client_store_identifier_body(client, identifier);
    }
    return answer;
}

static bool client_journal_running(const mqtt_client *client)
{
    return client->store.state == MQTT_CLIENT_STORE_RUNNING;
}

static bool client_journal_keeps_messages(const mqtt_client *client)
{
    return client_journal_running(client) &&
           (client->store.config.messages != MQTT_STORE_MEMORY);
}

static mqtt_store_durability client_journal_most(mqtt_store_durability first,
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
        /* Neither has to outlast anything. */
    }
    return most;
}

static void client_journal_break(mqtt_client *client)
{
    client->store.state = MQTT_CLIENT_STORE_BROKEN;
    /* What the open group held is lost: the snapshot that mends the journal
     * holds it instead. */
    client->store.open = false;
    client->store.group_durability = MQTT_STORE_MEMORY;
}

static bool client_journal_emit(mqtt_client *client,
                                mqtt_journal_builder *builder, uint8_t kind,
                                mqtt_store_durability durability)
{
    /** @brief The journal. */
    mqtt_client_store *store = &client->store;
    /** @brief true once it is written. */
    bool written = false;

    if (mqtt_journal_finish(builder, kind)) {
        /** @brief What append is given. */
        mqtt_store_record record;

        record.runs = builder->runs;
        record.run_count = builder->count;
        record.length = builder->length;
        record.sequence = store->closed + 1U;
        record.durability = durability;
        record.kind = kind;
        if (store->config.append(store->config.user_tag, &record) == MQTT_OK) {
            written = true;
            store->journal_bytes += (uint64_t)builder->length;
            store->group_durability =
                client_journal_most(store->group_durability, durability);
            if (kind != (uint8_t)MQTT_STORE_RECORD_COMMIT) {
                store->open = true;
            }
            if (store->phase != MQTT_CLIENT_SNAPSHOT_IDLE) {
                store->snapshot_changed = true;
            }
        } else {
            client_journal_break(client);
        }
    }
    return written;
}

static void client_journal_session_fields(mqtt_journal_builder *builder,
                                          const mqtt_client *client)
{
    /** @brief The journal. */
    const mqtt_client_store *store = &client->store;
    /** @brief The Client Identifier, empty if it isn't known. */
    mqtt_utf8 identifier = {0U, NULL};

    if (store->identifier_block != MQTT_ARENA_NONE) {
        identifier.data =
            mqtt_arena_bytes(&client->arena, store->identifier_block);
        identifier.length = store->identifier_length;
    }
    mqtt_journal_integer(builder, (uint64_t)client->version, 1U);
    mqtt_journal_integer(builder,
                         client->session_exists ?
                             (uint64_t)MQTT_STORE_CLIENT_EXISTS :
                             0U,
                         1U);
    mqtt_journal_string(builder, &identifier);
}

static void client_journal_message_fields(mqtt_journal_builder *builder,
                                          const mqtt_client *client,
                                          const mqtt_client_message *entry,
                                          mqtt_client_message_state stage)
{
    mqtt_journal_integer(builder, (uint64_t)entry->packet_identifier, 2U);
    mqtt_journal_integer(builder, (uint64_t)stage, 1U);
    mqtt_journal_integer(builder, (uint64_t)entry->qos, 1U);
    mqtt_journal_integer(builder,
                         entry->retain ? (uint64_t)MQTT_STORE_RETAIN : 0U, 1U);
    mqtt_journal_integer(builder, (uint64_t)entry->topic_alias, 2U);
    if (entry->block != MQTT_ARENA_NONE) {
        mqtt_journal_span(builder,
                          mqtt_arena_bytes(&client->arena, entry->block),
                          entry->size);
    }
}

static void client_journal_pending_fields(mqtt_journal_builder *builder,
                                          const mqtt_client *client,
                                          const mqtt_client_pending *entry)
{
    mqtt_journal_integer(builder, (uint64_t)entry->packet_identifier, 2U);
    if (entry->block != MQTT_ARENA_NONE) {
        mqtt_journal_span(builder,
                          mqtt_arena_bytes(&client->arena, entry->block),
                          entry->length);
    }
}

static void client_journal_raise(mqtt_client *client, bool direct)
{
    /** @brief The journal. */
    mqtt_client_store *store = &client->store;
    /** @brief true if every answer waits for a snapshot to be whole. */
    const bool gap = (store->state == MQTT_CLIENT_STORE_BROKEN) ||
                     (client_journal_running(client) && store->recovering);

    if (gap || (client_journal_running(client) &&
                (store->open || (store->closed > store->stored)))) {
        if (store->hold_bytes == MQTT_CLIENT_NONE) {
            store->hold_bytes = client->ring.used;
            store->hold_direct = direct;
        }
        store->hold_sequence = (gap || store->open) ? (store->closed + 1U) :
                                                      store->closed;
    }
}

static bool client_journal_holds(const mqtt_client *client, uint8_t first_byte)
{
    /** @brief The MQTT Control Packet type. */
    const uint32_t type = (uint32_t)first_byte >> 4U;
    /** @brief The QoS, if it is a PUBLISH packet. */
    const uint32_t qos =
        ((uint32_t)first_byte & CLIENT_JOURNAL_QOS_BITS) >>
        CLIENT_JOURNAL_QOS_SHIFT;

    return (type == MQTT_CONNECT) || (type == MQTT_PUBACK) ||
           (type == MQTT_PUBREC) || (type == MQTT_PUBREL) ||
           (type == MQTT_PUBCOMP) ||
           ((type == MQTT_PUBLISH) && (qos == MQTT_QOS_2)) ||
           (((type == MQTT_SUBSCRIBE) || (type == MQTT_UNSUBSCRIBE)) &&
            (client->version == MQTT_VERSION_31));
}

static void client_journal_release(mqtt_client *client)
{
    /** @brief The journal. */
    mqtt_client_store *store = &client->store;

    if ((store->hold_bytes != MQTT_CLIENT_NONE) &&
        (store->state == MQTT_CLIENT_STORE_RUNNING) && (!store->recovering) &&
        (store->hold_sequence <= store->stored)) {
        store->hold_bytes = MQTT_CLIENT_NONE;
        store->hold_direct = false;
    }
}

static uint8_t client_snapshot_step(mqtt_client *client,
                                    mqtt_journal_builder *builder)
{
    /** @brief The journal. */
    mqtt_client_store *store = &client->store;
    /** @brief true if the records about messages are kept. */
    const bool messages = (store->config.messages != MQTT_STORE_MEMORY);
    /** @brief The kind of record given, or zero. */
    uint8_t kind = 0U;

    if (store->phase == MQTT_CLIENT_SNAPSHOT_SESSION) {
        store->phase = MQTT_CLIENT_SNAPSHOT_MESSAGES;
        store->snapshot_item = client->list_head;
        store->session_due = false;
        store->written_version = client->version;
        store->written_exists = client->session_exists;
        if (client->version_known) {
            client_journal_session_fields(builder, client);
            kind = (uint8_t)MQTT_STORE_RECORD_CLIENT_SESSION;
        }
    } else if (store->phase == MQTT_CLIENT_SNAPSHOT_MESSAGES) {
        if (messages) {
            kind = client_snapshot_message(client, builder);
        }
        if (kind == 0U) {
            store->phase = MQTT_CLIENT_SNAPSHOT_INCOMING;
            store->snapshot_item = 0U;
        }
    } else if (store->phase == MQTT_CLIENT_SNAPSHOT_INCOMING) {
        if (messages && (store->snapshot_item < client->incoming_used)) {
            mqtt_journal_integer(
                builder,
                (uint64_t)client->storage.incoming[store->snapshot_item], 2U);
            store->snapshot_item += 1U;
            kind = (uint8_t)MQTT_STORE_RECORD_CLIENT_INCOMING;
        } else {
            store->phase = MQTT_CLIENT_SNAPSHOT_PENDING;
            store->snapshot_item = 0U;
        }
    } else if (store->phase == MQTT_CLIENT_SNAPSHOT_PENDING) {
        if (messages) {
            kind = client_snapshot_pending(client, builder);
        }
        if (kind == 0U) {
            store->phase = MQTT_CLIENT_SNAPSHOT_END;
        }
    } else {
        mqtt_journal_integer(builder, store->snapshot_records, 8U);
        mqtt_journal_integer(builder, 0U, 8U);
        store->phase = MQTT_CLIENT_SNAPSHOT_OVER;
        kind = (uint8_t)MQTT_STORE_RECORD_END;
    }
    return kind;
}

static uint8_t client_snapshot_message(mqtt_client *client,
                                       mqtt_journal_builder *builder)
{
    /** @brief The journal. */
    mqtt_client_store *store = &client->store;
    /** @brief The kind of record given, or zero. */
    uint8_t kind = 0U;

    /* QoS 0 messages waiting for their turn don't outlast the connection. */
    while ((store->snapshot_item != MQTT_CLIENT_NONE) &&
           (client->storage.messages[store->snapshot_item].packet_identifier ==
            0U)) {
        store->snapshot_item =
            client->storage.messages[store->snapshot_item].next;
    }
    if (store->snapshot_item != MQTT_CLIENT_NONE) {
        /** @brief The message. */
        const mqtt_client_message *entry =
            &client->storage.messages[store->snapshot_item];

        client_journal_message_fields(builder, client, entry, entry->state);
        store->snapshot_item = entry->next;
        kind = (uint8_t)MQTT_STORE_RECORD_CLIENT_MESSAGE;
    }
    return kind;
}

static uint8_t client_snapshot_pending(mqtt_client *client,
                                       mqtt_journal_builder *builder)
{
    /** @brief The journal. */
    mqtt_client_store *store = &client->store;
    /** @brief The kind of record given, or zero. */
    uint8_t kind = 0U;

    while ((client->version == MQTT_VERSION_31) && (kind == 0U) &&
           (store->snapshot_item < client->storage.pending_count)) {
        /** @brief The packet. */
        const mqtt_client_pending *entry =
            &client->storage.pending[store->snapshot_item];

        if (entry->packet_identifier != 0U) {
            client_journal_pending_fields(builder, client, entry);
            kind = (uint8_t)MQTT_STORE_RECORD_CLIENT_PENDING;
        }
        store->snapshot_item += 1U;
    }
    return kind;
}
