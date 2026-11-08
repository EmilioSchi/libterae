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
 * @file base_server_connect.c
 * @brief The MQTT Server: CONNECT and AUTH packets, and accepting a connection.
 *
 * A CONNECT packet is checked against what this Server accepts. It is then
 * accepted at once or, if it names an Authentication Method, kept until the
 * exchange of AUTH packets ends. Accepting a connection gives it a Session and
 * sends the CONNACK packet.
 *
 * A Client that sends no Client Identifier is given one made from random
 * bytes, when the server has a source of them. Another Client that could guess
 * it could connect with it, and so end that Client's connection and take its
 * Session. Without a source, the identifiers are numbered.
 *
 * [V31] MQTT V3.1 Protocol Specification.
 * @li Cited: 2.2, 3.1, 3.2.
 * @li Source: public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015.
 * @li Cited: 3.1, 3.2.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 3.1, 3.2, p.30-53. 3.3.2.3.5, p.58. 3.15, p.89-91. 4.12,
 * p.106-108.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */

#include <stddef.h>

#include "base_client_id.h"
#include "base_keep_alive.h"
#include "base_remaining.h"
#include "base_server_internal.h"
#include "base_string.h"
#include "base_topic_check.h"
#include "base_type.h"
#include "base_utf8_check.h"
#include "v311_rc.h"
#include "v5_prop.h"
#include "v5_prop_value.h"
#include "v5_rc.h"

/**
 * @brief Number of bytes used on the stack for a CONNACK packet; longer ones
 * use the arena.
 */
#define CONNECT_REPLY_BYTES 256U
/**
 * @brief Number of hex digits after the prefix of an Assigned Client
 * Identifier.
 */
#define CONNECT_ASSIGNED_DIGITS 16U
/** @brief Number of bytes in the prefix of an Assigned Client Identifier. */
#define CONNECT_ASSIGNED_PREFIX_LENGTH 4U
/**
 * @brief Number of random bytes in an Assigned Client Identifier: one for each
 * two of its digits.
 */
#define CONNECT_RANDOM_BYTES 8U
/**
 * @brief The most draws of random bytes for one Assigned Client Identifier.
 * Two draws in use at once are next to impossible with a working source, so
 * this only stops one that gives the same bytes every time.
 */
#define CONNECT_RANDOM_TRIES 4U

void mqtt_connect_refuse(mqtt_server *server, uint32_t connection,
                         mqtt_version version, uint8_t reason_code)
{
    /** @brief The bytes. */
    uint8_t bytes[8] = {0};
    /** @brief The writer. */
    mqtt_writer writer = {NULL, 0U, 0U, MQTT_OK};
    /** @brief The packet. */
    mqtt_connack packet;

    mqtt_connack_clear(&packet);
    packet.reason_code = reason_code;
    /* A code a CONNACK can't carry, as the application may give one, becomes
     * the general one: [MQTT-3.2.2-8]. */
    if (version == MQTT_VERSION_5) {
        if (!mqtt5_reason_code_is_valid(MQTT_CONNACK, reason_code)) {
            packet.reason_code = (uint8_t)MQTT5_RC_UNSPECIFIED_ERROR;
        }
    }
    mqtt_writer_init(&writer, bytes, 8U);
    mqtt_connack_encode(&writer, version, &packet);
    /* It closes all the same, and an operator can see that a reason did not get
     * through. */
    if (mqtt_server_send(server, connection, bytes, writer.position) !=
        MQTT_OK) {
        server->statistics.replies_lost += 1U;
    }
    mqtt_server_begin_closing(server, connection);
}

/**
 * @brief Find the Connect Return Code of MQTT 3.1 and 3.1.1 closest to a
 * status.
 * @param[in] status The status.
 * @return The Connect Return Code.
 */
static uint8_t connect_return_code(mqtt_status status)
{
    /** @brief The code. */
    uint32_t code = MQTT311_RC_SERVER_UNAVAILABLE;

    switch (status) {
    case MQTT_UNSUPPORTED_PROTOCOL_VERSION:
        code = MQTT311_RC_UNACCEPTABLE_PROTOCOL_VERSION;
        break;
    case MQTT_CLIENT_IDENTIFIER_NOT_VALID:
        code = MQTT311_RC_IDENTIFIER_REJECTED;
        break;
    case MQTT_BAD_USER_NAME_OR_PASSWORD:
        code = MQTT311_RC_BAD_USER_NAME_OR_PASSWORD;
        break;
    case MQTT_NOT_AUTHORIZED:
        code = MQTT311_RC_NOT_AUTHORIZED;
        break;
    default:
        /* The rest amount to the Server not being able to take the
         * connection. */
        break;
    }
    return (uint8_t)code;
}

/**
 * @brief Find the Session Expiry Interval a CONNECT packet asks for.
 * @param[in] packet The CONNECT packet.
 * @return The interval, or 0U if the packet doesn't hold one, which is what
 * its absence means: section 3.1.2.11.2.
 */
static uint32_t connect_asked_expiry(const mqtt_connect *packet)
{
    return mqtt5_properties_has(&packet->properties,
                                MQTT5_PROP_SESSION_EXPIRY_INTERVAL) ?
               packet->properties.session_expiry_interval :
               0U;
}

/**
 * @brief Find the Authentication Data of a packet, for the callback.
 * @param[in] properties The packet's Properties.
 * @return The data, or empty data if the packet holds none.
 */
