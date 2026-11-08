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
 * @file base_server_publish.c
 * @brief The MQTT Server: PUBLISH packets from Clients, and the packets that
 * acknowledge them and the messages sent to Clients.
 *
 * [V31] MQTT V3.1 Protocol Specification.
 * @li Cited: 3.3-3.7, 4.1.
 * @li Source: public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015.
 * @li Cited: 3.3-3.7, 4.3.3, 4.7.2.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 3.3-3.7, p.53-71. 4.3.3, p.95-96. 4.7.2, p.99. 4.9, p.103.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */

#include <stddef.h>

#include "base_qos.h"
#include "base_server_internal.h"
#include "base_type.h"
#include "v5_prop.h"
#include "v5_rc.h"
#include "v5_topic.h"

/**
 * @brief Send a PUBACK, PUBREC, PUBREL or PUBCOMP packet.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] type The packet type.
 * @param[in] packet_identifier The Packet Identifier.
 * @param[in] reason_code The Reason Code, which only MQTT 5.0 can carry.
 * @return MQTT_OK, or MQTT_BUFFER_TOO_SMALL if there is no room.
 */
static mqtt_status publish_send_response(mqtt_server *server,
                                         uint32_t connection, uint8_t type,
                                         uint16_t packet_identifier,
                                         uint8_t reason_code)
{
    /** @brief The bytes. */
    uint8_t bytes[8] = {0};
    /** @brief The writer. */
    mqtt_writer writer = {NULL, 0U, 0U, MQTT_OK};
    /** @brief The packet. */
    mqtt_publish_response packet;

    mqtt_publish_response_clear(&packet);
    packet.type = type;
    packet.packet_identifier = packet_identifier;
    packet.reason_code = reason_code;
    mqtt_writer_init(&writer, bytes, 8U);
    mqtt_publish_response_encode(
        &writer, server->storage.connections[connection].version, &packet);
    return mqtt_server_send(server, connection, bytes, writer.position);
}

/**
 * @brief Send a PUBREL packet for a PUBREC packet that has come again.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] packet_identifier The Packet Identifier.
 * @return MQTT_OK, or why it could not be sent.
 */
static mqtt_status publish_send_release_again(mqtt_server *server,
                                              uint32_t connection,
                                              uint16_t packet_identifier)
{
    /** @brief The bytes. */
    uint8_t bytes[8] = {0};
    /** @brief The writer. */
    mqtt_writer writer = {NULL, 0U, 0U, MQTT_OK};
    /** @brief The packet. */
    mqtt_publish_response packet;

    mqtt_publish_response_clear(&packet);
    packet.type = MQTT_PUBREL;
    packet.dup = true;
    packet.packet_identifier = packet_identifier;
    mqtt_writer_init(&writer, bytes, 8U);
    mqtt_publish_response_encode(
        &writer, server->storage.connections[connection].version, &packet);
    return mqtt_server_send(server, connection, bytes, writer.position);
}

/**
 * @brief Find a QoS 2 Packet Identifier in a Session's incoming list.
 * @param[in] server The server.
 * @param[in] session The Session.
 * @param[in] packet_identifier The Packet Identifier.
 * @return The entry, or MQTT_SERVER_NONE.
 */
static uint32_t publish_incoming_find(const mqtt_server *server,
                                      const mqtt_session *session,
                                      uint16_t packet_identifier)
{
    /** @brief The entry being looked at. */
    uint32_t index = session->incoming_head;

    while ((index != MQTT_SERVER_NONE) &&
           (server->storage.deliveries[index].packet_identifier !=
            packet_identifier)) {
        index = server->storage.deliveries[index].next;
    }
    return index;
}

/**
 * @brief Find a Packet Identifier among the messages sent and not yet
 * acknowledged.
 * @param[in] server The server.
 * @param[in] session The Session.
 * @param[in] packet_identifier The Packet Identifier.
 * @return The entry, or MQTT_SERVER_NONE.
 */
static uint32_t publish_inflight_find(const mqtt_server *server,
                                      const mqtt_session *session,
                                      uint16_t packet_identifier)
{
    /** @brief The entry being looked at. */
    uint32_t index = session->inflight_head;

    while ((index != MQTT_SERVER_NONE) &&
           (server->storage.deliveries[index].packet_identifier !=
            packet_identifier)) {
        index = server->storage.deliveries[index].next;
    }
    return index;
}

