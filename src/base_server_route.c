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
 * @file base_server_route.c
 * @brief Storing messages, finding where they go, and turning them into bytes.
 *
 * A message at QoS 0 going to a Client with nothing waiting and room in its
 * ring goes straight in, with no copy made in the arena. Anything else is
 * stored, and held by a delivery in the Session's queue until mqtt_route_pump
 * gets to it. So one stored message can have many holders, and is freed when
 * the last one lets go.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 3.3, p.60-71. 4.3, 4.4, 4.6, 4.8.2, 4.9, p.92-110.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015.
 * @li Cited: 3.3, 4.3, 4.4, 4.6.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 */

#include <stddef.h>

#include "base_fixed.h"
#include "base_flags.h"
#include "base_qos.h"
#include "base_server_internal.h"
#include "base_string.h"
#include "base_type.h"
#include "base_wire.h"
#include "v5_prop.h"
#include "v5_subscribe.h"
#include "v5_topic.h"

/** @brief Payloads at least this large are not copied into a ring. */
#define ROUTE_DIRECT_PAYLOAD 4096U
/**
 * @brief Largest number of bytes before the Topic Name: Fixed Header and its
 * length.
 */
#define ROUTE_HEAD_BYTES 7U
/** @brief Largest number of bytes between Topic Name and Properties. */
#define ROUTE_MIDDLE_BYTES 16U

/** @brief The parts of a PUBLISH packet, ready to be put in a ring. */
typedef struct {
    /** @brief The Fixed Header and the Topic Name's length. */
    uint8_t head[ROUTE_HEAD_BYTES];
    /**
     * @brief The Packet Identifier, the Property Length and the Properties that
     * vary.
     */
    uint8_t middle[ROUTE_MIDDLE_BYTES];
    /** @brief Number of bytes in head. */
    uint32_t head_length;
    /** @brief Number of bytes in middle. */
    uint32_t middle_length;
    /** @brief Number of bytes in the whole packet. */
    uint32_t total;
    /** @brief Number of bytes before the Payload. */
    uint32_t before_payload;
} route_frame;

/** @brief What a PUBLISH packet is made from. */
typedef struct {
    /** @brief The Topic Name. */
    const uint8_t *topic;
    /** @brief The Properties. */
    const uint8_t *properties;
    /** @brief The Payload. */
    const uint8_t *payload;
    /** @brief The time the Message Expiry Interval ends, or zero. */
    uint64_t expiry_ms;
    /** @brief The Subscription Identifier, or zero. */
    uint32_t identifier;
    /** @brief Number of bytes of Properties. */
    uint32_t properties_length;
    /** @brief Number of bytes in the Payload. */
    uint32_t payload_length;
    /** @brief The Packet Identifier. */
    uint16_t packet_identifier;
    /** @brief Number of bytes in the Topic Name. */
    uint16_t topic_length;
    /** @brief The QoS. */
    uint8_t qos;
    /** @brief The RETAIN flag. */
    bool retain;
    /** @brief The DUP flag. */
    bool dup;
} route_parts;

/**
 * @brief What should happen to a message at the front of a connection's work.
 */
typedef enum {
    /** @brief It has gone into the ring, or is being sent directly. */
    ROUTE_SEND_DONE = 0,
    /** @brief There is not enough room yet. */
    ROUTE_SEND_WAIT,
    /** @brief It can never be sent, and is treated as sent. */
    ROUTE_SEND_TOO_LARGE
} route_send_result;

/**
 * @brief Find how many bytes of messages a connection's ring may take now, the
 * rest of its quota being kept for replies.
 * @param[in] server The server.
 * @param[in] entry The connection.
 * @return The number of bytes.
 */
static uint32_t route_ring_room(const mqtt_server *server,
                                const mqtt_connection *entry)
{
    return mqtt_server_room(server, entry, MQTT_SERVER_FOR_MESSAGE);
}

/**
 * @brief Note that a connection's ring had no room, and wait for blocks if no
 * write will bring the connection back.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 */
static void route_no_room(mqtt_server *server, uint32_t connection)
{
    /** @brief The connection. */
    const mqtt_connection *entry = &server->storage.connections[connection];

    if ((entry->ring.used == 0U) && (entry->direct_left == 0U)) {
        mqtt_server_wait_blocks(server, connection);
    }
}

/**
 * @brief Work out the header bytes of a PUBLISH packet.
 * @param[in] server The server.
 * @param[in] version The protocol of the connection it is for.
 * @param[in] parts What the packet is made from.
 * @param[out] frame The result.
 */
