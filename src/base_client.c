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
 * @file base_client.c
 * @brief The MQTT Client: what the application calls, the messages held for
 * the Server, and the bytes waiting to go out.
 *
 * base_client_receive.c handles the packets the Server sends.
 *
 * Every message at QoS 1 or QoS 2 is encoded once and for all when it is
 * published, and the bytes are held in the arena until the message's flow ends.
 * So sending it again only has to set the DUP bit. A packet that is large
 * compared with the ring goes straight from the arena.
 *
 * [V31] MQTT V3.1 Protocol Specification.
 * @li Cited: 3, 4.
 * @li Source: public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015.
 * @li Cited: 3, 4.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 3, 4, p.42-117.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */

#include <stddef.h>

#include "base_client_id.h"
#include "base_client_internal.h"
#include "base_fixed.h"
#include "base_flags.h"
#include "base_keep_alive.h"
#include "base_qos.h"
#include "base_string.h"
#include "base_topic_check.h"
#include "base_type.h"
#include "base_utf8_check.h"
#include "v31_connect.h"
#include "v5_prop.h"
#include "v5_prop_value.h"
#include "v5_rc.h"
#include "v5_subscribe.h"

/** @brief How long to wait for a CONNACK packet, in milliseconds. */
#define CLIENT_CONNECT_TIMEOUT_MS 30000U
/**
 * @brief How often to look at the timers when nothing else says when, in
 * milliseconds.
 */
#define CLIENT_TICK_MS 1000U
/** @brief Smallest ring the client can work with, in bytes. */
#define CLIENT_RING_MIN 256U
/**
 * @brief How long, in milliseconds, what is left to send can hold up a closing
 * connection.
 */
#define CLIENT_CLOSING_LIMIT_MS 5000U

/**
 * @brief Count the bytes the ring could hold with every block.
 * @param[in] client The client.
 * @return The number of bytes.
 */
static uint32_t client_ring_capacity(const mqtt_client *client)
{
    return client->pool.block_count * client->pool.block_size;
}

uint32_t mqtt_client_ring_free(const mqtt_client *client)
{
    return mqtt_ring_room(&client->ring, client->pool.free_count);
}

/**
 * @brief Find the room in the ring for packets other than replies, which keep
 * a quarter of the blocks.
 * @param[in] client The client.
 * @return The number of bytes.
 */
static uint32_t client_ring_room(const mqtt_client *client)
{
    /** @brief The blocks kept for replies. */
    const uint32_t margin = client->pool.block_count / 4U;
    /** @brief The free blocks. */
    const uint32_t free_count = client->pool.free_count;

    return mqtt_ring_room(&client->ring,
                          (free_count > margin) ? (free_count - margin) : 0U);
}

void mqtt_client_report(const mqtt_client *client,
                        const mqtt_client_event *event)
{
    if (client->storage.on_event != NULL) {
        client->storage.on_event(client->storage.user_tag, event);
    }
}

bool mqtt_client_ready(const mqtt_client *client)
{
    return (client != NULL) && mqtt_bool_is_set(client->ready);
}

void mqtt_client_begin_closing(mqtt_client *client)
{
    if (client->state != MQTT_CLIENT_CLOSING) {
        client->closing_ms = client->now_ms;
    }
    client->state = MQTT_CLIENT_CLOSING;
}

/**
 * @brief Remove bytes from the front of the ring, wiping first those that may
 * hold a secret.
 * @param[in,out] client The client.
 * @param[in] count Number of bytes.
 */
static void client_ring_consume(mqtt_client *client, uint32_t count)
{
    /** @brief Number of them to wipe. */
    const uint32_t wipe =
        (count < client->secret_left) ? count : client->secret_left;

    mqtt_ring_wipe_front(&client->ring, wipe);
    client->secret_left -= wipe;
    (void)mqtt_ring_consume(&client->ring, count);
}

/**
 * @brief Empty the ring, wiping first the bytes that may hold a secret.
 * @param[in,out] client The client.
 */
static void client_ring_clear(mqtt_client *client)
{
    mqtt_ring_wipe_front(&client->ring, client->secret_left);
    client->secret_left = 0U;
    (void)mqtt_ring_clear(&client->ring);
    mqtt_client_journal_lost(client);
}

/**
 * @brief Queue a packet that may hold a secret, then wipe the block it was
 * written in and give it back.
 * @param[in,out] client The client.
 * @param[in] block The block holding the packet.
 * @param[in] length Number of bytes in the packet.
 * @return MQTT_OK, or why it could not be queued.
 */
static mqtt_status client_send_secret(mqtt_client *client, uint32_t block,
                                      uint32_t length)
{
    /** @brief The outcome. */
    const mqtt_status status = mqtt_client_send(
        client, mqtt_arena_bytes(&client->arena, block), length);

    if (status == MQTT_OK) {
        client->secret_left = client->ring.used;
    }
    mqtt_arena_wipe(&client->arena, block);
    mqtt_arena_give(&client->arena, block);
    return status;
}

mqtt_status mqtt_client_send(mqtt_client *client, const uint8_t *bytes,
                             uint32_t length)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (length > mqtt_client_ring_free(client)) {
        status = MQTT_BUFFER_TOO_SMALL;
    } else {
        mqtt_client_journal_hold(client, bytes, length);
        status = mqtt_ring_append(&client->ring, bytes, length);
        client->last_sent_ms = client->now_ms;
    }
    return status;
}

void mqtt_client_fail(mqtt_client *client, mqtt_status status)
{
    if ((client->version == MQTT_VERSION_5) &&
        (client->state == MQTT_CLIENT_ACTIVE)) {
        /** @brief The packet. */
        mqtt_reason packet;
        /** @brief The bytes. */
        uint8_t bytes[4] = {0};
        /** @brief The writer. */
        mqtt_writer writer = {NULL, 0U, 0U, MQTT_OK};

        mqtt_reason_clear(&packet);
        packet.reason_code = mqtt5_reason_code(status);
        mqtt_writer_init(&writer, bytes, 4U);
        mqtt_disconnect_encode(&writer, MQTT_VERSION_5, &packet);
        /* It closes all the same, and the application can see a reason did not
         * get through. */
        if (mqtt_client_send(client, bytes, writer.position) != MQTT_OK) {
            client->replies_lost += 1U;
        }
    }
    mqtt_client_begin_closing(client);
}

mqtt_status mqtt_client_resend_release(mqtt_client *client,
                                       uint16_t packet_identifier)
{
    /** @brief The packet. */
    mqtt_publish_response packet;
    /** @brief The bytes. */
    uint8_t bytes[8] = {0};
    /** @brief The writer. */
    mqtt_writer writer = {NULL, 0U, 0U, MQTT_OK};

    mqtt_publish_response_clear(&packet);
    packet.type = MQTT_PUBREL;
    packet.dup = true;
    packet.packet_identifier = packet_identifier;
    mqtt_writer_init(&writer, bytes, 8U);
    mqtt_publish_response_encode(&writer, client->version, &packet);
    return mqtt_client_send(client, bytes, writer.position);
}

void mqtt_client_message_remove(mqtt_client *client, uint32_t index)
{
    /** @brief The message. */
    mqtt_client_message *entry = &client->storage.messages[index];

    if (entry->packet_identifier != 0U) {
        mqtt_client_journal_mark(client,
                                 (uint8_t)MQTT_STORE_RECORD_CLIENT_FINISHED,
                                 entry->packet_identifier);
    }
    if (client->send_cursor == index) {
        client->send_cursor = entry->next;
    }
    if (client->release_cursor == index) {
        client->release_cursor = entry->next;
    }
    if (entry->previous == MQTT_CLIENT_NONE) {
        client->list_head = entry->next;
    } else {
        client->storage.messages[entry->previous].next = entry->next;
    }
    if (entry->next == MQTT_CLIENT_NONE) {
        client->list_tail = entry->previous;
    } else {
        client->storage.messages[entry->next].previous = entry->previous;
    }
    mqtt_arena_give(&client->arena, entry->block);
    entry->block = MQTT_ARENA_NONE;
    entry->next = client->free_message;
    client->free_message = index;
}