/**
 * @brief Resolve the Topic Name of a PUBLISH packet through its Topic Alias.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in,out] packet The packet. Its Topic Name is set if the packet leaves
 * it out.
 * @return MQTT_OK, MQTT_TOPIC_ALIAS_INVALID, MQTT_PROTOCOL_ERROR, or
 * MQTT_QUOTA_EXCEEDED.
 */
static mqtt_status publish_topic_alias(mqtt_server *server, uint32_t connection,
                                       mqtt_publish *packet)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (mqtt5_properties_has(&packet->properties, MQTT5_PROP_TOPIC_ALIAS)) {
        /** @brief The Topic Alias. */
        const uint16_t alias = packet->properties.topic_alias;

        if ((alias == 0U) || (alias > server->config.topic_alias_maximum)) {
            status = MQTT_TOPIC_ALIAS_INVALID;
        } else {
            /** @brief The entry for this Topic Alias on this connection. */
            mqtt_topic_alias *entry =
                &server->storage.aliases[(connection *
                                          server->config.topic_alias_maximum) +
                                         ((uint32_t)alias - 1U)];

            if (packet->topic_name.length == 0U) {
                if (entry->block == MQTT_PAGE_ARENA_NONE) {
                    status = MQTT_PROTOCOL_ERROR;
                } else {
                    packet->topic_name.data =
                        mqtt_page_arena_bytes(&server->arena, entry->block);
                    packet->topic_name.length = entry->length;
                }
            } else {
                /** @brief A block for the Topic Name. */
                const uint32_t block = mqtt_page_arena_take(
                    &server->arena, (uint32_t)packet->topic_name.length);

                if (block == MQTT_PAGE_ARENA_NONE) {
                    status = MQTT_QUOTA_EXCEEDED;
                } else {
                    mqtt_page_arena_give(&server->arena, entry->block);
                    entry->block = block;
                    entry->length = packet->topic_name.length;
                    status = mqtt_page_arena_write(
                        &server->arena, block, packet->topic_name.data,
                        (uint32_t)packet->topic_name.length);
                }
            }
        }
    }
    return status;
}

/**
 * @brief Check what the Properties of a PUBLISH packet can only be checked
 * against this Server.
 * @param[in] server The server.
 * @param[in] packet The packet.
 * @param[in] version The protocol in use.
 * @return MQTT_OK, or the reason to refuse the packet.
 */
static mqtt_status publish_check(const mqtt_server *server,
                                 const mqtt_publish *packet,
                                 mqtt_version version)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (mqtt5_properties_has(&packet->properties,
                             MQTT5_PROP_SUBSCRIPTION_IDENTIFIER)) {
        status = MQTT_PROTOCOL_ERROR;
    } else if (version == MQTT_VERSION_31) {
        /* It has no way to be told of these limits, and they don't stop it
         * being answered. */
    } else if (packet->qos > server->config.maximum_qos) {
        status = MQTT_PROTOCOL_ERROR;
    } else if (packet->retain && (!server->config.retain_available)) {
        status = MQTT_PROTOCOL_ERROR;
    } else {
        /* The rest is a matter for the Fixed Header check and the routing. */
    }
    return status;
}

/**
 * @brief Find the Reason Code for the refusal of a PUBLISH packet by this
 * Server's limits.
 * @param[in] server The server.
 * @param[in] packet The packet.
 * @return The Reason Code to go in the DISCONNECT packet.
 */
static uint8_t publish_refusal(const mqtt_server *server,
                               const mqtt_publish *packet)
{
    /** @brief The Reason Code. */
    uint32_t code = MQTT5_RC_PROTOCOL_ERROR;

    if (packet->qos > server->config.maximum_qos) {
        code = MQTT5_RC_QOS_NOT_SUPPORTED;
    } else if (packet->retain && (!server->config.retain_available)) {
        code = MQTT5_RC_RETAIN_NOT_SUPPORTED;
    } else {
        /* The default covers the others. */
    }
    return (uint8_t)code;
}

/**
 * @brief Take in an Application Message: retain it if asked, and send it to its
 * subscribers.
 * @param[in,out] server The server.
 * @param[in] packet The PUBLISH packet.
 * @param[in] payload Its Payload, in the runs it arrived in.
 * @param[in] publisher The Session that sent it.
 * @param[out] matched Number of subscriptions it matched.
 * @return MQTT_OK, or MQTT_QUOTA_EXCEEDED if it could not be retained.
 */