static void route_frame_build(const mqtt_server *server, mqtt_version version,
                              const route_parts *parts, route_frame *frame)
{
    /** @brief Writer over the first bytes. */
    mqtt_writer head = {NULL, 0U, 0U, MQTT_OK};
    /** @brief Writer over the second bytes. */
    mqtt_writer middle = {NULL, 0U, 0U, MQTT_OK};
    /** @brief The first byte. */
    uint32_t first = ((uint32_t)MQTT_PUBLISH << MQTT_TYPE_POS) |
                     ((uint32_t)parts->qos << MQTT_QOS_POS);
    /**
     * @brief Number of bytes in the Properties that vary from one send to
     * another.
     */
    uint32_t varying = 0U;
    /** @brief Number of bytes in the Remaining Length. */
    uint32_t remaining =
        2U + (uint32_t)parts->topic_length + parts->payload_length;

    if (parts->dup) {
        first |= MQTT_DUP;
    }
    if (parts->retain) {
        first |= MQTT_RETAIN;
    }
    if (parts->qos != MQTT_QOS_0) {
        remaining += 2U;
    }
    if (version == MQTT_VERSION_5) {
        if (parts->expiry_ms != 0U) {
            varying += 5U;
        }
        if (parts->identifier != 0U) {
            varying += 1U + mqtt_variable_byte_integer_size(parts->identifier);
        }
        varying += parts->properties_length;
        remaining += mqtt_variable_byte_integer_size(varying) + varying;
    }
    mqtt_writer_init(&head, frame->head, ROUTE_HEAD_BYTES);
    mqtt_writer_init(&middle, frame->middle, ROUTE_MIDDLE_BYTES);
    mqtt_write_byte(&head, (uint8_t)first);
    mqtt_write_variable_byte_integer(&head, remaining);
    mqtt_write_two_byte_integer(&head, parts->topic_length);
    if (parts->qos != MQTT_QOS_0) {
        mqtt_write_two_byte_integer(&middle, parts->packet_identifier);
    }
    if (version == MQTT_VERSION_5) {
        mqtt_write_variable_byte_integer(&middle, varying);
        if (parts->expiry_ms != 0U) {
            /**
             * @brief Number of whole seconds left, rounded up so it does not
             * read as expired.
             */
            const uint64_t left_ms = (parts->expiry_ms > server->now_ms) ?
                                         (parts->expiry_ms - server->now_ms) :
                                         0U;

            mqtt_write_byte(&middle,
                            (uint8_t)MQTT5_PROP_MESSAGE_EXPIRY_INTERVAL);
            mqtt_write_four_byte_integer(&middle,
                                         (uint32_t)((left_ms + 999U) / 1000U));
        }
        if (parts->identifier != 0U) {
            mqtt_write_byte(&middle,
                            (uint8_t)MQTT5_PROP_SUBSCRIPTION_IDENTIFIER);
            mqtt_write_variable_byte_integer(&middle, parts->identifier);
        }
    }
    frame->head_length = head.position;
    frame->middle_length = middle.position;
    frame->total = head.position + remaining - 2U;
    frame->before_payload = frame->total - parts->payload_length;
}

/**
 * @brief Add bytes to a ring, unless an earlier part has failed.
 * @param[in,out] ring The ring.
 * @param[in] data The bytes.
 * @param[in] length Number of bytes.
 * @param[in,out] status MQTT_OK until a part fails.
 */
static void route_append(mqtt_ring *ring, const uint8_t *data, uint32_t length,
                         mqtt_status *status)
{
    if (*status == MQTT_OK) {
        *status = mqtt_ring_append(ring, data, length);
    }
}

/**
 * @brief Put a PUBLISH packet in a ring, all of it or none: the bytes up to the
 * Payload, then the runs of the Payload given.
 * @param[in,out] ring The ring.
 * @param[in] version The protocol of the connection.
 * @param[in] parts What the packet is made from.
 * @param[in] frame The header bytes.
 * @param[in] payload The runs of the Payload, or NULL to leave it out.
 * @param[in] payload_runs Number of runs.
 * @return MQTT_OK, or MQTT_RING_FULL with nothing added.
 */
static mqtt_status route_put(mqtt_ring *ring, mqtt_version version,
                             const route_parts *parts, const route_frame *frame,
                             const mqtt_run *payload, uint32_t payload_runs)
{
    /** @brief Number of bytes to add. */
    uint32_t length = frame->before_payload;
    /** @brief Index of the run. */
    uint32_t run = 0U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_RING_FULL;

    while (run < payload_runs) {
        length += payload[run].length;
        run += 1U;
    }
    /* Each part below then finds its blocks free, so none can fail. */
    if (mqtt_ring_blocks_for(ring, length) <= ring->pool->free_count) {
        status = MQTT_OK;
        route_append(ring, frame->head, frame->head_length, &status);
        route_append(ring, parts->topic, (uint32_t)parts->topic_length,
                     &status);
        route_append(ring, frame->middle, frame->middle_length, &status);
        if (version == MQTT_VERSION_5) {
            route_append(ring, parts->properties, parts->properties_length,
                         &status);
        }
        run = 0U;
        while (run < payload_runs) {
            route_append(ring, payload[run].data, payload[run].length,
                         &status);
            run += 1U;
        }
    }
    return status;
}

/**
 * @brief Put a PUBLISH packet in a connection's ring, as route_put does. One
 * at QoS 2 sent for the first time first writes its Packet Identifier, and
 * then waits for it to be stored, as it has to be kept before it goes.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] delivery The delivery it is made from.
 * @param[in] parts What the packet is made from.
 * @param[in] frame The header bytes.
 * @param[in] payload The runs of the Payload, or NULL to leave it out.
 * @param[in] payload_runs Number of runs.
 * @return MQTT_OK, or MQTT_RING_FULL with nothing added.
 */
static mqtt_status route_hold_put(mqtt_server *server, uint32_t connection,
                                  const mqtt_delivery *delivery,
                                  const route_parts *parts,
                                  const route_frame *frame,
                                  const mqtt_run *payload,
                                  uint32_t payload_runs)
{
    /** @brief The connection. */
    mqtt_connection *entry = &server->storage.connections[connection];

    if (parts->qos == MQTT_QOS_2) {
        if (!delivery->dup) {
            mqtt_journal_sent(server, delivery);
        }
        mqtt_journal_hold(server, connection);
    }
    return route_put(&entry->ring, entry->version, parts, frame, payload,
                     payload_runs);
}

void mqtt_route_mark_pending(mqtt_server *server, uint32_t connection)
{
    /** @brief The connection. */
    mqtt_connection *entry = &server->storage.connections[connection];

    if (!entry->pending) {
        entry->pending = true;
        entry->next_pending = MQTT_SERVER_NONE;
        if (server->pending_tail == MQTT_SERVER_NONE) {
            server->pending_head = connection;
        } else {
            server->storage.connections[server->pending_tail].next_pending =
                connection;
        }
        server->pending_tail = connection;
    }
}