static const mqtt_binary *connect_auth_data(const mqtt5_properties *properties)
{
    /** @brief Binary Data with nothing in it. */
    static const mqtt_binary none = {0U, NULL};

    return mqtt5_properties_has(properties, MQTT5_PROP_AUTHENTICATION_DATA) ?
               &properties->authentication_data :
               &none;
}

/**
 * @brief Mark the QoS 2 flows a resumed Session brings with it as from an
 * earlier connection.
 * @param[in,out] server The server.
 * @param[in] session The Session.
 */
static void connect_incoming_from_before(mqtt_server *server, uint32_t session)
{
    /** @brief The entry being looked at. */
    uint32_t cursor = server->storage.sessions[session].incoming_head;

    while (cursor != MQTT_SERVER_NONE) {
        server->storage.deliveries[cursor].on_wire = false;
        cursor = server->storage.deliveries[cursor].next;
    }
}

/**
 * @brief Keep a copy of a text in the arena, for as long as a connection needs
 * it.
 * @param[in,out] server The server.
 * @param[in] text The text.
 * @param[out] block The block, or MQTT_PAGE_ARENA_NONE if text is empty.
 * @return MQTT_OK or MQTT_QUOTA_EXCEEDED.
 */
static mqtt_status connect_keep_text(mqtt_server *server, const mqtt_utf8 *text,
                                     uint32_t *block)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    *block = MQTT_PAGE_ARENA_NONE;
    if (text->length > 0U) {
        *block = mqtt_page_arena_take(&server->arena, (uint32_t)text->length);
        if (*block == MQTT_PAGE_ARENA_NONE) {
            status = MQTT_QUOTA_EXCEEDED;
        } else {
            status = mqtt_page_arena_write(&server->arena, *block, text->data,
                                      (uint32_t)text->length);
        }
    }
    return status;
}

/**
 * @brief Write the digits of an Assigned Client Identifier.
 * @param[out] text The identifier, which has room for the prefix and digits.
 * @param[in] value The number the digits stand for.
 */
static void connect_identifier_write(uint8_t *text, uint64_t value)
{
    /** @brief The hex digits. */
    static const uint8_t digits[] = {0x30U, 0x31U, 0x32U, 0x33U, 0x34U, 0x35U,
                                     0x36U, 0x37U, 0x38U, 0x39U, 0x61U, 0x62U,
                                     0x63U, 0x64U, 0x65U, 0x66U};
    /** @brief What is left of the number. */
    uint64_t number = value;
    /** @brief Index of the digit. */
    uint32_t index = CONNECT_ASSIGNED_DIGITS;

    while (index > 0U) {
        index -= 1U;
        text[CONNECT_ASSIGNED_PREFIX_LENGTH + index] = digits[number & 15U];
        number >>= 4U;
    }
}

/**
 * @brief Make a number of random bytes.
 * @param[in] bytes CONNECT_RANDOM_BYTES bytes, the first the highest.
 * @return The number.
 */
static uint64_t connect_random_value(const uint8_t *bytes)
{
    /** @brief The number. */
    uint64_t value = 0U;
    /** @brief Index of the byte. */
    uint32_t index = 0U;

    while (index < CONNECT_RANDOM_BYTES) {
        value = (value << 8U) | (uint64_t)bytes[index];
        index += 1U;
    }
    return value;
}

/**
 * @brief Write the digits of an Assigned Client Identifier from random bytes,
 * drawing again while the identifier is in another Session.
 * @param[in,out] server The server.
 * @param[out] text The identifier, which has room for the prefix and digits.
 * @param[in] identifier A view of it.
 * @return true once it is in no other Session, or false if no random bytes
 * came or every draw was in use.
 */
static bool connect_identifier_random(mqtt_server *server, uint8_t *text,
                                      const mqtt_utf8 *identifier)
{
    /** @brief Number of draws made. */
    uint32_t tries = 0U;
    /** @brief true once it is known to be in no other Session. */
    bool unique = false;
    /** @brief true while random bytes come. */
    bool drawing = (server->config.random_bytes != NULL);

    while (drawing && (!unique) && (tries < CONNECT_RANDOM_TRIES)) {
        /** @brief The random bytes. */
        uint8_t bytes[CONNECT_RANDOM_BYTES] = {0};
        /** @brief The outcome of the draw. */
        mqtt_status drawn = MQTT_OK;

        tries += 1U;
        drawn = server->config.random_bytes(server->config.user_tag, bytes,
                                            CONNECT_RANDOM_BYTES);
        drawing = (drawn == MQTT_OK);
        if (drawing) {
            connect_identifier_write(text, connect_random_value(bytes));
            unique =
                (mqtt_session_find(server, identifier) == MQTT_SERVER_NONE);
        }
    }
    return unique;
}

/**
 * @brief Build a unique Client Identifier: from random bytes if the server has
 * them, or else from the next number.
 * @param[in,out] server The server.
 * @param[out] text Where to put it, which has room for the prefix and digits.
 * @param[out] identifier A view of it.
 */
static void connect_assign_identifier(mqtt_server *server, uint8_t *text,
                                      mqtt_utf8 *identifier)
{
    /** @brief The prefix, in the characters all Servers must allow. */
    static const uint8_t prefix[] = {0x61U, 0x75U, 0x74U, 0x6FU};
    /** @brief true once it is known to be in no other Session. */
    bool unique = false;

    /* text holds MQTT_SERVER_ASSIGNED_LENGTH bytes, more than the prefix. */
    mqtt_bytes_put(text, prefix, CONNECT_ASSIGNED_PREFIX_LENGTH);
    identifier->data = text;
    identifier->length = (uint16_t)MQTT_SERVER_ASSIGNED_LENGTH;
    unique = connect_identifier_random(server, text, identifier);
    /* Sessions are finite, so some number is in none of them. */
    while (!unique) {
        connect_identifier_write(text, server->assigned_counter);
        server->assigned_counter += 1U;
        unique = (mqtt_session_find(server, identifier) == MQTT_SERVER_NONE);
    }
}

