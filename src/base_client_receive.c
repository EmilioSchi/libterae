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
 * @file base_client_receive.c
 * @brief The MQTT Client: taking the bytes that arrive, and handling the
 * packets the Server sends.
 *
 * A packet that arrives whole is handled where it lies. One that arrives in
 * parts is gathered in receive_bytes first, which is what limits the largest
 * packet this Client takes. The replies a packet calls for go into the ring
 * with the rest.
 *
 * [V31] MQTT V3.1 Protocol Specification.
 * @li Cited: 3.2-3.7, 3.9, 3.11, 3.13, 4.1.
 * @li Source: public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015.
 * @li Cited: 3.2-3.7, 3.9, 3.11, 3.13, 4.3, 4.4.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 3.1.2.11.4, 3.1.2.11.7, p.36-38. 3.2-3.7, p.44-71. 3.9, 3.11,
 * 3.13-3.15, p.77-91. 4.3-4.5, p.93-97. 4.12, p.106-109.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */

#include <stddef.h>

#include "base_client_internal.h"
#include "base_qos.h"
#include "base_string.h"
#include "base_type.h"
#include "v5_prop.h"
#include "v5_prop_value.h"
#include "v5_rc.h"
#include "v5_type.h"

/**
 * @brief Send a PUBACK, PUBREC, PUBREL or PUBCOMP packet.
 * @param[in,out] client The client.
 * @param[in] type The packet type.
 * @param[in] packet_identifier The Packet Identifier.
 * @param[in] reason_code The Reason Code.
 * @return MQTT_OK or MQTT_BUFFER_TOO_SMALL.
 */
static mqtt_status client_respond(mqtt_client *client, uint8_t type,
                                  uint16_t packet_identifier,
                                  uint8_t reason_code)
{
    /** @brief The packet. */
    mqtt_publish_response packet;
    /** @brief The bytes. */
    uint8_t bytes[8] = {0};
    /** @brief The writer. */
    mqtt_writer writer = {NULL, 0U, 0U, MQTT_OK};

    mqtt_publish_response_clear(&packet);
    packet.type = type;
    packet.packet_identifier = packet_identifier;
    packet.reason_code = reason_code;
    mqtt_writer_init(&writer, bytes, 8U);
    mqtt_publish_response_encode(&writer, client->version, &packet);
    return mqtt_client_send(client, bytes, writer.position);
}

/**
 * @brief Refuse a Reason String or User Property the CONNECT packet asked the
 * Server not to send.
 * @param[in] client The client.
 * @param[in] properties The Properties of a packet other than PUBLISH, CONNACK
 * and DISCONNECT.
 * @return MQTT_OK or MQTT_PROTOCOL_ERROR: [MQTT-3.1.2-29].
 */
static mqtt_status client_check_problem_information(
    const mqtt_client *client, const mqtt5_properties *properties)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (client->problem_information_off &&
        (mqtt5_properties_has(properties, MQTT5_PROP_REASON_STRING) ||
         (properties->user_pair_count > 0U))) {
        status = MQTT_PROTOCOL_ERROR;
    }
    return status;
}

/**
 * @brief Note the limits a CONNACK packet sets, which this Client keeps to
 * while the connection lasts.
 * @param[in,out] client The client.
 * @param[in] packet The CONNACK packet.
 */
static void client_note_server_limits(mqtt_client *client,
                                      const mqtt_connack *packet)
{
    client->send_quota = MQTT5_RECEIVE_MAXIMUM_DEFAULT;
    client->server_maximum_packet_size = 0xFFFFFFFFU;
    client->server_topic_alias_maximum = 0U;
    client->server_maximum_qos = MQTT_QOS_2;
    client->server_retain_available = true;
    if (client->version == MQTT_VERSION_5) {
        if (mqtt5_properties_has(&packet->properties,
                                 MQTT5_PROP_RECEIVE_MAXIMUM)) {
            client->send_quota = (uint32_t)packet->properties.receive_maximum;
        }
        if (mqtt5_properties_has(&packet->properties,
                                 MQTT5_PROP_MAXIMUM_PACKET_SIZE)) {
            client->server_maximum_packet_size =
                packet->properties.maximum_packet_size;
        }
        if (mqtt5_properties_has(&packet->properties,
                                 MQTT5_PROP_TOPIC_ALIAS_MAXIMUM)) {
            client->server_topic_alias_maximum =
                packet->properties.topic_alias_maximum;
        }
        if (mqtt5_properties_has(&packet->properties, MQTT5_PROP_MAXIMUM_QOS)) {
            client->server_maximum_qos = packet->properties.maximum_qos;
        }
        if (mqtt5_properties_has(&packet->properties,
                                 MQTT5_PROP_RETAIN_AVAILABLE)) {
            client->server_retain_available =
                (packet->properties.retain_available != 0U);
        }
        if (mqtt5_properties_has(&packet->properties,
                                 MQTT5_PROP_SERVER_KEEP_ALIVE)) {
            client->keep_alive_ms =
                (uint64_t)packet->properties.server_keep_alive * 1000U;
        }
    }
}