/**
 * @brief Copy the Properties of a message that may be forwarded.
 *
 * Topic Alias belongs to one connection. Subscription Identifier is set for
 * each subscriber. Message Expiry Interval is worked out again at each send.
 * Will Delay Interval is not part of a PUBLISH.
 *
 * @param[out] bytes The destination, which has room for them all.
 * @param[in] properties A block that has been checked.
 * @return The number of bytes copied.
 */
static uint32_t route_copy_properties(uint8_t *bytes,
                                      const mqtt_payload *properties)
{
    /** @brief A cursor over the block. */
    mqtt_reader cursor = {NULL, 0U, 0U, MQTT_OK};
    /** @brief The Property read. */
    mqtt5_property property;
    /** @brief The number of bytes copied. */
    uint32_t copied = 0U;
    /** @brief Index in the block where the Property starts. */
    uint32_t start = 0U;

    mqtt_reader_init(&cursor, properties->data, properties->length);
    while (mqtt5_property_next(&cursor, &property)) {
        if ((property.identifier != MQTT5_PROP_TOPIC_ALIAS) &&
            (property.identifier != MQTT5_PROP_SUBSCRIPTION_IDENTIFIER) &&
            (property.identifier != MQTT5_PROP_MESSAGE_EXPIRY_INTERVAL) &&
            (property.identifier != MQTT5_PROP_WILL_DELAY_INTERVAL)) {
            /** @brief Number of bytes the Property takes. */
            const uint32_t size = cursor.position - start;

            /* bytes has room for every Property copied. */
            mqtt_bytes_put(&bytes[copied], &properties->data[start], size);
            copied += size;
        }
        start = cursor.position;
    }
    return copied;
}

/**
 * @brief Copy the runs of a Payload into one place.
 * @param[out] to Where they go, with room for all of them.
 * @param[in] payload The runs.
 */
static void route_copy_runs(uint8_t *to, const mqtt_run_list *payload)
{
    /** @brief Index of the run. */
    uint32_t run = 0U;
    /** @brief Index of the next byte to write. */
    uint32_t position = 0U;

    while (run < payload->count) {
        /** @brief The run's bytes. */
        const uint8_t *data = payload->runs[run].data;
        /** @brief Number of bytes in the run. */
        const uint32_t length = payload->runs[run].length;

        /* to has room for every run. */
        mqtt_bytes_put(&to[position], data, length);
        position += length;
        run += 1U;
    }
}

void mqtt_route_payload(mqtt_route_source *source, mqtt_run *run,
                        const uint8_t *data, uint32_t length)
{
    run->data = data;
    run->length = length;
    source->payload.runs = run;
    source->payload.count = (length > 0U) ? 1U : 0U;
    source->payload.length = length;
}

uint32_t mqtt_route_store(mqtt_server *server, const mqtt_route_source *source)
{
    /** @brief The message. */
    uint32_t index = mqtt_slab_take(&server->message_slots);

    if (index != MQTT_SLAB_NONE) {
        /** @brief The message. */
        mqtt_message *message = &server->storage.messages[index];
        /** @brief Number of bytes needed. */
        const uint64_t size = (uint64_t)source->topic_name.length +
                              (uint64_t)source->properties.length +
                              (uint64_t)source->payload.length;
        /** @brief The block. */
        const uint32_t block =
            (size > (uint64_t)0x7FFFFFFFU) ?
                MQTT_PAGE_ARENA_NONE :
                mqtt_page_arena_take(&server->arena, (uint32_t)size);

        if (block == MQTT_PAGE_ARENA_NONE) {
            (void)mqtt_slab_give(&server->message_slots, index);
            index = MQTT_SERVER_NONE;
        } else {
            /** @brief Where the bytes go. */
            uint8_t *bytes = mqtt_page_arena_bytes(&server->arena, block);
            /** @brief Index of the next byte to write. */
            uint32_t position = (uint32_t)source->topic_name.length;

            message->block = block;
            message->store_id = 0U;
            message->store_generation = 0U;
            message->references = 1U;
            message->payload_length = source->payload.length;
            message->properties_length = source->properties.length;
            message->expiry_ms = source->expiry_ms;
            message->topic_length = source->topic_name.length;
            message->qos = source->qos;
            message->retain = source->retain;
            mqtt_bytes_put(bytes, source->topic_name.data, position);
            if (source->properties_raw) {
                message->properties_length = route_copy_properties(
                    &bytes[position], &source->properties);
                position += message->properties_length;
            } else if (source->properties.length > 0U) {
                mqtt_bytes_put(&bytes[position], source->properties.data,
                               source->properties.length);
                position += source->properties.length;
            } else {
                /* There are no Properties to copy. */
            }
            route_copy_runs(&bytes[position], &source->payload);
        }
    }
    return index;
}

void mqtt_route_hold(mqtt_server *server, uint32_t message)
{
    server->storage.messages[message].references += 1U;
}

void mqtt_route_release(mqtt_server *server, uint32_t message)
{
    if (message != MQTT_SERVER_NONE) {
        /** @brief The message. */
        mqtt_message *entry = &server->storage.messages[message];

        entry->references -= 1U;
        if (entry->references == 0U) {
            mqtt_journal_message_gone(server, message);
            mqtt_page_arena_give(&server->arena, entry->block);
            (void)mqtt_slab_give(&server->message_slots, message);
        }
    }
}

bool mqtt_route_expired(const mqtt_server *server, uint32_t message)
{
    /** @brief The message. */
    const mqtt_message *entry = &server->storage.messages[message];

    return (entry->expiry_ms != 0U) && (entry->expiry_ms <= server->now_ms);
}