/**
 * @brief Check a CONNECT packet against what this Server will accept.
 * @param[in] server The server.
 * @param[in] packet The packet.
 * @return MQTT_OK, or the reason to refuse the connection.
 */
static mqtt_status connect_check(const mqtt_server *server,
                                 const mqtt_connect *packet)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief Number of bytes in the Client Identifier. */
    const uint32_t length = (uint32_t)packet->client_identifier.length;

    if (packet->version == MQTT_VERSION_31) {
        /** @brief Number of characters, which is what MQTT 3.1 limits. */
        const uint32_t characters =
            mqtt_utf8_character_count(packet->client_identifier.data, length);

        if ((characters < MQTT_CLIENT_ID_MIN) ||
            (characters > MQTT_CLIENT_ID_MAX)) {
            status = MQTT_CLIENT_IDENTIFIER_NOT_VALID;
        }
    } else if ((length == 0U) && (packet->version == MQTT_VERSION_311) &&
               (!packet->clean_start)) {
        status = MQTT_CLIENT_IDENTIFIER_NOT_VALID;
    } else if (length > server->config.maximum_client_identifier) {
        status = MQTT_CLIENT_IDENTIFIER_NOT_VALID;
    } else {
        /* An empty one gets a value assigned to it, which is the only other
         * case. */
    }
    if ((status == MQTT_OK) && packet->will_flag && packet->will_retain &&
        (packet->version != MQTT_VERSION_5) &&
        (!server->config.retain_available)) {
        /* It can't be published as retained, as [V311 MQTT-3.1.2-17] asks, and
         * only a refusal that names no reason is left to say so. */
        status = MQTT_NOT_AUTHORIZED;
    }
    if ((status == MQTT_OK) && packet->will_flag &&
        (packet->version == MQTT_VERSION_5)) {
        if (packet->will_qos > server->config.maximum_qos) {
            status = MQTT_PROTOCOL_ERROR;
        } else if (packet->will_retain && (!server->config.retain_available)) {
            status = MQTT_PROTOCOL_ERROR;
        } else if (mqtt5_properties_has(&packet->will_properties,
                                        MQTT5_PROP_RESPONSE_TOPIC) &&
                   (mqtt_topic_name_check(
                        &packet->will_properties.response_topic,
                        packet->version) != MQTT_OK)) {
            /* [MQTT-3.3.2-14] */
            status = MQTT_PROTOCOL_ERROR;
        } else {
            /* A Will Message with no limit of this Server against it. */
        }
    }
    /* Authentication Data only means something with an Authentication Method:
     * section 3.1.2.11.10. */
    if ((status == MQTT_OK) &&
        mqtt5_properties_has(&packet->properties,
                             MQTT5_PROP_AUTHENTICATION_DATA) &&
        (!mqtt5_properties_has(&packet->properties,
                               MQTT5_PROP_AUTHENTICATION_METHOD))) {
        status = MQTT_PROTOCOL_ERROR;
    }
    if ((status == MQTT_OK) && (server->config.authenticate != NULL) &&
        ((packet->version != MQTT_VERSION_5) ||
         (!mqtt5_properties_has(&packet->properties,
                                MQTT5_PROP_AUTHENTICATION_METHOD)))) {
        status = server->config.authenticate(server->config.user_tag, packet);
    }
    return status;
}

/**
 * @brief Take a Session away from the connection that has it, as a new one is
 * arriving.
 * @param[in,out] server The server.
 * @param[in] session The Session.
 * @param[in] session_ends true if the Session does not go on after this.
 */
static void connect_takeover(mqtt_server *server, uint32_t session,
                             bool session_ends)
{
    /** @brief The Session. */
    mqtt_session *entry = &server->storage.sessions[session];
    /**
     * @brief true if a connection still has the Session, and is being closed
     * for this.
     */
    const bool taken_over = (entry->connection != MQTT_SERVER_NONE);

    if (taken_over) {
        /** @brief The connection that has it. */
        const uint32_t old = entry->connection;

        mqtt_server_disconnect(server, old,
                               (uint8_t)MQTT5_RC_SESSION_TAKEN_OVER);
        server->storage.connections[old].session = MQTT_SERVER_NONE;
        entry->connection = MQTT_SERVER_NONE;
    }
    /* A new connection while a Will Message waits out its delay deletes it,
     * whatever else that connection asks for: [MQTT-3.1.2-8]. One being closed
     * now has only just started its delay, so it goes out if it has none, or if
     * its Session ends with this: section 3.1.4. */
    if (entry->will_message != MQTT_SERVER_NONE) {
        if (taken_over && (session_ends || (entry->will_delay_s == 0U))) {
            mqtt_session_publish_will(server, session);
        } else {
            mqtt_journal_will_remove(server, session);
            mqtt_route_release(server, entry->will_message);
            entry->will_message = MQTT_SERVER_NONE;
        }
    }
    entry->will_due_ms = 0U;
    entry->expiry_due_ms = 0U;
}

/**
 * @brief Store the Will Message of a CONNECT packet.
 * @param[in,out] server The server.
 * @param[in] session The Session.
 * @param[in] packet The packet.
 * @return MQTT_OK or MQTT_QUOTA_EXCEEDED.
 */