uint32_t mqtt_client_message_find(const mqtt_client *client,
                                  uint16_t packet_identifier)
{
    /** @brief The entry being looked at. */
    uint32_t index = client->list_head;

    while ((index != MQTT_CLIENT_NONE) &&
           (client->storage.messages[index].packet_identifier !=
            packet_identifier)) {
        index = client->storage.messages[index].next;
    }
    return index;
}

/**
 * @brief Find out whether a Packet Identifier is in use.
 * @param[in] client The client.
 * @param[in] candidate The Packet Identifier.
 * @return true if a message or a SUBSCRIBE or UNSUBSCRIBE packet is using it.
 */
static bool client_identifier_in_use(const mqtt_client *client,
                                     uint16_t candidate)
{
    /** @brief The answer. */
    bool used = false;
    /** @brief Index of the entry being looked at. */
    uint32_t index = 0U;

    while ((!used) && (index < client->storage.pending_count)) {
        used = (client->storage.pending[index].packet_identifier == candidate);
        index += 1U;
    }
    if (!used) {
        used = (mqtt_client_message_find(client, candidate) !=
                MQTT_CLIENT_NONE);
    }
    return used;
}

uint16_t mqtt_client_next_identifier(mqtt_client *client)
{
    /** @brief The value tried. */
    uint16_t candidate = client->last_packet_identifier;
    /** @brief Number of values tried. */
    uint32_t tries = 0U;
    /** @brief The answer, which stays zero if every value is in use. */
    uint16_t found = 0U;

    while ((found == 0U) && (tries < (uint32_t)UINT16_MAX)) {
        candidate = (candidate == UINT16_MAX) ?
                        (uint16_t)1U :
                        (uint16_t)((uint32_t)candidate + 1U);
        if (!client_identifier_in_use(client, candidate)) {
            found = candidate;
        }
        tries += 1U;
    }
    if (found != 0U) {
        client->last_packet_identifier = found;
    }
    return found;
}

uint32_t mqtt_client_message_add(mqtt_client *client, uint32_t block,
                                 uint32_t size, uint16_t packet_identifier)
{
    /** @brief The message. */
    const uint32_t index = client->free_message;
    /** @brief Its entry. */
    mqtt_client_message *entry = &client->storage.messages[index];

    client->free_message = entry->next;
    entry->block = block;
    entry->size = size;
    entry->packet_identifier = packet_identifier;
    entry->state = MQTT_CLIENT_MESSAGE_QUEUED;
    entry->on_wire = false;
    entry->next = MQTT_CLIENT_NONE;
    entry->previous = client->list_tail;
    if (client->list_tail == MQTT_CLIENT_NONE) {
        client->list_head = index;
    } else {
        client->storage.messages[client->list_tail].next = index;
    }
    client->list_tail = index;
    if (client->send_cursor == MQTT_CLIENT_NONE) {
        client->send_cursor = index;
    }
    return index;
}

/**
 * @brief Tell the application what became of a message that has left the list.
 * @param[in] client The client.
 * @param[in] packet_identifier The message's Packet Identifier.
 * @param[in] code The Reason Code.
 */
static void client_report_finished(const mqtt_client *client,
                                   uint16_t packet_identifier, uint8_t code)
{
    /** @brief The answer the Server would have given. */
    mqtt_publish_response response;
    /** @brief The event. */
    mqtt_client_event event;

    mqtt_client_event_init(&event, MQTT_CLIENT_EVENT_PUBLISHED);
    mqtt_publish_response_clear(&response);
    response.packet_identifier = packet_identifier;
    response.reason_code = code;
    event.response = &response;
    mqtt_client_report(client, &event);
}

void mqtt_client_discard_session(mqtt_client *client)
{
    /** @brief true while there may be more. */
    bool again = true;

    /* Each is reported, and the list is looked at afresh, as a callback may
     * change it. */
    while (again) {
        /** @brief The entry being looked at. */
        uint32_t index = client->list_head;

        while ((index != MQTT_CLIENT_NONE) &&
               (client->storage.messages[index].state ==
                MQTT_CLIENT_MESSAGE_QUEUED)) {
            index = client->storage.messages[index].next;
        }
        if (index == MQTT_CLIENT_NONE) {
            again = false;
        } else {
            /** @brief The message. */
            const mqtt_client_message *entry = &client->storage.messages[index];
            /** @brief The message's Packet Identifier. */
            const uint16_t identifier = entry->packet_identifier;
            /** @brief The Reason Code: past PUBREC, the Server had taken it. */
            const uint8_t code =
                (entry->state == MQTT_CLIENT_MESSAGE_RELEASED) ?
                    (uint8_t)MQTT5_RC_SUCCESS :
                    (uint8_t)MQTT5_RC_UNSPECIFIED_ERROR;

            mqtt_client_message_remove(client, index);
            client_report_finished(client, identifier, code);
        }
    }
    client->incoming_used = 0U;
    client->session_exists = false;
    mqtt_client_journal_session(client);
}

void mqtt_client_mark_resends(mqtt_client *client)
{
    /** @brief The entry being looked at. */
    uint32_t index = client->list_head;

    while (index != MQTT_CLIENT_NONE) {
        /** @brief The entry. */
        mqtt_client_message *entry = &client->storage.messages[index];

        if (entry->state == MQTT_CLIENT_MESSAGE_SENT) {
            /** @brief The bytes of the packet. */
            uint8_t *bytes = mqtt_arena_bytes(&client->arena, entry->block);

            bytes[0] = (uint8_t)((uint32_t)bytes[0] | MQTT_DUP);
        }
        /* The send quota starts again with each connection, so nothing counts
         * against it yet. */
        entry->on_wire = false;
        index = entry->next;
    }
}

/**
 * @brief Forget the Topic Aliases: they do not carry over to a new connection:
 * [MQTT-3.3.2-7].
 * @param[in,out] client The client.
 */
static void client_clear_aliases(mqtt_client *client)
{
    /** @brief Index of the entry. */
    uint32_t index = 0U;

    while (index < client->storage.alias_count) {
        mqtt_arena_give(&client->arena, client->storage.aliases[index].block);
        client->storage.aliases[index].block = MQTT_ARENA_NONE;
        client->storage.aliases[index].length = 0U;
        index += 1U;
    }
}

/**
 * @brief Forget what was asked of the Server on a connection that has gone.
 * @param[in,out] client The client.
 */
static void client_clear_pending(mqtt_client *client)
{
    /** @brief Index of the entry. */
    uint32_t index = 0U;

    while (index < client->storage.pending_count) {
        /** @brief The entry. */
        mqtt_client_pending *entry = &client->storage.pending[index];

        mqtt_arena_give(&client->arena, entry->block);
        entry->block = MQTT_ARENA_NONE;
        entry->packet_identifier = 0U;
        index += 1U;
    }
    client->pending_cursor = client->storage.pending_count;
}

/**
 * @brief Find the limit of the connected Server that a held message cannot be
 * sent under.
 * @param[in] client The client.
 * @param[in] entry The message.
 * @return The Reason Code that names the limit, or zero if there is none.
 */
static uint8_t client_held_refusal(const mqtt_client *client,
                                   const mqtt_client_message *entry)
{
    /** @brief The Reason Code. */
    uint32_t code = 0U;

    if (entry->qos > client->server_maximum_qos) {
        /* [MQTT-3.2.2-11] */
        code = MQTT5_RC_QOS_NOT_SUPPORTED;
    } else if (entry->retain && (!client->server_retain_available)) {
        /* [MQTT-3.2.2-14] */
        code = MQTT5_RC_RETAIN_NOT_SUPPORTED;
    } else if (entry->size > client->server_maximum_packet_size) {
        /* [MQTT-3.2.2-15] */
        code = MQTT5_RC_PACKET_TOO_LARGE;
    } else if (entry->topic_alias > client->server_topic_alias_maximum) {
        /* [MQTT-3.3.2-9] */
        code = MQTT5_RC_TOPIC_ALIAS_INVALID;
    } else {
        /* It can be sent. */
    }
    return (uint8_t)code;
}

