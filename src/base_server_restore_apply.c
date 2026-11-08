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
 * @file base_server_restore_apply.c
 * @brief The MQTT Server: what each record of a store does to a server being
 * restored.
 *
 * A record that passes its CRC is still read as what a Client sends is: every
 * length against the content, every string by the rules of its version, every
 * Topic Filter and option as SUBSCRIBE would take them, and every limit of
 * this configuration. A restored Session so holds nothing that a CONNECT, a
 * SUBSCRIBE or a PUBLISH couldn't have made here.
 *
 * Records say what something is, so applying one twice changes nothing more.
 * A record that names a Session, a delivery or a flow the restore doesn't hold
 * does nothing, as what removed it comes later in the journal. One that names
 * a message it doesn't hold is damage, as each segment writes its messages
 * before naming them.
 *
 * [V31] MQTT V3.1 Protocol Specification.
 * @li Cited: 2.2, 3.1, 3.8.
 * @li Source: public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015.
 * @li Cited: 3.1.3.1, 3.8.3, 4.1, 4.7.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 3.1.3.1, p.40. 3.8.3.1, p.73-74. 4.1, p.92-93. 4.8.2, p.101-103.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base_client_id.h"
#include "base_qos.h"
#include "base_server_internal.h"
#include "base_topic.h"
#include "base_topic_check.h"
#include "base_utf8_check.h"
#include "base_wire.h"
#include "v5_prop_value.h"
#include "v5_session.h"
#include "v5_subscribe.h"

/** @brief The most time a record may say is left, so sums can't overflow. */
#define APPLY_LEFT_MAX 0x3FFFFFFFFFFFFFFFU

/**
 * @brief The value a timer of a Session that had a connection at the end
 * holds, until the restore ends and starts it from then.
 */
#define APPLY_AT_END UINT64_MAX

/**
 * @brief Read an 8-byte integer.
 * @param[in,out] reader The reader.
 * @return The integer, or 0 after a failure.
 */
static uint64_t apply_u64(mqtt_reader *reader);

/**
 * @brief Find whether a record's content was read exactly.
 * @param[in] reader The reader.
 * @return true if every byte was read and nothing failed.
 */
static bool apply_done(const mqtt_reader *reader);

/**
 * @brief Turn the time a record says is left into a deadline on the store's
 * line, kept one above it so that zero still means no timer.
 * @param[in] restore The restore.
 * @param[in] left Milliseconds left.
 * @return The deadline.
 */
static uint64_t apply_deadline(const mqtt_restore *restore, uint64_t left);

/**
 * @brief Find a Session by the Client Identifier a record gives.
 * @param[in] restore The restore.
 * @param[in] identifier The Client Identifier.
 * @return The Session, or MQTT_SERVER_NONE.
 */
static uint32_t apply_session_of(const mqtt_restore *restore,
                                 const mqtt_utf8 *identifier);

/**
 * @brief Check a Client Identifier by the rules of a version, and against
 * this configuration.
 * @param[in] restore The restore.
 * @param[in] identifier The Client Identifier.
 * @param[in] version The version.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status apply_identifier(mqtt_restore *restore,
                                    const mqtt_utf8 *identifier,
                                    mqtt_version version);

/**
 * @brief Forget the map entries of a list of deliveries.
 * @param[in,out] restore The restore.
 * @param[in] head The first delivery.
 */
static void apply_forget_list(mqtt_restore *restore, uint32_t head);

/**
 * @brief End a Session without publishing its Will Message, as the Session
 * ended before the restart.
 * @param[in,out] restore The restore.
 * @param[in] session The Session.
 */
static void apply_end_session(mqtt_restore *restore, uint32_t session);

/**
 * @brief Free a Session's deliveries, which the snapshot is about to give
 * again from its start.
 * @param[in,out] restore The restore.
 * @param[in] session The Session.
 */
static void apply_restart_session(mqtt_restore *restore, uint32_t session);

/**
 * @brief Name the protocol version a SESSION record gives.
 * @param[in] version The byte.
 * @param[out] named The version, if it is one.
 * @return true if it is one the library knows.
 */
static bool apply_version(uint32_t version, mqtt_version *named);