/**
 * @brief Take up a connection the Server has accepted, and resume or start the
 * Session on it.
 * @param[in,out] client The client.
 * @param[in] packet The CONNACK packet, which accepts the connection.
 * @param[in] event The event that reports the packet.
 */
static void client_accept(mqtt_client *client, const mqtt_connack *packet,
                          const mqtt_client_event *event)
{
    if ((!packet->session_present) && (client->version == MQTT_VERSION_5)) {
        /* The Server starts afresh, and so must this Client: [MQTT-3.2.2-5].
         * The earlier versions let it go on with what it has, and send it
         * again: [V311 MQTT-4.4.0-1]. */
        mqtt_client_discard_session(client);
    }
    client->session_exists = true;
    if (mqtt5_properties_has(&packet->properties,
                             MQTT5_PROP_ASSIGNED_CLIENT_IDENTIFIER)) {
        /* The Session goes on under the name the Server gave it. */
        mqtt_client_journal_identifier(
            client, &packet->properties.assigned_client_identifier);
    }
    mqtt_client_journal_session(client);
    client->on_wire = 0U;
    client->pending_cursor = 0U;
    mqtt_client_mark_resends(client);
    client->send_cursor = client->list_head;
    client->release_cursor = client->list_head;
    client_note_server_limits(client, packet);
    mqtt_client_refuse_held(client);
    /* Only now can anything be sent, as a callback above may have done
     * something else. */
    if (client->state == MQTT_CLIENT_CONNECTING) {
        client->state = MQTT_CLIENT_ACTIVE;
        mqtt_client_report(client, event);
    }
}

/**
 * @brief Handle a CONNACK packet.
 * @param[in,out] client The client.
 * @param[in] body The bytes after the Fixed Header.
 * @param[in] length Remaining Length.
 * @return MQTT_OK, or the reason to close.
 */
static mqtt_status client_handle_connack(mqtt_client *client,
                                         const uint8_t *body, uint32_t length)
{
    /** @brief The packet. */
    mqtt_connack packet;
    /** @brief The outcome. */
    mqtt_status status =
        mqtt_connack_decode(body, length, client->version, &packet);

    if (client->state != MQTT_CLIENT_CONNECTING) {
        status = MQTT_PROTOCOL_ERROR;
    }
    if (status == MQTT_OK) {
        /** @brief The event. */
        mqtt_client_event event;

        mqtt_client_event_init(&event, MQTT_CLIENT_EVENT_CONNACK);
        event.connack = &packet;
        if (mqtt5_properties_has(&packet.properties,
                                 MQTT5_PROP_AUTHENTICATION_METHOD) &&
            ((!client->authentication_method_sent) ||
             (!mqtt_client_method_matches(client, &packet.properties)))) {
            /* [MQTT-4.12.0-6], and the method can't change: [MQTT-4.12.0-5]. */
            status = MQTT_PROTOCOL_ERROR;
        } else if (packet.reason_code != MQTT5_RC_SUCCESS) {
            /* Refused: there is nothing else to do with this connection. */
            mqtt_client_report(client, &event);
            mqtt_client_begin_closing(client);
            status = MQTT_NOT_AUTHORIZED;
        } else if (packet.session_present && (!client->session_exists)) {
            /* The Server holds a Session this Client does not have:
             * [MQTT-3.2.2-4]. */
            status = MQTT_PROTOCOL_ERROR;
        } else {
            client_accept(client, &packet, &event);
        }
    }
    return status;
}

/**
 * @brief Resolve the Topic Name of a PUBLISH packet through its Topic Alias.
 * @param[in,out] client The client.
 * @param[in,out] packet The packet. Its Topic Name is set if the packet leaves
 * it out.
 * @return MQTT_OK, MQTT_TOPIC_ALIAS_INVALID, MQTT_PROTOCOL_ERROR, or
 * MQTT_QUOTA_EXCEEDED.
 */
static mqtt_status client_topic_alias(mqtt_client *client, mqtt_publish *packet)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (mqtt5_properties_has(&packet->properties, MQTT5_PROP_TOPIC_ALIAS)) {
        /** @brief The Topic Alias. */
        const uint32_t alias = (uint32_t)packet->properties.topic_alias;

        if ((alias == 0U) || (alias > client->alias_limit)) {
            /* The limit is what the CONNECT packet said: [MQTT-3.3.2-10]. */
            status = MQTT_TOPIC_ALIAS_INVALID;
        } else {
            /** @brief The entry for this Topic Alias. */
            mqtt_client_alias *entry = &client->storage.aliases[alias - 1U];

            if (packet->topic_name.length == 0U) {
                if (entry->block == MQTT_ARENA_NONE) {
                    status = MQTT_PROTOCOL_ERROR;
                } else {
                    packet->topic_name.data =
                        mqtt_arena_bytes(&client->arena, entry->block);
                    packet->topic_name.length = entry->length;
                }
            } else {
                /** @brief A block for the Topic Name. */
                const uint32_t block = mqtt_arena_take(
                    &client->arena, (uint32_t)packet->topic_name.length);

                if (block == MQTT_ARENA_NONE) {
                    status = MQTT_QUOTA_EXCEEDED;
                } else {
                    mqtt_arena_give(&client->arena, entry->block);
                    entry->block = block;
                    entry->length = packet->topic_name.length;
                    status = mqtt_arena_write(
                        &client->arena, block, packet->topic_name.data,
                        (uint32_t)packet->topic_name.length);
                }
            }
        }
    }
    return status;
}

