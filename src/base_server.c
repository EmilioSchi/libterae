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
 * @file base_server.c
 * @brief The MQTT Server: reading packets and deciding what they mean.
 *
 * The checks go from the cheapest to the dearest: those of the Fixed Header
 * come before any memory is used for a packet, and a packet is only parsed
 * once it has fully arrived.
 *
 * Replies go through mqtt_server_send, and a failure to find room for one
 * closes the connection. See mqtt_server_receive_capacity for why that should
 * not happen with a Client that reads.
 *
 * CONNECT and AUTH packets are handled in base_server_connect.c, and PUBLISH
 * packets and their acknowledgements in base_server_publish.c.
 *
 * [V31] MQTT V3.1 Protocol Specification.
 * @li Cited: 3.1-3.14, 4.
 * @li Source: public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015.
 * @li Cited: 3.1-3.14, 4.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 3.1-3.15, 4, p.42-117.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */

#include <stddef.h>

#include "base_client_id.h"
#include "base_flags.h"
#include "base_qos.h"
#include "base_remaining.h"
#include "base_server_internal.h"
#include "base_string.h"
#include "base_topic.h"
#include "base_topic_check.h"
#include "base_type.h"
#include "v311_suback.h"
#include "v5_payload.h"
#include "v5_prop.h"
#include "v5_prop_value.h"
#include "v5_properties.h"
#include "v5_rc.h"
#include "v5_session.h"
#include "v5_subscribe.h"
#include "v5_type.h"

/**
 * @brief How long a closing connection may take to send what it still has, in
 * milliseconds.
 */
#define SERVER_CLOSING_LIMIT_MS 5000U
/** @brief How often the timers run, in milliseconds. */
#define SERVER_SWEEP_MS 1000U
/**
 * @brief Number of arenas of objects of one type: connections, Sessions,
 * deliveries, messages, subscriptions, groups and tree nodes.
 */
#define SERVER_OBJECT_ARENAS 7U
/**
 * @brief What mqtt_server_meta_words gives for more words of page metadata
 * than a 32-bit count holds, which no storage can have.
 */
#define SERVER_META_TOO_MANY 0xFFFFFFFFU
/**
 * @brief Number of bytes used on the stack for a reply; longer ones use the
 * arena.
 */
#define SERVER_REPLY_BYTES 256U
/**
 * @brief Number of filters a SUBSCRIBE may have before its replies are kept in
 * the arena.
 */
#define SERVER_SUBSCRIBE_STACK 64U

mqtt_status mqtt_server_send(mqtt_server *server, uint32_t connection,
                             const uint8_t *bytes, uint32_t length)
{
    /** @brief The connection. */
    mqtt_connection *entry = &server->storage.connections[connection];
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (length > entry->peer_maximum_packet_size) {
        /* [MQTT-3.1.2-24] */
        status = MQTT_PACKET_TOO_LARGE;
    } else if (length >
               mqtt_server_room(server, entry, MQTT_SERVER_FOR_REPLY)) {
        status = MQTT_BUFFER_TOO_SMALL;
    } else {
        if ((length > 0U) && mqtt_journal_holds(bytes[0])) {
            mqtt_journal_hold(server, connection);
        }
        status = mqtt_ring_append(&entry->ring, bytes, length);
        mqtt_route_mark_pending(server, connection);
    }
    return status;
}

void mqtt_server_begin_closing(mqtt_server *server, uint32_t connection)
{
    /** @brief The connection. */
    mqtt_connection *entry = &server->storage.connections[connection];

    if (entry->state != MQTT_CONNECTION_CLOSING) {
        entry->state = MQTT_CONNECTION_CLOSING;
        entry->last_received_ms = server->now_ms;
        mqtt_route_mark_pending(server, connection);
    }
}

/**
 * @brief Do what mqtt_server_retained_next does, for arguments known to be
 * valid.
 * @param[in] server The server.
 * @param[in] now_ms The time.
 * @param[in,out] cursor Where to look from. Set this to zero to start.
 * @param[out] message The message.
 * @return true if there was one, or false if there are no more.
 */
static bool server_retained_next_body(const mqtt_server *server,
                                      uint64_t now_ms, uint32_t *cursor,
                                      mqtt_server_retained *message)
{
    /** @brief Index of the node being looked at. */
    uint32_t index = *cursor;
    /** @brief true once one has been found. */
    bool found = false;

    if (mqtt_server_ready(server)) {
        index = mqtt_tree_next(&server->tree, index);
    }
    while (mqtt_server_ready(server) && (!found) &&
           (index != MQTT_TREE_NONE)) {
        /** @brief The retained message here. */
        const uint32_t held = server->tree.nodes[index].retained;

        if (held != MQTT_SERVER_NONE) {
            /** @brief The message. */
            const mqtt_message *entry = &server->storage.messages[held];

            /* One that has expired stays for the timers to remove, and is as
             * good as gone. */
            if ((entry->expiry_ms == 0U) || (entry->expiry_ms > now_ms)) {
                /** @brief Where its bytes are. */
                const uint8_t *bytes =
                    mqtt_page_arena_bytes(&server->arena, entry->block);
                /** @brief Number of bytes in the Topic Name. */
                const uint32_t topic = (uint32_t)entry->topic_length;

                message->topic_name.length = entry->topic_length;
                message->topic_name.data = bytes;
                message->properties.length = entry->properties_length;
                message->properties.data = &bytes[topic];
                message->payload.length = entry->payload_length;
                message->payload.data =
                    &bytes[topic + entry->properties_length];
                message->expiry_left_ms =
                    (entry->expiry_ms == 0U) ? 0U : (entry->expiry_ms - now_ms);
                message->qos = entry->qos;
                found = true;
            }
        }
        index = mqtt_tree_next(&server->tree, index + 1U);
    }
    *cursor = index;
    return found;
}

bool mqtt_server_retained_next(const mqtt_server *server, uint64_t now_ms,
                               uint32_t *cursor, mqtt_server_retained *message)
{
    /** @brief The answer. */
    bool answer = false;

    if ((cursor != NULL) && (message != NULL)) {
        answer = server_retained_next_body(server, now_ms, cursor, message);
    }
    return answer;
}

/**
 * @brief Decide whether the Properties of a retained message put back are ones
 * a stored message can have: valid for a PUBLISH, and without what is taken
 * out when a message is stored.
 * @param[in] properties The Properties, without their length.
 * @return true if they are.
 */
static bool server_retained_properties(const mqtt_payload *properties)
{
    /** @brief The Properties, decoded. */
    mqtt5_properties decoded;
    /** @brief The answer. */
    bool valid = true;

    if (properties->length > 0U) {
        valid = (mqtt5_properties_decode_block(properties->data,
                                               properties->length,
                                               MQTT_PUBLISH,
                                               &decoded) == MQTT_OK) &&
                (!mqtt5_properties_has(&decoded, MQTT5_PROP_TOPIC_ALIAS)) &&
                (!mqtt5_properties_has(&decoded,
                                       MQTT5_PROP_SUBSCRIPTION_IDENTIFIER)) &&
                (!mqtt5_properties_has(&decoded,
                                       MQTT5_PROP_MESSAGE_EXPIRY_INTERVAL));
    }
    return valid;
}

mqtt_status mqtt_server_retained_restore(mqtt_server *server,
                                         const mqtt_server_retained *message,
                                         uint64_t now_ms)
{
    /** @brief The outcome. This takes what any version could have sent. */
    mqtt_status status =
        (mqtt_server_ready(server) && (message != NULL)) ?
            mqtt_topic_name_check(&message->topic_name, MQTT_VERSION_31) :
            MQTT_INVALID_ARGUMENT;

    if (status == MQTT_OK) {
        server->now_ms = now_ms;
    }
    if ((status == MQTT_OK) &&
        ((!server->config.retain_available) ||
         (message->payload.length == 0U) || (message->qos > MQTT_QOS_2))) {
        status = MQTT_INVALID_ARGUMENT;
    }
    if (status == MQTT_OK) {
        if (!server_retained_properties(&message->properties)) {
            status = MQTT_INVALID_ARGUMENT;
        }
    }
    if (status == MQTT_OK) {
        /** @brief The message. */
        mqtt_route_source source;
        /** @brief The run that holds its Payload. */
        mqtt_run run = {NULL, 0U};

        source.topic_name = message->topic_name;
        source.properties = message->properties;
        mqtt_route_payload(&source, &run, message->payload.data,
                           message->payload.length);
        source.expiry_ms = (message->expiry_left_ms == 0U) ?
                               0U :
                               (now_ms + message->expiry_left_ms);
        source.message = MQTT_SERVER_NONE;
        source.publisher = MQTT_SERVER_NONE;
        source.qos = message->qos;
        source.retain = true;
        source.properties_raw = false;
        source.lost = false;
        if (mqtt_route_too_large(server, &source)) {
            status = MQTT_QUOTA_EXCEEDED;
        } else {
            status = mqtt_session_retain(server, &source);
            mqtt_route_release(server, source.message);
        }
    }
    return status;
}