uint32_t mqtt_route_delivery_take(mqtt_server *server)
{
    /** @brief The delivery. */
    const uint32_t index = mqtt_slab_take(&server->delivery_slots);

    if (index != MQTT_SLAB_NONE) {
        /** @brief The delivery. */
        mqtt_delivery *entry = &server->storage.deliveries[index];

        entry->store_id = 0U;
        entry->next = MQTT_SERVER_NONE;
        entry->previous = MQTT_SERVER_NONE;
        entry->message = MQTT_SERVER_NONE;
        entry->on_wire = false;
        entry->subscription_identifier = 0U;
        entry->released = false;
        entry->packet_identifier = 0U;
        entry->qos = 0U;
        entry->retain = false;
        entry->dup = false;
    }
    return index;
}

void mqtt_route_delivery_give(mqtt_server *server, uint32_t delivery)
{
    /** @brief The delivery. */
    const mqtt_delivery *entry = &server->storage.deliveries[delivery];

    mqtt_journal_finished(server, delivery);
    mqtt_snapshot_changed(server);
    mqtt_route_release(server, entry->message);
    (void)mqtt_slab_give(&server->delivery_slots, delivery);
}

bool mqtt_route_enqueue(mqtt_server *server, uint32_t session, uint32_t message,
                        uint8_t qos, bool retain, uint32_t identifier)
{
    /** @brief The Session. */
    mqtt_session *entry = &server->storage.sessions[session];
    /** @brief true once it is in the queue. */
    bool queued = false;

    if (entry->queued_count >= server->config.maximum_queued) {
        server->statistics.messages_dropped += 1U;
    } else {
        /** @brief The new entry in the queue. */
        const uint32_t index = mqtt_route_delivery_take(server);

        if (index == MQTT_SERVER_NONE) {
            server->statistics.messages_dropped += 1U;
        } else {
            /** @brief The new entry in the queue. */
            mqtt_delivery *delivery = &server->storage.deliveries[index];

            delivery->message = message;
            delivery->qos = qos;
            delivery->retain = retain;
            delivery->subscription_identifier = identifier;
            mqtt_route_hold(server, message);
            delivery->previous = entry->queued_tail;
            if (entry->queued_tail == MQTT_SERVER_NONE) {
                entry->queued_head = index;
            } else {
                server->storage.deliveries[entry->queued_tail].next = index;
            }
            entry->queued_tail = index;
            entry->queued_count += 1U;
            queued = true;
            mqtt_journal_delivery(server, session, index);
            if (entry->connection != MQTT_SERVER_NONE) {
                mqtt_route_mark_pending(server, entry->connection);
            }
        }
    }
    return queued;
}

/**
 * @brief Fill in what a packet is made from, out of a stored message.
 * @param[in] server The server.
 * @param[in] delivery The delivery, which has a message.
 * @param[out] parts The result.
 */
static void route_parts_from_delivery(const mqtt_server *server,
                                      const mqtt_delivery *delivery,
                                      route_parts *parts)
{
    /** @brief The message. */
    const mqtt_message *message = &server->storage.messages[delivery->message];
    /** @brief The bytes of the message. */
    const uint8_t *bytes =
        mqtt_page_arena_bytes(&server->arena, message->block);
    /** @brief Index of the Properties. */
    const uint32_t properties_at = (uint32_t)message->topic_length;

    parts->topic = bytes;
    parts->topic_length = message->topic_length;
    parts->properties = &bytes[properties_at];
    parts->properties_length = message->properties_length;
    parts->payload = &bytes[properties_at + message->properties_length];
    parts->payload_length = message->payload_length;
    parts->expiry_ms = message->expiry_ms;
    parts->identifier = delivery->subscription_identifier;
    parts->packet_identifier = delivery->packet_identifier;
    parts->qos = delivery->qos;
    parts->retain = delivery->retain;
    parts->dup = delivery->dup;
}

/**
 * @brief Try to turn a stored message into bytes for a connection.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] delivery The delivery.
 * @return What happened.
 */
static route_send_result route_send_delivery(mqtt_server *server,
                                             uint32_t connection,
                                             const mqtt_delivery *delivery)
{
    /** @brief The connection. */
    mqtt_connection *entry = &server->storage.connections[connection];
    /** @brief What the packet is made from. */
    route_parts parts;
    /** @brief The header bytes. */
    route_frame frame;
    /** @brief The outcome. */
    route_send_result result = ROUTE_SEND_DONE;

    route_parts_from_delivery(server, delivery, &parts);
    route_frame_build(server, entry->version, &parts, &frame);
    /* [MQTT-3.1.2-25], and what mqtt_route_too_large keeps from ever arriving
     * here. */
    if ((frame.total > entry->peer_maximum_packet_size) ||
        (frame.before_payload > server->message_room)) {
        result = ROUTE_SEND_TOO_LARGE;
    } else if ((parts.payload_length >= ROUTE_DIRECT_PAYLOAD) ||
               (frame.total > server->message_room)) {
        /* The Payload goes straight from the arena, and only what comes before
         * it goes in here. */
        if (route_ring_room(server, entry) < frame.before_payload) {
            result = ROUTE_SEND_WAIT;
        } else if (route_hold_put(server, connection, delivery, &parts,
                                  &frame, NULL, 0U) != MQTT_OK) {
            result = ROUTE_SEND_WAIT;
        } else {
            entry->direct_ring_bytes = entry->ring.used;
            entry->direct_data = parts.payload;
            entry->direct_left = parts.payload_length;
            entry->direct_message = delivery->message;
            mqtt_route_hold(server, delivery->message);
        }
    } else {
        /** @brief The Payload, which is in one place in the arena. */
        const mqtt_run payload = {parts.payload, parts.payload_length};

        if (route_ring_room(server, entry) < frame.total) {
            result = ROUTE_SEND_WAIT;
        } else if (route_hold_put(server, connection, delivery, &parts,
                                  &frame, &payload, 1U) != MQTT_OK) {
            result = ROUTE_SEND_WAIT;
        } else {
            /* The whole packet is in the ring. */
        }
    }
    if (result == ROUTE_SEND_WAIT) {
        route_no_room(server, connection);
    }
    if (result == ROUTE_SEND_DONE) {
        server->statistics.messages_sent += 1U;
    }
    return result;
}