/**
 * @brief Handle a PUBLISH packet.
 *
 * Rules [MQTT-4.3.2-4], [MQTT-4.3.3-8] to -12 and [MQTT-4.5.0-2] apply here.
 *
 * @param[in,out] client The client.
 * @param[in] header The Fixed Header.
 * @param[in] body The bytes after the Fixed Header.
 * @return MQTT_OK, or the reason to close.
 */
static mqtt_status client_handle_publish(mqtt_client *client,
                                         const mqtt_fixed_header *header,
                                         const uint8_t *body)
{
    /** @brief The packet. */
    mqtt_publish packet;
    /** @brief The outcome. */
    mqtt_status status =
        mqtt_publish_decode(header, body, client->version, &packet);
    /** @brief The event. */
    mqtt_client_event event;

    mqtt_client_event_init(&event, MQTT_CLIENT_EVENT_MESSAGE);
    if ((status == MQTT_OK) && (client->version == MQTT_VERSION_5)) {
        status = client_topic_alias(client, &packet);
    }
    event.publish = &packet;
    if (status != MQTT_OK) {
        /* There is nothing to do with a packet that is not valid. */
    } else if (packet.qos == MQTT_QOS_0) {
        mqtt_client_report(client, &event);
    } else if (packet.qos == MQTT_QOS_1) {
        mqtt_client_report(client, &event);
        status = client_respond(client, MQTT_PUBACK, packet.packet_identifier,
                                (uint8_t)MQTT5_RC_SUCCESS);
    } else {
        /** @brief Index of the entry being looked at. */
        uint32_t index = 0U;
        /**
         * @brief true if this Packet Identifier is already between PUBLISH and
         * PUBREL.
         */
        bool seen = false;

        while ((!seen) && (index < client->incoming_used)) {
            seen =
                (client->storage.incoming[index] == packet.packet_identifier);
            index += 1U;
        }
        if (!seen) {
            if (client->incoming_used >= client->receive_limit) {
                /* The limit is what the CONNECT packet said: section 3.3.4. */
                status = MQTT_RECEIVE_MAXIMUM_EXCEEDED;
            } else {
                client->storage.incoming[client->incoming_used] =
                    packet.packet_identifier;
                client->incoming_used += 1U;
                mqtt_client_journal_mark(
                    client, (uint8_t)MQTT_STORE_RECORD_CLIENT_INCOMING,
                    packet.packet_identifier);
                mqtt_client_report(client, &event);
            }
        }
        if (status == MQTT_OK) {
            status =
                client_respond(client, MQTT_PUBREC, packet.packet_identifier,
                               (uint8_t)MQTT5_RC_SUCCESS);
        }
    }
    return status;
}

/**
 * @brief Handle a PUBREL packet, which ends the flow of a QoS 2 message from
 * the Server.
 * @param[in,out] client The client.
 * @param[in] packet The packet.
 * @return MQTT_OK, or the reason to close.
 */
static mqtt_status client_handle_pubrel(mqtt_client *client,
                                        const mqtt_publish_response *packet)
{
    /** @brief Index of the entry being looked at. */
    uint32_t index = 0U;
    /** @brief true once the entry has been found and removed. */
    bool found = false;

    while ((!found) && (index < client->incoming_used)) {
        if (client->storage.incoming[index] == packet->packet_identifier) {
            client->incoming_used -= 1U;
            client->storage.incoming[index] =
                client->storage.incoming[client->incoming_used];
            found = true;
            mqtt_client_journal_mark(
                client, (uint8_t)MQTT_STORE_RECORD_CLIENT_INCOMING_REMOVE,
                packet->packet_identifier);
        }
        index += 1U;
    }
    return client_respond(
        client, MQTT_PUBCOMP, packet->packet_identifier,
        found ? (uint8_t)MQTT5_RC_SUCCESS :
                (uint8_t)MQTT5_RC_PACKET_IDENTIFIER_NOT_FOUND);
}

/**
 * @brief Send the PUBREL packet that a PUBREC packet accepting a QoS 2 message
 * calls for, and let go of the bytes of its PUBLISH packet.
 * @param[in,out] client The client.
 * @param[in,out] entry The message.
 * @param[in] packet The PUBREC packet.
 * @return MQTT_OK, or the reason to close.
 */