/**
 * @brief Find out whether an index the application gave names a connection that
 * is open.
 *
 * Every call that takes a connection checks this first, so an index that was
 * never handed out, or one already closed, does nothing rather than reach past
 * the table or into an entry that is free.
 *
 * @param[in] server The server.
 * @param[in] connection The index.
 * @return true if it is in the table and in use.
 */
static bool server_connection_in_use(const mqtt_server *server,
                                     uint32_t connection)
{
    return mqtt_server_connection_ready(server, connection);
}

bool mqtt_server_ready(const mqtt_server *server)
{
    return (server != NULL) && mqtt_bool_is_set(server->ready);
}

bool mqtt_server_connection_ready(const mqtt_server *server,
                                  uint32_t connection)
{
    return mqtt_server_ready(server) &&
           (connection < server->storage.connection_count) &&
           mqtt_bool_is_set(server->storage.connections[connection].open) &&
           (server->storage.connections[connection].state !=
            MQTT_CONNECTION_FREE);
}

void mqtt_server_disconnect(mqtt_server *server, uint32_t connection,
                            uint8_t reason_code)
{
    if (server_connection_in_use(server, connection)) {
        /** @brief The connection. */
        const mqtt_connection *entry = &server->storage.connections[connection];

        /* A DISCONNECT packet may only follow a CONNACK with a Reason Code
         * below 0x80. */
        if ((entry->state == MQTT_CONNECTION_ACTIVE) &&
            (entry->version == MQTT_VERSION_5)) {
            /** @brief The bytes. */
            uint8_t bytes[4] = {0};
            /** @brief The writer. */
            mqtt_writer writer = {NULL, 0U, 0U, MQTT_OK};
            /** @brief The packet. */
            mqtt_reason packet;

            mqtt_reason_clear(&packet);
            /* A code a DISCONNECT can't carry, as the application may give one,
             * becomes the general one: [MQTT-3.14.2-1]. */
            packet.reason_code =
                mqtt5_reason_code_is_valid(MQTT_DISCONNECT, reason_code) ?
                    reason_code :
                    (uint8_t)MQTT5_RC_UNSPECIFIED_ERROR;
            mqtt_writer_init(&writer, bytes, 4U);
            mqtt_disconnect_encode(&writer, MQTT_VERSION_5, &packet);
            /* It closes all the same, and an operator can see that a reason did
             * not get through. */
            if (mqtt_server_send(server, connection, bytes, writer.position) !=
                MQTT_OK) {
                server->statistics.replies_lost += 1U;
            }
        }
        mqtt_server_begin_closing(server, connection);
    }
}

void mqtt_server_fail(mqtt_server *server, uint32_t connection,
                      mqtt_status status)
{
    /** @brief The connection. */
    const mqtt_connection *entry = &server->storage.connections[connection];

    server->statistics.protocol_errors += 1U;
    if (entry->state == MQTT_CONNECTION_ACTIVE) {
        mqtt_server_disconnect(server, connection, mqtt5_reason_code(status));
    } else {
        mqtt_server_begin_closing(server, connection);
    }
}

/**
 * @brief Ask the application whether a Client may subscribe to a filter.
 * @param[in] server The server.
 * @param[in] identifier The Client Identifier.
 * @param[in] filter The Topic Filter.
 * @return true if it may, which is the answer when nothing is there to ask.
 */
static bool server_may_subscribe(const mqtt_server *server,
                                 const mqtt_utf8 *identifier,
                                 const mqtt_utf8 *filter)
{
    /** @brief The answer. */
    bool allowed = true;

    if (server->config.authorize_subscribe != NULL) {
        allowed = server->config.authorize_subscribe(server->config.user_tag,
                                                     identifier, filter);
    }
    return allowed;
}

/**
 * @brief Decide whether a filter with the share prefix is shared on a
 * connection.
 * @param[in] server The server.
 * @param[in] version The connection's protocol.
 * @return true if it is.
 */
static bool server_shares_filters(const mqtt_server *server,
                                  mqtt_version version)
{
    return (version == MQTT_VERSION_5) ||
           ((version == MQTT_VERSION_311) &&
            server->config.shared_subscription_311);
}

/**
 * @brief Decide whether a filter has a wildcard.
 * @param[in] filter The Topic Filter.
 * @return true if it has one.
 */
static bool server_filter_has_wildcard(const mqtt_utf8 *filter)
{
    /** @brief Index of the byte being looked at. */
    uint32_t index = 0U;
    /** @brief The answer. */
    bool found = false;

    while ((!found) && (index < (uint32_t)filter->length)) {
        /** @brief The byte being looked at. */
        const uint32_t byte = (uint32_t)filter->data[index];

        found = (byte == MQTT_MULTI_LEVEL_WILDCARD) ||
                (byte == MQTT_SINGLE_LEVEL_WILDCARD);
        index += 1U;
    }
    return found;
}

/**
 * @brief Give a Session the subscription an entry of a SUBSCRIBE packet asks
 * for, and queue the retained messages it should get.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] filter The Topic Filter.
 * @param[in] inner The filter without its share prefix.
 * @param[in] options The Subscription Options.
 * @param[in] identifier The Subscription Identifier, or zero.
 * @param[in] shared true for a Shared Subscription.
 * @return The QoS granted, or MQTT5_RC_QUOTA_EXCEEDED.
 */
static uint32_t server_subscribe_grant(mqtt_server *server, uint32_t connection,
                                       const mqtt_utf8 *filter,
                                       const mqtt_utf8 *inner, uint8_t options,
                                       uint32_t identifier, bool shared)
{
    /** @brief The connection. */
    const mqtt_connection *entry = &server->storage.connections[connection];
    /** @brief The Session. */
    const uint32_t session = entry->session;
    /** @brief The protocol in use. */
    const mqtt_version version = entry->version;
    /** @brief The QoS asked for. */
    const uint8_t requested =
        (uint8_t)((uint32_t)options & MQTT5_MAXIMUM_QOS_MASK);
    /** @brief The QoS granted. */
    const uint8_t granted = (requested < server->config.maximum_qos) ?
                                requested : server->config.maximum_qos;
    /** @brief true if the Session did not have this subscription. */
    bool was_new = false;
    /** @brief The other option bits. They only exist in MQTT 5.0. */
    const uint32_t others = (uint32_t)options & ~MQTT5_MAXIMUM_QOS_MASK;
    /** @brief The options as stored. */
    const uint32_t kept = (version == MQTT_VERSION_5) ?
                              (others | (uint32_t)granted) :
                              (uint32_t)granted;
    /** @brief The code to send. */
    uint32_t code = MQTT5_RC_SUCCESS;

    if (mqtt_session_subscribe(server, session, filter, (uint8_t)kept,
                               identifier, shared, &was_new) != MQTT_OK) {
        code = MQTT5_RC_QUOTA_EXCEEDED;
    } else {
        /** @brief Retain Handling. */
        const uint32_t handling =
            (kept & MQTT5_RETAIN_HANDLING_MASK) >> MQTT5_RETAIN_HANDLING_POS;

        code = granted;
        /* A Shared Subscription gets no retained messages when it starts. */
        if ((!shared) && ((handling == MQTT5_RETAIN_HANDLING_0) ||
                          ((handling == MQTT5_RETAIN_HANDLING_1) && was_new))) {
            mqtt_session_send_retained(server, session, inner, granted,
                                       identifier);
        }
    }
    return code;
}

/**
 * @brief Handle one entry of a SUBSCRIBE packet.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] filter The Topic Filter.
 * @param[in] options The Subscription Options.
 * @param[in] identifier The Subscription Identifier, or zero.
 * @return The value for this entry in the SUBACK packet, in the version's
 * terms.
 */