/**
 * @brief Apply a SESSION record.
 * @param[in,out] restore The restore.
 * @param[in,out] reader The content.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status apply_session(mqtt_restore *restore, mqtt_reader *reader);

/**
 * @brief Check what a SUBSCRIBE record holds against this configuration.
 * @param[in,out] restore The restore.
 * @param[in] filter The whole Topic Filter.
 * @param[in] options The Subscription Options.
 * @param[in] identifier The Subscription Identifier.
 * @param[in] shared true for a Shared Subscription.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status apply_subscription_check(mqtt_restore *restore,
                                            const mqtt_utf8 *filter,
                                            uint32_t options,
                                            uint32_t identifier, bool shared);

/**
 * @brief Apply a SUBSCRIBE or UNSUBSCRIBE record.
 * @param[in,out] restore The restore.
 * @param[in,out] reader The content.
 * @param[in] subscribe true for SUBSCRIBE.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status apply_subscribe(mqtt_restore *restore, mqtt_reader *reader,
                                   bool subscribe);

/**
 * @brief Apply a WILL or WILL_REMOVE record.
 * @param[in,out] restore The restore.
 * @param[in,out] reader The content.
 * @param[in] set true for WILL.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status apply_will(mqtt_restore *restore, mqtt_reader *reader,
                              bool set);

/**
 * @brief Apply a MESSAGE_GONE or MESSAGE_EXPIRY record.
 * @param[in,out] restore The restore.
 * @param[in,out] reader The content.
 * @param[in] gone true for MESSAGE_GONE.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status apply_message_change(mqtt_restore *restore,
                                        mqtt_reader *reader, bool gone);

/**
 * @brief Apply a RETAINED or RETAINED_REMOVE record.
 * @param[in,out] restore The restore.
 * @param[in,out] reader The content.
 * @param[in] set true for RETAINED.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status apply_retained(mqtt_restore *restore, mqtt_reader *reader,
                                  bool set);

/**
 * @brief Make the delivery a DELIVERY record describes.
 * @param[in,out] restore The restore.
 * @param[in] session The Session.
 * @param[in] store_id The delivery's store id.
 * @param[in] message The message, or MQTT_SERVER_NONE for one released.
 * @param[in] qos The QoS.
 * @param[in] retain The RETAIN flag.
 * @param[in] identifier The Subscription Identifier.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status apply_delivery_make(mqtt_restore *restore, uint32_t session,
                                       uint64_t store_id, uint32_t message,
                                       uint8_t qos, bool retain,
                                       uint32_t identifier);

/**
 * @brief Apply a DELIVERY record.
 * @param[in,out] restore The restore.
 * @param[in,out] reader The content.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status apply_delivery(mqtt_restore *restore, mqtt_reader *reader);

/**
 * @brief Find the delivery a SENT, RELEASED or FINISHED record names.
 * @param[in] restore The restore.
 * @param[in] store_id The store id.
 * @param[out] session Its Session.
 * @return The delivery, or MQTT_SERVER_NONE.
 */
static uint32_t apply_delivery_of(const mqtt_restore *restore,
                                  uint64_t store_id, uint32_t *session);

/**
 * @brief Take a delivery off the list of a Session it is on.
 * @param[in,out] server The server.
 * @param[in] session The Session.
 * @param[in] delivery The delivery.
 */
static void apply_unlink(mqtt_server *server, uint32_t session,
                         uint32_t delivery);

/**
 * @brief Apply a SENT, RELEASED or FINISHED record.
 * @param[in,out] restore The restore.
 * @param[in,out] reader The content.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status apply_flow(mqtt_restore *restore, mqtt_reader *reader);

/**
 * @brief Apply an INCOMING or INCOMING_REMOVE record.
 * @param[in,out] restore The restore.
 * @param[in,out] reader The content.
 * @param[in] received true for INCOMING.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status apply_incoming(mqtt_restore *restore, mqtt_reader *reader,
                                  bool received);

/**
 * @brief Apply a COMMIT or END record.
 * @param[in,out] restore The restore.
 * @param[in,out] reader The content.
 * @return MQTT_OK, or MQTT_STORE_DAMAGED.
 */
static mqtt_status apply_mark(mqtt_restore *restore, mqtt_reader *reader);

static uint64_t apply_u64(mqtt_reader *reader)
{
    /** @brief The high half. */
    const uint64_t high = (uint64_t)mqtt_read_four_byte_integer(reader);
    /** @brief The low half. */
    const uint64_t low = (uint64_t)mqtt_read_four_byte_integer(reader);

    return (high << 32U) | low;
}

static bool apply_done(const mqtt_reader *reader)
{
    return (reader->status == MQTT_OK) && (mqtt_reader_left(reader) == 0U);
}

static uint64_t apply_deadline(const mqtt_restore *restore, uint64_t left)
{
    /** @brief The time left, no more than sums can take. */
    const uint64_t bounded = (left < APPLY_LEFT_MAX) ? left : APPLY_LEFT_MAX;

    return restore->clock + bounded + 1U;
}

static uint32_t apply_session_of(const mqtt_restore *restore,
                                 const mqtt_utf8 *identifier)
{
    return (identifier->length == 0U) ?
               MQTT_SERVER_NONE :
               mqtt_session_find(restore->server, identifier);
}

static mqtt_status apply_identifier(mqtt_restore *restore,
                                    const mqtt_utf8 *identifier,
                                    mqtt_version version)
{
    /** @brief The bytes. */
    const uint8_t *data = identifier->data;
    /** @brief Number of them. */
    const uint32_t length = (uint32_t)identifier->length;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (length == 0U) {
        status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_CONTENT);
    } else if (version == MQTT_VERSION_31) {
        /** @brief true if it is UTF-8 at all. */
        const bool formed = mqtt_utf8_is_well_formed(data, length);
        /** @brief Number of characters, which is what MQTT 3.1 limits. */
        const uint32_t characters =
            formed ? mqtt_utf8_character_count(data, length) : 0U;

        if ((characters < MQTT_CLIENT_ID_MIN) ||
            (characters > MQTT_CLIENT_ID_MAX)) {
            status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_CONTENT);
        }
    } else if (!mqtt_utf8_is_valid(data, length)) {
        status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_CONTENT);
    } else if (length > restore->server->config.maximum_client_identifier) {
        status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_CONFIG);
    } else {
        /* It is one a CONNECT could have given. */
    }
    return status;
}