static mqtt_status client_handle_pubrec(mqtt_client *client,
                                        mqtt_client_message *entry,
                                        const mqtt_publish_response *packet)
{
    /**
     * @brief true if this PUBREC has come before, so the PUBREL goes again.
     */
    const bool again = (entry->state != MQTT_CLIENT_MESSAGE_SENT);

    /* The bytes of the PUBLISH are no longer needed: [MQTT-4.3.3-6]. */
    if (entry->state == MQTT_CLIENT_MESSAGE_SENT) {
        mqtt_client_journal_mark(client,
                                 (uint8_t)MQTT_STORE_RECORD_CLIENT_RELEASED,
                                 entry->packet_identifier);
        entry->state = MQTT_CLIENT_MESSAGE_RELEASED;
        if ((client->direct_left > 0U) &&
            (client->direct_block == entry->block)) {
            client->direct_free_block = entry->block;
        } else {
            mqtt_arena_give(&client->arena, entry->block);
        }
        entry->block = MQTT_ARENA_NONE;
    }
    return again ? mqtt_client_resend_release(client,
                                              packet->packet_identifier) :
                   client_respond(client, MQTT_PUBREL,
                                  packet->packet_identifier,
                                  (uint8_t)MQTT5_RC_SUCCESS);
}

/**
 * @brief Take a message whose flow is finished off the list, and say so to the
 * application.
 * @param[in,out] client The client.
 * @param[in] index The message.
 * @param[in] packet The answer that finished it.
 */
static void client_finish_message(mqtt_client *client, uint32_t index,
                                  const mqtt_publish_response *packet)
{
    /** @brief The message. */
    mqtt_client_message *entry = &client->storage.messages[index];
    /** @brief The event. */
    mqtt_client_event event;

    mqtt_client_event_init(&event, MQTT_CLIENT_EVENT_PUBLISHED);
    /* One sent on an earlier connection never counted against this one's
     * quota. */
    if (entry->on_wire) {
        client->on_wire -= 1U;
    }
    if ((client->direct_left > 0U) && (entry->block != MQTT_ARENA_NONE) &&
        (client->direct_block == entry->block)) {
        client->direct_free_block = entry->block;
        entry->block = MQTT_ARENA_NONE;
    }
    mqtt_client_message_remove(client, index);
    event.response = packet;
    mqtt_client_report(client, &event);
}

/**
 * @brief Move a message's flow on by the Server's answer to it.
 * @param[in,out] client The client.
 * @param[in] header The Fixed Header of the answer.
 * @param[in] index The message.
 * @param[in] packet The answer: a PUBACK, PUBREC or PUBCOMP packet.
 * @return MQTT_OK, or the reason to close.
 */
static mqtt_status client_apply_answer(mqtt_client *client,
                                       const mqtt_fixed_header *header,
                                       uint32_t index,
                                       const mqtt_publish_response *packet)
{
    /** @brief The message. */
    mqtt_client_message *entry = &client->storage.messages[index];
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief true if the message's flow is finished. */
    bool finished = false;

    if (entry->state == MQTT_CLIENT_MESSAGE_QUEUED) {
        /* It has not been sent on this connection yet, and the answer may be
         * for an earlier one. */
    } else if (header->type == MQTT_PUBACK) {
        finished = (entry->qos == MQTT_QOS_1);
        /* It is inconsistent with the state of this flow, which is what a
         * Protocol Error is. */
        if (!finished) {
            status = MQTT_PROTOCOL_ERROR;
        }
    } else if (header->type == MQTT_PUBCOMP) {
        finished = (entry->state == MQTT_CLIENT_MESSAGE_RELEASED);
        if (!finished) {
            status = MQTT_PROTOCOL_ERROR;
        }
    } else if (entry->qos != MQTT_QOS_2) {
        status = MQTT_PROTOCOL_ERROR;
    } else if (packet->reason_code >= MQTT5_RC_FAILURE_MIN) {
        /* Treated as acknowledged, and not sent again: [MQTT-4.4.0-2]. */
        finished = (entry->state == MQTT_CLIENT_MESSAGE_SENT);
    } else {
        status = client_handle_pubrec(client, entry, packet);
    }
    if (finished) {
        client_finish_message(client, index, packet);
    }
    return status;
}

/**
 * @brief Handle a PUBACK, PUBREC or PUBCOMP packet, which answers a message
 * this Client sent.
 * @param[in,out] client The client.
 * @param[in] header The Fixed Header.
 * @param[in] packet The packet.
 * @return MQTT_OK, or the reason to close.
 */