/**
 * @brief Decide whether a Packet Identifier belongs to a message from before
 * the last wrap-around.
 *
 * Those are the only ones it can belong to. The list is in the order messages
 * were sent, and since the wrap-around the values have only gone up, so
 * everything after the boundary is below it.
 *
 * @param[in] server The server.
 * @param[in] session The Session.
 * @param[in] candidate The value.
 * @return true if it does.
 */
static bool route_identifier_in_flight(const mqtt_server *server,
                                       const mqtt_session *session,
                                       uint32_t candidate)
{
    /** @brief The entry to compare with. */
    uint32_t cursor = session->inflight_head;
    /** @brief The answer. */
    bool used = false;
    /** @brief true once the boundary has been compared with. */
    bool done = (session->identifier_boundary == MQTT_SERVER_NONE);

    while ((!used) && (!done)) {
        /** @brief The entry. */
        const mqtt_delivery *delivery = &server->storage.deliveries[cursor];

        used = ((uint32_t)delivery->packet_identifier == candidate);
        done = (cursor == session->identifier_boundary);
        cursor = delivery->next;
    }
    return used;
}

/**
 * @brief Pick the next unused Packet Identifier.
 * @param[in,out] server The server.
 * @param[in,out] session The Session.
 * @return The Packet Identifier, or zero if every value is in use.
 */
static uint16_t route_next_packet_identifier(const mqtt_server *server,
                                             mqtt_session *session)
{
    /** @brief The value tried. */
    uint32_t candidate = (uint32_t)session->last_packet_identifier;
    /** @brief Number of values tried. */
    uint32_t tries = 0U;
    /** @brief The answer, which stays zero if every value is in use. */
    uint32_t found = 0U;

    while ((found == 0U) && (tries < (uint32_t)UINT16_MAX)) {
        if (candidate >= (uint32_t)UINT16_MAX) {
            candidate = 1U;
            session->identifier_boundary = session->inflight_tail;
        } else {
            candidate += 1U;
        }
        /* [MQTT-2.2.1-4] */
        if (!route_identifier_in_flight(server, session, candidate)) {
            found = candidate;
        }
        tries += 1U;
    }
    if (found != 0U) {
        session->last_packet_identifier = (uint16_t)found;
    }
    return (uint16_t)found;
}

void mqtt_route_inflight_reset(mqtt_server *server, uint32_t session)
{
    /** @brief The entry being looked at. */
    uint32_t cursor = server->storage.sessions[session].inflight_head;

    while (cursor != MQTT_SERVER_NONE) {
        /** @brief The entry. */
        mqtt_delivery *delivery = &server->storage.deliveries[cursor];

        delivery->on_wire = false;
        cursor = delivery->next;
    }
}

bool mqtt_route_too_large(const mqtt_server *server,
                          const mqtt_route_source *source)
{
    /** @brief The room in a ring for what comes before a Payload. */
    const uint32_t room = server->message_room;
    /**
     * @brief The bytes of a PUBLISH packet other than its Topic Name,
     * Properties and Payload.
     */
    const uint32_t others = ROUTE_HEAD_BYTES + ROUTE_MIDDLE_BYTES;

    return ((uint64_t)source->topic_name.length +
            (uint64_t)source->properties.length + (uint64_t)others) >
           (uint64_t)room;
}

void mqtt_route_inflight_remove(mqtt_server *server, uint32_t session,
                                uint32_t delivery)
{
    /** @brief The Session. */
    mqtt_session *entry = &server->storage.sessions[session];
    /** @brief The delivery. */
    const mqtt_delivery *item = &server->storage.deliveries[delivery];

    /* Whatever came before it is then the last one from before the
     * wrap-around. */
    if (entry->identifier_boundary == delivery) {
        entry->identifier_boundary = item->previous;
    }
    if (item->previous == MQTT_SERVER_NONE) {
        entry->inflight_head = item->next;
    } else {
        server->storage.deliveries[item->previous].next = item->next;
    }
    if (item->next == MQTT_SERVER_NONE) {
        entry->inflight_tail = item->previous;
    } else {
        server->storage.deliveries[item->next].previous = item->previous;
    }
    mqtt_route_delivery_give(server, delivery);
}

/**
 * @brief Send the PUBREL packets a resumed Session still owes.
 *
 * They do not wait for the window, which only holds back PUBLISH packets:
 * [MQTT-3.3.4-10].
 *
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @return true if there is nothing more to do for this.
 */
static bool route_pump_release(mqtt_server *server, uint32_t connection)
{
    /** @brief The connection. */
    mqtt_connection *entry = &server->storage.connections[connection];
    /** @brief true while there is room to go on. */
    bool going = true;

    while (going && (entry->resend_release != MQTT_SERVER_NONE)) {
        /** @brief The delivery. */
        const mqtt_delivery *delivery =
            &server->storage.deliveries[entry->resend_release];

        if (delivery->released) {
            if (mqtt_server_room(server, entry, MQTT_SERVER_FOR_REPLY) < 8U) {
                going = false;
                route_no_room(server, connection);
            } else {
                /**
                 * @brief A PUBREL packet, which takes four bytes without
                 * Properties.
                 */
                mqtt_publish_response packet;
                /** @brief The bytes. */
                uint8_t bytes[8] = {0};
                /** @brief The writer. */
                mqtt_writer writer = {NULL, 0U, 0U, MQTT_OK};

                mqtt_publish_response_clear(&packet);
                packet.type = MQTT_PUBREL;
                packet.dup = true;
                packet.packet_identifier = delivery->packet_identifier;
                mqtt_writer_init(&writer, bytes, 8U);
                mqtt_publish_response_encode(&writer, entry->version, &packet);
                mqtt_journal_hold(server, connection);
                if (mqtt_ring_append(&entry->ring, bytes, writer.position) !=
                    MQTT_OK) {
                    going = false;
                }
            }
        }
        if (going) {
            entry->resend_release = delivery->next;
        }
    }
    return going;
}