static uint8_t server_subscribe_one(mqtt_server *server, uint32_t connection,
                                    const mqtt_utf8 *filter, uint8_t options,
                                    uint32_t identifier)
{
    /** @brief The connection. */
    const mqtt_connection *entry = &server->storage.connections[connection];
    /** @brief The Session. */
    const uint32_t session = entry->session;
    /** @brief The protocol in use. */
    const mqtt_version version = entry->version;
    /** @brief The ShareName, if the filter is shared. */
    mqtt_utf8 share_name = {0U, NULL};
    /** @brief The filter without its share prefix. */
    mqtt_utf8 inner = *filter;
    /** @brief The code to send. */
    uint32_t code = MQTT5_RC_SUCCESS;
    /** @brief The Client Identifier, for the authorization callback. */
    const mqtt_utf8 client = mqtt_session_identifier(server, session);

    if (mqtt_topic_filter_check(filter, version) != MQTT_OK) {
        code = MQTT5_RC_TOPIC_FILTER_INVALID;
    } else {
        /**
         * @brief true if a Shared Subscription, which only MQTT 5.0 defines.
         */
        bool shared = false;

        if (server_shares_filters(server, version)) {
            shared = mqtt_topic_share_split(filter, &share_name, &inner);
        }
        if (shared && (!server->config.shared_subscription_available)) {
            code = MQTT5_RC_SHARED_SUBSCRIPTIONS_NOT_SUPPORTED;
        } else if ((!server->config.wildcard_subscription_available) &&
                   server_filter_has_wildcard(&inner)) {
            code = MQTT5_RC_WILDCARD_SUBSCRIPTIONS_NOT_SUPPORTED;
        } else if ((identifier != 0U) &&
                   (!server->config.subscription_identifier_available)) {
            code = MQTT5_RC_SUBSCRIPTION_IDENTIFIERS_NOT_SUPPORTED;
        } else if (!server_may_subscribe(server, &client, filter)) {
            /* MQTT 3.1 can't say so, and asks for a SUBACK that reads as if
             * this had worked. */
            code = (version == MQTT_VERSION_31) ?
                       ((uint32_t)options & MQTT5_MAXIMUM_QOS_MASK) :
                       MQTT5_RC_NOT_AUTHORIZED;
        } else {
            code = server_subscribe_grant(server, connection, filter, &inner,
                                          options, identifier, shared);
        }
    }
    if ((version != MQTT_VERSION_5) && (code >= MQTT5_RC_FAILURE_MIN)) {
        code = MQTT311_RC_FAILURE;
    }
    return (uint8_t)code;
}

/**
 * @brief Write a SUBACK or UNSUBACK packet into a connection's ring.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] version The protocol in use.
 * @param[in] reply The packet.
 * @param[in] type MQTT_SUBACK or MQTT_UNSUBACK.
 * @return MQTT_OK, or the reason to close the connection.
 */
static mqtt_status server_send_subscribe_reply(
    mqtt_server *server, uint32_t connection, mqtt_version version,
    const mqtt_subscribe_response *reply, uint32_t type)
{
    /** @brief A writer that only counts. */
    mqtt_writer counter = {NULL, 0U, 0U, MQTT_OK};
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    mqtt_writer_init(&counter, NULL, 0U);
    if (type == MQTT_SUBACK) {
        mqtt_suback_encode(&counter, version, reply);
    } else {
        mqtt_unsuback_encode(&counter, version, reply);
    }
    if (counter.position >
        mqtt_server_room(server, &server->storage.connections[connection],
                         MQTT_SERVER_FOR_REPLY)) {
        status = MQTT_BUFFER_TOO_SMALL;
    } else {
        /**
         * @brief An arena block for the bytes of the reply, when there are
         * many.
         */
        const uint32_t block =
            (counter.position > SERVER_REPLY_BYTES) ?
                mqtt_page_arena_take(&server->arena, counter.position) :
                MQTT_PAGE_ARENA_NONE;

        if ((counter.position > SERVER_REPLY_BYTES) &&
            (block == MQTT_PAGE_ARENA_NONE)) {
            status = MQTT_QUOTA_EXCEEDED;
        } else {
            /** @brief The bytes of the reply, when there are few. */
            uint8_t bytes[SERVER_REPLY_BYTES];
            /** @brief The writer. */
            mqtt_writer writer = {NULL, 0U, 0U, MQTT_OK};
            /** @brief Where the bytes go. */
            uint8_t *destination =
                (block == MQTT_PAGE_ARENA_NONE) ?
                    bytes :
                    mqtt_page_arena_bytes(&server->arena, block);

            mqtt_writer_init(&writer, destination, counter.position);
            if (type == MQTT_SUBACK) {
                mqtt_suback_encode(&writer, version, reply);
            } else {
                mqtt_unsuback_encode(&writer, version, reply);
            }
            status = mqtt_server_send(server, connection, destination,
                                      writer.position);
            mqtt_page_arena_give(&server->arena, block);
        }
    }
    return status;
}

/**
 * @brief Handle the entries of a SUBSCRIBE packet in order, stopping at one
 * that has to close the connection.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] packet The packet.
 * @param[out] codes Where the replies go, with room for every entry.
 * @param[out] replies Number of replies.
 * @return MQTT_OK, or the reason to close the connection.
 */
static mqtt_status server_subscribe_entries(mqtt_server *server,
                                            uint32_t connection,
                                            const mqtt_subscribe *packet,
                                            uint8_t *codes, uint32_t *replies)
{
    /** @brief The protocol in use. */
    const mqtt_version version =
        server->storage.connections[connection].version;
    /** @brief Number of replies so far. */
    uint32_t count = 0U;
    /** @brief A cursor over the Payload. */
    mqtt_reader cursor = {NULL, 0U, 0U, MQTT_OK};
    /** @brief An entry's Topic Filter. */
    mqtt_utf8 filter = {0U, NULL};
    /** @brief An entry's Subscription Options. */
    uint8_t options = 0U;
    /** @brief true while there is another entry to look at. */
    bool more = false;
    /** @brief The Subscription Identifier. */
    const uint32_t identifier =
        mqtt5_properties_has(&packet->properties,
                             MQTT5_PROP_SUBSCRIPTION_IDENTIFIER) ?
            packet->properties.subscription_identifier :
            0U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    mqtt_reader_init(&cursor, packet->payload.data, packet->payload.length);
    more = mqtt_subscribe_next(&cursor, &filter, &options);
    while (more) {
        codes[count] = server_subscribe_one(server, connection, &filter,
                                            options, identifier);
        count += 1U;
        if ((version == MQTT_VERSION_31) &&
            (codes[count - 1U] >= MQTT5_RC_FAILURE_MIN)) {
            /* The SUBACK of MQTT 3.1 can only hold a QoS, so this cannot be
             * said in one. */
            status = MQTT_PROTOCOL_ERROR;
            more = false;
        } else if ((version == MQTT_VERSION_5) &&
                   ((codes[count - 1U] ==
                     MQTT5_RC_SHARED_SUBSCRIPTIONS_NOT_SUPPORTED) ||
                    (codes[count - 1U] ==
                     MQTT5_RC_SUBSCRIPTION_IDENTIFIERS_NOT_SUPPORTED) ||
                    (codes[count - 1U] ==
                     MQTT5_RC_WILDCARD_SUBSCRIPTIONS_NOT_SUPPORTED))) {
            /* The CONNACK said these aren't on offer, so using one is a
             * Protocol Error with a code of its own: sections 3.2.2.3.11 to
             * 3.2.2.3.13, and [MQTT-4.13.1-1]. */
            mqtt_server_disconnect(server, connection, codes[count - 1U]);
            status = MQTT_PROTOCOL_ERROR;
            more = false;
        } else {
            more = mqtt_subscribe_next(&cursor, &filter, &options);
        }
    }
    *replies = count;
    return status;
}

/**
 * @brief Handle a SUBSCRIBE packet.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] body The bytes after the Fixed Header.
 * @param[in] length Remaining Length.
 * @return MQTT_OK, or the reason to close the connection.
 */
static mqtt_status server_handle_subscribe(mqtt_server *server,
                                           uint32_t connection,
                                           const uint8_t *body, uint32_t length)
{
    /** @brief The protocol in use. */
    const mqtt_version version =
        server->storage.connections[connection].version;
    /** @brief The packet. */
    mqtt_subscribe packet;
    /** @brief The outcome. */
    mqtt_status status = mqtt_subscribe_decode(body, length, version, &packet);

    if (status == MQTT_OK) {
        /** @brief Space for the replies of a SUBSCRIBE with few filters. */
        uint8_t stack_codes[SERVER_SUBSCRIBE_STACK];
        /** @brief Where the replies go. */
        uint8_t *codes = stack_codes;
        /** @brief An arena block, for a SUBSCRIBE with many filters. */
        uint32_t block = MQTT_PAGE_ARENA_NONE;
        /**
         * @brief An entry with an empty filter takes three bytes, which bounds
         * their number.
         */
        const uint32_t most = packet.payload.length / 3U;
        /** @brief Number of replies. */
        uint32_t count = 0U;

        if (most > SERVER_SUBSCRIBE_STACK) {
            block = mqtt_page_arena_take(&server->arena, most);
            if (block == MQTT_PAGE_ARENA_NONE) {
                status = MQTT_QUOTA_EXCEEDED;
            } else {
                codes = mqtt_page_arena_bytes(&server->arena, block);
            }
        }
        if (status == MQTT_OK) {
            status = server_subscribe_entries(server, connection, &packet,
                                              codes, &count);
        }
        if (status == MQTT_OK) {
            /** @brief The reply. */
            mqtt_subscribe_response reply;

            mqtt_subscribe_response_clear(&reply);
            reply.packet_identifier = packet.packet_identifier;
            reply.reason_codes.data = codes;
            reply.reason_codes.length = count;
            status = server_send_subscribe_reply(server, connection, version,
                                                 &reply, MQTT_SUBACK);
        }
        mqtt_page_arena_give(&server->arena, block);
    }
    return status;
}