static mqtt_status client_handle_answer(mqtt_client *client,
                                        const mqtt_fixed_header *header,
                                        const mqtt_publish_response *packet)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief The message this answers. */
    const uint32_t index =
        mqtt_client_message_find(client, packet->packet_identifier);

    if (index == MQTT_CLIENT_NONE) {
        /* A PUBREC with a failure ends the flow, so there is nothing to
         * release: [MQTT-4.3.3-4]. */
        if ((header->type == MQTT_PUBREC) &&
            (packet->reason_code < MQTT5_RC_FAILURE_MIN)) {
            status = client_respond(
                client, MQTT_PUBREL, packet->packet_identifier,
                (uint8_t)MQTT5_RC_PACKET_IDENTIFIER_NOT_FOUND);
        }
    } else {
        status = client_apply_answer(client, header, index, packet);
    }
    return status;
}

/**
 * @brief Handle a PUBACK, PUBREC, PUBREL or PUBCOMP packet.
 * @param[in,out] client The client.
 * @param[in] header The Fixed Header.
 * @param[in] body The bytes after the Fixed Header.
 * @return MQTT_OK, or the reason to close.
 */
static mqtt_status client_handle_response(mqtt_client *client,
                                          const mqtt_fixed_header *header,
                                          const uint8_t *body)
{
    /** @brief The packet. */
    mqtt_publish_response packet;
    /** @brief The outcome. */
    mqtt_status status =
        mqtt_publish_response_decode(header, body, client->version, &packet);

    if (status == MQTT_OK) {
        status = client_check_problem_information(client, &packet.properties);
    }

    if (status != MQTT_OK) {
        /* There is nothing to do with a packet that is not valid. */
    } else if (header->type == MQTT_PUBREL) {
        status = client_handle_pubrel(client, &packet);
    } else {
        status = client_handle_answer(client, header, &packet);
    }
    return status;
}

/**
 * @brief Handle a SUBACK or UNSUBACK packet.
 * @param[in,out] client The client.
 * @param[in] header The Fixed Header.
 * @param[in] body The bytes after the Fixed Header.
 * @return MQTT_OK, or the reason to close.
 */
static mqtt_status client_handle_subscribed(mqtt_client *client,
                                            const mqtt_fixed_header *header,
                                            const uint8_t *body)
{
    /** @brief The packet. */
    mqtt_subscribe_response packet;
    /** @brief The outcome. */
    mqtt_status status =
        (header->type == MQTT_SUBACK) ?
            mqtt_suback_decode(body, header->remaining_length, client->version,
                               &packet) :
            mqtt_unsuback_decode(body, header->remaining_length,
                                 client->version, &packet);

    if (status == MQTT_OK) {
        status = client_check_problem_information(client, &packet.properties);
    }

    if (status == MQTT_OK) {
        /** @brief Index of the entry being looked at. */
        uint32_t index = 0U;
        /** @brief The event. */
        mqtt_client_event event;

        mqtt_client_event_init(&event, MQTT_CLIENT_EVENT_SUBACK);
        while (index < client->storage.pending_count) {
            if (client->storage.pending[index].packet_identifier ==
                packet.packet_identifier) {
                mqtt_client_journal_mark(
                    client, (uint8_t)MQTT_STORE_RECORD_CLIENT_PENDING_DONE,
                    packet.packet_identifier);
                client->storage.pending[index].packet_identifier = 0U;
                mqtt_arena_give(&client->arena,
                                client->storage.pending[index].block);
                client->storage.pending[index].block = MQTT_ARENA_NONE;
            }
            index += 1U;
        }
        if (header->type == MQTT_UNSUBACK) {
            event.type = MQTT_CLIENT_EVENT_UNSUBACK;
        }
        event.subscribed = &packet;
        mqtt_client_report(client, &event);
    }
    return status;
}

/**
 * @brief Handle a PINGRESP packet.
 * @param[in,out] client The client.
 * @param[in] header The Fixed Header.
 * @return MQTT_OK, or the reason to close.
 */
static mqtt_status client_handle_pingresp(mqtt_client *client,
                                          const mqtt_fixed_header *header)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief The event. */
    mqtt_client_event event;

    mqtt_client_event_init(&event, MQTT_CLIENT_EVENT_PINGRESP);
    if (header->remaining_length != 0U) {
        status = MQTT_MALFORMED_PACKET;
    } else {
        client->ping_sent_ms = 0U;
        mqtt_client_report(client, &event);
    }
    return status;
}

/**
 * @brief Handle a DISCONNECT packet.
 * @param[in,out] client The client.
 * @param[in] header The Fixed Header.
 * @param[in] body The bytes after the Fixed Header.
 * @return MQTT_OK, or the reason to close.
 */
static mqtt_status client_handle_disconnect(mqtt_client *client,
                                            const mqtt_fixed_header *header,
                                            const uint8_t *body)
{
    /** @brief The packet. */
    mqtt_reason packet;
    /** @brief The outcome. */
    mqtt_status status = mqtt_disconnect_decode(body, header->remaining_length,
                                                client->version, &packet);
    /** @brief The event. */
    mqtt_client_event event;

    mqtt_client_event_init(&event, MQTT_CLIENT_EVENT_DISCONNECT);
    /* A Server may only send this in MQTT 5.0. */
    if (client->version != MQTT_VERSION_5) {
        status = MQTT_PROTOCOL_ERROR;
    }
    if (status == MQTT_OK) {
        event.reason = &packet;
        mqtt_client_report(client, &event);
        mqtt_client_begin_closing(client);
    }
    return status;
}