/**
 * @brief Send the PUBLISH packets a resumed Session has no acknowledgement for.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @return true if there is nothing more to do for this.
 */
static bool route_pump_resend(mqtt_server *server, uint32_t connection)
{
    /** @brief The connection. */
    mqtt_connection *entry = &server->storage.connections[connection];
    /** @brief true while there is room to go on. */
    bool going = true;

    while (going && (entry->resend != MQTT_SERVER_NONE) &&
           (entry->direct_left == 0U)) {
        /** @brief The delivery. */
        mqtt_delivery *delivery = &server->storage.deliveries[entry->resend];

        if (delivery->released) {
            /* Its PUBREL has its own turn. */
            entry->resend = delivery->next;
        } else if (entry->on_wire >= entry->peer_window) {
            going = false;
        } else {
            /** @brief What happened. */
            route_send_result result = ROUTE_SEND_DONE;

            delivery->dup = true;
            result = route_send_delivery(server, connection, delivery);
            if (result == ROUTE_SEND_WAIT) {
                going = false;
            } else if (result == ROUTE_SEND_TOO_LARGE) {
                /** @brief The entry after it. */
                const uint32_t next = delivery->next;

                server->statistics.messages_dropped += 1U;
                mqtt_route_inflight_remove(server, entry->session,
                                           entry->resend);
                entry->resend = next;
            } else {
                entry->on_wire += 1U;
                delivery->on_wire = true;
                entry->resend = delivery->next;
            }
        }
    }
    return going && (entry->resend == MQTT_SERVER_NONE);
}

/**
 * @brief Take the delivery at the front of a Session's queue off the queue.
 * @param[in,out] server The server.
 * @param[in,out] session The Session.
 * @return The delivery.
 */
static uint32_t route_queue_pop(mqtt_server *server, mqtt_session *session)
{
    /** @brief The delivery. */
    const uint32_t index = session->queued_head;
    /** @brief The delivery. */
    const mqtt_delivery *entry = &server->storage.deliveries[index];

    session->queued_head = entry->next;
    if (entry->next == MQTT_SERVER_NONE) {
        session->queued_tail = MQTT_SERVER_NONE;
    } else {
        server->storage.deliveries[entry->next].previous = MQTT_SERVER_NONE;
    }
    session->queued_count -= 1U;
    /* A snapshot walking this Session's lists has to start it again. */
    mqtt_snapshot_changed(server);
    return index;
}

/**
 * @brief Put a delivery at the end of a Session's list of deliveries in
 * flight.
 * @param[in,out] server The server.
 * @param[in,out] session The Session.
 * @param[in,out] delivery The delivery, which is on no list.
 * @param[in] index The delivery's index.
 */
static void route_inflight_append(mqtt_server *server, mqtt_session *session,
                                  mqtt_delivery *delivery, uint32_t index)
{
    delivery->next = MQTT_SERVER_NONE;
    delivery->previous = session->inflight_tail;
    if (session->inflight_tail == MQTT_SERVER_NONE) {
        session->inflight_head = index;
    } else {
        /** @brief The delivery that was last. */
        const uint32_t last = session->inflight_tail;

        server->storage.deliveries[last].next = index;
    }
    session->inflight_tail = index;
}

/**
 * @brief Send the delivery at the front of a Session's queue, if there is room
 * for it.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in,out] session The Session.
 * @param[in,out] delivery The delivery at the front of the queue.
 * @return true if there is room to go on.
 */
static bool route_pump_send(mqtt_server *server, uint32_t connection,
                            mqtt_session *session, mqtt_delivery *delivery)
{
    /** @brief The connection. */
    mqtt_connection *entry = &server->storage.connections[connection];
    /** @brief true while there is room to go on. */
    bool going = true;
    /** @brief The value to restore if there is no room yet. */
    const uint16_t last_identifier = session->last_packet_identifier;
    /** @brief What happened. */
    route_send_result result = ROUTE_SEND_DONE;

    if (delivery->qos != MQTT_QOS_0) {
        delivery->packet_identifier =
            route_next_packet_identifier(server, session);
    }
    if ((delivery->qos != MQTT_QOS_0) && (delivery->packet_identifier == 0U)) {
        /* Every value is in use, so this waits until one is free. */
        result = ROUTE_SEND_WAIT;
    } else {
        result = route_send_delivery(server, connection, delivery);
    }

    if (result == ROUTE_SEND_WAIT) {
        session->last_packet_identifier = last_identifier;
        going = false;
    } else if (result == ROUTE_SEND_TOO_LARGE) {
        /* The rules say to act as if it had been sent, with no reply
         * expected. */
        server->statistics.messages_dropped += 1U;
        mqtt_route_delivery_give(server, route_queue_pop(server, session));
    } else {
        /** @brief The delivery, now off the queue. */
        const uint32_t index = route_queue_pop(server, session);

        if (delivery->qos == MQTT_QOS_0) {
            mqtt_route_delivery_give(server, index);
        } else {
            route_inflight_append(server, session, delivery, index);
            entry->on_wire += 1U;
            delivery->on_wire = true;
        }
    }
    return going;
}