/**
 * @brief Handle an UNSUBSCRIBE packet.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] body The bytes after the Fixed Header.
 * @param[in] length Remaining Length.
 * @return MQTT_OK, or the reason to close the connection.
 */
static mqtt_status server_handle_unsubscribe(mqtt_server *server,
                                             uint32_t connection,
                                             const uint8_t *body,
                                             uint32_t length)
{
    /** @brief The connection. */
    const mqtt_connection *entry = &server->storage.connections[connection];
    /** @brief The protocol in use. */
    const mqtt_version version = entry->version;
    /** @brief The packet. */
    mqtt_subscribe packet;
    /** @brief The outcome. */
    mqtt_status status =
        mqtt_unsubscribe_decode(body, length, version, &packet);

    if (status == MQTT_OK) {
        /** @brief Space for the replies of an UNSUBSCRIBE with few filters. */
        uint8_t stack_codes[SERVER_SUBSCRIBE_STACK];
        /** @brief Where the replies go. */
        uint8_t *codes = stack_codes;
        /** @brief An arena block, for an UNSUBSCRIBE with many filters. */
        uint32_t block = MQTT_PAGE_ARENA_NONE;
        /**
         * @brief An entry with an empty filter takes two bytes, which bounds
         * their number.
         */
        const uint32_t most = packet.payload.length / 2U;
        /** @brief Number of replies so far. */
        uint32_t count = 0U;
        /** @brief A cursor over the Payload. */
        mqtt_reader cursor = {NULL, 0U, 0U, MQTT_OK};
        /** @brief An entry's Topic Filter. */
        mqtt_utf8 filter = {0U, NULL};
        /** @brief true while there is another entry to look at. */
        bool more = false;

        /* Only an UNSUBACK of MQTT 5.0 has a code for each filter. */
        if ((version == MQTT_VERSION_5) && (most > SERVER_SUBSCRIBE_STACK)) {
            block = mqtt_page_arena_take(&server->arena, most);
            if (block == MQTT_PAGE_ARENA_NONE) {
                status = MQTT_QUOTA_EXCEEDED;
            } else {
                codes = mqtt_page_arena_bytes(&server->arena, block);
            }
        }
        mqtt_reader_init(&cursor, packet.payload.data, packet.payload.length);
        if (status == MQTT_OK) {
            more = mqtt_unsubscribe_next(&cursor, &filter);
        }
        while (more) {
            /** @brief The ShareName, if the filter is shared. */
            mqtt_utf8 share_name = {0U, NULL};
            /** @brief The filter without its share prefix. */
            mqtt_utf8 inner = filter;
            /** @brief The code to send. */
            uint32_t code = MQTT5_RC_NO_SUBSCRIPTION_EXISTED;

            if (mqtt_topic_filter_check(&filter, version) != MQTT_OK) {
                code = MQTT5_RC_TOPIC_FILTER_INVALID;
            } else {
                /**
                 * @brief true if a Shared Subscription, which only MQTT 5.0
                 * defines.
                 */
                bool shared = false;

                if (server_shares_filters(server, version)) {
                    shared =
                        mqtt_topic_share_split(&filter, &share_name, &inner);
                }
                if (mqtt_session_unsubscribe(server, entry->session, &filter,
                                             shared) == MQTT_OK) {
                    code = MQTT5_RC_SUCCESS;
                }
            }
            if (version == MQTT_VERSION_5) {
                codes[count] = (uint8_t)code;
                count += 1U;
            }
            more = mqtt_unsubscribe_next(&cursor, &filter);
        }
        if (status == MQTT_OK) {
            /** @brief The reply. */
            mqtt_subscribe_response reply;

            mqtt_subscribe_response_clear(&reply);
            reply.packet_identifier = packet.packet_identifier;
            if (version == MQTT_VERSION_5) {
                reply.reason_codes.data = codes;
                reply.reason_codes.length = count;
            }
            status = server_send_subscribe_reply(server, connection, version,
                                                 &reply, MQTT_UNSUBACK);
        }
        mqtt_page_arena_give(&server->arena, block);
    }
    return status;
}

/**
 * @brief Handle a DISCONNECT packet.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] body The bytes after the Fixed Header.
 * @param[in] length Remaining Length.
 * @return MQTT_OK, or the reason to close the connection.
 */
static mqtt_status server_handle_disconnect(mqtt_server *server,
                                            uint32_t connection,
                                            const uint8_t *body,
                                            uint32_t length)
{
    /** @brief The connection. */
    const mqtt_connection *entry = &server->storage.connections[connection];
    /** @brief The Session. */
    mqtt_session *session = &server->storage.sessions[entry->session];
    /** @brief The packet. */
    mqtt_reason packet;
    /** @brief The outcome. */
    mqtt_status status =
        mqtt_disconnect_decode(body, length, entry->version, &packet);

    if (status == MQTT_OK) {
        if (mqtt5_properties_has(&packet.properties,
                                 MQTT5_PROP_SESSION_EXPIRY_INTERVAL)) {
            /* A Session that was to end with its connection cannot be given
             * more time now. */
            if (entry->expiry_asked_zero &&
                (packet.properties.session_expiry_interval != 0U)) {
                status = MQTT_PROTOCOL_ERROR;
            } else {
                session->expiry_s =
                    (packet.properties.session_expiry_interval >
                     server->config.maximum_session_expiry) ?
                        server->config.maximum_session_expiry :
                        packet.properties.session_expiry_interval;
            }
        }
    }
    if (status == MQTT_OK) {
        /* Only a normal disconnection discards the Will Message:
         * [MQTT-3.14.4-3]. */
        if ((packet.reason_code == MQTT5_RC_SUCCESS) &&
            (session->will_message != MQTT_SERVER_NONE)) {
            mqtt_journal_will_remove(server, entry->session);
            mqtt_route_release(server, session->will_message);
            session->will_message = MQTT_SERVER_NONE;
        }
        mqtt_server_begin_closing(server, connection);
    }
    return status;
}

void mqtt_server_start_expiry(mqtt_server *server, uint32_t session)
{
    /** @brief The Session. */
    mqtt_session *entry = &server->storage.sessions[session];

    if (entry->expiry_s != MQTT5_SESSION_EXPIRY_NEVER) {
        entry->expiry_due_ms =
            server->now_ms + ((uint64_t)entry->expiry_s * 1000U);
    }
}

void mqtt_server_session_timers(mqtt_server *server, uint32_t session)
{
    /** @brief The Session. */
    const mqtt_session *entry = &server->storage.sessions[session];

    if (entry->connection == MQTT_SERVER_NONE) {
        if ((entry->will_due_ms != 0U) &&
            (server->now_ms >= entry->will_due_ms)) {
            mqtt_session_publish_will(server, session);
        }
        if ((entry->expiry_due_ms != 0U) &&
            (server->now_ms >= entry->expiry_due_ms)) {
            mqtt_session_discard(server, session);
        }
    }
}

/**
 * @brief Handle a PINGREQ packet.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] length Remaining Length.
 * @return MQTT_OK, or the reason to close the connection.
 */
static mqtt_status server_handle_pingreq(mqtt_server *server,
                                         uint32_t connection, uint32_t length)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (length != 0U) {
        status = MQTT_MALFORMED_PACKET;
    } else {
        /** @brief The bytes. */
        uint8_t bytes[2] = {0};
        /** @brief The writer. */
        mqtt_writer writer = {NULL, 0U, 0U, MQTT_OK};

        mqtt_writer_init(&writer, bytes, 2U);
        mqtt_pingresp_encode(&writer);
        status = mqtt_server_send(server, connection, bytes, writer.position);
    }
    return status;
}

/**
 * @brief Handle a whole packet from a connection whose CONNECT packet has been
 * accepted.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] header The Fixed Header.
 * @param[in] body The bytes after the Fixed Header.
 * @return MQTT_OK, or the reason to close the connection.
 */