/**
 * @brief Handle an AUTH packet.
 * @param[in] client The client.
 * @param[in] header The Fixed Header.
 * @param[in] body The bytes after the Fixed Header.
 * @return MQTT_OK, or the reason to close.
 */
static mqtt_status client_handle_auth(const mqtt_client *client,
                                      const mqtt_fixed_header *header,
                                      const uint8_t *body)
{
    /** @brief The packet. */
    mqtt_reason packet;
    /** @brief The outcome. */
    mqtt_status status =
        mqtt_auth_decode(body, header->remaining_length, &packet);
    /** @brief The event. */
    mqtt_client_event event;

    mqtt_client_event_init(&event, MQTT_CLIENT_EVENT_AUTH);
    if ((status == MQTT_OK) &&
        (!mqtt_client_method_matches(client, &packet.properties))) {
        /* The method can't change during the exchange: [MQTT-4.12.0-5]. */
        status = MQTT_PROTOCOL_ERROR;
    }
    if (status == MQTT_OK) {
        status = client_check_problem_information(client, &packet.properties);
    }
    if (status == MQTT_OK) {
        event.reason = &packet;
        mqtt_client_report(client, &event);
    }
    return status;
}

/**
 * @brief Handle a whole packet.
 * @param[in,out] client The client.
 * @param[in] header The Fixed Header.
 * @param[in] body The bytes after the Fixed Header.
 * @return MQTT_OK, or the reason to close.
 */
static mqtt_status client_handle_packet(mqtt_client *client,
                                        const mqtt_fixed_header *header,
                                        const uint8_t *body)
{
    /** @brief The packet type. */
    const uint32_t type = (uint32_t)header->type;
    /** @brief The outcome. */
    mqtt_status status = mqtt_fixed_header_check(header, client->version);

    if (status != MQTT_OK) {
        /* The flags are wrong, and the packet does not get any further. */
    } else if ((client->state == MQTT_CLIENT_CONNECTING) &&
               (type != MQTT_CONNACK) && (type != MQTT5_AUTH)) {
        /* The Server must send a CONNACK before anything else: [MQTT-3.2.0-1],
         * [MQTT-3.14.0-1]. */
        status = MQTT_PROTOCOL_ERROR;
    } else if ((type == MQTT5_AUTH) && (!client->authentication_method_sent)) {
        /* [MQTT-4.12.0-6], [MQTT-4.12.0-7] */
        status = MQTT_PROTOCOL_ERROR;
    } else {
        switch (type) {
        case MQTT_CONNACK:
            status =
                client_handle_connack(client, body, header->remaining_length);
            break;
        case MQTT_PUBLISH:
            status = client_handle_publish(client, header, body);
            break;
        case MQTT_PUBACK:
        case MQTT_PUBREC:
        case MQTT_PUBREL:
        case MQTT_PUBCOMP:
            status = client_handle_response(client, header, body);
            break;
        case MQTT_SUBACK:
        case MQTT_UNSUBACK:
            status = client_handle_subscribed(client, header, body);
            break;
        case MQTT_PINGRESP:
            status = client_handle_pingresp(client, header);
            break;
        case MQTT_DISCONNECT:
            status = client_handle_disconnect(client, header, body);
            break;
        case MQTT5_AUTH:
            status = client_handle_auth(client, header, body);
            break;
        default:
            /* Only a Client sends the rest. */
            status = MQTT_PROTOCOL_ERROR;
            break;
        }
    }
    return status;
}

/**
 * @brief Take the next bytes of a packet that has arrived in parts.
 * @param[in,out] client The client.
 * @param[in] data The bytes.
 * @param[in] length Number of bytes in data.
 * @param[in,out] position Index of the next byte to take.
 * @return MQTT_OK, or the reason to close.
 */