static mqtt_status publish_take_message(mqtt_server *server,
                                        const mqtt_publish *packet,
                                        const mqtt_run_list *payload,
                                        uint32_t publisher, uint32_t *matched)
{
    /** @brief The Properties that must not be forwarded as they are. */
    const uint64_t changed =
        ((uint64_t)1U << MQTT5_PROP_TOPIC_ALIAS) |
        ((uint64_t)1U << MQTT5_PROP_MESSAGE_EXPIRY_INTERVAL);
    /** @brief The message being routed. */
    mqtt_route_source source;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    source.topic_name = packet->topic_name;
    source.properties = packet->properties.raw;
    source.payload = *payload;
    source.expiry_ms = 0U;
    source.message = MQTT_SERVER_NONE;
    source.publisher = publisher;
    source.qos = packet->qos;
    source.retain = packet->retain;
    source.properties_raw = mqtt_bool_mask_any(&packet->properties.present,
                                               changed);
    if (mqtt5_properties_has(&packet->properties,
                             MQTT5_PROP_MESSAGE_EXPIRY_INTERVAL)) {
        source.expiry_ms =
            server->now_ms +
            ((uint64_t)packet->properties.message_expiry_interval * 1000U);
    }
    source.lost = false;
    if (mqtt_route_too_large(server, &source)) {
        /* It could never be sent to anyone, so this Server does not take
         * ownership of it. */
        *matched = 0U;
        server->statistics.messages_dropped += 1U;
        status = MQTT_QUOTA_EXCEEDED;
    } else {
        if (packet->retain && server->config.retain_available) {
            status = mqtt_session_retain(server, &source);
        }
        *matched = mqtt_route_publish(server, &source);
        mqtt_route_release(server, source.message);
        if (source.lost) {
            status = MQTT_QUOTA_EXCEEDED;
        }
    }
    server->statistics.messages_received += 1U;
    return status;
}

/**
 * @brief Find the Reason Code to acknowledge a QoS 1 or QoS 2 message with.
 * @param[in] server The server.
 * @param[in] status The outcome of taking the message.
 * @param[in] matched Number of subscriptions it matched.
 * @return The Reason Code.
 */
static uint8_t publish_reason(const mqtt_server *server, mqtt_status status,
                              uint32_t matched)
{
    /** @brief The Reason Code. */
    uint32_t code = MQTT5_RC_SUCCESS;

    if (status != MQTT_OK) {
        code = MQTT5_RC_QUOTA_EXCEEDED;
    } else if ((matched == 0U) &&
               server->config.report_no_matching_subscribers) {
        code = MQTT5_RC_NO_MATCHING_SUBSCRIBERS;
    } else {
        /* The message was accepted. */
    }
    return (uint8_t)code;
}

/**
 * @brief Ask the application whether a Client may publish to a topic.
 * @param[in] server The server.
 * @param[in] identifier The Client Identifier.
 * @param[in] topic The Topic Name.
 * @return true if it may, which is the answer when nothing is there to ask.
 */
static bool publish_allowed(const mqtt_server *server,
                            const mqtt_utf8 *identifier, const mqtt_utf8 *topic)
{
    /** @brief The answer. */
    bool allowed = true;

    if (server->config.authorize_publish != NULL) {
        allowed = server->config.authorize_publish(server->config.user_tag,
                                                   identifier, topic);
    }
    return allowed;
}

/**
 * @brief Take a record for a new QoS 2 flow from the pool, if the Session may
 * have one more.
 *
 * Every record of the Session counts, those an earlier connection left
 * included, so reconnecting can't hold more of them. Receive Maximum itself
 * counts this connection's flows only, as it starts again with each connection:
 * section 4.9.
 *
 * @param[in,out] server The server.
 * @param[in] session The Session.
 * @return The record, or MQTT_SERVER_NONE if the Session has as many as
 * receive_maximum or the pool is empty.
 */
static uint32_t publish_incoming_take(mqtt_server *server,
                                      const mqtt_session *session)
{
    /** @brief The record. */
    uint32_t index = MQTT_SERVER_NONE;

    if (session->incoming_count < server->config.receive_maximum) {
        index = mqtt_route_delivery_take(server);
    }
    return index;
}

/**
 * @brief Take a QoS 2 flow that came in off its Session's list and give its
 * record back, so that its Packet Identifier starts a new message.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] index The record, in the Session's incoming list.
 */