static mqtt_status server_handle_active(mqtt_server *server,
                                        uint32_t connection,
                                        const mqtt_fixed_header *header,
                                        const uint8_t *body)
{
    /** @brief The packet type. */
    const uint32_t type = (uint32_t)header->type;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    switch (type) {
    case MQTT_PUBLISH:
        status = mqtt_publish_handle(server, connection, header, body);
        break;
    case MQTT_PUBACK:
    case MQTT_PUBREC:
    case MQTT_PUBREL:
    case MQTT_PUBCOMP:
        status = mqtt_publish_handle_response(server, connection, header, body);
        break;
    case MQTT_SUBSCRIBE:
        status = server_handle_subscribe(server, connection, body,
                                         header->remaining_length);
        break;
    case MQTT_UNSUBSCRIBE:
        status = server_handle_unsubscribe(server, connection, body,
                                           header->remaining_length);
        break;
    case MQTT_PINGREQ:
        status =
            server_handle_pingreq(server, connection, header->remaining_length);
        break;
    case MQTT_DISCONNECT:
        status = server_handle_disconnect(server, connection, body,
                                          header->remaining_length);
        break;
    case MQTT5_AUTH:
        status = mqtt_connect_handle_auth(server, connection, body,
                                          header->remaining_length);
        break;
    default:
        /* A second CONNECT, or a packet only a Server sends. */
        status = MQTT_PROTOCOL_ERROR;
        break;
    }
    return status;
}

mqtt_status mqtt_server_handle_packet(mqtt_server *server, uint32_t connection,
                                      const mqtt_fixed_header *header,
                                      const uint8_t *body)
{
    /** @brief The connection. */
    mqtt_connection *entry = &server->storage.connections[connection];
    /** @brief The packet type. */
    const uint32_t type = (uint32_t)header->type;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /**
     * @brief true if what handled the packet has told the peer of any failure
     * itself.
     */
    bool reported = false;

    entry->last_received_ms = server->now_ms;
    if (entry->state == MQTT_CONNECTION_WAIT_CONNECT) {
        /* It reports its own failures, and has already started the close. */
        reported = true;
        status = mqtt_connect_handle(server, connection, header, body);
    } else if (entry->state == MQTT_CONNECTION_AUTHENTICATING) {
        if (type == MQTT5_AUTH) {
            /* It reports its own failures, and has already started the
             * close. */
            reported = true;
            status = mqtt_connect_handle_auth(server, connection, body,
                                              header->remaining_length);
        } else if (type == MQTT_DISCONNECT) {
            mqtt_server_begin_closing(server, connection);
        } else {
            mqtt_connect_refuse(server, connection, MQTT_VERSION_5,
                                (uint8_t)MQTT5_RC_PROTOCOL_ERROR);
            status = MQTT_PROTOCOL_ERROR;
        }
    } else {
        status = server_handle_active(server, connection, header, body);
    }
    if ((status != MQTT_OK) && (!reported)) {
        mqtt_server_fail(server, connection, status);
    }
    return status;
}

/**
 * @brief Do what mqtt_server_config_default does, for arguments known to be
 * valid.
 * @param[out] config The configuration.
 */
static void server_config_default_body(mqtt_server_config *config)
{
    /* Every field is written: the memory may hold anything. */
    config->user_tag = 0U;
    config->authenticate = NULL;
    config->auth_step = NULL;
    config->authorize_publish = NULL;
    config->authorize_subscribe = NULL;
    config->random_bytes = NULL;
    config->maximum_packet_size = MQTT_REMAINING_LENGTH_MAX;
    /* Holds any CONNECT with the longest of each of its strings, and room for
     * Properties. */
    config->maximum_connect_size = 1024U * 1024U;
    /* As many as maximum_inflight. Each QoS 2 flow a Session holds is a
     * delivery from the pool every Session shares, found by a walk, so the
     * most one Session may hold is kept small. Below 65,535, CONNACK tells
     * Clients of MQTT 5.0. */
    config->receive_maximum = 1024U;
    config->maximum_inflight = 1024U;
    config->maximum_queued = 4096U;
    config->maximum_session_expiry = MQTT5_SESSION_EXPIRY_NEVER;
    config->persistent_session_expiry = MQTT5_SESSION_EXPIRY_NEVER;
    config->maximum_client_identifier = 256U;
    config->shared_subscription_311 = false;
    config->dollar_publish_allowed = false;
    config->connect_timeout_ms = 30000U;
    config->retry_interval_ms = 30000U;
    config->topic_alias_maximum = 16U;
    config->server_keep_alive = 0U;
    config->maximum_qos = MQTT_QOS_2;
    config->retain_available = true;
    config->wildcard_subscription_available = true;
    config->subscription_identifier_available = true;
    config->shared_subscription_available = true;
    config->report_no_matching_subscribers = false;
    config->pool_soft_quota = 4U;
    config->pool_hard_quota = 16U;
    config->pool_reserve = 64U;
    config->pool_borrow_reserve = 128U;
    config->store_append = NULL;
    config->store_sessions = MQTT_STORE_MEMORY;
    config->store_messages = MQTT_STORE_MEMORY;
    /* About a second of a busy server's messages, and little to read back
     * after a crash. */
    config->store_snapshot_bytes = 64U * 1024U * 1024U;
}

void mqtt_server_config_default(mqtt_server_config *config)
{
    if (config != NULL) {
        server_config_default_body(config);
    }
}

/**
 * @brief Find the number of pages of each arena of objects of one type.
 * @param[in] storage The storage.
 * @param[out] pages The pages of the connections, Sessions, deliveries,
 * messages, subscriptions, groups and tree nodes, in that order.
 */
static void server_arena_pages(const mqtt_server_storage *storage,
                               uint32_t *pages)
{
    pages[0] = mqtt_slab_pages(MQTT_SERVER_PAGE_CONNECTIONS,
                               storage->connection_count);
    pages[1] =
        mqtt_slab_pages(MQTT_SERVER_PAGE_SESSIONS, storage->session_count);
    pages[2] =
        mqtt_slab_pages(MQTT_SERVER_PAGE_DELIVERIES, storage->delivery_count);
    pages[3] =
        mqtt_slab_pages(MQTT_SERVER_PAGE_MESSAGES, storage->message_count);
    pages[4] = mqtt_slab_pages(MQTT_SERVER_PAGE_SUBSCRIPTIONS,
                               storage->subscription_count);
    pages[5] = mqtt_slab_pages(MQTT_SERVER_PAGE_GROUPS, storage->group_count);
    pages[6] = mqtt_slab_pages(MQTT_TREE_PAGE_NODES, storage->node_count);
}

/**
 * @brief Do what mqtt_server_meta_words does, for arguments known to be valid.
 * @param[in] storage The storage, whose counts and numbers of pages are set.
 * @return Number of words for page_meta, or 0xFFFFFFFF if no array could hold
 * them.
 */
static uint32_t server_meta_words_body(const mqtt_server_storage *storage)
{
    /** @brief The pages of each arena of objects. */
    uint32_t pages[SERVER_OBJECT_ARENAS];
    /** @brief The pages of every arena. */
    uint64_t total = (uint64_t)storage->topic_text_pages +
                     (uint64_t)storage->arena_pages;
    /** @brief Index of the arena. */
    uint32_t index = 0U;

    server_arena_pages(storage, pages);
    while (index < SERVER_OBJECT_ARENAS) {
        total += (uint64_t)pages[index];
        index += 1U;
    }
    total *= (uint64_t)MQTT_PAGE_META_WORDS;
    return (total >= (uint64_t)SERVER_META_TOO_MANY) ? SERVER_META_TOO_MANY :
                                                       (uint32_t)total;
}

uint32_t mqtt_server_meta_words(const mqtt_server_storage *storage)
{
    /** @brief The answer. */
    uint32_t answer = SERVER_META_TOO_MANY;

    if (storage != NULL) {
        answer = server_meta_words_body(storage);
    }
    return answer;
}

/**
 * @brief Give each arena its share of the page metadata, and set the arenas
 * and the tree up.
 * @param[in,out] server The server.
 * @return MQTT_OK or MQTT_INVALID_ARGUMENT.
 */