void mqtt_client_refuse_held(mqtt_client *client)
{
    /** @brief true while there may be more. */
    bool again = true;

    while (again && (client->state == MQTT_CLIENT_CONNECTING)) {
        /** @brief The entry being looked at. */
        uint32_t index = client->list_head;
        /** @brief Why the entry found cannot be sent. */
        uint8_t code = 0U;

        while ((index != MQTT_CLIENT_NONE) && (code == 0U)) {
            /** @brief The entry. */
            const mqtt_client_message *entry = &client->storage.messages[index];

            if (entry->state != MQTT_CLIENT_MESSAGE_RELEASED) {
                code = client_held_refusal(client, entry);
            }
            if (code == 0U) {
                index = entry->next;
            }
        }
        if (index == MQTT_CLIENT_NONE) {
            again = false;
        } else {
            /** @brief The message's Packet Identifier. */
            const uint16_t identifier =
                client->storage.messages[index].packet_identifier;

            mqtt_client_message_remove(client, index);
            client_report_finished(client, identifier, code);
        }
    }
}

/**
 * @brief Forget the QoS 0 messages that were waiting for their turn, as the
 * connection has gone.
 *
 * One may depend on a Topic Alias, which ended with the connection.
 *
 * @param[in,out] client The client.
 */
static void client_drop_unsent_qos0(mqtt_client *client)
{
    /** @brief The entry being looked at. */
    uint32_t index = client->list_head;

    while (index != MQTT_CLIENT_NONE) {
        /** @brief The entry. */
        const mqtt_client_message *entry = &client->storage.messages[index];
        /** @brief The entry after it. */
        const uint32_t next = entry->next;

        if (entry->packet_identifier == 0U) {
            mqtt_client_message_remove(client, index);
        }
        index = next;
    }
}

/**
 * @brief Send again the SUBSCRIBE and UNSUBSCRIBE packets that resuming a
 * Session has made due, as far as there is room.
 * @param[in,out] client The client.
 * @return true if there was room for all of them.
 */
static bool client_pump_pending(mqtt_client *client)
{
    /** @brief true while there is room to go on. */
    bool going = true;

    while (going && (client->pending_cursor < client->storage.pending_count)) {
        /** @brief The packet. */
        const mqtt_client_pending *entry =
            &client->storage.pending[client->pending_cursor];

        if ((entry->packet_identifier != 0U) &&
            (entry->block != MQTT_ARENA_NONE)) {
            /**
             * @brief Its bytes, which from now on are marked as sent before.
             */
            uint8_t *bytes = mqtt_arena_bytes(&client->arena, entry->block);

            bytes[0] = (uint8_t)((uint32_t)bytes[0] | MQTT_DUP);
            going = (mqtt_client_send(client, bytes, entry->length) == MQTT_OK);
        }
        if (going) {
            client->pending_cursor += 1U;
        }
    }
    return going;
}

/**
 * @brief Send the PUBREL packets that a reconnection has made due again, as far
 * as there is room.
 * @param[in,out] client The client.
 * @return true if there was room for all of them.
 */
static bool client_pump_releases(mqtt_client *client)
{
    /** @brief true while there is room to go on. */
    bool going = true;

    while (going && (client->release_cursor != MQTT_CLIENT_NONE)) {
        /** @brief The message. */
        const mqtt_client_message *entry =
            &client->storage.messages[client->release_cursor];

        if (entry->state == MQTT_CLIENT_MESSAGE_RELEASED) {
            going = (mqtt_client_resend_release(
                         client, entry->packet_identifier) == MQTT_OK);
        }
        if (going) {
            client->release_cursor = entry->next;
        }
    }
    return going;
}

/**
 * @brief Move held PUBLISH packets into the ring, in order, as room and the
 * send quota allow.
 * @param[in,out] client The client.
 */
static void client_pump_publishes(mqtt_client *client)
{
    /** @brief true while there is room to go on. */
    bool going = true;

    while (going && (client->send_cursor != MQTT_CLIENT_NONE) &&
           (client->direct_left == 0U)) {
        /** @brief The message. */
        const uint32_t index = client->send_cursor;
        /** @brief The message. */
        mqtt_client_message *entry = &client->storage.messages[index];

        if (entry->state == MQTT_CLIENT_MESSAGE_RELEASED) {
            /* Its PUBREL has had its own turn. */
            client->send_cursor = entry->next;
        } else {
            /** @brief The bytes. */
            const uint8_t *bytes =
                mqtt_arena_bytes(&client->arena, entry->block);
            /**
             * @brief true if the message has an identifier, which is to say QoS
             * 1 or QoS 2.
             */
            const bool acknowledged = (entry->packet_identifier != 0U);
            /** @brief Number of bytes messages may use in the ring. */
            const uint32_t room = client_ring_room(client);

            if (acknowledged && (client->on_wire >= client->send_quota)) {
                going = false;
            } else if ((entry->size >
                        ((client_ring_capacity(client) * 3U) / 4U)) ||
                       (entry->size >= 4096U)) {
                mqtt_client_journal_sending(client, entry, true);
                client->direct_ring_bytes = client->ring.used;
                client->direct_data = bytes;
                client->direct_left = entry->size;
                client->direct_free_block = MQTT_ARENA_NONE;
                client->direct_block = entry->block;
            } else if (entry->size > room) {
                going = false;
            } else {
                mqtt_client_journal_sending(client, entry, false);
                going = (mqtt_ring_append(&client->ring, bytes, entry->size) ==
                         MQTT_OK);
            }
            if (going) {
                client->last_sent_ms = client->now_ms;
                client->send_cursor = entry->next;
                if (acknowledged) {
                    client->on_wire += 1U;
                    entry->on_wire = true;
                    entry->state = MQTT_CLIENT_MESSAGE_SENT;
                } else if (client->direct_left > 0U) {
                    /* A QoS 0 message whose bytes go directly stays until they
                     * have gone. */
                    client->direct_free_block = entry->block;
                    entry->block = MQTT_ARENA_NONE;
                    mqtt_client_message_remove(client, index);
                } else {
                    mqtt_client_message_remove(client, index);
                }
            }
        }
    }
}

void mqtt_client_pump(mqtt_client *client)
{
    /** @brief true while there is room to go on. */
    bool going = (client->state == MQTT_CLIENT_ACTIVE);

    if (going) {
        going = client_pump_pending(client);
    }
    /* The send quota only holds back PUBLISH packets, so these do not wait for
     * it: [MQTT-3.3.4-8]. */
    if (going) {
        going = client_pump_releases(client);
    }
    if (going) {
        client_pump_publishes(client);
    }
}

/**
 * @brief Decide whether a string of a CONNECT packet holds what its version
 * allows.
 * @param[in] text The string.
 * @param[in] version The protocol.
 * @return true if it does.
 */
static bool client_text_is_valid(const mqtt_utf8 *text, mqtt_version version)
{
    return (version == MQTT_VERSION_31) ?
               mqtt_utf8_is_well_formed(text->data, (uint32_t)text->length) :
               mqtt_utf8_is_valid(text->data, (uint32_t)text->length);
}

/**
 * @brief Note the limits a CONNECT packet gives the Server, which are the ones
 * this Client holds the Server to, and the Authentication Method it uses.
 * @param[in,out] client The client.
 * @param[in] sent The CONNECT packet as it goes out.
 */