static void apply_forget_list(mqtt_restore *restore, uint32_t head)
{
    /** @brief The delivery being forgotten. */
    uint32_t index = head;

    while (index != MQTT_SERVER_NONE) {
        /** @brief The delivery. */
        const mqtt_delivery *entry =
            &restore->server->storage.deliveries[index];

        if (entry->store_id != 0U) {
            /** @brief Its entry in map. */
            const uint32_t found =
                mqtt_restore_find(restore, entry->store_id, false);

            if (found != MQTT_SERVER_NONE) {
                mqtt_restore_forget(restore, found);
            }
        }
        index = entry->next;
    }
}

static void apply_end_session(mqtt_restore *restore, uint32_t session)
{
    /** @brief The server. */
    mqtt_server *server = restore->server;
    /** @brief The Session. */
    mqtt_session *entry = &server->storage.sessions[session];

    apply_forget_list(restore, entry->queued_head);
    apply_forget_list(restore, entry->inflight_head);
    mqtt_route_release(server, entry->will_message);
    entry->will_message = MQTT_SERVER_NONE;
    mqtt_session_discard(server, session);
}

static void apply_restart_session(mqtt_restore *restore, uint32_t session)
{
    /** @brief The server. */
    mqtt_server *server = restore->server;
    /** @brief The Session. */
    mqtt_session *entry = &server->storage.sessions[session];
    /** @brief The two lists, the waiting then those in flight. */
    const uint32_t heads[2] = {entry->queued_head, entry->inflight_head};
    /** @brief Index of the list being freed. */
    uint32_t list = 0U;

    while (list < 2U) {
        /** @brief The delivery being freed. */
        uint32_t index = heads[list];

        while (index != MQTT_SERVER_NONE) {
            /** @brief The delivery after it. */
            const uint32_t next = server->storage.deliveries[index].next;

            server->storage.deliveries[index].store_id = 0U;
            mqtt_route_delivery_give(server, index);
            index = next;
        }
        list += 1U;
    }
    entry->queued_head = MQTT_SERVER_NONE;
    entry->queued_tail = MQTT_SERVER_NONE;
    entry->queued_count = 0U;
    entry->inflight_head = MQTT_SERVER_NONE;
    entry->inflight_tail = MQTT_SERVER_NONE;
    restore->last_delivery = MQTT_SERVER_NONE;
    /* The deliveries made since the Session began are the last in map. */
    restore->map_used = restore->mark_map;
}

static bool apply_version(uint32_t version, mqtt_version *named)
{
    /** @brief The answer. */
    bool known = true;

    if (version == (uint32_t)MQTT_VERSION_31) {
        *named = MQTT_VERSION_31;
    } else if (version == (uint32_t)MQTT_VERSION_311) {
        *named = MQTT_VERSION_311;
    } else if (version == (uint32_t)MQTT_VERSION_5) {
        *named = MQTT_VERSION_5;
    } else {
        known = false;
    }
    return known;
}

static mqtt_status apply_session(mqtt_restore *restore, mqtt_reader *reader)
{
    /** @brief The server. */
    mqtt_server *server = restore->server;
    /** @brief The protocol version. */
    const uint32_t version = (uint32_t)mqtt_read_byte(reader);
    /** @brief The flags. */
    const uint32_t flags = (uint32_t)mqtt_read_byte(reader);
    /** @brief The Session Expiry Interval. */
    const uint32_t expiry = mqtt_read_four_byte_integer(reader);
    /** @brief Milliseconds left to its end, if its Client is away. */
    const uint64_t expiry_left = apply_u64(reader);
    /** @brief Milliseconds left to its Will Message. */
    const uint64_t will_left = apply_u64(reader);
    /** @brief The Client Identifier. */
    const mqtt_utf8 identifier = mqtt_read_utf8(reader);
    /** @brief The version, as the library names it. */
    mqtt_version named = MQTT_VERSION_5;
    /** @brief true if the version is one the library knows. */
    const bool known = apply_version(version, &named);
    /** @brief The Session. */
    uint32_t session = MQTT_SERVER_NONE;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if ((!apply_done(reader)) || (!known) ||
        ((flags & ~(MQTT_STORE_SESSION_AWAY | MQTT_STORE_SESSION_WILL_TIMER)) !=
         0U) ||
        (expiry == 0U)) {
        status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_CONTENT);
    } else {
        status = apply_identifier(restore, &identifier, named);
    }
    if (status == MQTT_OK) {
        session = mqtt_session_find(server, &identifier);
        if (session == MQTT_SERVER_NONE) {
            session = mqtt_session_create(server, &identifier);
        } else if (restore->snapshot && (session == restore->mark_session)) {
            apply_restart_session(restore, session);
        } else {
            /* A Session the journal writes again. */
        }
        if (session == MQTT_SERVER_NONE) {
            status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_ROOM);
        }
    }
    if (status == MQTT_OK) {
        /** @brief The Session. */
        mqtt_session *entry = &server->storage.sessions[session];
        /** @brief true if its Client was away. */
        const bool away = ((flags & MQTT_STORE_SESSION_AWAY) != 0U);

        if (restore->snapshot && (session != restore->mark_session)) {
            restore->mark_session = session;
            restore->mark_map = restore->map_used;
        }
        entry->version = named;
        entry->expiry_s = (expiry > server->config.maximum_session_expiry) ?
                              server->config.maximum_session_expiry :
                              expiry;
        entry->expiry_due_ms =
            (!away) ? APPLY_AT_END :
                      ((entry->expiry_s == MQTT5_SESSION_EXPIRY_NEVER) ?
                           0U :
                           apply_deadline(restore, expiry_left));
        entry->will_due_ms =
            (!away) ? APPLY_AT_END :
                      (((flags & MQTT_STORE_SESSION_WILL_TIMER) != 0U) ?
                           apply_deadline(restore, will_left) :
                           0U);
        entry->stored = true;
    }
    return status;
}