static mqtt_status server_init_arenas(mqtt_server *server)
{
    /** @brief The memory. */
    const mqtt_server_storage *storage = &server->storage;
    /**
     * @brief The slabs of the arenas of objects, in the order of
     * server_arena_pages, apart from the tree's.
     */
    mqtt_slab *const slabs[SERVER_OBJECT_ARENAS - 1U] = {
        &server->connection_slots, &server->session_slots,
        &server->delivery_slots, &server->message_slots,
        &server->subscription_slots, &server->group_slots};
    /** @brief The slots in a page of each. */
    static const uint32_t page_slots[SERVER_OBJECT_ARENAS - 1U] = {
        MQTT_SERVER_PAGE_CONNECTIONS, MQTT_SERVER_PAGE_SESSIONS,
        MQTT_SERVER_PAGE_DELIVERIES, MQTT_SERVER_PAGE_MESSAGES,
        MQTT_SERVER_PAGE_SUBSCRIPTIONS, MQTT_SERVER_PAGE_GROUPS};
    /** @brief The number of objects in each. */
    const uint32_t counts[SERVER_OBJECT_ARENAS - 1U] = {
        storage->connection_count, storage->session_count,
        storage->delivery_count, storage->message_count,
        storage->subscription_count, storage->group_count};
    /** @brief The pages of each arena of objects. */
    uint32_t pages[SERVER_OBJECT_ARENAS];
    /** @brief The tree's memory. */
    mqtt_tree_storage tree;
    /** @brief The first word of the next arena's share. */
    uint32_t word = 0U;
    /** @brief Index of the arena being set up. */
    uint32_t index = 0U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    server_arena_pages(storage, pages);
    while ((status == MQTT_OK) && (index < (SERVER_OBJECT_ARENAS - 1U))) {
        status = mqtt_slab_init_all(slabs[index], &storage->page_meta[word],
                                    page_slots[index], counts[index]);
        word += pages[index] * MQTT_PAGE_META_WORDS;
        index += 1U;
    }
    tree.nodes = storage->nodes;
    tree.node_count = storage->node_count;
    tree.branches = storage->branches;
    tree.branch_count = storage->branch_count;
    tree.node_meta = &storage->page_meta[word];
    word += pages[6] * MQTT_PAGE_META_WORDS;
    tree.text = storage->topic_text;
    tree.text_pages = storage->topic_text_pages;
    tree.text_meta = &storage->page_meta[word];
    word += storage->topic_text_pages * MQTT_PAGE_META_WORDS;
    if (status == MQTT_OK) {
        status = mqtt_tree_init(&server->tree, &tree);
    }
    if (status == MQTT_OK) {
        status = mqtt_page_arena_init(&server->arena, storage->arena_bytes,
                                      &storage->page_meta[word],
                                      storage->arena_pages);
    }
    return status;
}

/**
 * @brief Set every count to zero.
 * @param[out] statistics The counts.
 */
static void server_statistics_clear(mqtt_server_statistics *statistics)
{
    statistics->connections_accepted = 0U;
    statistics->messages_received = 0U;
    statistics->messages_sent = 0U;
    statistics->messages_dropped = 0U;
    statistics->protocol_errors = 0U;
    statistics->messages_refused = 0U;
    statistics->replies_lost = 0U;
    statistics->receive_throttled = 0U;
    statistics->records_written = 0U;
    statistics->records_refused = 0U;
}

/**
 * @brief Set up every field of a connection that is being opened, apart from
 * its places on the lists of those with work and those waiting for blocks.
 * The slot may hold anything from the connection before, so nothing is left
 * as it was.
 * @param[in,out] server The server.
 * @param[out] entry The connection.
 * @param[in] now_ms The time now.
 */
static void server_connection_reset(mqtt_server *server,
                                    mqtt_connection *entry, uint64_t now_ms)
{
    entry->open = MQTT_BOOL_SET;
    entry->state = MQTT_CONNECTION_WAIT_CONNECT;
    entry->version = MQTT_VERSION_311;
    mqtt_ring_init(&entry->ring, &server->pool);
    mqtt_ring_init(&entry->receive, &server->pool);
    entry->session = MQTT_SERVER_NONE;
    entry->opened_ms = now_ms;
    entry->last_received_ms = now_ms;
    entry->keep_alive_limit_ms = 0U;
    entry->keep_alive = 0U;
    entry->activity = MQTT_SERVER_ACTIVITY_NONE;
    entry->assembly_block = MQTT_PAGE_ARENA_NONE;
    entry->assembly_used = 0U;
    entry->assembly_size = 0U;
    entry->direct_ring_bytes = 0U;
    entry->direct_data = NULL;
    entry->direct_left = 0U;
    entry->direct_message = MQTT_SERVER_NONE;
    entry->resend = MQTT_SERVER_NONE;
    entry->resend_release = MQTT_SERVER_NONE;
    entry->on_wire = 0U;
    entry->retry_last_ms = 0U;
    entry->retry_delay_ms = 0U;
    entry->retry_first = 0U;
    entry->peer_maximum_packet_size = 0xFFFFFFFFU;
    entry->peer_window = 1U;
    entry->connect_block = MQTT_PAGE_ARENA_NONE;
    entry->connect_size = 0U;
    entry->method_block = MQTT_PAGE_ARENA_NONE;
    entry->incoming_here = 0U;
    entry->reauthenticating = false;
    entry->expiry_asked_zero = false;
    entry->method_length = 0U;
    entry->throttled = false;
    entry->hold_bytes = MQTT_SERVER_NONE;
    entry->hold_sequence = 0U;
}

/**
 * @brief Set up the entries of the server's tables that are read before they
 * are first taken.
 * @param[in,out] server The server.
 */
static void server_init_tables(mqtt_server *server)
{
    /** @brief The memory. */
    const mqtt_server_storage *storage = &server->storage;
    /** @brief Index of the entry being set up. */
    uint32_t index = storage->connection_count;

    while (index > 0U) {
        index -= 1U;
        storage->connections[index].open = MQTT_BOOL_UNSET;
        storage->connections[index].state = MQTT_CONNECTION_FREE;
        storage->connections[index].pending = false;
        storage->connections[index].next_pending = MQTT_SERVER_NONE;
        storage->connections[index].waiting = false;
        storage->connections[index].next_waiter = MQTT_SERVER_NONE;
        storage->connections[index].held_listed = false;
        storage->connections[index].next_held = MQTT_SERVER_NONE;
    }
    index = storage->session_count;
    while (index > 0U) {
        index -= 1U;
        storage->sessions[index].in_use = false;
    }
    server->session_table_used = 0U;
    if (server->config.topic_alias_maximum > 0U) {
        index = storage->connection_count *
                (uint32_t)server->config.topic_alias_maximum;
        while (index > 0U) {
            index -= 1U;
            storage->aliases[index].block = MQTT_PAGE_ARENA_NONE;
            storage->aliases[index].length = 0U;
        }
    }
    server->pending_head = MQTT_SERVER_NONE;
    server->pending_tail = MQTT_SERVER_NONE;
    server->waiter_head = MQTT_SERVER_NONE;
    server->waiter_tail = MQTT_SERVER_NONE;
}

/**
 * @brief Find the largest packet a receive ring keeps: what the scratch holds,
 * half the hard quota, as the other half is kept for its reply, and what
 * MQTT_SERVER_RECEIVE_RUNS runs can hold wherever it starts in a block.
 * @param[in] config The configuration.
 * @param[in] storage The memory.
 * @return The number of bytes.
 */
static uint32_t server_receive_inline(const mqtt_server_config *config,
                                      const mqtt_server_storage *storage)
{
    /** @brief Half the hard quota. */
    const uint32_t half =
        (config->pool_hard_quota * storage->pool_block_size) / 2U;
    /** @brief What the runs hold. */
    const uint32_t runs =
        (MQTT_SERVER_RECEIVE_RUNS - 2U) * storage->pool_block_size;
    /** @brief The answer. */
    uint32_t limit = storage->receive_scratch_size;

    if (half < limit) {
        limit = half;
    }
    if (runs < limit) {
        limit = runs;
    }
    return limit;
}

/**
 * @brief Set the server's own fields from what it was given. The memory may
 * hold anything before.
 * @param[out] server The server.
 * @param[in] config The configuration, which has been checked.
 * @param[in] storage The memory, which has been checked.
 */
static void server_init_fields(mqtt_server *server,
                               const mqtt_server_config *config,
                               const mqtt_server_storage *storage)
{
    /* Copied as bytes, as assigning structs this size would be done with a
     * call to memcpy. */
    mqtt_bytes_put((uint8_t *)&server->config, (const uint8_t *)config,
                   (uint32_t)sizeof(*config));
    mqtt_bytes_put((uint8_t *)&server->storage, (const uint8_t *)storage,
                   (uint32_t)sizeof(*storage));
    server_statistics_clear(&server->statistics);
    server->now_ms = 0U;
    server->next_sweep_ms = 0U;
    server->assigned_counter = 1U;
    server->message_room =
        (uint32_t)(((uint64_t)config->pool_hard_quota *
                    (uint64_t)storage->pool_block_size * 3U) /
                   4U);
    server->receive_inline = server_receive_inline(config, storage);
    mqtt_journal_init(server);
}