static void client_note_limits(mqtt_client *client, const mqtt_connect *sent)
{
    /** @brief The properties. */
    const mqtt5_properties *properties = &sent->properties;
    /** @brief true for MQTT 5.0, the only version with these. */
    const bool v5 = (sent->version == MQTT_VERSION_5);

    client->alias_limit =
        (v5 &&
         mqtt5_properties_has(properties, MQTT5_PROP_TOPIC_ALIAS_MAXIMUM)) ?
            (uint32_t)properties->topic_alias_maximum :
            0U;
    client->receive_limit = client->storage.incoming_count;
    if (v5 && mqtt5_properties_has(properties, MQTT5_PROP_RECEIVE_MAXIMUM) &&
        ((uint32_t)properties->receive_maximum < client->receive_limit)) {
        client->receive_limit = (uint32_t)properties->receive_maximum;
    }
    client->packet_limit = client->storage.receive_size;
    if (v5 &&
        mqtt5_properties_has(properties, MQTT5_PROP_MAXIMUM_PACKET_SIZE) &&
        (properties->maximum_packet_size < client->packet_limit)) {
        client->packet_limit = properties->maximum_packet_size;
    }
    client->problem_information_off =
        v5 &&
        mqtt5_properties_has(properties,
                             MQTT5_PROP_REQUEST_PROBLEM_INFORMATION) &&
        (properties->request_problem_information == 0U);
    mqtt_arena_give(&client->arena, client->method_block);
    client->method_block = MQTT_ARENA_NONE;
    client->method_length = 0U;
    if (v5 &&
        mqtt5_properties_has(properties, MQTT5_PROP_AUTHENTICATION_METHOD) &&
        (properties->authentication_method.length > 0U)) {
        client->method_block = mqtt_arena_take(
            &client->arena, (uint32_t)properties->authentication_method.length);
        if (client->method_block != MQTT_ARENA_NONE) {
            client->method_length = properties->authentication_method.length;
            /* The block was just taken for the method, so it fits. */
            mqtt_bytes_put(
                mqtt_arena_bytes(&client->arena, client->method_block),
                properties->authentication_method.data,
                (uint32_t)properties->authentication_method.length);
        }
    }
}

bool mqtt_client_method_matches(const mqtt_client *client,
                                const mqtt5_properties *properties)
{
    /** @brief The answer. */
    bool matches =
        mqtt5_properties_has(properties, MQTT5_PROP_AUTHENTICATION_METHOD);

    if (matches && (client->method_block != MQTT_ARENA_NONE)) {
        matches =
            (properties->authentication_method.length ==
             client->method_length) &&
            mqtt_bytes_equal(
                properties->authentication_method.data,
                mqtt_arena_bytes(&client->arena, client->method_block),
                (uint32_t)client->method_length);
    }
    return matches;
}

/**
 * @brief Give the fields of a client their starting values.
 * @param[in,out] client The client, whose storage and ring are set up already.
 * @param[in] storage The memory.
 */
static void client_init_fields(mqtt_client *client,
                               const mqtt_client_storage *storage)
{
    mqtt_client_journal_init(client);
    client->state = MQTT_CLIENT_OFFLINE;
    client->version = MQTT_VERSION_311;
    client->method_block = MQTT_ARENA_NONE;
    client->method_length = 0U;
    client->problem_information_off = false;
    client->alias_limit = 0U;
    client->receive_limit = storage->incoming_count;
    client->packet_limit = storage->receive_size;
    client->now_ms = 0U;
    client->last_sent_ms = 0U;
    client->connect_sent_ms = 0U;
    client->ping_sent_ms = 0U;
    client->keep_alive_ms = 0U;
    client->list_head = MQTT_CLIENT_NONE;
    client->list_tail = MQTT_CLIENT_NONE;
    client->send_cursor = MQTT_CLIENT_NONE;
    client->free_message = MQTT_CLIENT_NONE;
    client->on_wire = 0U;
    client->send_quota = MQTT5_RECEIVE_MAXIMUM_DEFAULT;
    client->server_maximum_packet_size = 0xFFFFFFFFU;
    client->receive_used = 0U;
    client->receive_expected = 0U;
    client->incoming_used = 0U;
    client->direct_ring_bytes = 0U;
    client->secret_left = 0U;
    client->direct_data = NULL;
    client->direct_left = 0U;
    client->direct_free_block = MQTT_ARENA_NONE;
    client->direct_block = MQTT_ARENA_NONE;
    client->release_cursor = MQTT_CLIENT_NONE;
    client->last_packet_identifier = 0U;
    client->server_topic_alias_maximum = 0U;
    client->server_maximum_qos = MQTT_QOS_2;
    client->server_retain_available = true;
    client->session_exists = false;
    client->version_known = false;
    client->authentication_method_sent = false;
    client->replies_lost = 0U;
}

/**
 * @brief Put every message on the free list, and mark every Topic Alias and
 * every packet waiting for an answer as holding no bytes.
 * @param[in,out] client The client, whose fields are set up already.
 * @param[in] storage The memory.
 */
static void client_init_tables(mqtt_client *client,
                               const mqtt_client_storage *storage)
{
    /** @brief Index of the entry being set up. */
    uint32_t index = storage->message_count;

    while (index > 0U) {
        index -= 1U;
        storage->messages[index].block = MQTT_ARENA_NONE;
        storage->messages[index].next = client->free_message;
        client->free_message = index;
    }
    index = storage->alias_count;
    while (index > 0U) {
        index -= 1U;
        storage->aliases[index].block = MQTT_ARENA_NONE;
        storage->aliases[index].length = 0U;
    }
    index = storage->pending_count;
    while (index > 0U) {
        index -= 1U;
        storage->pending[index].block = MQTT_ARENA_NONE;
        storage->pending[index].length = 0U;
    }
}

/**
 * @brief Do what mqtt_client_init does, for arguments known to be valid.
 * @param[out] client The client.
 * @param[in] storage The memory, whose description is copied.
 * @return MQTT_OK or MQTT_INVALID_ARGUMENT.
 */
static mqtt_status client_init_body(mqtt_client *client,
                                    const mqtt_client_storage *storage)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    client->ready = MQTT_BOOL_UNSET;
    if ((storage->receive_bytes == NULL) ||
        (storage->receive_size < MQTT_FIXED_HEADER_BYTES) ||
        ((uint64_t)storage->pool_block_size *
             (uint64_t)storage->pool_block_count <
         (uint64_t)CLIENT_RING_MIN) ||
        (storage->messages == NULL) ||
        (storage->message_count == 0U) || (storage->incoming == NULL) ||
        (storage->incoming_count == 0U) || (storage->pending == NULL) ||
        (storage->pending_count == 0U) ||
        ((storage->aliases == NULL) && (storage->alias_count != 0U))) {
        status = MQTT_INVALID_ARGUMENT;
    } else {
        status = mqtt_pool_init(&client->pool, storage->pool_bytes,
                                storage->pool_next, storage->pool_block_size,
                                storage->pool_block_count);
    }
    if (status == MQTT_OK) {
        status = mqtt_arena_init(&client->arena, storage->arena_bytes,
                                 storage->arena_states, storage->arena_units);
    }
    if (status == MQTT_OK) {
        /* Copied as bytes, as assigning a struct this size would be done with
         * a call to memcpy. */
        mqtt_bytes_put((uint8_t *)&client->storage, (const uint8_t *)storage,
                       (uint32_t)sizeof(*storage));
        mqtt_ring_init(&client->ring, &client->pool);
        client_init_fields(client, storage);
        client_init_tables(client, storage);
        client_clear_pending(client);
        client->ready = MQTT_BOOL_SET;
    }
    return status;
}

mqtt_status mqtt_client_init(mqtt_client *client,
                             const mqtt_client_storage *storage)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if ((client != NULL) && (storage != NULL)) {
        answer = client_init_body(client, storage);
    }
    return answer;
}

/**
 * @brief Do what mqtt_client_assume_session does, for a client known to be
 * ready.
 * @param[in,out] client The client.
 */
static void client_assume_session_body(mqtt_client *client)
{
    client->session_exists = true;
}

void mqtt_client_assume_session(mqtt_client *client)
{
    if (mqtt_client_ready(client)) {
        client_assume_session_body(client);
    }
}