static mqtt_status apply_subscription_check(mqtt_restore *restore,
                                            const mqtt_utf8 *filter,
                                            uint32_t options,
                                            uint32_t identifier, bool shared)
{
    /** @brief The configuration. */
    const mqtt_server_config *config = &restore->server->config;
    /** @brief The ShareName. */
    mqtt_utf8 share_name = {0U, NULL};
    /** @brief The filter without its share prefix. */
    mqtt_utf8 inner = *filter;
    /** @brief The QoS granted. */
    const uint32_t qos = options & MQTT5_MAXIMUM_QOS_MASK;
    /** @brief Retain Handling. */
    const uint32_t handling = options & MQTT5_RETAIN_HANDLING_MASK;
    /** @brief The version whose rules the filter is checked by. */
    const mqtt_version rules = shared ? MQTT_VERSION_5 : MQTT_VERSION_31;
    /** @brief true if the filter is one SUBSCRIBE takes. */
    const bool valid = (mqtt_topic_filter_check(filter, rules) == MQTT_OK);
    /** @brief true if it splits as a Shared Subscription does. */
    bool split = false;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (valid && shared) {
        split = mqtt_topic_share_split(filter, &share_name, &inner);
    }
    if (((options & MQTT5_SUBSCRIPTION_OPTIONS_RESERVED) != 0U) ||
        (qos > (uint32_t)MQTT_QOS_2) ||
        (handling == MQTT5_RETAIN_HANDLING_MASK) ||
        (identifier > MQTT5_SUBSCRIPTION_IDENTIFIER_MAX) || (!valid) ||
        (shared && (!split))) {
        status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_CONTENT);
    } else {
        /** @brief Index of the byte being looked at. */
        uint32_t index = 0U;
        /** @brief true if the filter has a wildcard. */
        bool wildcard = false;

        while (index < (uint32_t)inner.length) {
            if (((uint32_t)inner.data[index] == MQTT_MULTI_LEVEL_WILDCARD) ||
                ((uint32_t)inner.data[index] == MQTT_SINGLE_LEVEL_WILDCARD)) {
                wildcard = true;
            }
            index += 1U;
        }
        if ((shared && (!config->shared_subscription_available)) ||
            (wildcard && (!config->wildcard_subscription_available)) ||
            ((identifier != 0U) &&
             (!config->subscription_identifier_available)) ||
            (qos > (uint32_t)config->maximum_qos)) {
            status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_CONFIG);
        }
    }
    return status;
}

static mqtt_status apply_subscribe(mqtt_restore *restore, mqtt_reader *reader,
                                   bool subscribe)
{
    /** @brief The Subscription Options. */
    const uint32_t options =
        subscribe ? (uint32_t)mqtt_read_byte(reader) : 0U;
    /** @brief The flags. */
    const uint32_t flags = (uint32_t)mqtt_read_byte(reader);
    /** @brief The Subscription Identifier. */
    const uint32_t identifier =
        subscribe ? mqtt_read_four_byte_integer(reader) : 0U;
    /** @brief The Client Identifier. */
    const mqtt_utf8 client = mqtt_read_utf8(reader);
    /** @brief The Topic Filter. */
    const mqtt_utf8 filter = mqtt_read_utf8(reader);
    /** @brief true for a Shared Subscription. */
    const bool shared = ((flags & MQTT_STORE_SHARED) != 0U);
    /** @brief The Session. */
    const uint32_t session = apply_session_of(restore, &client);
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if ((!apply_done(reader)) || ((flags & ~MQTT_STORE_SHARED) != 0U) ||
        (filter.length == 0U)) {
        status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_CONTENT);
    } else if (session == MQTT_SERVER_NONE) {
        /* Its Session ends later in the journal. */
    } else if (!subscribe) {
        /** @brief MQTT_NOT_FOUND if it was gone already, which is no fault. */
        const mqtt_status removed = mqtt_session_unsubscribe(
            restore->server, session, &filter, shared);

        status = (removed == MQTT_NOT_FOUND) ? MQTT_OK : removed;
    } else {
        /** @brief true if the Session didn't have it, which isn't needed. */
        bool was_new = false;

        status = apply_subscription_check(restore, &filter, options,
                                          identifier, shared);
        if (status == MQTT_OK) {
            status = mqtt_session_subscribe(restore->server, session, &filter,
                                            (uint8_t)options, identifier,
                                            shared, &was_new);
            if (status != MQTT_OK) {
                status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_ROOM);
            }
        }
    }
    return status;
}