/**
 * @brief Do what mqtt_server_init does, for arguments known to be valid.
 * @param[out] server The server.
 * @param[in] config The configuration, which is copied.
 * @param[in] storage The memory, whose description is copied.
 * @return MQTT_OK or MQTT_INVALID_ARGUMENT.
 */
static mqtt_status server_init_body(mqtt_server *server,
                                    const mqtt_server_config *config,
                                    const mqtt_server_storage *storage)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief Number of words of page metadata the storage needs. */
    const uint32_t meta = mqtt_server_meta_words(storage);

    server->ready = MQTT_BOOL_UNSET;
    if ((storage->connections == NULL) || (storage->connection_count == 0U) ||
        !mqtt_server_pool_valid(config, storage) ||
        (storage->sessions == NULL) ||
        (storage->session_count == 0U) || (storage->session_table == NULL) ||
        (storage->session_table_count < storage->session_count) ||
        (storage->deliveries == NULL) || (storage->delivery_count == 0U) ||
        (storage->messages == NULL) || (storage->message_count == 0U) ||
        (storage->subscriptions == NULL) ||
        (storage->subscription_count == 0U) ||
        ((storage->groups == NULL) && (storage->group_count != 0U)) ||
        ((storage->aliases == NULL) && (config->topic_alias_maximum != 0U)) ||
        /* The Topic Aliases of every connection are found by a 32-bit
         * index. */
        (((uint64_t)storage->connection_count *
          (uint64_t)config->topic_alias_maximum) > (uint64_t)UINT32_MAX) ||
        (config->maximum_inflight == 0U) ||
        (config->maximum_inflight > (uint32_t)UINT16_MAX) ||
        (config->receive_maximum == 0U) ||
        /* A Two Byte Integer that isn't 0: section 3.2.2.3.3. */
        (config->receive_maximum > (uint32_t)UINT16_MAX) ||
        /* Every Server takes 23 bytes: [MQTT-3.1.3-5]. */
        (config->maximum_client_identifier < MQTT_CLIENT_ID_MAX) ||
        !mqtt_journal_config_valid(config) ||
        (storage->page_meta == NULL) || (meta == SERVER_META_TOO_MANY) ||
        (storage->page_meta_words < meta)) {
        status = MQTT_INVALID_ARGUMENT;
    } else {
        server_init_fields(server, config, storage);
        status = mqtt_pool_init(&server->pool, storage->pool_bytes,
                                storage->pool_next, storage->pool_block_size,
                                storage->pool_block_count);
    }
    if (status == MQTT_OK) {
        status = server_init_arenas(server);
    }
    if (status == MQTT_OK) {
        server_init_tables(server);
        server->ready = MQTT_BOOL_SET;
    }
    return status;
}

mqtt_status mqtt_server_init(mqtt_server *server,
                             const mqtt_server_config *config,
                             const mqtt_server_storage *storage)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if ((server != NULL) && (config != NULL) && (storage != NULL)) {
        answer = server_init_body(server, config, storage);
    }
    return answer;
}

/**
 * @brief Do what mqtt_server_connection_open does, for arguments known to be
 * valid.
 * @param[in,out] server The server.
 * @param[in] now_ms The time.
 * @param[out] connection Its index.
 * @return MQTT_OK or MQTT_QUOTA_EXCEEDED.
 */
static mqtt_status server_connection_open_body(mqtt_server *server,
                                               uint64_t now_ms,
                                               uint32_t *connection)
{
    /** @brief The new connection. */
    uint32_t index = MQTT_SLAB_NONE;
    /** @brief The outcome. */
    mqtt_status status = MQTT_QUOTA_EXCEEDED;

    if (mqtt_server_ready(server)) {
        index = mqtt_slab_take(&server->connection_slots);
    }
    if (!mqtt_server_ready(server)) {
        status = MQTT_INVALID_ARGUMENT;
    } else if (index != MQTT_SLAB_NONE) {
        /** @brief The new connection. */
        mqtt_connection *entry = &server->storage.connections[index];

        server->now_ms = now_ms;
        /* Its places on the lists of those with work and of those waiting for
         * blocks stay as they are. */
        server_connection_reset(server, entry, now_ms);
        *connection = index;
        status = MQTT_OK;
    } else {
        server->now_ms = now_ms;
    }
    return status;
}

mqtt_status mqtt_server_connection_open(mqtt_server *server, uint64_t now_ms,
                                        uint32_t *connection)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if (connection != NULL) {
        answer = server_connection_open_body(server, now_ms, connection);
    }
    return answer;
}

/**
 * @brief Free a connection that is in use, and start what its Session does
 * without it.
 * @param[in,out] server The server.
 * @param[in] connection The connection, which is in use.
 * @param[in] now_ms The time.
 */
static void server_connection_free(mqtt_server *server, uint32_t connection,
                                   uint64_t now_ms)
{
    /** @brief The connection. */
    mqtt_connection *entry = &server->storage.connections[connection];
    /** @brief Index of the Topic Alias being freed. */
    uint32_t alias = 0U;

    if (entry->session != MQTT_SERVER_NONE) {
        /** @brief The Session. */
        const uint32_t index = entry->session;
        /** @brief The Session. */
        mqtt_session *session = &server->storage.sessions[index];

        session->connection = MQTT_SERVER_NONE;
        entry->session = MQTT_SERVER_NONE;
        mqtt_route_drop_queued_qos_0(server, index);
        if (session->will_message != MQTT_SERVER_NONE) {
            if ((session->will_delay_s == 0U) || (session->expiry_s == 0U)) {
                mqtt_session_publish_will(server, index);
            } else {
                session->will_due_ms =
                    now_ms + ((uint64_t)session->will_delay_s * 1000U);
            }
        }
        if (session->expiry_s == 0U) {
            mqtt_session_discard(server, index);
        } else {
            mqtt_server_start_expiry(server, index);
            mqtt_journal_session(server, index);
        }
    }
    /* What the connection still holds may be part of a CONNECT or an AUTH
     * packet, with a secret in it. */
    mqtt_page_arena_wipe(&server->arena, entry->assembly_block);
    mqtt_page_arena_wipe(&server->arena, entry->connect_block);
    mqtt_ring_wipe_front(&entry->receive, entry->receive.used);
    mqtt_page_arena_give(&server->arena, entry->assembly_block);
    mqtt_page_arena_give(&server->arena, entry->connect_block);
    mqtt_page_arena_give(&server->arena, entry->method_block);
    mqtt_route_release(server, entry->direct_message);
    mqtt_server_blocks_returned(server, mqtt_ring_clear(&entry->ring));
    mqtt_server_blocks_returned(server, mqtt_ring_clear(&entry->receive));
    while (alias < (uint32_t)server->config.topic_alias_maximum) {
        /** @brief The entry for this Topic Alias. */
        mqtt_topic_alias *item =
            &server->storage.aliases[(connection *
                                      server->config.topic_alias_maximum) +
                                     alias];

        mqtt_page_arena_give(&server->arena, item->block);
        item->block = MQTT_PAGE_ARENA_NONE;
        alias += 1U;
    }
    /* If it is still on the list of those with work, next_pending takes it off
     * when it comes up. */
    entry->open = MQTT_BOOL_UNSET;
    entry->state = MQTT_CONNECTION_FREE;
    (void)mqtt_slab_give(&server->connection_slots, connection);
}

void mqtt_server_connection_closed(mqtt_server *server, uint32_t connection,
                                   uint64_t now_ms)
{
    if (mqtt_server_ready(server)) {
        server->now_ms = now_ms;
    }
    if (server_connection_in_use(server, connection)) {
        server_connection_free(server, connection, now_ms);
    }
}

uint32_t mqtt_server_next_pending(mqtt_server *server)
{
    /** @brief The connection handed back. */
    uint32_t connection = MQTT_SERVER_NONE;

    while (mqtt_server_ready(server) && (connection == MQTT_SERVER_NONE) &&
           (server->pending_head != MQTT_SERVER_NONE)) {
        /** @brief The connection at the front. */
        const uint32_t index = server->pending_head;
        /** @brief The connection at the front. */
        mqtt_connection *entry = &server->storage.connections[index];

        server->pending_head = entry->next_pending;
        if (server->pending_head == MQTT_SERVER_NONE) {
            server->pending_tail = MQTT_SERVER_NONE;
        }
        entry->pending = false;
        if (entry->state != MQTT_CONNECTION_FREE) {
            connection = index;
        }
    }
    return connection;
}