static mqtt_status connect_store_will(mqtt_server *server, uint32_t session,
                                      const mqtt_connect *packet)
{
    /** @brief The Session. */
    mqtt_session *entry = &server->storage.sessions[session];
    /** @brief The Will Message, as a message to store. */
    mqtt_route_source source;
    /** @brief The run that holds its Payload. */
    mqtt_run run = {NULL, 0U};
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    source.topic_name = packet->will_topic;
    source.properties = packet->will_properties.raw;
    mqtt_route_payload(&source, &run, packet->will_payload.data,
                       (uint32_t)packet->will_payload.length);
    source.expiry_ms = 0U;
    source.message = MQTT_SERVER_NONE;
    source.publisher = session;
    source.qos = packet->will_qos;
    source.retain = packet->will_retain;
    source.properties_raw = true;
    source.lost = false;
    entry->will_message = mqtt_route_too_large(server, &source) ?
                              MQTT_SERVER_NONE :
                              mqtt_route_store(server, &source);
    if (entry->will_message == MQTT_SERVER_NONE) {
        status = MQTT_QUOTA_EXCEEDED;
    } else {
        entry->will_delay_s =
            mqtt5_properties_has(&packet->will_properties,
                                 MQTT5_PROP_WILL_DELAY_INTERVAL) ?
                packet->will_properties.will_delay_interval :
                0U;
        entry->will_has_expiry = mqtt5_properties_has(
            &packet->will_properties, MQTT5_PROP_MESSAGE_EXPIRY_INTERVAL);
        entry->will_expiry_s =
            entry->will_has_expiry ?
                packet->will_properties.message_expiry_interval :
                0U;
    }
    return status;
}

/**
 * @brief Write the Properties of a CONNACK packet that accepts a connection.
 * @param[in] server The server.
 * @param[in] packet The CONNECT packet.
 * @param[in] assigned The Assigned Client Identifier, or an empty string.
 * @param[in] expiry The Session Expiry Interval that applies.
 * @param[in] keep_alive The Keep Alive that applies.
 * @param[out] properties The Properties.
 */
static void connect_connack_properties(const mqtt_server *server,
                                       const mqtt_connect *packet,
                                       const mqtt_utf8 *assigned,
                                       uint32_t expiry, uint16_t keep_alive,
                                       mqtt5_properties *properties)
{
    /** @brief The configuration. */
    const mqtt_server_config *config = &server->config;

    mqtt5_properties_clear(properties);
    if (expiry != connect_asked_expiry(packet)) {
        properties->session_expiry_interval = expiry;
        mqtt5_properties_mark(properties, MQTT5_PROP_SESSION_EXPIRY_INTERVAL);
    }
    if (config->receive_maximum < MQTT5_RECEIVE_MAXIMUM_DEFAULT) {
        properties->receive_maximum = (uint16_t)config->receive_maximum;
        mqtt5_properties_mark(properties, MQTT5_PROP_RECEIVE_MAXIMUM);
    }
    if ((uint32_t)config->maximum_qos < MQTT5_MAXIMUM_QOS_DEFAULT) {
        properties->maximum_qos = config->maximum_qos;
        mqtt5_properties_mark(properties, MQTT5_PROP_MAXIMUM_QOS);
    }
    if (!config->retain_available) {
        properties->retain_available = 0U;
        mqtt5_properties_mark(properties, MQTT5_PROP_RETAIN_AVAILABLE);
    }
    if (config->maximum_packet_size <= MQTT_REMAINING_LENGTH_MAX) {
        properties->maximum_packet_size = config->maximum_packet_size;
        mqtt5_properties_mark(properties, MQTT5_PROP_MAXIMUM_PACKET_SIZE);
    }
    if (assigned->length > 0U) {
        properties->assigned_client_identifier = *assigned;
        mqtt5_properties_mark(properties,
                              MQTT5_PROP_ASSIGNED_CLIENT_IDENTIFIER);
    }
    if (config->topic_alias_maximum > 0U) {
        properties->topic_alias_maximum = config->topic_alias_maximum;
        mqtt5_properties_mark(properties, MQTT5_PROP_TOPIC_ALIAS_MAXIMUM);
    }
    if (!config->wildcard_subscription_available) {
        properties->wildcard_subscription_available = 0U;
        mqtt5_properties_mark(properties,
                              MQTT5_PROP_WILDCARD_SUBSCRIPTION_AVAILABLE);
    }
    if (!config->subscription_identifier_available) {
        properties->subscription_identifier_available = 0U;
        mqtt5_properties_mark(properties,
                              MQTT5_PROP_SUBSCRIPTION_IDENTIFIER_AVAILABLE);
    }
    if (!config->shared_subscription_available) {
        properties->shared_subscription_available = 0U;
        mqtt5_properties_mark(properties,
                              MQTT5_PROP_SHARED_SUBSCRIPTION_AVAILABLE);
    }
    if (keep_alive != packet->keep_alive) {
        properties->server_keep_alive = keep_alive;
        mqtt5_properties_mark(properties, MQTT5_PROP_SERVER_KEEP_ALIVE);
    }
}

/**
 * @brief Find the Session a connection being accepted takes: the one it
 * resumes, or a new one.
 * @param[in,out] server The server.
 * @param[in] packet The CONNECT packet.
 * @param[out] assigned The Assigned Client Identifier, set only if the packet
 * has an empty Client Identifier.
 * @param[out] session_present Set to true if an earlier Session goes on.
 * @return The Session, or MQTT_SERVER_NONE if a pool is exhausted.
 */