static mqtt_status apply_will(mqtt_restore *restore, mqtt_reader *reader,
                              bool set)
{
    /** @brief The server. */
    mqtt_server *server = restore->server;
    /** @brief The message's store id. */
    const uint64_t id = set ? apply_u64(reader) : 0U;
    /** @brief The Will Delay Interval. */
    const uint32_t delay = set ? mqtt_read_four_byte_integer(reader) : 0U;
    /** @brief The flags. */
    const uint32_t flags = set ? (uint32_t)mqtt_read_byte(reader) : 0U;
    /** @brief The Message Expiry Interval. */
    const uint32_t expiry = set ? mqtt_read_four_byte_integer(reader) : 0U;
    /** @brief The Client Identifier. */
    const mqtt_utf8 client = mqtt_read_utf8(reader);
    /** @brief The Session. */
    const uint32_t session = apply_session_of(restore, &client);
    /** @brief The message's entry in map. */
    const uint32_t found = set ? mqtt_restore_find(restore, id, true) :
                                 MQTT_SERVER_NONE;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if ((!apply_done(reader)) || ((flags & ~MQTT_STORE_WILL_EXPIRY) != 0U) ||
        (set && (found == MQTT_SERVER_NONE))) {
        status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_CONTENT);
    } else if (session != MQTT_SERVER_NONE) {
        /** @brief The Session. */
        mqtt_session *entry = &server->storage.sessions[session];

        mqtt_route_release(server, entry->will_message);
        entry->will_message = MQTT_SERVER_NONE;
        if (set) {
            /** @brief The message. */
            const uint32_t message = restore->map[found].index;
            /** @brief Its QoS and RETAIN flag. */
            const mqtt_message *stored = &server->storage.messages[message];

            if ((stored->qos > server->config.maximum_qos) ||
                (stored->retain && (!server->config.retain_available))) {
                status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_CONFIG);
            } else {
                mqtt_route_hold(server, message);
                entry->will_message = message;
                entry->will_delay_s = delay;
                entry->will_has_expiry =
                    ((flags & MQTT_STORE_WILL_EXPIRY) != 0U);
                entry->will_expiry_s = expiry;
            }
        }
    } else {
        /* Its Session ends later in the journal. */
    }
    return status;
}

static mqtt_status apply_message_change(mqtt_restore *restore,
                                        mqtt_reader *reader, bool gone)
{
    /** @brief The message's store id. */
    const uint64_t id = apply_u64(reader);
    /** @brief The milliseconds left before it expires. */
    const uint64_t left = gone ? 0U : apply_u64(reader);
    /** @brief The message's entry in map. */
    const uint32_t found = mqtt_restore_find(restore, id, true);
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (!apply_done(reader)) {
        status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_CONTENT);
    } else if (found == MQTT_SERVER_NONE) {
        /* It went before the snapshot reached it. */
    } else {
        /** @brief The message. */
        const uint32_t message = restore->map[found].index;
        /** @brief Its entry. */
        mqtt_message *entry = &restore->server->storage.messages[message];

        if (!gone) {
            entry->expiry_ms = apply_deadline(restore, left);
        } else if (entry->references != 1U) {
            /* Something still holds it, which the journal says nothing
             * does. */
            status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_CONTENT);
        } else {
            mqtt_restore_forget(restore, found);
            mqtt_route_release(restore->server, message);
        }
    }
    return status;
}

static mqtt_status apply_retained(mqtt_restore *restore, mqtt_reader *reader,
                                  bool set)
{
    /** @brief The server. */
    mqtt_server *server = restore->server;
    /** @brief The message's store id. */
    const uint64_t id = set ? apply_u64(reader) : 0U;
    /** @brief The message's entry in map. */
    const uint32_t found = set ? mqtt_restore_find(restore, id, true) :
                                 MQTT_SERVER_NONE;
    /** @brief The Topic Name. */
    mqtt_utf8 topic_name = {0U, NULL};
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (set && (found != MQTT_SERVER_NONE)) {
        /** @brief The message. */
        const mqtt_message *entry =
            &server->storage.messages[restore->map[found].index];

        topic_name.length = entry->topic_length;
        topic_name.data = mqtt_page_arena_bytes(&server->arena, entry->block);
    } else if (!set) {
        topic_name = mqtt_read_utf8(reader);
    } else {
        /* A message the store doesn't hold. */
    }
    if ((!apply_done(reader)) || (set && (found == MQTT_SERVER_NONE)) ||
        ((!set) &&
         (mqtt_topic_name_check(&topic_name, MQTT_VERSION_31) != MQTT_OK))) {
        status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_CONTENT);
    } else if (set && (!server->config.retain_available)) {
        status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_CONFIG);
    } else if (set) {
        /** @brief The node of the Topic Name. */
        uint32_t node = MQTT_TREE_NONE;

        if (mqtt_tree_insert(&server->tree, &topic_name, &node) != MQTT_OK) {
            status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_ROOM);
        } else {
            /** @brief The retained message it replaces. */
            const uint32_t old = server->tree.nodes[node].retained;

            mqtt_route_hold(server, restore->map[found].index);
            server->tree.nodes[node].retained = restore->map[found].index;
            mqtt_route_release(server, old);
        }
    } else {
        /** @brief The node of the Topic Name. */
        const uint32_t node = mqtt_tree_find(&server->tree, &topic_name);

        if ((node != MQTT_TREE_NONE) &&
            (server->tree.nodes[node].retained != MQTT_SERVER_NONE)) {
            mqtt_route_release(server, server->tree.nodes[node].retained);
            server->tree.nodes[node].retained = MQTT_SERVER_NONE;
            mqtt_tree_prune(&server->tree, node);
        }
    }
    return status;
}