static void publish_incoming_unlink(mqtt_server *server, uint32_t connection,
                                    uint32_t index)
{
    /** @brief The connection. */
    mqtt_connection *entry = &server->storage.connections[connection];
    /** @brief The Session. */
    mqtt_session *session = &server->storage.sessions[entry->session];
    /** @brief The record. */
    const mqtt_delivery *record = &server->storage.deliveries[index];

    if (record->previous == MQTT_SERVER_NONE) {
        session->incoming_head = record->next;
    } else {
        server->storage.deliveries[record->previous].next = record->next;
    }
    if (record->next != MQTT_SERVER_NONE) {
        server->storage.deliveries[record->next].previous = record->previous;
    }
    session->incoming_count -= 1U;
    if (record->on_wire && (entry->incoming_here > 0U)) {
        entry->incoming_here -= 1U;
    }
    mqtt_journal_incoming(server, entry->session, record->packet_identifier,
                          false);
    mqtt_route_delivery_give(server, index);
}

/**
 * @brief Keep the record of a new QoS 2 Packet Identifier, take in its message,
 * and answer it with a PUBREC packet.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] packet The packet.
 * @param[in] payload Its Payload, in the runs it arrived in.
 * @param[in] index The record, taken from the pool.
 * @return MQTT_OK, or the reason to close the connection.
 */
static mqtt_status publish_qos2_record(mqtt_server *server, uint32_t connection,
                                       const mqtt_publish *packet,
                                       const mqtt_run_list *payload,
                                       uint32_t index)
{
    /** @brief The connection. */
    const mqtt_connection *entry = &server->storage.connections[connection];
    /** @brief The Session. */
    mqtt_session *session = &server->storage.sessions[entry->session];
    /** @brief The record. */
    mqtt_delivery *delivery = &server->storage.deliveries[index];
    /** @brief Number of subscriptions it matched. */
    uint32_t matched = 0U;
    /** @brief The outcome of taking the message. */
    mqtt_status taken = MQTT_OK;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    delivery->released = false;
    delivery->packet_identifier = packet->packet_identifier;
    /* Receive Maximum counts this connection's flows only: section 4.9. */
    delivery->on_wire = true;
    delivery->next = session->incoming_head;
    if (session->incoming_head != MQTT_SERVER_NONE) {
        server->storage.deliveries[session->incoming_head].previous = index;
    }
    session->incoming_head = index;
    session->incoming_count += 1U;
    server->storage.connections[connection].incoming_here += 1U;
    mqtt_journal_incoming(server, entry->session, packet->packet_identifier,
                          true);
    taken = publish_take_message(server, packet, payload, entry->session,
                                 &matched);
    /* MQTT 5.0 says the flow has ended, so a later PUBLISH with this
     * value is a new message: [MQTT-4.3.3-9]. The earlier versions
     * can't say so and have to close, and if anyone got the message,
     * the record has to stay so that the resend isn't delivered again:
     * [V311 MQTT-4.3.3-2]. A message that matched no subscription
     * reached nobody, so its resend is a new message too. */
    if ((taken != MQTT_OK) &&
        ((entry->version == MQTT_VERSION_5) || (matched == 0U))) {
        publish_incoming_unlink(server, connection, index);
    }
    if ((taken != MQTT_OK) && (entry->version != MQTT_VERSION_5)) {
        status = taken;
    } else {
        status = publish_send_response(server, connection, MQTT_PUBREC,
                                       packet->packet_identifier,
                                       publish_reason(server, taken, matched));
    }
    return status;
}

/**
 * @brief Handle a PUBLISH packet at QoS 2.
 *
 * The message is sent on as soon as it arrives, and its Packet Identifier is
 * kept until the PUBREL packet, which is how a resend of the same message is
 * recognised. This is the second of the two methods the specifications show for
 * this flow.
 *
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] packet The packet.
 * @param[in] payload Its Payload, in the runs it arrived in.
 * @return MQTT_OK, or the reason to close the connection.
 */