/**
 * @brief Refuse a CONNECT packet its version doesn't allow, as the Server would
 * only close.
 * @param[in] packet The packet.
 * @return MQTT_OK, MQTT_INVALID_ARGUMENT, or MQTT_CLIENT_IDENTIFIER_NOT_VALID.
 */
static mqtt_status client_check_connect(const mqtt_connect *packet)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if ((packet->version != MQTT_VERSION_5) && packet->password_flag &&
        (!packet->user_name_flag)) {
        /* [V311 MQTT-3.1.2-22], and what section 2.2 of MQTT 3.1 says of these
         * flags. */
        status = MQTT_INVALID_ARGUMENT;
    } else if (packet->will_qos >= MQTT_QOS_RESERVED) {
        /* [MQTT-3.1.2-12], and what section 2.2 of MQTT 3.1 says of this
         * field. */
        status = MQTT_INVALID_ARGUMENT;
    } else if ((!packet->will_flag) &&
               ((packet->will_qos != 0U) || packet->will_retain)) {
        /* [MQTT-3.1.2-11], [MQTT-3.1.2-13], and in MQTT 3.1 bits that aren't
         * used are zero. */
        status = MQTT_INVALID_ARGUMENT;
    } else if ((packet->version == MQTT_VERSION_311) &&
               (packet->client_identifier.length == 0U) &&
               (!packet->clean_start)) {
        /* [V311 MQTT-3.1.3-7] */
        status = MQTT_CLIENT_IDENTIFIER_NOT_VALID;
    } else if (!client_text_is_valid(&packet->client_identifier,
                                     packet->version)) {
        /* [MQTT-3.1.3-4], and section 2.5 of MQTT 3.1. */
        status = MQTT_CLIENT_IDENTIFIER_NOT_VALID;
    } else if (packet->user_name_flag &&
               (!client_text_is_valid(&packet->user_name, packet->version))) {
        /* [MQTT-3.1.3-11] */
        status = MQTT_INVALID_ARGUMENT;
    } else if (packet->will_flag &&
               (mqtt_topic_name_check(&packet->will_topic, packet->version) !=
                MQTT_OK)) {
        /* [MQTT-3.1.3-11], [MQTT-4.7.1-1] and [MQTT-4.7.3-1] hold for a Will
         * Topic too. */
        status = MQTT_TOPIC_NAME_INVALID;
    } else if (packet->version == MQTT_VERSION_31) {
        /** @brief Number of characters, which is what MQTT 3.1 limits. */
        const uint32_t characters = mqtt_utf8_character_count(
            packet->client_identifier.data, packet->client_identifier.length);
        /** @brief Index of the byte of the Will Message being looked at. */
        uint32_t index = 0U;

        if ((characters < MQTT_CLIENT_ID_MIN) ||
            (characters > MQTT_CLIENT_ID_MAX)) {
            status = MQTT_CLIENT_IDENTIFIER_NOT_VALID;
        }
        while (packet->will_flag && (status == MQTT_OK) &&
               (index < packet->will_payload.length)) {
            /* It goes out without its length, so MQTT 3.1 keeps it to 7-bit
             * ASCII. */
            if ((uint32_t)packet->will_payload.data[index] >
                MQTT31_WILL_MESSAGE_ASCII_MAX) {
                status = MQTT_INVALID_ARGUMENT;
            }
            index += 1U;
        }
    } else {
        /* The other versions leave the length of the Client Identifier to the
         * Server. */
    }
    return status;
}

/**
 * @brief Decide whether a CONNECT packet gives the Server limits that this
 * Client's memory could not honour.
 * @param[in] client The client.
 * @param[in] sent The CONNECT packet.
 * @return true if it does, as the Client could not be held to more than its
 * memory allows.
 */
static bool client_limits_too_high(const mqtt_client *client,
                                   const mqtt_connect *sent)
{
    return (sent->version == MQTT_VERSION_5) &&
           ((mqtt5_properties_has(&sent->properties,
                                  MQTT5_PROP_TOPIC_ALIAS_MAXIMUM) &&
             ((uint32_t)sent->properties.topic_alias_maximum >
              client->storage.alias_count)) ||
            (mqtt5_properties_has(&sent->properties,
                                  MQTT5_PROP_RECEIVE_MAXIMUM) &&
             ((uint32_t)sent->properties.receive_maximum >
              client->storage.incoming_count)) ||
            (mqtt5_properties_has(&sent->properties,
                                  MQTT5_PROP_MAXIMUM_PACKET_SIZE) &&
             (sent->properties.maximum_packet_size >
              client->storage.receive_size)));
}

/**
 * @brief Add to an MQTT 5.0 CONNECT packet the limits this Client can honour
 * that it leaves out.
 * @param[in] client The client.
 * @param[in,out] sent The CONNECT packet.
 */
static void client_add_limits(const mqtt_client *client, mqtt_connect *sent)
{
    if ((!mqtt5_properties_has(&sent->properties,
                               MQTT5_PROP_RECEIVE_MAXIMUM)) &&
        (client->storage.incoming_count < MQTT5_RECEIVE_MAXIMUM_DEFAULT)) {
        sent->properties.receive_maximum =
            (uint16_t)client->storage.incoming_count;
        mqtt5_properties_mark(&sent->properties, MQTT5_PROP_RECEIVE_MAXIMUM);
    }
    if (!mqtt5_properties_has(&sent->properties,
                              MQTT5_PROP_MAXIMUM_PACKET_SIZE)) {
        sent->properties.maximum_packet_size = client->storage.receive_size;
        mqtt5_properties_mark(&sent->properties,
                              MQTT5_PROP_MAXIMUM_PACKET_SIZE);
    }
    if ((!mqtt5_properties_has(&sent->properties,
                               MQTT5_PROP_TOPIC_ALIAS_MAXIMUM)) &&
        (client->storage.alias_count > 0U) &&
        (client->storage.alias_count <= (uint32_t)UINT16_MAX)) {
        sent->properties.topic_alias_maximum =
            (uint16_t)client->storage.alias_count;
        mqtt5_properties_mark(&sent->properties,
                              MQTT5_PROP_TOPIC_ALIAS_MAXIMUM);
    }
}

/**
 * @brief Set a client up for the connection a CONNECT packet starts.
 * @param[in,out] client The client.
 * @param[in] packet The packet as the caller gave it.
 * @param[in] sent The packet as it goes out.
 * @param[in] now_ms The time.
 */
static void client_start_connection(mqtt_client *client,
                                    const mqtt_connect *packet,
                                    const mqtt_connect *sent, uint64_t now_ms)
{
    if (packet->clean_start) {
        mqtt_client_discard_session(client);
    }
    client->version = packet->version;
    client->version_known = true;
    client->authentication_method_sent = mqtt5_properties_has(
        &packet->properties, MQTT5_PROP_AUTHENTICATION_METHOD);
    client_note_limits(client, sent);
    client->state = MQTT_CLIENT_CONNECTING;
    client->connect_sent_ms = now_ms;
    client->ping_sent_ms = 0U;
    client->keep_alive_ms = (uint64_t)packet->keep_alive * 1000U;
    client->receive_used = 0U;
    client->receive_expected = 0U;
    client_clear_aliases(client);
    /* Only MQTT 3.1 sends these again, which it does along with the rest of a
     * Session. */
    if ((packet->version != MQTT_VERSION_31) || packet->clean_start) {
        client_clear_pending(client);
    }
    client->pending_cursor = client->storage.pending_count;
    mqtt_client_journal_identifier(client, &packet->client_identifier);
    mqtt_client_journal_session(client);
}

/**
 * @brief Add what this Client can honour to a CONNECT packet, and queue it.
 * @param[in,out] client The client, which is offline.
 * @param[in] packet The packet, which has been checked.
 * @param[in] now_ms The time.
 * @return MQTT_OK, or why it was not queued.
 */