void mqtt_route_pump(mqtt_server *server, uint32_t connection)
{
    /** @brief The connection. */
    const mqtt_connection *entry = &server->storage.connections[connection];
    /**
     * @brief true once what a resumed Session still owes has gone, so that new
     * messages can.
     */
    bool ready = (entry->state == MQTT_CONNECTION_ACTIVE) &&
                 (entry->session != MQTT_SERVER_NONE);

    if (ready) {
        ready = route_pump_release(server, connection);
    }
    if (ready) {
        ready = route_pump_resend(server, connection);
    }
    if (ready) {
        /** @brief The Session. */
        mqtt_session *session = &server->storage.sessions[entry->session];
        /** @brief true while there is room to go on. */
        bool going = true;

        while (going && (session->queued_head != MQTT_SERVER_NONE) &&
               (entry->direct_left == 0U)) {
            /** @brief The delivery. */
            mqtt_delivery *delivery =
                &server->storage.deliveries[session->queued_head];

            if (mqtt_route_expired(server, delivery->message)) {
                mqtt_route_delivery_give(server,
                                         route_queue_pop(server, session));
            } else if ((delivery->qos != MQTT_QOS_0) &&
                       (entry->on_wire >= entry->peer_window)) {
                going = false;
            } else {
                going = route_pump_send(server, connection, session, delivery);
            }
        }
    }
}

/**
 * @brief Try to put a message straight in a connection's ring, for QoS 0.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] source The message.
 * @param[in] retain The RETAIN flag to send it with.
 * @param[in] identifier The Subscription Identifier, or zero.
 * @return true if it went in.
 */
static bool route_try_direct(mqtt_server *server, uint32_t connection,
                             const mqtt_route_source *source, bool retain,
                             uint32_t identifier)
{
    /** @brief The connection. */
    mqtt_connection *entry = &server->storage.connections[connection];
    /** @brief What the packet is made from. */
    route_parts parts;
    /** @brief The header bytes. */
    route_frame frame;
    /** @brief The answer. */
    bool sent = false;

    parts.topic = source->topic_name.data;
    parts.topic_length = source->topic_name.length;
    parts.properties = source->properties.data;
    parts.properties_length = source->properties.length;
    parts.payload = NULL;
    parts.payload_length = source->payload.length;
    parts.expiry_ms = source->expiry_ms;
    parts.identifier = identifier;
    parts.packet_identifier = 0U;
    parts.qos = MQTT_QOS_0;
    parts.retain = retain;
    parts.dup = false;
    route_frame_build(server, entry->version, &parts, &frame);
    if (frame.total > entry->peer_maximum_packet_size) {
        /* It can never be sent, so it counts as done. */
        sent = true;
    } else if ((source->payload.length < ROUTE_DIRECT_PAYLOAD) &&
               (route_ring_room(server, entry) >= frame.total)) {
        if (route_put(&entry->ring, entry->version, &parts, &frame,
                      source->payload.runs,
                      source->payload.count) == MQTT_OK) {
            server->statistics.messages_sent += 1U;
            mqtt_route_mark_pending(server, connection);
            sent = true;
        }
    } else {
        /* There is no room yet, so it needs a stored copy. */
    }
    return sent;
}

/**
 * @brief Send a message to the Session of one subscription.
 * @param[in,out] server The server.
 * @param[in,out] source The message.
 * @param[in] subscription The subscription.
 */
static void route_to_subscription(mqtt_server *server,
                                  mqtt_route_source *source,
                                  const mqtt_subscription *subscription)
{
    /** @brief The Session. */
    const uint32_t session_index = subscription->session;
    /** @brief The Session. */
    const mqtt_session *session = &server->storage.sessions[session_index];
    /**
     * @brief The options. A Session resumed with an earlier version only keeps
     * the QoS.
     */
    const uint32_t options =
        (session->version == MQTT_VERSION_5) ?
            (uint32_t)subscription->options :
            ((uint32_t)subscription->options & MQTT5_MAXIMUM_QOS_MASK);
    /** @brief The QoS the subscription allows. */
    const uint8_t allowed = (uint8_t)(options & MQTT5_MAXIMUM_QOS_MASK);
    /** @brief The QoS to send it with: the lower of the two. */
    const uint8_t qos = (source->qos < allowed) ? source->qos : allowed;
    /** @brief The RETAIN flag to send it with. */
    const bool retain =
        ((options & MQTT5_RETAIN_AS_PUBLISHED) != 0U) && source->retain;
    /**
     * @brief true if the Client is connected, and has received its CONNACK
     * packet.
     */
    const bool connected = (session->connection != MQTT_SERVER_NONE);
    /**
     * @brief true if the message can be dropped rather than held for a Client
     * that is away.
     */
    const bool skip = (!connected) && (qos == MQTT_QOS_0);
    /**
     * @brief true if this subscription excludes messages from its own Session.
     */
    const bool own = ((options & MQTT5_NO_LOCAL) != 0U) &&
                     (session_index == source->publisher);

    if ((!skip) && (!own)) {
        /** @brief true if nothing more needs to be done. */
        bool sent = false;

        if ((qos == MQTT_QOS_0) && (session->queued_head == MQTT_SERVER_NONE) &&
            (!source->properties_raw)) {
            /** @brief The connection. */
            const mqtt_connection *connection =
                &server->storage.connections[session->connection];

            if ((connection->state == MQTT_CONNECTION_ACTIVE) &&
                (connection->direct_left == 0U)) {
                sent = route_try_direct(server, session->connection, source,
                                        retain, subscription->identifier);
            }
        }
        if (!sent) {
            if (source->message == MQTT_SERVER_NONE) {
                source->message = mqtt_route_store(server, source);
            }
            if (source->message == MQTT_SERVER_NONE) {
                server->statistics.messages_dropped += 1U;
                source->lost = true;
            } else if ((!mqtt_route_enqueue(server, session_index,
                                            source->message, qos, retain,
                                            subscription->identifier)) &&
                       (qos != MQTT_QOS_0)) {
                /* A QoS 0 message may be lost. One of a higher QoS has to be
                 * reported. */
                source->lost = true;
            } else {
                /* It is on its way. */
            }
        }
    }
}