static mqtt_status publish_qos2(mqtt_server *server, uint32_t connection,
                                const mqtt_publish *packet,
                                const mqtt_run_list *payload)
{
    /** @brief The connection. */
    const mqtt_connection *entry = &server->storage.connections[connection];
    /** @brief The Session. */
    const mqtt_session *session = &server->storage.sessions[entry->session];
    /** @brief The record of this Packet Identifier, if the flow is open. */
    const uint32_t found =
        publish_incoming_find(server, session, packet->packet_identifier);
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (found != MQTT_SERVER_NONE) {
        /* Only a resend can repeat a Packet Identifier while the flow is
         * open. */
        /** @brief The Reason Code. */
        const uint8_t code =
            (packet->dup || (entry->version != MQTT_VERSION_5)) ?
                (uint8_t)MQTT5_RC_SUCCESS :
                (uint8_t)MQTT5_RC_PACKET_IDENTIFIER_IN_USE;

        /* With a failure, the next PUBLISH with this value is a new message:
         * [MQTT-4.3.3-9]. */
        if (code >= MQTT5_RC_FAILURE_MIN) {
            publish_incoming_unlink(server, connection, found);
        }
        status = publish_send_response(server, connection, MQTT_PUBREC,
                                       packet->packet_identifier, code);
    } else if (server->storage.connections[connection].incoming_here >=
               server->config.receive_maximum) {
        status = (entry->version == MQTT_VERSION_5) ?
                     MQTT_RECEIVE_MAXIMUM_EXCEEDED :
                     MQTT_QUOTA_EXCEEDED;
    } else {
        /** @brief The record of this Packet Identifier. */
        const uint32_t index = publish_incoming_take(server, session);

        if (index == MQTT_SERVER_NONE) {
            /* The Session is full or the pool is empty. In MQTT 5.0 this can be
             * said. Before that the connection has to close. */
            if (entry->version == MQTT_VERSION_5) {
                status = publish_send_response(
                    server, connection, MQTT_PUBREC, packet->packet_identifier,
                    (uint8_t)MQTT5_RC_QUOTA_EXCEEDED);
            } else {
                status = MQTT_QUOTA_EXCEEDED;
            }
        } else {
            status = publish_qos2_record(server, connection, packet, payload,
                                         index);
        }
    }
    return status;
}

/**
 * @brief Act on a PUBLISH packet that has been decoded.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] decoded The outcome of decoding it.
 * @param[in,out] packet The packet. A Topic Alias gives it its Topic Name.
 * @param[in] payload Its Payload, in the runs it arrived in.
 * @return MQTT_OK, or the reason to close the connection.
 */
static mqtt_status publish_handle_decoded(mqtt_server *server,
                                          uint32_t connection,
                                          mqtt_status decoded,
                                          mqtt_publish *packet,
                                          const mqtt_run_list *payload)
{
    /** @brief The connection. */
    const mqtt_connection *entry = &server->storage.connections[connection];
    /** @brief The outcome. */
    mqtt_status status = decoded;

    if (status == MQTT_OK) {
        status = publish_check(server, packet, entry->version);
        if (status != MQTT_OK) {
            /* The refusal gets a precise Reason Code here, as it is about this
             * Server's limits. Before MQTT 5.0 closing the connection is the
             * only way to refuse a message. */
            mqtt_server_disconnect(server, connection,
                                   publish_refusal(server, packet));
        } else if (entry->version == MQTT_VERSION_5) {
            status = publish_topic_alias(server, connection, packet);
        } else {
            /* There are no Topic Aliases before MQTT 5.0. */
        }
    }
    if (status == MQTT_OK) {
        /** @brief The Client Identifier, for the authorization callback. */
        const mqtt_utf8 identifier =
            mqtt_session_identifier(server, entry->session);

        /* Topics starting with a dollar are for the Server's own use, and a
         * Client of these versions should be kept from using them to reach
         * other Clients: section 4.7.2. */
        /** @brief true if this is refused for that reason. */
        const bool dollar =
            (entry->version != MQTT_VERSION_31) &&
            (!server->config.dollar_publish_allowed) &&
            (packet->topic_name.length > 0U) &&
            ((uint32_t)packet->topic_name.data[0] == MQTT5_TOPIC_DOLLAR);

        /** @brief true if the application allows it. */
        const bool allowed =
            publish_allowed(server, &identifier, &packet->topic_name);

        if (dollar || (!allowed)) {
            /* Refused: in MQTT 3.1 and 3.1.1 this is a positive
             * acknowledgement, as the rules allow. */
            if (packet->qos == MQTT_QOS_1) {
                status = publish_send_response(
                    server, connection, MQTT_PUBACK, packet->packet_identifier,
                    (uint8_t)MQTT5_RC_NOT_AUTHORIZED);
            } else if (packet->qos == MQTT_QOS_2) {
                status = publish_send_response(
                    server, connection, MQTT_PUBREC, packet->packet_identifier,
                    (uint8_t)MQTT5_RC_NOT_AUTHORIZED);
            } else {
                /* There is nothing to send for QoS 0. */
            }
        } else if (packet->qos == MQTT_QOS_0) {
            /** @brief Number of subscriptions it matched. */
            uint32_t matched = 0U;

            /* QoS 0 has no way to say so, which leaves this as the only record
             * of it. */
            if (publish_take_message(server, packet, payload,
                                     entry->session, &matched) != MQTT_OK) {
                server->statistics.messages_refused += 1U;
            }
        } else if (packet->qos == MQTT_QOS_1) {
            /** @brief Number of subscriptions it matched. */
            uint32_t matched = 0U;
            /** @brief The outcome of taking the message. */
            const mqtt_status taken =
                publish_take_message(server, packet, payload, entry->session,
                                     &matched);

            if ((taken != MQTT_OK) && (entry->version != MQTT_VERSION_5)) {
                /* There is no way to say so, and a PUBACK would claim the
                 * message is safe. */
                status = taken;
            } else {
                status = publish_send_response(
                    server, connection, MQTT_PUBACK, packet->packet_identifier,
                    publish_reason(server, taken, matched));
            }
        } else {
            status = publish_qos2(server, connection, packet, payload);
        }
    }
    return status;
}