static mqtt_status client_send_connect(mqtt_client *client,
                                       const mqtt_connect *packet,
                                       uint64_t now_ms)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief The packet, with what this Client can honour added. */
    mqtt_connect sent;
    /** @brief A writer that only counts. */
    mqtt_writer counter = {NULL, 0U, 0U, MQTT_OK};

    /* Copied as bytes, as assigning a struct this size would be done with a
     * call to memcpy. */
    mqtt_bytes_put((uint8_t *)&sent, (const uint8_t *)packet,
                   (uint32_t)sizeof(*packet));
    if (client_limits_too_high(client, &sent)) {
        /* It could not be held to more than its memory allows. */
        status = MQTT_INVALID_ARGUMENT;
    } else if (packet->version == MQTT_VERSION_5) {
        client_add_limits(client, &sent);
    } else {
        /* The earlier versions have none of these to add. */
    }
    mqtt_writer_init(&counter, NULL, 0U);
    mqtt_connect_encode(&counter, &sent);
    if (status != MQTT_OK) {
        /* Refused above, before anything was sent or changed. */
    } else if (counter.status != MQTT_OK) {
        status = counter.status;
    } else if (counter.position > mqtt_client_ring_free(client)) {
        status = MQTT_BUFFER_TOO_SMALL;
    } else {
        /** @brief A block for the bytes. */
        const uint32_t block =
            mqtt_arena_take(&client->arena, counter.position);

        if (block == MQTT_ARENA_NONE) {
            status = MQTT_QUOTA_EXCEEDED;
        } else {
            /** @brief The writer. */
            mqtt_writer writer = {NULL, 0U, 0U, MQTT_OK};

            mqtt_writer_init(&writer, mqtt_arena_bytes(&client->arena, block),
                             counter.position);
            mqtt_connect_encode(&writer, &sent);
            client_start_connection(client, packet, &sent, now_ms);
            status = client_send_secret(client, block, writer.position);
        }
    }
    return status;
}

/**
 * @brief Do what mqtt_client_connect does, for a client known to be ready.
 * @param[in,out] client The client.
 * @param[in] packet The packet. The bytes it refers to are not used after this
 * call.
 * @param[in] now_ms The time.
 * @return MQTT_OK, MQTT_INVALID_STATE, or MQTT_BUFFER_TOO_SMALL.
 */
static mqtt_status client_connect_body(mqtt_client *client,
                                       const mqtt_connect *packet,
                                       uint64_t now_ms)
{
    /** @brief The outcome. */
    mqtt_status status = client_check_connect(packet);

    client->now_ms = now_ms;
    if (status != MQTT_OK) {
        /* Nothing has been changed, so the caller can try again with a better
         * one. */
    } else if (client->state != MQTT_CLIENT_OFFLINE) {
        status = MQTT_INVALID_STATE;
    } else if (client->version_known && (packet->version != client->version) &&
               (client->list_head != MQTT_CLIENT_NONE)) {
        /* What is held is encoded for the other version, so it could not be
         * sent on this one. */
        status = MQTT_INVALID_STATE;
    } else {
        status = client_send_connect(client, packet, now_ms);
    }
    return status;
}

mqtt_status mqtt_client_connect(mqtt_client *client, const mqtt_connect *packet,
                                uint64_t now_ms)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if (mqtt_client_ready(client) && (packet != NULL)) {
        answer = client_connect_body(client, packet, now_ms);
    }
    return answer;
}

/**
 * @brief Do what mqtt_client_connection_lost does, for a client known to be
 * ready.
 * @param[in,out] client The client.
 * @param[in] now_ms The time.
 */
static void client_connection_lost_body(mqtt_client *client, uint64_t now_ms)
{
    client->now_ms = now_ms;
    client->state = MQTT_CLIENT_OFFLINE;
    client_ring_clear(client);
    client->direct_ring_bytes = 0U;
    client->direct_left = 0U;
    client->direct_data = NULL;
    client->direct_block = MQTT_ARENA_NONE;
    mqtt_arena_give(&client->arena, client->direct_free_block);
    client->direct_free_block = MQTT_ARENA_NONE;
    client->on_wire = 0U;
    client_drop_unsent_qos0(client);
    client->send_cursor = client->list_head;
    client->release_cursor = MQTT_CLIENT_NONE;
}

void mqtt_client_connection_lost(mqtt_client *client, uint64_t now_ms)
{
    if (mqtt_client_ready(client)) {
        client_connection_lost_body(client, now_ms);
    }
}

/**
 * @brief Do what mqtt_client_transmit_view does, for a client known to be
 * ready.
 * @param[in,out] client The client.
 * @param[out] runs The runs, in the order they go, valid until the next call
 * about this client.
 * @param[in] max_runs Number of entries runs can hold. One for each block of
 * the ring, and one for a message sent straight from the arena, is enough for
 * everything that is waiting.
 * @return Number of runs, or zero if there is nothing to send now.
 */
static uint32_t client_transmit_view_body(mqtt_client *client, mqtt_run *runs,
                                          uint32_t max_runs)
{
    mqtt_client_pump(client);
    return mqtt_client_journal_view(client, runs, max_runs);
}

uint32_t mqtt_client_transmit_view(mqtt_client *client, mqtt_run *runs,
                                   uint32_t max_runs)
{
    /** @brief The answer. */
    uint32_t answer = 0U;

    if (mqtt_client_ready(client) && ((runs != NULL) || (max_runs == 0U))) {
        answer = client_transmit_view_body(client, runs, max_runs);
    }
    return answer;
}

/**
 * @brief Do what mqtt_client_transmit_consume does, for a client known to be
 * ready.
 * @param[in,out] client The client.
 * @param[in] count Number of bytes sent, which may end in any run. No more than
 * are waiting are taken.
 */
static void client_transmit_consume_body(mqtt_client *client, uint32_t count)
{
    /** @brief Number of bytes still to take. */
    uint32_t left = count;
    /** @brief Number taken from the ring before the payload. */
    const uint32_t before =
        (left < client->direct_ring_bytes) ? left : client->direct_ring_bytes;

    client->direct_ring_bytes -= before;
    client_ring_consume(client, before);
    mqtt_client_journal_consumed(client, before);
    left -= before;
    if ((left > 0U) && (client->direct_left > 0U)) {
        /** @brief Number taken from the payload. */
        const uint32_t taken =
            (left < client->direct_left) ? left : client->direct_left;

        client->direct_left -= taken;
        client->direct_data = &client->direct_data[taken];
        left -= taken;
        if (client->direct_left == 0U) {
            client->direct_data = NULL;
            client->direct_block = MQTT_ARENA_NONE;
            mqtt_arena_give(&client->arena, client->direct_free_block);
            client->direct_free_block = MQTT_ARENA_NONE;
        }
    }
    if ((left > 0U) && (client->direct_left == 0U)) {
        client_ring_consume(client, left);
        mqtt_client_journal_consumed(client, left);
    }
}

void mqtt_client_transmit_consume(mqtt_client *client, uint32_t count)
{
    if (mqtt_client_ready(client)) {
        client_transmit_consume_body(client, count);
    }
}

/**
 * @brief Do what mqtt_client_should_close does, for a client known to be ready.
 * @param[in] client The client.
 * @return true if it is closing and has nothing left to send.
 */
static bool client_should_close_body(const mqtt_client *client)
{
    return (client->state == MQTT_CLIENT_CLOSING) &&
           (client->ring.used == 0U) && (client->direct_left == 0U);
}

bool mqtt_client_should_close(const mqtt_client *client)
{
    /** @brief The answer. */
    bool answer = false;

    if (mqtt_client_ready(client)) {
        answer = client_should_close_body(client);
    }
    return answer;
}

/**
 * @brief Do what mqtt_client_tick does, for a client known to be ready.
 * @param[in,out] client The client.
 * @param[in] now_ms The time.
 * @return Number of milliseconds until the timers should next run.
 */