/**
 * @brief Pick a Session of a shared group to send the next message to.
 * @param[in,out] server The server.
 * @param[in,out] group The group.
 * @return The subscription: the next member that is connected, or else the next
 * member.
 */
static uint32_t route_group_pick(const mqtt_server *server,
                                 mqtt_share_group *group)
{
    /** @brief The member the search started with. */
    const uint32_t start =
        (group->cursor == MQTT_SERVER_NONE) ? group->members : group->cursor;
    /** @brief The member being looked at. */
    uint32_t member = start;
    /** @brief The member chosen. */
    uint32_t chosen = MQTT_SERVER_NONE;
    /** @brief true once the whole group has been looked at. */
    bool done = (start == MQTT_SERVER_NONE);

    while (!done) {
        /** @brief The member being looked at. */
        const mqtt_subscription *entry = &server->storage.subscriptions[member];

        if (server->storage.sessions[entry->session].connection !=
            MQTT_SERVER_NONE) {
            chosen = member;
            done = true;
        } else {
            member = (entry->next_here == MQTT_SERVER_NONE) ? group->members :
                                                              entry->next_here;
            done = (member == start);
        }
    }
    if ((chosen == MQTT_SERVER_NONE) && (start != MQTT_SERVER_NONE)) {
        chosen = start;
    }
    if (chosen != MQTT_SERVER_NONE) {
        /** @brief The member after the one chosen. */
        const uint32_t after = server->storage.subscriptions[chosen].next_here;

        group->cursor = (after == MQTT_SERVER_NONE) ? group->members : after;
    }
    return chosen;
}

void mqtt_route_drop_queued_qos_0(mqtt_server *server, uint32_t session)
{
    /** @brief The Session. */
    mqtt_session *entry = &server->storage.sessions[session];
    /** @brief The delivery being looked at. */
    uint32_t cursor = entry->queued_head;

    while (cursor != MQTT_SERVER_NONE) {
        /** @brief The delivery. */
        const mqtt_delivery *delivery = &server->storage.deliveries[cursor];
        /** @brief The one after it, as this one may go. */
        const uint32_t next = delivery->next;

        if (delivery->qos == MQTT_QOS_0) {
            if (delivery->previous == MQTT_SERVER_NONE) {
                entry->queued_head = next;
            } else {
                server->storage.deliveries[delivery->previous].next = next;
            }
            if (next == MQTT_SERVER_NONE) {
                entry->queued_tail = delivery->previous;
            } else {
                server->storage.deliveries[next].previous = delivery->previous;
            }
            entry->queued_count -= 1U;
            mqtt_route_delivery_give(server, cursor);
        }
        cursor = next;
    }
}

/**
 * @brief Send a message to the MQTT 3.1 subscriptions whose wildcard at the
 * root reaches its topic, which starts with a dollar. MQTT 3.1 gives a dollar
 * no meaning, unlike the later versions.
 * @param[in,out] server The server.
 * @param[in,out] source The message.
 * @return Number of subscriptions it matched.
 */
static uint32_t route_publish_dollar(mqtt_server *server,
                                     mqtt_route_source *source)
{
    /** @brief The state of the match. */
    mqtt_tree_match match;
    /** @brief The node found. */
    uint32_t node = MQTT_TREE_NONE;
    /** @brief Number of subscriptions it matched. */
    uint32_t matched = 0U;

    mqtt_tree_match_begin_root_wildcards(&match, &source->topic_name);
    node = mqtt_tree_match_next(&server->tree, &match);
    while (node != MQTT_TREE_NONE) {
        /** @brief The subscription being looked at. */
        uint32_t subscription = server->tree.nodes[node].subscriptions;

        while (subscription != MQTT_SERVER_NONE) {
            /** @brief The subscription. */
            const mqtt_subscription *item =
                &server->storage.subscriptions[subscription];

            if (server->storage.sessions[item->session].version ==
                MQTT_VERSION_31) {
                route_to_subscription(server, source, item);
                matched += 1U;
            }
            subscription = item->next_here;
        }
        node = mqtt_tree_match_next(&server->tree, &match);
    }
    return matched;
}

uint32_t mqtt_route_publish(mqtt_server *server, mqtt_route_source *source)
{
    /** @brief The state of the match. */
    mqtt_tree_match match;
    /** @brief The node found. */
    uint32_t node = MQTT_TREE_NONE;
    /** @brief Number of subscriptions it matched. */
    uint32_t matched = 0U;

    mqtt_tree_match_begin(&match, &source->topic_name);
    node = mqtt_tree_match_next(&server->tree, &match);
    while (node != MQTT_TREE_NONE) {
        /** @brief The node. */
        const mqtt_tree_node *entry = &server->tree.nodes[node];
        /** @brief The subscription being looked at. */
        uint32_t subscription = entry->subscriptions;
        /** @brief The group being looked at. */
        uint32_t group = entry->groups;

        while (subscription != MQTT_SERVER_NONE) {
            /** @brief The subscription. */
            const mqtt_subscription *item =
                &server->storage.subscriptions[subscription];

            route_to_subscription(server, source, item);
            matched += 1U;
            subscription = item->next_here;
        }
        while (group != MQTT_SERVER_NONE) {
            /** @brief The group. */
            mqtt_share_group *item = &server->storage.groups[group];
            /** @brief The member chosen. */
            const uint32_t member = route_group_pick(server, item);

            if (member != MQTT_SERVER_NONE) {
                route_to_subscription(server, source,
                                      &server->storage.subscriptions[member]);
                matched += 1U;
            }
            group = item->next;
        }
        node = mqtt_tree_match_next(&server->tree, &match);
    }
    if ((source->topic_name.length > 0U) &&
        ((uint32_t)source->topic_name.data[0] == MQTT5_TOPIC_DOLLAR)) {
        matched += route_publish_dollar(server, source);
    }
    return matched;
}