mqtt_status mqtt_publish_handle_runs(mqtt_server *server, uint32_t connection,
                                     const mqtt_fixed_header *header,
                                     const uint8_t *head, uint32_t head_length,
                                     const mqtt_run_list *payload)
{
    /** @brief The connection. */
    const mqtt_connection *entry = &server->storage.connections[connection];
    /** @brief The packet. */
    mqtt_publish packet;
    /** @brief The outcome of decoding it. */
    mqtt_status status = mqtt_publish_decode_head(header, head, head_length,
                                                  entry->version, &packet);

    /* The caller worked out where the Payload starts, and the decoder must
     * agree. */
    if ((status == MQTT_OK) && (packet.payload.length != 0U)) {
        status = MQTT_MALFORMED_PACKET;
    }
    return publish_handle_decoded(server, connection, status, &packet,
                                  payload);
}

mqtt_status mqtt_publish_handle(mqtt_server *server, uint32_t connection,
                                const mqtt_fixed_header *header,
                                const uint8_t *body)
{
    /** @brief The connection. */
    const mqtt_connection *entry = &server->storage.connections[connection];
    /** @brief The packet. */
    mqtt_publish packet;
    /** @brief The Payload, which is in one place. */
    mqtt_run run = {NULL, 0U};
    /** @brief The Payload as a list of runs. */
    mqtt_run_list payload = {NULL, 0U, 0U};
    /** @brief The outcome of decoding it. */
    const mqtt_status status =
        mqtt_publish_decode(header, body, entry->version, &packet);

    if (status == MQTT_OK) {
        run.data = packet.payload.data;
        run.length = packet.payload.length;
        payload.runs = &run;
        payload.count = (run.length > 0U) ? 1U : 0U;
        payload.length = run.length;
    }
    return publish_handle_decoded(server, connection, status, &packet,
                                  &payload);
}

/**
 * @brief Handle a PUBREL packet: end the QoS 2 flow it names, and answer it
 * with a PUBCOMP packet.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] packet The packet.
 * @return MQTT_OK, or the reason to close the connection.
 */
static mqtt_status publish_handle_release(mqtt_server *server,
                                          uint32_t connection,
                                          const mqtt_publish_response *packet)
{
    /** @brief The connection. */
    const mqtt_connection *entry = &server->storage.connections[connection];
    /** @brief The Session. */
    const mqtt_session *session = &server->storage.sessions[entry->session];
    /** @brief The record of this Packet Identifier. */
    const uint32_t index =
        publish_incoming_find(server, session, packet->packet_identifier);
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (index == MQTT_SERVER_NONE) {
        status = publish_send_response(
            server, connection, MQTT_PUBCOMP, packet->packet_identifier,
            (uint8_t)MQTT5_RC_PACKET_IDENTIFIER_NOT_FOUND);
    } else {
        publish_incoming_unlink(server, connection, index);
        status = publish_send_response(server, connection, MQTT_PUBCOMP,
                                       packet->packet_identifier,
                                       (uint8_t)MQTT5_RC_SUCCESS);
    }
    return status;
}