static uint32_t client_tick_body(mqtt_client *client, uint64_t now_ms)
{
    /** @brief How long until this should run again. */
    uint32_t wait = CLIENT_TICK_MS;

    client->now_ms = now_ms;
    if (client->state == MQTT_CLIENT_CONNECTING) {
        if (now_ms >=
            (client->connect_sent_ms + (uint64_t)CLIENT_CONNECT_TIMEOUT_MS)) {
            mqtt_client_begin_closing(client);
        }
    } else if (client->state == MQTT_CLIENT_CLOSING) {
        /* A Server that never reads must not hold on to this connection for
         * ever. */
        if (now_ms >=
            (client->closing_ms + (uint64_t)CLIENT_CLOSING_LIMIT_MS)) {
            client_ring_clear(client);
            client->direct_ring_bytes = 0U;
            client->direct_left = 0U;
            client->direct_data = NULL;
            client->direct_block = MQTT_ARENA_NONE;
            mqtt_arena_give(&client->arena, client->direct_free_block);
            client->direct_free_block = MQTT_ARENA_NONE;
        }
    } else if ((client->state == MQTT_CLIENT_ACTIVE) &&
               (client->keep_alive_ms != 0U)) {
        if (client->ping_sent_ms != 0U) {
            /* A PINGRESP has not come in a whole Keep Alive period: close. */
            if (now_ms >= (client->ping_sent_ms + client->keep_alive_ms)) {
                mqtt_client_fail(client, MQTT_INVALID_STATE);
            }
        } else if (now_ms >= (client->last_sent_ms + client->keep_alive_ms)) {
            /** @brief The bytes. */
            uint8_t bytes[2] = {0};
            /** @brief The writer. */
            mqtt_writer writer = {NULL, 0U, 0U, MQTT_OK};

            mqtt_writer_init(&writer, bytes, 2U);
            mqtt_pingreq_encode(&writer);
            if (mqtt_client_send(client, bytes, writer.position) == MQTT_OK) {
                client->ping_sent_ms = now_ms;
            }
        } else {
            /** @brief Time until a PINGREQ is due. */
            const uint64_t until =
                (client->last_sent_ms + client->keep_alive_ms) - now_ms;

            if (until < (uint64_t)CLIENT_TICK_MS) {
                wait = (uint32_t)until;
            }
        }
    } else {
        /* There is nothing to wait for in the other states. */
    }
    return wait;
}

uint32_t mqtt_client_tick(mqtt_client *client, uint64_t now_ms)
{
    /** @brief The answer. */
    uint32_t answer = CLIENT_TICK_MS;

    if (mqtt_client_ready(client)) {
        answer = client_tick_body(client, now_ms);
    }
    return answer;
}

/**
 * @brief Check a message against the limits the Server has set.
 * @param[in] client The client.
 * @param[in] packet The packet.
 * @return MQTT_OK, or why it must not be sent.
 */
static mqtt_status client_check_publish(const mqtt_client *client,
                                        const mqtt_publish *packet)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (packet->qos > MQTT_QOS_2) {
        status = MQTT_INVALID_ARGUMENT;
    } else if (mqtt5_properties_has(&packet->properties,
                                    MQTT5_PROP_SUBSCRIPTION_IDENTIFIER)) {
        /* Only a Server sends one: [MQTT-3.3.4-6]. */
        status = MQTT_INVALID_ARGUMENT;
    } else if (mqtt5_properties_has(&packet->properties,
                                    MQTT5_PROP_TOPIC_ALIAS) &&
               (packet->properties.topic_alias == 0U)) {
        /* [MQTT-3.3.2-8], whenever it would go out. */
        status = MQTT_TOPIC_ALIAS_INVALID;
    } else if (packet->topic_name.length > 0U) {
        status = mqtt_topic_name_check(&packet->topic_name, client->version);
    } else if ((!mqtt5_properties_has(&packet->properties,
                                      MQTT5_PROP_TOPIC_ALIAS)) ||
               (packet->qos != MQTT_QOS_0)) {
        /* A Topic Alias does not carry over to the connection a resend may go
         * out on: [MQTT-3.3.2-7]. */
        status = MQTT_TOPIC_NAME_INVALID;
    } else {
        /* An empty Topic Name is only right with a Topic Alias. */
    }
    if ((status == MQTT_OK) && (!client->version_known)) {
        /* Without knowing the version, the packet cannot be encoded. */
        status = MQTT_INVALID_STATE;
    }
    if ((status == MQTT_OK) && (client->state == MQTT_CLIENT_ACTIVE)) {
        if (packet->qos > client->server_maximum_qos) {
            /* [MQTT-3.2.2-11] */
            status = MQTT_INVALID_ARGUMENT;
        } else if (packet->retain && (!client->server_retain_available)) {
            /* [MQTT-3.2.2-14] */
            status = MQTT_INVALID_ARGUMENT;
        } else if (mqtt5_properties_has(&packet->properties,
                                        MQTT5_PROP_TOPIC_ALIAS) &&
                   (packet->properties.topic_alias >
                    client->server_topic_alias_maximum)) {
            /* [MQTT-3.3.2-9], [MQTT-3.2.2-17], [MQTT-3.2.2-18]. An alias of
             * 0 has been refused above. */
            status = MQTT_TOPIC_ALIAS_INVALID;
        } else {
            /* It is within the Server's limits. */
        }
    }
    return status;
}

/**
 * @brief Put a QoS 0 message straight into the ring, with nothing held.
 * @param[in,out] client The client, which has room in the ring for it.
 * @param[in,out] packet The packet. Its Packet Identifier is set here.
 * @param[in] size The size of the packet.
 * @param[in] now_ms The time.
 * @return MQTT_OK, or why it was not sent.
 */
static mqtt_status client_publish_direct(mqtt_client *client,
                                         mqtt_publish *packet, uint32_t size,
                                         uint64_t now_ms)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief Where the bytes can go straight in, next to those waiting. */
    mqtt_space space = {NULL, 0U};

    packet->packet_identifier = 0U;
    packet->dup = false;
    /* With no room left in the newest block, a new one may hold the packet. */
    if ((mqtt_ring_space(&client->ring, &space, 1U) == 0U) &&
        (size <= client->pool.block_size)) {
        (void)mqtt_ring_reserve(&client->ring, 1U);
        (void)mqtt_ring_space(&client->ring, &space, 1U);
    }
    if (size <= space.length) {
        /** @brief The writer. */
        mqtt_writer writer = {NULL, 0U, 0U, MQTT_OK};

        mqtt_writer_init(&writer, space.data, space.length);
        mqtt_publish_encode(&writer, client->version, packet);
        (void)mqtt_ring_commit(&client->ring, writer.position);
        client->last_sent_ms = now_ms;
    } else {
        /**
         * @brief A block for the bytes, as they would not be in one place in
         * the ring.
         */
        const uint32_t block = mqtt_arena_take(&client->arena, size);

        if (block == MQTT_ARENA_NONE) {
            status = MQTT_QUOTA_EXCEEDED;
        } else {
            /** @brief The writer. */
            mqtt_writer writer = {NULL, 0U, 0U, MQTT_OK};

            mqtt_writer_init(&writer, mqtt_arena_bytes(&client->arena, block),
                             size);
            mqtt_publish_encode(&writer, client->version, packet);
            status = mqtt_client_send(
                client, mqtt_arena_bytes(&client->arena, block), size);
            mqtt_arena_give(&client->arena, block);
        }
    }
    return status;
}

/**
 * @brief Encode a message into the arena, and hold it until it has been sent
 * and, at QoS 1 or QoS 2, until its flow ends.
 * @param[in,out] client The client.
 * @param[in,out] packet The packet. Its Packet Identifier is set here.
 * @param[in] size The size of the packet.
 * @return MQTT_OK, or why it was not accepted.
 */