static mqtt_status apply_delivery_make(mqtt_restore *restore, uint32_t session,
                                       uint64_t store_id, uint32_t message,
                                       uint8_t qos, bool retain,
                                       uint32_t identifier)
{
    /** @brief The server. */
    mqtt_server *server = restore->server;
    /** @brief The Session. */
    mqtt_session *entry = &server->storage.sessions[session];
    /** @brief The delivery. */
    uint32_t index = MQTT_SERVER_NONE;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (entry->queued_count >= server->config.maximum_queued) {
        status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_CONFIG);
    } else {
        index = mqtt_route_delivery_take(server);
    }
    if ((status == MQTT_OK) && (index == MQTT_SERVER_NONE)) {
        status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_ROOM);
    }
    if (status == MQTT_OK) {
        /** @brief The delivery. */
        mqtt_delivery *delivery = &server->storage.deliveries[index];

        status = mqtt_restore_add(restore, store_id, index, session);
        /* A delivery no later record could find is not made at all. */
        if (status == MQTT_OK) {
            delivery->store_id = store_id;
            delivery->message = message;
            delivery->qos = qos;
            delivery->retain = retain;
            delivery->subscription_identifier = identifier;
            delivery->released = (message == MQTT_SERVER_NONE);
            if (message != MQTT_SERVER_NONE) {
                mqtt_route_hold(server, message);
            }
            delivery->previous = entry->queued_tail;
            if (entry->queued_tail == MQTT_SERVER_NONE) {
                entry->queued_head = index;
            } else {
                server->storage.deliveries[entry->queued_tail].next = index;
            }
            entry->queued_tail = index;
            entry->queued_count += 1U;
            restore->last_delivery = index;
        }
    }
    return status;
}

static mqtt_status apply_delivery(mqtt_restore *restore, mqtt_reader *reader)
{
    /** @brief The delivery's store id. */
    const uint64_t id = apply_u64(reader);
    /** @brief The message's store id, or 0 for one released. */
    const uint64_t message = apply_u64(reader);
    /** @brief The QoS. */
    const uint8_t qos = mqtt_read_byte(reader);
    /** @brief The flags. */
    const uint32_t flags = (uint32_t)mqtt_read_byte(reader);
    /** @brief The Subscription Identifier. */
    const uint32_t identifier = mqtt_read_four_byte_integer(reader);
    /** @brief The Client Identifier. */
    const mqtt_utf8 client = mqtt_read_utf8(reader);
    /** @brief The Session. */
    const uint32_t session = apply_session_of(restore, &client);
    /** @brief The message's entry in map. */
    const uint32_t found = (message == 0U) ?
                               MQTT_SERVER_NONE :
                               mqtt_restore_find(restore, message, true);
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    restore->last_delivery = MQTT_SERVER_NONE;
    if ((!apply_done(reader)) || (id == 0U) || (qos == MQTT_QOS_0) ||
        (qos > MQTT_QOS_2) || ((flags & ~MQTT_STORE_RETAIN) != 0U) ||
        (identifier > MQTT5_SUBSCRIPTION_IDENTIFIER_MAX) ||
        ((message == 0U) && ((!restore->snapshot) || (qos != MQTT_QOS_2))) ||
        ((message != 0U) && (found == MQTT_SERVER_NONE))) {
        status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_CONTENT);
    } else if ((session == MQTT_SERVER_NONE) ||
               ((!restore->snapshot) &&
                (mqtt_restore_find(restore, id, false) != MQTT_SERVER_NONE))) {
        /* Its Session ends later, or the snapshot holds it already. */
    } else {
        status = apply_delivery_make(
            restore, session, id,
            (found == MQTT_SERVER_NONE) ? MQTT_SERVER_NONE :
                                          restore->map[found].index,
            qos, (flags & MQTT_STORE_RETAIN) != 0U, identifier);
    }
    return status;
}

static uint32_t apply_delivery_of(const mqtt_restore *restore,
                                  uint64_t store_id, uint32_t *session)
{
    /** @brief The delivery. */
    uint32_t delivery = MQTT_SERVER_NONE;

    *session = MQTT_SERVER_NONE;
    if (restore->snapshot) {
        /** @brief The delivery the last DELIVERY record made. */
        const uint32_t last = restore->last_delivery;

        /* The snapshot gives SENT and RELEASED right after their DELIVERY. */
        if ((last != MQTT_SERVER_NONE) &&
            (restore->server->storage.deliveries[last].store_id ==
             store_id)) {
            delivery = restore->last_delivery;
            *session = restore->mark_session;
        }
    } else {
        /** @brief Its entry in map. */
        const uint32_t found = mqtt_restore_find(restore, store_id, false);

        if (found != MQTT_SERVER_NONE) {
            delivery = restore->map[found].index;
            *session = restore->map[found].session;
        }
    }
    return delivery;
}