static uint32_t connect_session(mqtt_server *server, const mqtt_connect *packet,
                                mqtt_utf8 *assigned, bool *session_present)
{
    /** @brief The Client Identifier in use. */
    mqtt_utf8 identifier = packet->client_identifier;
    /** @brief The Session. */
    uint32_t session = MQTT_SERVER_NONE;

    if (identifier.length == 0U) {
        connect_assign_identifier(server, server->assigned_text, assigned);
        identifier = *assigned;
    }
    session = mqtt_session_find(server, &identifier);
    if (session != MQTT_SERVER_NONE) {
        /* Its time may be up even though the timers have not run since. */
        mqtt_server_session_timers(server, session);
        session = mqtt_session_find(server, &identifier);
    }
    if (session != MQTT_SERVER_NONE) {
        /**
         * @brief true if the Session lasts only as long as the connection this
         * takes it from.
         */
        const bool session_ends =
            packet->clean_start ||
            (server->storage.sessions[session].expiry_s == 0U);

        connect_takeover(server, session, session_ends);
        if (session_ends) {
            mqtt_session_discard(server, session);
            session = MQTT_SERVER_NONE;
        } else {
            *session_present = true;
        }
    }
    if (session == MQTT_SERVER_NONE) {
        session = mqtt_session_create(server, &identifier);
    }
    return session;
}

/**
 * @brief Find the Session Expiry Interval that applies to a connection being
 * accepted.
 * @param[in] server The server.
 * @param[in] packet The CONNECT packet.
 * @return The Session Expiry Interval.
 */
static uint32_t connect_expiry(const mqtt_server *server,
                               const mqtt_connect *packet)
{
    /** @brief The Session Expiry Interval that applies. */
    uint32_t expiry = connect_asked_expiry(packet);

    if (packet->version != MQTT_VERSION_5) {
        expiry =
            packet->clean_start ? 0U : server->config.persistent_session_expiry;
    } else if (expiry > server->config.maximum_session_expiry) {
        expiry = server->config.maximum_session_expiry;
    } else {
        /* The value the Client asked for applies. */
    }
    return expiry;
}

/**
 * @brief Find the Keep Alive that applies to a connection being accepted.
 * @param[in] server The server.
 * @param[in] packet The CONNECT packet.
 * @return The Keep Alive.
 */
static uint16_t connect_keep_alive(const mqtt_server *server,
                                   const mqtt_connect *packet)
{
    /** @brief The Keep Alive that applies. */
    uint16_t keep_alive = packet->keep_alive;

    if ((server->config.server_keep_alive != 0U) &&
        (packet->version == MQTT_VERSION_5)) {
        keep_alive = server->config.server_keep_alive;
    }
    return keep_alive;
}

/**
 * @brief Give a connection being accepted its Session, and set it up with what
 * its CONNECT packet asks for.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] packet The CONNECT packet.
 * @param[in] session The Session.
 * @param[in] expiry The Session Expiry Interval that applies.
 * @param[in] keep_alive The Keep Alive that applies.
 */
static void connect_activate(mqtt_server *server, uint32_t connection,
                             const mqtt_connect *packet, uint32_t session,
                             uint32_t expiry, uint16_t keep_alive)
{
    /** @brief The connection. */
    mqtt_connection *entry = &server->storage.connections[connection];
    /** @brief The Session. */
    mqtt_session *held = &server->storage.sessions[session];

    held->expiry_s = expiry;
    held->connection = connection;
    held->version = packet->version;
    /* A QoS 0 message waiting from before is for a Client that has been
     * away. */
    mqtt_route_drop_queued_qos_0(server, session);
    entry->session = session;
    entry->state = MQTT_CONNECTION_ACTIVE;
    entry->version = packet->version;
    entry->resend = held->inflight_head;
    entry->resend_release = held->inflight_head;
    entry->on_wire = 0U;
    mqtt_route_inflight_reset(server, session);
    entry->last_received_ms = server->now_ms;
    entry->retry_last_ms = server->now_ms;
    entry->retry_delay_ms = server->config.retry_interval_ms;
    entry->retry_first = held->inflight_head;
    entry->incoming_here = 0U;
    entry->reauthenticating = false;
    entry->expiry_asked_zero =
        (packet->version != MQTT_VERSION_5) ||
        (!mqtt5_properties_has(&packet->properties,
                               MQTT5_PROP_SESSION_EXPIRY_INTERVAL)) ||
        (packet->properties.session_expiry_interval == 0U);
    connect_incoming_from_before(server, session);
    entry->keep_alive_limit_ms =
        ((uint64_t)keep_alive * MQTT_KEEP_ALIVE_GRACE_NUMERATOR * 1000U) /
        MQTT_KEEP_ALIVE_GRACE_DENOMINATOR;
    entry->keep_alive = keep_alive;
    entry->peer_maximum_packet_size = 0xFFFFFFFFU;
    entry->peer_window = server->config.maximum_inflight;
    if (packet->version == MQTT_VERSION_5) {
        if (mqtt5_properties_has(&packet->properties,
                                 MQTT5_PROP_MAXIMUM_PACKET_SIZE)) {
            entry->peer_maximum_packet_size =
                packet->properties.maximum_packet_size;
        }
        if (mqtt5_properties_has(&packet->properties,
                                 MQTT5_PROP_RECEIVE_MAXIMUM) &&
            ((uint32_t)packet->properties.receive_maximum <
             entry->peer_window)) {
            entry->peer_window = (uint32_t)packet->properties.receive_maximum;
        }
    }
}