static mqtt_status client_publish_hold(mqtt_client *client,
                                       mqtt_publish *packet, uint32_t size)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief A block for the bytes. */
    const uint32_t block = mqtt_arena_take(&client->arena, size);

    if (block == MQTT_ARENA_NONE) {
        status = MQTT_QUOTA_EXCEEDED;
    } else if (client->free_message == MQTT_CLIENT_NONE) {
        mqtt_arena_give(&client->arena, block);
        status = MQTT_QUOTA_EXCEEDED;
    } else {
        /** @brief The writer. */
        mqtt_writer writer = {NULL, 0U, 0U, MQTT_OK};

        packet->dup = false;
        packet->packet_identifier = (packet->qos == MQTT_QOS_0) ?
                                        (uint16_t)0U :
                                        mqtt_client_next_identifier(client);
        if ((packet->qos != MQTT_QOS_0) && (packet->packet_identifier == 0U)) {
            mqtt_arena_give(&client->arena, block);
            status = MQTT_QUOTA_EXCEEDED;
        } else {
            /** @brief The message. */
            uint32_t index = MQTT_CLIENT_NONE;
            /** @brief The entry for it. */
            mqtt_client_message *entry = NULL;

            mqtt_writer_init(&writer, mqtt_arena_bytes(&client->arena, block),
                             size);
            mqtt_publish_encode(&writer, client->version, packet);
            index = mqtt_client_message_add(client, block, size,
                                            packet->packet_identifier);
            entry = &client->storage.messages[index];
            entry->qos = packet->qos;
            entry->retain = packet->retain;
            entry->topic_alias =
                mqtt5_properties_has(&packet->properties,
                                     MQTT5_PROP_TOPIC_ALIAS) ?
                    packet->properties.topic_alias :
                    (uint16_t)0U;
            mqtt_client_journal_message(client, index);
            mqtt_client_pump(client);
        }
    }
    return status;
}

/**
 * @brief Do what mqtt_client_publish does, for a client known to be ready.
 * @param[in,out] client The client.
 * @param[in,out] packet The packet. Its Packet Identifier is set here.
 * @param[in] now_ms The time.
 * @return MQTT_OK, or why it was not accepted, in which case nothing has been
 * kept.
 */
static mqtt_status client_publish_body(mqtt_client *client,
                                       mqtt_publish *packet, uint64_t now_ms)
{
    /** @brief The outcome. */
    mqtt_status status = client_check_publish(client, packet);
    /** @brief The size of the packet. */
    const uint32_t size = mqtt_publish_size(client->version, packet);

    client->now_ms = now_ms;
    if (status != MQTT_OK) {
        /* It has been refused already. */
    } else if ((size == 0U) || (size > client->server_maximum_packet_size)) {
        /* [MQTT-3.2.2-15] */
        status = MQTT_PACKET_TOO_LARGE;
    } else if ((packet->qos == MQTT_QOS_0) &&
               (client->state != MQTT_CLIENT_ACTIVE)) {
        status = MQTT_INVALID_STATE;
    } else if ((packet->qos == MQTT_QOS_0) &&
               (client->send_cursor == MQTT_CLIENT_NONE) &&
               (client->direct_left == 0U) &&
               (size <= client_ring_room(client))) {
        status = client_publish_direct(client, packet, size, now_ms);
    } else {
        status = client_publish_hold(client, packet, size);
    }
    return status;
}

mqtt_status mqtt_client_publish(mqtt_client *client, mqtt_publish *packet,
                                uint64_t now_ms)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if (mqtt_client_ready(client) && (packet != NULL)) {
        answer = client_publish_body(client, packet, now_ms);
    }
    return answer;
}

/**
 * @brief Do what mqtt_client_disconnect does, for a client known to be ready.
 * @param[in,out] client The client.
 * @param[in] packet The packet, or NULL for a normal disconnection.
 * @return MQTT_OK, or why it could not be queued, in which case it closes
 * anyway.
 */
static mqtt_status client_disconnect_body(mqtt_client *client,
                                          const mqtt_reason *packet)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief A normal disconnection, if none was given. */
    static const mqtt_reason normal = {0};

    if (client->state == MQTT_CLIENT_ACTIVE) {
        /** @brief The bytes. */
        uint8_t bytes[1024];
        /** @brief The writer. */
        mqtt_writer writer = {NULL, 0U, 0U, MQTT_OK};

        mqtt_writer_init(&writer, bytes, 1024U);
        mqtt_disconnect_encode(&writer, client->version,
                               (packet != NULL) ? packet : &normal);
        status = writer.status;
        if ((status == MQTT_OK) &&
            (writer.position > client->server_maximum_packet_size) &&
            (packet != NULL)) {
            /* The Properties must go if they would make it too large:
             * [MQTT-3.14.2-3], -4. */
            /** @brief The same packet, with no Properties. */
            mqtt_reason plain;

            mqtt_reason_clear(&plain);
            plain.reason_code = packet->reason_code;
            mqtt_writer_init(&writer, bytes, 1024U);
            mqtt_disconnect_encode(&writer, client->version, &plain);
            status = writer.status;
        }
        if (status == MQTT_OK) {
            status = mqtt_client_send(client, bytes, writer.position);
        }
    }
    mqtt_client_begin_closing(client);
    return status;
}

mqtt_status mqtt_client_disconnect(mqtt_client *client,
                                   const mqtt_reason *packet)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if (mqtt_client_ready(client)) {
        answer = client_disconnect_body(client, packet);
    }
    return answer;
}

/**
 * @brief Do what mqtt_client_auth does, for a client known to be ready.
 * @param[in,out] client The client.
 * @param[in] packet The packet.
 * @return MQTT_OK, or why it could not be queued.
 */
static mqtt_status client_auth_body(mqtt_client *client,
                                    const mqtt_reason *packet)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if ((client->version != MQTT_VERSION_5) ||
        ((client->state != MQTT_CLIENT_ACTIVE) &&
         (client->state != MQTT_CLIENT_CONNECTING))) {
        status = MQTT_INVALID_STATE;
    } else if (!client->authentication_method_sent) {
        /* [MQTT-4.12.0-7] */
        status = MQTT_INVALID_STATE;
    } else if ((!mqtt_client_method_matches(client, &packet->properties)) ||
               ((packet->reason_code != MQTT5_RC_CONTINUE_AUTHENTICATION) &&
                (packet->reason_code != MQTT5_RC_RE_AUTHENTICATE)) ||
               ((packet->reason_code == MQTT5_RC_RE_AUTHENTICATE) &&
                (client->state != MQTT_CLIENT_ACTIVE))) {
        /* The same method all through: [MQTT-4.12.0-5], [MQTT-4.12.1-1]. A
         * Client only sends these two codes, and starts a re-authentication
         * once connected: [MQTT-3.15.2-1]. */
        status = MQTT_INVALID_ARGUMENT;
    } else {
        /** @brief A writer that only counts. */
        mqtt_writer counter = {NULL, 0U, 0U, MQTT_OK};
        /** @brief A block for the bytes. */
        uint32_t block = MQTT_ARENA_NONE;

        mqtt_writer_init(&counter, NULL, 0U);
        mqtt_auth_encode(&counter, packet);
        block = mqtt_arena_take(&client->arena, counter.position);
        if (block == MQTT_ARENA_NONE) {
            status = MQTT_QUOTA_EXCEEDED;
        } else if (counter.position > client->server_maximum_packet_size) {
            /* An exchange cannot go on without its Properties: [MQTT-3.15.2-2],
             * -3. */
            mqtt_arena_give(&client->arena, block);
            status = MQTT_PACKET_TOO_LARGE;
        } else {
            /** @brief The writer. */
            mqtt_writer writer = {NULL, 0U, 0U, MQTT_OK};

            mqtt_writer_init(&writer, mqtt_arena_bytes(&client->arena, block),
                             counter.position);
            mqtt_auth_encode(&writer, packet);
            status = client_send_secret(client, block, writer.position);
        }
    }
    return status;
}

mqtt_status mqtt_client_auth(mqtt_client *client, const mqtt_reason *packet)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if (mqtt_client_ready(client) && (packet != NULL)) {
        answer = client_auth_body(client, packet);
    }
    return answer;
}