static mqtt_status client_receive_parts(mqtt_client *client,
                                        const uint8_t *data, uint32_t length,
                                        uint32_t *position)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    /* While the size is not known, take a byte at a time, until the Fixed
     * Header is whole. */
    while ((status == MQTT_OK) && (client->receive_expected == 0U) &&
           (*position < length)) {
        /** @brief The Fixed Header. */
        mqtt_fixed_header header = {0U, 0U, 0U, 0U, true};
        /** @brief The outcome of decoding it. */
        mqtt_status decoded = MQTT_OK;

        client->storage.receive_bytes[client->receive_used] = data[*position];
        client->receive_used += 1U;
        *position += 1U;
        decoded = mqtt_fixed_header_decode(client->storage.receive_bytes,
                                           client->receive_used, &header);
        if (decoded == MQTT_OK) {
            if (((uint64_t)header.size + (uint64_t)header.remaining_length) >
                (uint64_t)client->packet_limit) {
                status = MQTT_PACKET_TOO_LARGE;
            } else {
                client->receive_expected =
                    header.size + header.remaining_length;
            }
        } else if (decoded != MQTT_INCOMPLETE) {
            status = decoded;
        } else {
            /* More bytes are needed. */
        }
    }
    if ((status == MQTT_OK) && (client->receive_expected != 0U)) {
        /** @brief Number of bytes still needed. */
        const uint32_t need = client->receive_expected - client->receive_used;
        /** @brief Number of bytes available. */
        const uint32_t left = length - *position;
        /** @brief Number of bytes to take. */
        const uint32_t take = (need < left) ? need : left;

        if (mqtt_bytes_copy(
                &client->storage.receive_bytes[client->receive_used],
                client->storage.receive_size - client->receive_used,
                &data[*position], take) == MQTT_OK) {
            client->receive_used += take;
            *position += take;
        } else {
            status = MQTT_INVALID_ARGUMENT;
        }
        if ((status == MQTT_OK) &&
            (client->receive_used == client->receive_expected)) {
            /** @brief The Fixed Header. */
            mqtt_fixed_header header = {0U, 0U, 0U, 0U, true};

            status = mqtt_fixed_header_decode(client->storage.receive_bytes,
                                              client->receive_used, &header);
            client->receive_used = 0U;
            client->receive_expected = 0U;
            if (status == MQTT_OK) {
                status = client_handle_packet(
                    client, &header,
                    &client->storage.receive_bytes[header.size]);
            }
        }
    }
    return status;
}

/**
 * @brief Take the next packet from bytes that start at a packet boundary.
 * @param[in,out] client The client.
 * @param[in] data The bytes.
 * @param[in] length Number of bytes in data.
 * @param[in,out] position Index of the next byte to take.
 * @return MQTT_OK, or the reason to close.
 */
static mqtt_status client_receive_packet(mqtt_client *client,
                                         const uint8_t *data, uint32_t length,
                                         uint32_t *position)
{
    /** @brief Number of bytes available. */
    const uint32_t left = length - *position;
    /** @brief The Fixed Header. */
    mqtt_fixed_header header = {0U, 0U, 0U, 0U, true};
    /** @brief The outcome. */
    mqtt_status status =
        mqtt_fixed_header_decode(&data[*position], left, &header);

    if (status == MQTT_INCOMPLETE) {
        status = client_receive_parts(client, data, length, position);
    } else if (status != MQTT_OK) {
        /* The Remaining Length is not valid. */
    } else if (((uint64_t)header.size + (uint64_t)header.remaining_length) >
               (uint64_t)client->packet_limit) {
        /* Larger than this Client said it takes, even if it arrived whole:
         * section 3.1.2.11.4. */
        status = MQTT_PACKET_TOO_LARGE;
    } else if (((uint64_t)header.size + (uint64_t)header.remaining_length) >
               (uint64_t)left) {
        status = client_receive_parts(client, data, length, position);
    } else {
        /** @brief The body of the packet. */
        const uint8_t *body = &data[*position + header.size];

        *position += header.size + header.remaining_length;
        status = client_handle_packet(client, &header, body);
    }
    return status;
}

/**
 * @brief Do what mqtt_client_receive_capacity does, for a client known to be
 * ready.
 * @param[in] client The client.
 * @return The number of bytes.
 */
static uint32_t client_receive_capacity_body(const mqtt_client *client)
{
    return (client->state == MQTT_CLIENT_CLOSING) ?
               0U :
               mqtt_client_ring_free(client);
}

uint32_t mqtt_client_receive_capacity(const mqtt_client *client)
{
    /** @brief The answer. */
    uint32_t answer = 0U;

    if (mqtt_client_ready(client)) {
        answer = client_receive_capacity_body(client);
    }
    return answer;
}

/**
 * @brief Do what mqtt_client_receive_space does, for a client known to be
 * ready.
 * @param[in] client The client.
 * @param[out] space The place.
 * @return Number of bytes it holds, zero if there is no room now.
 */
static uint32_t client_receive_space_body(const mqtt_client *client,
                                          mqtt_space *space)
{
    /** @brief The room left in the buffer. */
    const uint32_t left = client->storage.receive_size - client->receive_used;
    /** @brief The room for replies. */
    const uint32_t capacity = mqtt_client_receive_capacity(client);

    space->data = &client->storage.receive_bytes[client->receive_used];
    space->length = (left < capacity) ? left : capacity;
    return space->length;
}

uint32_t mqtt_client_receive_space(const mqtt_client *client, mqtt_space *space)
{
    /** @brief The answer. */
    uint32_t answer = 0U;

    if (mqtt_client_ready(client) && (space != NULL)) {
        answer = client_receive_space_body(client, space);
    }
    return answer;
}

/**
 * @brief Handle the whole packets at the front of the receive buffer.
 * @param[in,out] client The client.
 * @param[out] position Index of the first byte not handled.
 * @return MQTT_OK, or the reason to close.
 */