/**
 * @brief Copy the Authentication Method of a CONNECT packet, if it has one,
 * into the Properties of the CONNACK packet, with any Authentication Data.
 * @param[in] packet The CONNECT packet.
 * @param[in] auth_data Authentication Data to send, or NULL.
 * @param[in,out] properties The Properties of the CONNACK packet.
 */
static void connect_auth_properties(const mqtt_connect *packet,
                                    const mqtt_binary *auth_data,
                                    mqtt5_properties *properties)
{
    if (mqtt5_properties_has(&packet->properties,
                             MQTT5_PROP_AUTHENTICATION_METHOD)) {
        properties->authentication_method =
            packet->properties.authentication_method;
        mqtt5_properties_mark(properties, MQTT5_PROP_AUTHENTICATION_METHOD);
        if ((auth_data != NULL) && (auth_data->length > 0U)) {
            properties->authentication_data = *auth_data;
            mqtt5_properties_mark(properties, MQTT5_PROP_AUTHENTICATION_DATA);
        }
    }
}

/**
 * @brief Write a CONNACK packet into a connection's ring.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] version The protocol in use.
 * @param[in] reply The packet.
 * @return MQTT_OK, or the reason to refuse the connection.
 */
static mqtt_status connect_send_connack(mqtt_server *server,
                                        uint32_t connection,
                                        mqtt_version version,
                                        const mqtt_connack *reply)
{
    /** @brief A writer that only counts. */
    mqtt_writer counter = {NULL, 0U, 0U, MQTT_OK};
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    mqtt_writer_init(&counter, NULL, 0U);
    mqtt_connack_encode(&counter, version, reply);
    {
        /**
         * @brief An arena block for the bytes of the reply, when there are
         * many.
         */
        const uint32_t block =
            (counter.position > CONNECT_REPLY_BYTES) ?
                mqtt_page_arena_take(&server->arena, counter.position) :
                MQTT_PAGE_ARENA_NONE;

        if ((counter.position > CONNECT_REPLY_BYTES) &&
            (block == MQTT_PAGE_ARENA_NONE)) {
            status = MQTT_QUOTA_EXCEEDED;
        } else {
            /** @brief The bytes of the reply, when there are few. */
            uint8_t bytes[CONNECT_REPLY_BYTES];
            /** @brief The writer. */
            mqtt_writer writer = {NULL, 0U, 0U, MQTT_OK};
            /** @brief Where the bytes go. */
            uint8_t *destination =
                (block == MQTT_PAGE_ARENA_NONE) ?
                    bytes :
                    mqtt_page_arena_bytes(&server->arena, block);

            mqtt_writer_init(&writer, destination, counter.position);
            mqtt_connack_encode(&writer, version, reply);
            status = mqtt_server_send(server, connection, destination,
                                      writer.position);
            mqtt_page_arena_give(&server->arena, block);
        }
    }
    return status;
}

/**
 * @brief Accept a connection: set up its Session, and send the CONNACK packet.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] packet The CONNECT packet.
 * @param[in] auth_data Authentication Data to send, or NULL.
 * @return MQTT_OK, or the reason to refuse the connection.
 */
static mqtt_status connect_accept(mqtt_server *server, uint32_t connection,
                                  const mqtt_connect *packet,
                                  const mqtt_binary *auth_data)
{
    /** @brief The Assigned Client Identifier, or an empty string. */
    mqtt_utf8 assigned = {0U, NULL};
    /** @brief The Session. */
    uint32_t session = MQTT_SERVER_NONE;
    /** @brief The Session Present flag. */
    bool session_present = false;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    session = connect_session(server, packet, &assigned, &session_present);
    if (session == MQTT_SERVER_NONE) {
        status = MQTT_QUOTA_EXCEEDED;
    }
    if ((status == MQTT_OK) && packet->will_flag) {
        status = connect_store_will(server, session, packet);
        if (status != MQTT_OK) {
            /* A Session with no connection and no use must not stay. */
            if (!session_present) {
                mqtt_session_discard(server, session);
            } else {
                /* Its connection has gone, so its time starts now, as after any
                 * other close. */
                mqtt_server_start_expiry(server, session);
                mqtt_journal_session(server, session);
            }
        }
    }
    if (status == MQTT_OK) {
        /** @brief The Session Expiry Interval that applies. */
        const uint32_t expiry = connect_expiry(server, packet);
        /** @brief The Keep Alive that applies. */
        const uint16_t keep_alive = connect_keep_alive(server, packet);
        /** @brief The reply. */
        mqtt_connack reply;

        mqtt_connack_clear(&reply);
        connect_activate(server, connection, packet, session, expiry,
                         keep_alive);
        /* Written before the CONNACK, which waits for it. */
        mqtt_journal_session(server, session);
        if (packet->version == MQTT_VERSION_5) {
            connect_connack_properties(server, packet, &assigned, expiry,
                                       keep_alive, &reply.properties);
            connect_auth_properties(packet, auth_data, &reply.properties);
        }
        reply.session_present =
            (packet->version != MQTT_VERSION_31) && session_present;
        reply.reason_code = (uint8_t)MQTT5_RC_SUCCESS;
        status =
            connect_send_connack(server, connection, packet->version, &reply);
        if (status == MQTT_OK) {
            server->statistics.connections_accepted += 1U;
            mqtt_route_mark_pending(server, connection);
        }
    }
    return status;
}

/**
 * @brief Send an AUTH packet.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] reason_code The Reason Code.
 * @param[in] method Authentication Method.
 * @param[in] data Authentication Data.
 * @return MQTT_OK, or the reason to close the connection.
 */