static void apply_unlink(mqtt_server *server, uint32_t session,
                         uint32_t delivery)
{
    /** @brief The Session. */
    mqtt_session *entry = &server->storage.sessions[session];
    /** @brief The delivery. */
    const mqtt_delivery *item = &server->storage.deliveries[delivery];
    /** @brief true if it is in flight, false if it waits. */
    const bool inflight = (item->packet_identifier != 0U);

    if (item->previous == MQTT_SERVER_NONE) {
        if (inflight) {
            entry->inflight_head = item->next;
        } else {
            entry->queued_head = item->next;
        }
    } else {
        server->storage.deliveries[item->previous].next = item->next;
    }
    if (item->next == MQTT_SERVER_NONE) {
        if (inflight) {
            entry->inflight_tail = item->previous;
        } else {
            entry->queued_tail = item->previous;
        }
    } else {
        server->storage.deliveries[item->next].previous = item->previous;
    }
    if (!inflight) {
        entry->queued_count -= 1U;
    }
}

static mqtt_status apply_flow(mqtt_restore *restore, mqtt_reader *reader)
{
    /** @brief The server. */
    mqtt_server *server = restore->server;
    /** @brief The delivery's store id. */
    const uint64_t id = apply_u64(reader);
    /** @brief The Packet Identifier, for SENT. */
    const uint16_t packet_identifier =
        (restore->kind == (uint8_t)MQTT_STORE_RECORD_SENT) ?
            mqtt_read_two_byte_integer(reader) :
            0U;
    /** @brief Its Session. */
    uint32_t session = MQTT_SERVER_NONE;
    /** @brief The delivery. */
    const uint32_t delivery = apply_delivery_of(restore, id, &session);
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if ((!apply_done(reader)) ||
        ((restore->kind == (uint8_t)MQTT_STORE_RECORD_SENT) &&
         (packet_identifier == 0U)) ||
        (restore->snapshot && (delivery == MQTT_SERVER_NONE))) {
        status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_CONTENT);
    } else if (delivery == MQTT_SERVER_NONE) {
        /* It finished before the snapshot reached it. */
    } else {
        /** @brief The delivery. */
        mqtt_delivery *item = &server->storage.deliveries[delivery];

        if (restore->kind == (uint8_t)MQTT_STORE_RECORD_FINISHED) {
            apply_unlink(server, session, delivery);
            mqtt_restore_forget(restore,
                                mqtt_restore_find(restore, id, false));
            item->store_id = 0U;
            mqtt_route_delivery_give(server, delivery);
        } else if (restore->kind == (uint8_t)MQTT_STORE_RECORD_RELEASED) {
            if ((item->qos != MQTT_QOS_2) || (item->packet_identifier == 0U)) {
                status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_CONTENT);
            } else if (!item->released) {
                item->released = true;
                mqtt_route_release(server, item->message);
                item->message = MQTT_SERVER_NONE;
            } else {
                /* Released already. */
            }
        } else if (item->released) {
            /* Its PUBREC came after: nothing goes back. */
        } else {
            /** @brief The Session. */
            mqtt_session *entry = &server->storage.sessions[session];

            if (item->packet_identifier == 0U) {
                apply_unlink(server, session, delivery);
                item->next = MQTT_SERVER_NONE;
                item->previous = entry->inflight_tail;
                if (entry->inflight_tail == MQTT_SERVER_NONE) {
                    entry->inflight_head = delivery;
                } else {
                    server->storage.deliveries[entry->inflight_tail].next =
                        delivery;
                }
                entry->inflight_tail = delivery;
            }
            item->packet_identifier = packet_identifier;
        }
    }
    return status;
}

static mqtt_status apply_incoming(mqtt_restore *restore, mqtt_reader *reader,
                                  bool received)
{
    /** @brief The server. */
    mqtt_server *server = restore->server;
    /** @brief The Packet Identifier. */
    const uint16_t packet_identifier = mqtt_read_two_byte_integer(reader);
    /** @brief The Client Identifier. */
    const mqtt_utf8 client = mqtt_read_utf8(reader);
    /** @brief The Session. */
    const uint32_t session = apply_session_of(restore, &client);
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if ((!apply_done(reader)) || (packet_identifier == 0U)) {
        status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_CONTENT);
    } else if (session != MQTT_SERVER_NONE) {
        /** @brief The Session. */
        mqtt_session *entry = &server->storage.sessions[session];
        /** @brief The flow being looked at. */
        uint32_t index = entry->incoming_head;

        while ((index != MQTT_SERVER_NONE) &&
               (server->storage.deliveries[index].packet_identifier !=
                packet_identifier)) {
            index = server->storage.deliveries[index].next;
        }
        if (received && (index == MQTT_SERVER_NONE)) {
            if (entry->incoming_count >= server->config.receive_maximum) {
                status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_CONFIG);
            } else {
                index = mqtt_route_delivery_take(server);
            }
            if ((status == MQTT_OK) && (index == MQTT_SERVER_NONE)) {
                status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_ROOM);
            } else if (status == MQTT_OK) {
                server->storage.deliveries[index].packet_identifier =
                    packet_identifier;
                server->storage.deliveries[index].next = entry->incoming_head;
                if (entry->incoming_head != MQTT_SERVER_NONE) {
                    server->storage.deliveries[entry->incoming_head].previous =
                        index;
                }
                entry->incoming_head = index;
                entry->incoming_count += 1U;
            } else {
                /* The fault is described. */
            }
        } else if ((!received) && (index != MQTT_SERVER_NONE)) {
            /** @brief The flow. */
            const mqtt_delivery *flow = &server->storage.deliveries[index];

            if (flow->previous == MQTT_SERVER_NONE) {
                entry->incoming_head = flow->next;
            } else {
                server->storage.deliveries[flow->previous].next = flow->next;
            }
            if (flow->next != MQTT_SERVER_NONE) {
                server->storage.deliveries[flow->next].previous =
                    flow->previous;
            }
            entry->incoming_count -= 1U;
            mqtt_route_delivery_give(server, index);
        } else {
            /* It is there already, or gone already. */
        }
    } else {
        /* Its Session ends later in the journal. */
    }
    return status;
}