bool mqtt_server_should_close(const mqtt_server *server, uint32_t connection)
{
    /** @brief The answer. */
    bool close = false;

    if (server_connection_in_use(server, connection)) {
        /** @brief The connection. */
        const mqtt_connection *entry = &server->storage.connections[connection];

        close = (entry->state == MQTT_CONNECTION_CLOSING) &&
                (entry->ring.used == 0U) && (entry->direct_left == 0U);
    }
    return close;
}

/**
 * @brief Send an MQTT 3.1 Client's unanswered messages again, if it has been
 * quiet for long enough.
 *
 * MQTT 3.1 section 4.2 suggests this, with a time that can be set and that
 * grows with each retry, and that doesn't run out while a message is still
 * being sent. This is what a resumed Session does, so nothing is sent again
 * until the bytes of the last attempt have gone. The time runs from when the
 * oldest unanswered message became the oldest, so other traffic doesn't hold it
 * back.
 *
 * @param[in,out] server The server.
 * @param[in] connection The connection, which is active.
 */
static void server_retry_unanswered(mqtt_server *server, uint32_t connection)
{
    /** @brief The connection. */
    mqtt_connection *entry = &server->storage.connections[connection];
    /** @brief The oldest unanswered message. */
    const uint32_t first =
        server->storage.sessions[entry->session].inflight_head;

    if (first != entry->retry_first) {
        /* The Client has answered, or something new has gone out, so this one
         * starts afresh. */
        entry->retry_first = first;
        entry->retry_last_ms = server->now_ms;
        entry->retry_delay_ms = server->config.retry_interval_ms;
    } else if (first != MQTT_SERVER_NONE) {
        if ((server->now_ms >=
             (entry->retry_last_ms + (uint64_t)entry->retry_delay_ms)) &&
            (entry->ring.used == 0U) && (entry->direct_left == 0U) &&
            (entry->resend == MQTT_SERVER_NONE) &&
            (entry->resend_release == MQTT_SERVER_NONE)) {
            entry->retry_last_ms = server->now_ms;
            if (entry->retry_delay_ms <= (UINT32_MAX / 2U)) {
                entry->retry_delay_ms *= 2U;
            }
            entry->resend = first;
            entry->resend_release = first;
            entry->on_wire = 0U;
            mqtt_route_inflight_reset(server, entry->session);
            mqtt_route_pump(server, connection);
        }
    } else {
        /* Nothing is waiting for an answer. */
    }
}

/**
 * @brief Run the timers of one connection.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 */
static void server_sweep_connection(mqtt_server *server, uint32_t connection)
{
    /** @brief The connection. */
    mqtt_connection *entry = &server->storage.connections[connection];
    /** @brief The time. */
    const uint64_t now = server->now_ms;

    if (entry->state == MQTT_CONNECTION_ACTIVE) {
        if ((entry->keep_alive_limit_ms != 0U) &&
            (now >= (entry->last_received_ms + entry->keep_alive_limit_ms))) {
            mqtt_server_disconnect(server, connection,
                                   (uint8_t)MQTT5_RC_KEEP_ALIVE_TIMEOUT);
        } else if ((entry->version == MQTT_VERSION_31) &&
                   (server->config.retry_interval_ms != 0U)) {
            server_retry_unanswered(server, connection);
        } else {
            /* Nothing is due on this connection. */
        }
    } else if ((entry->state == MQTT_CONNECTION_WAIT_CONNECT) ||
               (entry->state == MQTT_CONNECTION_AUTHENTICATING)) {
        /* 0 sets no limit. */
        if ((server->config.connect_timeout_ms != 0U) &&
            (now >= (entry->opened_ms +
                     (uint64_t)server->config.connect_timeout_ms))) {
            mqtt_server_begin_closing(server, connection);
        }
    } else if (entry->state == MQTT_CONNECTION_CLOSING) {
        /* A Client that never reads must not hold on to its connection for
         * ever. */
        if (now >=
            (entry->last_received_ms + (uint64_t)SERVER_CLOSING_LIMIT_MS)) {
            mqtt_server_blocks_returned(server, mqtt_ring_clear(&entry->ring));
            mqtt_ring_wipe_front(&entry->receive, entry->receive.used);
            mqtt_server_blocks_returned(server,
                                        mqtt_ring_clear(&entry->receive));
            entry->direct_ring_bytes = 0U;
            entry->direct_left = 0U;
            mqtt_route_mark_pending(server, connection);
        }
    } else {
        /* A free entry has no timers. */
    }
}

/**
 * @brief Run the timers of the Sessions and of retained messages.
 * @param[in,out] server The server.
 */
static void server_sweep_sessions(mqtt_server *server)
{
    /** @brief Index of the Session or node being looked at. */
    uint32_t index = 0U;

    while (index < server->storage.session_count) {
        if (server->storage.sessions[index].in_use) {
            mqtt_server_session_timers(server, index);
        }
        index += 1U;
    }
    /* Removing a node moves no other, so the nodes left are all seen. */
    index = mqtt_tree_next(&server->tree, 0U);
    while (index != MQTT_TREE_NONE) {
        /** @brief The retained message at this node. */
        const uint32_t message = server->tree.nodes[index].retained;

        if ((message != MQTT_SERVER_NONE) &&
            mqtt_route_expired(server, message)) {
            /** @brief The message. */
            const mqtt_message *stored = &server->storage.messages[message];
            /** @brief Its Topic Name, which starts its block. */
            const mqtt_utf8 topic_name = {
                stored->topic_length,
                mqtt_page_arena_bytes(&server->arena, stored->block)};

            mqtt_journal_retained_remove(server, &topic_name);
            mqtt_route_release(server, message);
            server->tree.nodes[index].retained = MQTT_SERVER_NONE;
            mqtt_tree_prune(&server->tree, index);
        }
        index = mqtt_tree_next(&server->tree, index + 1U);
    }
}

uint32_t mqtt_server_tick(mqtt_server *server, uint64_t now_ms)
{
    /** @brief The time until the next sweep. */
    uint32_t wait = SERVER_SWEEP_MS;

    if (!mqtt_server_ready(server)) {
        /* There is nothing to run, and the caller comes back later. */
    } else if (now_ms >= server->next_sweep_ms) {
        /** @brief Index of the connection being looked at. */
        uint32_t index = 0U;

        server->now_ms = now_ms;
        server->next_sweep_ms = now_ms + SERVER_SWEEP_MS;
        while (index < server->storage.connection_count) {
            server_sweep_connection(server, index);
            index += 1U;
        }
        server_sweep_sessions(server);
    } else {
        server->now_ms = now_ms;
        wait = (uint32_t)(server->next_sweep_ms - now_ms);
    }
    return wait;
}

mqtt_status mqtt_server_publish(mqtt_server *server,
                                const mqtt_utf8 *topic_name,
                                const mqtt_payload *payload, uint8_t qos,
                                bool retain, uint64_t now_ms)
{
    /** @brief The outcome. */
    mqtt_status status = (mqtt_server_ready(server) && (payload != NULL)) ?
                             mqtt_topic_name_check(topic_name, MQTT_VERSION_5) :
                             MQTT_INVALID_ARGUMENT;

    if (status == MQTT_OK) {
        server->now_ms = now_ms;
    }
    /* No version lets a PUBLISH packet have a QoS of 3: [MQTT-3.3.1-4]. One
     * kept as retained would come back from mqtt_server_retained_next as a
     * message mqtt_server_retained_restore refuses. */
    if ((status == MQTT_OK) &&
        ((qos > MQTT_QOS_2) ||
         (retain && (!server->config.retain_available)))) {
        status = MQTT_INVALID_ARGUMENT;
    }
    if (status == MQTT_OK) {
        /** @brief The message being routed. */
        mqtt_route_source source;
        /** @brief The run that holds its Payload. */
        mqtt_run run = {NULL, 0U};

        source.topic_name = *topic_name;
        source.properties.data = NULL;
        source.properties.length = 0U;
        mqtt_route_payload(&source, &run, payload->data, payload->length);
        source.expiry_ms = 0U;
        source.message = MQTT_SERVER_NONE;
        source.publisher = MQTT_SERVER_NONE;
        source.qos = qos;
        source.retain = retain;
        source.properties_raw = false;
        source.lost = false;
        if (mqtt_route_too_large(server, &source)) {
            status = MQTT_QUOTA_EXCEEDED;
        } else {
            if (retain) {
                status = mqtt_session_retain(server, &source);
            }
            (void)mqtt_route_publish(server, &source);
            mqtt_route_release(server, source.message);
            if (source.lost) {
                status = MQTT_QUOTA_EXCEEDED;
            }
        }
    }
    return status;
}