static mqtt_status connect_send_auth(mqtt_server *server, uint32_t connection,
                                     uint8_t reason_code,
                                     const mqtt_utf8 *method,
                                     const mqtt_binary *data)
{
    /** @brief The packet. */
    mqtt_reason packet;
    /** @brief A writer that only counts. */
    mqtt_writer counter = {NULL, 0U, 0U, MQTT_OK};
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    mqtt_reason_clear(&packet);
    packet.reason_code = reason_code;
    packet.properties.authentication_method = *method;
    mqtt5_properties_mark(&packet.properties, MQTT5_PROP_AUTHENTICATION_METHOD);
    if (data->length > 0U) {
        packet.properties.authentication_data = *data;
        mqtt5_properties_mark(&packet.properties,
                              MQTT5_PROP_AUTHENTICATION_DATA);
    }
    mqtt_writer_init(&counter, NULL, 0U);
    mqtt_auth_encode(&counter, &packet);
    {
        /** @brief A block for the bytes. */
        const uint32_t block =
            mqtt_page_arena_take(&server->arena, counter.position);

        if (block == MQTT_PAGE_ARENA_NONE) {
            status = MQTT_QUOTA_EXCEEDED;
        } else {
            /** @brief The writer. */
            mqtt_writer writer = {NULL, 0U, 0U, MQTT_OK};
            /** @brief Where the bytes go. */
            uint8_t *destination = mqtt_page_arena_bytes(&server->arena, block);

            mqtt_writer_init(&writer, destination, counter.position);
            mqtt_auth_encode(&writer, &packet);
            status = mqtt_server_send(server, connection, destination,
                                      writer.position);
            mqtt_page_arena_give(&server->arena, block);
        }
    }
    return status;
}

/**
 * @brief Start enhanced authentication for a CONNECT packet.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] packet The CONNECT packet.
 * @param[in] body The bytes after the Fixed Header.
 * @param[in] length Remaining Length.
 * @return MQTT_OK, or the reason to refuse the connection.
 */
static mqtt_status connect_auth_start(mqtt_server *server, uint32_t connection,
                                      const mqtt_connect *packet,
                                      const uint8_t *body, uint32_t length)
{
    /** @brief The connection. */
    mqtt_connection *entry = &server->storage.connections[connection];
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief Authentication Data to send. */
    mqtt_binary data_out = {0U, NULL};

    if (server->config.auth_step == NULL) {
        status = MQTT_BAD_AUTHENTICATION_METHOD;
    } else {
        status = server->config.auth_step(
            server->config.user_tag, connection,
            &packet->properties.authentication_method,
            connect_auth_data(&packet->properties), &data_out);
    }
    if (status == MQTT_OK) {
        status =
            connect_keep_text(server, &packet->properties.authentication_method,
                              &entry->method_block);
        entry->method_length = packet->properties.authentication_method.length;
        if (status == MQTT_OK) {
            status = connect_accept(server, connection, packet, &data_out);
        }
    } else if (status == MQTT_INCOMPLETE) {
        /* The CONNECT packet is needed again once the exchange ends, so a copy
         * is kept. */
        entry->connect_block = mqtt_page_arena_take(&server->arena, length);
        if (entry->connect_block == MQTT_PAGE_ARENA_NONE) {
            status = MQTT_QUOTA_EXCEEDED;
        } else {
            status = connect_keep_text(
                server, &packet->properties.authentication_method,
                &entry->method_block);
            entry->method_length =
                packet->properties.authentication_method.length;
            if (status == MQTT_OK) {
                status = mqtt_page_arena_write(
                    &server->arena, entry->connect_block, body, length);
            }
            entry->connect_size = length;
            entry->version = MQTT_VERSION_5;
            entry->state = MQTT_CONNECTION_AUTHENTICATING;
            if (status == MQTT_OK) {
                status = connect_send_auth(
                    server, connection,
                    (uint8_t)MQTT5_RC_CONTINUE_AUTHENTICATION,
                    &packet->properties.authentication_method, &data_out);
            }
        }
    } else {
        /* The status says why it was refused. */
    }
    return status;
}

mqtt_status mqtt_connect_handle(mqtt_server *server, uint32_t connection,
                                const mqtt_fixed_header *header,
                                const uint8_t *body)
{
    /** @brief Remaining Length. */
    const uint32_t length = header->remaining_length;
    /** @brief The packet. */
    mqtt_connect packet;
    /** @brief The outcome. */
    mqtt_status status = mqtt_connect_decode(body, length, &packet);

    /* The flags could not be checked before now, as the rules depend on the
     * version. */
    if (status == MQTT_OK) {
        status = mqtt_fixed_header_check(header, packet.version);
    }
    if (status == MQTT_OK) {
        status = connect_check(server, &packet);
    }
    if (status == MQTT_OK) {
        if ((packet.version == MQTT_VERSION_5) &&
            mqtt5_properties_has(&packet.properties,
                                 MQTT5_PROP_AUTHENTICATION_METHOD)) {
            status =
                connect_auth_start(server, connection, &packet, body, length);
        } else {
            status = connect_accept(server, connection, &packet, NULL);
        }
    }
    if (status != MQTT_OK) {
        if (status == MQTT_UNSUPPORTED_PROTOCOL_VERSION) {
            /* The version is not known, so this uses the form of the most
             * widely known one. */
            mqtt_connect_refuse(server, connection, MQTT_VERSION_311,
                                connect_return_code(status));
        } else if (packet.version == MQTT_VERSION_5) {
            if (packet.will_flag &&
                (packet.will_qos > server->config.maximum_qos)) {
                mqtt_connect_refuse(server, connection, MQTT_VERSION_5,
                                    (uint8_t)MQTT5_RC_QOS_NOT_SUPPORTED);
            } else if (packet.will_flag && packet.will_retain &&
                       (!server->config.retain_available)) {
                mqtt_connect_refuse(server, connection, MQTT_VERSION_5,
                                    (uint8_t)MQTT5_RC_RETAIN_NOT_SUPPORTED);
            } else {
                mqtt_connect_refuse(server, connection, MQTT_VERSION_5,
                                    mqtt5_reason_code(status));
            }
        } else if ((status == MQTT_MALFORMED_PACKET) ||
                   (status == MQTT_PROTOCOL_ERROR) ||
                   (status == MQTT_TOPIC_NAME_INVALID)) {
            /* These versions have no code for this, and say to close. */
            mqtt_server_begin_closing(server, connection);
        } else {
            mqtt_connect_refuse(server, connection, packet.version,
                                connect_return_code(status));
        }
        server->statistics.protocol_errors += 1U;
    }
    return status;
}