static mqtt_status apply_mark(mqtt_restore *restore, mqtt_reader *reader)
{
    /** @brief The milliseconds, or the number of records. */
    const uint64_t first = apply_u64(reader);
    /** @brief The next store id of an END. */
    const uint64_t next =
        (restore->kind == (uint8_t)MQTT_STORE_RECORD_END) ? apply_u64(reader) :
                                                            0U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (!apply_done(reader)) {
        status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_CONTENT);
    } else if (restore->kind == (uint8_t)MQTT_STORE_RECORD_END) {
        if (next > restore->next_id) {
            restore->next_id = next;
        }
    } else {
        restore->clock =
            restore->segment_offset +
            ((first < APPLY_LEFT_MAX) ? first : APPLY_LEFT_MAX);
    }
    return status;
}

mqtt_status mqtt_restore_apply(mqtt_restore *restore, const uint8_t *content,
                               uint32_t length)
{
    /** @brief The content. */
    mqtt_reader reader = {NULL, 0U, 0U, MQTT_OK};
    /** @brief The kind. */
    const uint32_t kind = (uint32_t)restore->kind;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    mqtt_reader_init(&reader, content, length);
    if (kind == MQTT_STORE_RECORD_SESSION) {
        status = apply_session(restore, &reader);
    } else if (kind == MQTT_STORE_RECORD_SESSION_REMOVE) {
        /** @brief The Client Identifier. */
        const mqtt_utf8 client = mqtt_read_utf8(&reader);
        /** @brief The Session. */
        const uint32_t session = apply_session_of(restore, &client);

        if (!apply_done(&reader)) {
            status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_CONTENT);
        } else if (session != MQTT_SERVER_NONE) {
            apply_end_session(restore, session);
        } else {
            /* It ended before the snapshot reached it. */
        }
    } else if ((kind == MQTT_STORE_RECORD_SUBSCRIBE) ||
               (kind == MQTT_STORE_RECORD_UNSUBSCRIBE)) {
        status = apply_subscribe(restore, &reader,
                                 kind == MQTT_STORE_RECORD_SUBSCRIBE);
    } else if ((kind == MQTT_STORE_RECORD_WILL) ||
               (kind == MQTT_STORE_RECORD_WILL_REMOVE)) {
        status = apply_will(restore, &reader, kind == MQTT_STORE_RECORD_WILL);
    } else if ((kind == MQTT_STORE_RECORD_MESSAGE_GONE) ||
               (kind == MQTT_STORE_RECORD_MESSAGE_EXPIRY)) {
        status = apply_message_change(restore, &reader,
                                      kind == MQTT_STORE_RECORD_MESSAGE_GONE);
    } else if ((kind == MQTT_STORE_RECORD_RETAINED) ||
               (kind == MQTT_STORE_RECORD_RETAINED_REMOVE)) {
        status = apply_retained(restore, &reader,
                                kind == MQTT_STORE_RECORD_RETAINED);
    } else if (kind == MQTT_STORE_RECORD_DELIVERY) {
        status = apply_delivery(restore, &reader);
    } else if ((kind == MQTT_STORE_RECORD_SENT) ||
               (kind == MQTT_STORE_RECORD_RELEASED) ||
               (kind == MQTT_STORE_RECORD_FINISHED)) {
        status = apply_flow(restore, &reader);
    } else if ((kind == MQTT_STORE_RECORD_INCOMING) ||
               (kind == MQTT_STORE_RECORD_INCOMING_REMOVE)) {
        status = apply_incoming(restore, &reader,
                                kind == MQTT_STORE_RECORD_INCOMING);
    } else if ((kind == MQTT_STORE_RECORD_COMMIT) ||
               (kind == MQTT_STORE_RECORD_END)) {
        status = apply_mark(restore, &reader);
    } else if ((kind & MQTT_STORE_RECORD_OPTIONAL) != 0U) {
        /* A later minor version's record, which this one may skip. */
    } else {
        status = mqtt_restore_fail(restore, MQTT_STORE_FAULT_CONTENT);
    }
    return status;
}