static mqtt_status client_receive_buffered(mqtt_client *client,
                                           uint32_t *position)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief true while a whole packet may be waiting. */
    bool going = true;

    *position = 0U;
    while (going && (status == MQTT_OK) &&
           (client->state != MQTT_CLIENT_CLOSING) &&
           (*position < client->receive_used)) {
        /** @brief The packet's first byte. */
        const uint8_t *bytes = &client->storage.receive_bytes[*position];
        /** @brief Number of bytes from there on. */
        const uint32_t left = client->receive_used - *position;
        /** @brief The Fixed Header. */
        mqtt_fixed_header header = {0U, 0U, 0U, 0U, true};
        /** @brief The outcome of decoding it. */
        const mqtt_status decoded =
            mqtt_fixed_header_decode(bytes, left, &header);

        if (decoded == MQTT_INCOMPLETE) {
            going = false;
        } else if (decoded != MQTT_OK) {
            status = decoded;
        } else if (((uint64_t)header.size +
                    (uint64_t)header.remaining_length) >
                   (uint64_t)client->packet_limit) {
            status = MQTT_PACKET_TOO_LARGE;
        } else if ((header.size + header.remaining_length) > left) {
            going = false;
        } else {
            *position += header.size + header.remaining_length;
            status = client_handle_packet(client, &header,
                                          &bytes[header.size]);
        }
    }
    return status;
}

/**
 * @brief Do what mqtt_client_receive_commit does, for a client known to be
 * ready.
 * @param[in,out] client The client.
 * @param[in] count Number of bytes written. No more than the place holds are
 * counted.
 * @param[in] now_ms The time.
 * @return MQTT_OK, or the reason the connection is now closing.
 */
static mqtt_status client_receive_commit_body(mqtt_client *client,
                                              uint32_t count, uint64_t now_ms)
{
    /** @brief The room left in the buffer. */
    const uint32_t left = client->storage.receive_size - client->receive_used;
    /** @brief Index of the first byte not handled. */
    uint32_t position = 0U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    client->now_ms = now_ms;
    client->receive_used += (count < left) ? count : left;
    client->receive_expected = 0U;
    status = client_receive_buffered(client, &position);
    if (position > 0U) {
        /**
         * @brief The buffer, held apart from the client, so each byte written
         * can't be taken to change it.
         */
        uint8_t *bytes = client->storage.receive_bytes;
        /** @brief Number of bytes left to move. */
        const uint32_t rest = client->receive_used - position;
        /** @brief Index of the byte being moved. */
        uint32_t index = 0U;

        /* The two ranges overlap, so this stays a forward byte loop:
         * mqtt_bytes_put may be memcpy, which needs them apart. */
        while (index < rest) {
            bytes[index] = bytes[position + index];
            index += 1U;
        }
        client->receive_used = rest;
    }
    if (status != MQTT_OK) {
        if (client->state != MQTT_CLIENT_CLOSING) {
            mqtt_client_fail(client, status);
        }
    } else {
        mqtt_client_pump(client);
    }
    return status;
}

mqtt_status mqtt_client_receive_commit(mqtt_client *client, uint32_t count,
                                       uint64_t now_ms)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if (mqtt_client_ready(client)) {
        answer = client_receive_commit_body(client, count, now_ms);
    }
    return answer;
}

/**
 * @brief Do what mqtt_client_receive does, for a client known to be ready.
 * @param[in,out] client The client.
 * @param[in] data The bytes.
 * @param[in] length Number of bytes in data.
 * @param[in] now_ms The time.
 * @return MQTT_OK, or the reason the connection is now closing.
 */
static mqtt_status client_receive_body(mqtt_client *client, const uint8_t *data,
                                       uint32_t length, uint64_t now_ms)
{
    /** @brief Index of the next byte to take. */
    uint32_t position = 0U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    client->now_ms = now_ms;
    while ((status == MQTT_OK) && (position < length) &&
           (client->state != MQTT_CLIENT_CLOSING)) {
        if (client->receive_used > 0U) {
            status = client_receive_parts(client, data, length, &position);
        } else {
            status = client_receive_packet(client, data, length, &position);
        }
    }
    if (status != MQTT_OK) {
        if (client->state != MQTT_CLIENT_CLOSING) {
            mqtt_client_fail(client, status);
        }
    } else {
        mqtt_client_pump(client);
    }
    return status;
}

mqtt_status mqtt_client_receive(mqtt_client *client, const uint8_t *data,
                                uint32_t length, uint64_t now_ms)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if (mqtt_client_ready(client) && ((data != NULL) || (length == 0U))) {
        answer = client_receive_body(client, data, length, now_ms);
    }
    return answer;
}

void mqtt_client_event_init(mqtt_client_event *event,
                            mqtt_client_event_type type)
{
    event->type = type;
    event->connack = NULL;
    event->publish = NULL;
    event->response = NULL;
    event->subscribed = NULL;
    event->reason = NULL;
}