/**
 * @brief Take the next step of an authentication exchange, and answer the AUTH
 * packet that asked for it.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] method The Authentication Method the exchange started with.
 * @param[in] packet The AUTH packet.
 * @return MQTT_OK, or the reason to close the connection.
 */
static mqtt_status connect_auth_step(mqtt_server *server, uint32_t connection,
                                     const mqtt_utf8 *method,
                                     const mqtt_reason *packet)
{
    /** @brief The connection. */
    mqtt_connection *entry = &server->storage.connections[connection];
    /** @brief Authentication Data to send. */
    mqtt_binary data_out = {0U, NULL};
    /** @brief The outcome. */
    mqtt_status status = server->config.auth_step(
        server->config.user_tag, connection, method,
        connect_auth_data(&packet->properties), &data_out);

    if (status == MQTT_INCOMPLETE) {
        entry->reauthenticating = (entry->state == MQTT_CONNECTION_ACTIVE);
        status = connect_send_auth(server, connection,
                                   (uint8_t)MQTT5_RC_CONTINUE_AUTHENTICATION,
                                   method, &data_out);
    } else if (status != MQTT_OK) {
        /* The status says why it was refused. */
    } else if (entry->state == MQTT_CONNECTION_AUTHENTICATING) {
        /** @brief The CONNECT packet, as kept. */
        mqtt_connect kept;

        status = mqtt_connect_decode(
            mqtt_page_arena_bytes(&server->arena, entry->connect_block),
            entry->connect_size, &kept);
        if (status == MQTT_OK) {
            status = connect_accept(server, connection, &kept, &data_out);
        }
        /* It holds the Password and Authentication Data. */
        mqtt_page_arena_wipe(&server->arena, entry->connect_block);
        mqtt_page_arena_give(&server->arena, entry->connect_block);
        entry->connect_block = MQTT_PAGE_ARENA_NONE;
    } else {
        entry->reauthenticating = false;
        status = connect_send_auth(
            server, connection, (uint8_t)MQTT5_RC_SUCCESS, method, &data_out);
    }
    return status;
}

mqtt_status mqtt_connect_handle_auth(mqtt_server *server, uint32_t connection,
                                     const uint8_t *body, uint32_t length)
{
    /** @brief The connection. */
    const mqtt_connection *entry = &server->storage.connections[connection];
    /** @brief The packet. */
    mqtt_reason packet;
    /**
     * @brief true if this is the exchange of a CONNECT, whose failure gets a
     * CONNACK.
     */
    const bool connecting = (entry->state == MQTT_CONNECTION_AUTHENTICATING);
    /** @brief The outcome. */
    mqtt_status status = mqtt_auth_decode(body, length, &packet);

    if (status == MQTT_OK) {
        /** @brief The method the exchange started with. */
        mqtt_utf8 method = {0U, NULL};

        method.length = entry->method_length;
        if (entry->method_block != MQTT_PAGE_ARENA_NONE) {
            method.data =
                mqtt_page_arena_bytes(&server->arena, entry->method_block);
        }
        /* The Authentication Method may not change, and only a known one may be
         * used at all. */
        if ((entry->method_block == MQTT_PAGE_ARENA_NONE) ||
            (server->config.auth_step == NULL) ||
            (!mqtt5_properties_has(&packet.properties,
                                   MQTT5_PROP_AUTHENTICATION_METHOD)) ||
            (!mqtt_utf8_equal(&packet.properties.authentication_method,
                              &method))) {
            status = MQTT_PROTOCOL_ERROR;
        } else if ((entry->state == MQTT_CONNECTION_AUTHENTICATING) &&
                   (packet.reason_code != MQTT5_RC_CONTINUE_AUTHENTICATION)) {
            status = MQTT_PROTOCOL_ERROR;
        } else if ((entry->state == MQTT_CONNECTION_ACTIVE) &&
                   (packet.reason_code !=
                    (entry->reauthenticating ?
                         MQTT5_RC_CONTINUE_AUTHENTICATION :
                         MQTT5_RC_RE_AUTHENTICATE))) {
            /* A re-authentication starts with 0x19 and goes on with 0x18:
             * section 4.12.1. */
            status = MQTT_PROTOCOL_ERROR;
        } else {
            status = connect_auth_step(server, connection, &method, &packet);
        }
    }
    if ((status != MQTT_OK) && connecting) {
        mqtt_connect_refuse(server, connection, MQTT_VERSION_5,
                            mqtt5_reason_code(status));
        server->statistics.protocol_errors += 1U;
    }
    return status;
}