/**
 * @brief Take a message whose flow has finished off the list of those sent and
 * not acknowledged.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] index The delivery.
 */
static void publish_inflight_finish(mqtt_server *server, uint32_t connection,
                                    uint32_t index)
{
    /** @brief The connection. */
    mqtt_connection *entry = &server->storage.connections[connection];
    /** @brief The message this answers. */
    const mqtt_delivery *delivery = &server->storage.deliveries[index];

    if (entry->resend == index) {
        entry->resend = delivery->next;
    }
    if (entry->resend_release == index) {
        entry->resend_release = delivery->next;
    }
    /* One sent on an earlier connection never counted against this one's
     * window. */
    if (delivery->on_wire) {
        entry->on_wire -= 1U;
    }
    mqtt_route_inflight_remove(server, entry->session, index);
}

/**
 * @brief Handle a PUBACK, PUBREC or PUBCOMP packet, which answers a message
 * sent and not yet acknowledged.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] header The Fixed Header.
 * @param[in] packet The packet.
 * @return MQTT_OK, or the reason to close the connection.
 */
static mqtt_status publish_inflight_answer(mqtt_server *server,
                                           uint32_t connection,
                                           const mqtt_fixed_header *header,
                                           const mqtt_publish_response *packet)
{
    /** @brief The connection. */
    const mqtt_connection *entry = &server->storage.connections[connection];
    /** @brief The Session. */
    const mqtt_session *session = &server->storage.sessions[entry->session];
    /** @brief The message this answers. */
    const uint32_t index =
        publish_inflight_find(server, session, packet->packet_identifier);
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (index == MQTT_SERVER_NONE) {
        /* A PUBREC with a failure ends the flow, so there is nothing to
         * release: [MQTT-4.3.3-4]. */
        if ((header->type == MQTT_PUBREC) &&
            (packet->reason_code < MQTT5_RC_FAILURE_MIN)) {
            status = publish_send_response(
                server, connection, MQTT_PUBREL, packet->packet_identifier,
                (uint8_t)MQTT5_RC_PACKET_IDENTIFIER_NOT_FOUND);
        }
    } else {
        /** @brief The message this answers. */
        mqtt_delivery *delivery = &server->storage.deliveries[index];
        /** @brief true if the message's flow is finished. */
        bool finished = false;

        if ((header->type == MQTT_PUBREC) && (delivery->qos == MQTT_QOS_2)) {
            if (packet->reason_code >= MQTT5_RC_FAILURE_MIN) {
                finished = true;
            } else if (delivery->released) {
                /* The PUBREC has come again, so its PUBREL goes again, marked
                 * as such. */
                status = publish_send_release_again(server, connection,
                                                    packet->packet_identifier);
            } else {
                delivery->released = true;
                mqtt_journal_released(server, index);
                mqtt_route_release(server, delivery->message);
                delivery->message = MQTT_SERVER_NONE;
                status = publish_send_response(server, connection, MQTT_PUBREL,
                                               packet->packet_identifier,
                                               (uint8_t)MQTT5_RC_SUCCESS);
            }
        } else if (((header->type == MQTT_PUBACK) &&
                    (delivery->qos == MQTT_QOS_1)) ||
                   ((header->type == MQTT_PUBCOMP) && delivery->released)) {
            finished = true;
        } else {
            /* It is inconsistent with the state of this flow, which is what a
             * Protocol Error is. */
            status = MQTT_PROTOCOL_ERROR;
        }
        if (finished) {
            publish_inflight_finish(server, connection, index);
        }
    }
    return status;
}

mqtt_status mqtt_publish_handle_response(mqtt_server *server,
                                         uint32_t connection,
                                         const mqtt_fixed_header *header,
                                         const uint8_t *body)
{
    /** @brief The connection. */
    const mqtt_connection *entry = &server->storage.connections[connection];
    /** @brief The packet. */
    mqtt_publish_response packet;
    /** @brief The outcome. */
    mqtt_status status =
        mqtt_publish_response_decode(header, body, entry->version, &packet);

    if (status != MQTT_OK) {
        /* There is nothing to do with a packet that is not valid. */
    } else if (header->type == MQTT_PUBREL) {
        status = publish_handle_release(server, connection, &packet);
    } else {
        status = publish_inflight_answer(server, connection, header, &packet);
    }
    return status;
}
