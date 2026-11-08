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
 * @file base_server_receive.c
 * @brief Taking in the bytes each connection sends, and cutting them into
 * packets.
 *
 * The network layer asks where bytes can go, reads straight into the blocks of
 * the connection's receive ring, and says how many came. Each whole packet is
 * then handled where it is: in place when its body is in one block, and
 * otherwise gathered into the server's scratch, except for a PUBLISH Payload,
 * which goes on as the runs it arrived in. A packet larger than
 * receive_inline goes to an arena block instead, and the bytes after it are
 * read straight into that block.
 *
 * A reply never takes more bytes than the packet it answers, so a read takes
 * no more than the room left for replies: about half of what the quota still
 * allows, the other half going to the bytes read. The bytes of a packet that
 * has only partly arrived count against that room, as its reply is still to
 * come. With no room at all, a connection holding part of a packet whose size
 * is known moves it to the arena, so that its blocks go back to the pool for
 * others.
 *
 * [V31] MQTT V3.1 Protocol Specification.
 * @li Cited: 2.
 * @li Source: public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015.
 * @li Cited: 2.2, 3.3.2.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 2.1, 3.3.2, p.17-20, p.57-63.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */

#include <stdbool.h>
#include <stddef.h>

#include "base_fixed.h"
#include "base_flags.h"
#include "base_server_internal.h"
#include "base_string.h"
#include "base_type.h"
#include "base_wire.h"
#include "v5_type.h"

/** @brief Number of places a copy-in through mqtt_server_receive asks for. */
#define RECEIVE_SPACES 8U

/**
 * @brief Find a connection that is in use.
 * @param[in] server The server.
 * @param[in] connection The index.
 * @return The connection, or NULL if the index is not in the table or the
 * entry is free.
 */
static const mqtt_connection *receive_connection(const mqtt_server *server,
                                                 uint32_t connection)
{
    /** @brief The connection. */
    const mqtt_connection *entry = NULL;

    if (mqtt_server_connection_ready(server, connection)) {
        entry = &server->storage.connections[connection];
    }
    return entry;
}

/**
 * @brief Check a Fixed Header before any memory is used for its packet, and
 * note that a packet has started.
 * @param[in] server The server.
 * @param[in,out] entry The connection.
 * @param[in] header The Fixed Header.
 * @return MQTT_OK, or the reason to close the connection.
 */
static mqtt_status receive_check_header(const mqtt_server *server,
                                        mqtt_connection *entry,
                                        const mqtt_fixed_header *header)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    /* The header is judged again when the rest of it comes, which changes
     * nothing here. */
    if ((uint32_t)header->type != MQTT_PINGREQ) {
        entry->activity = MQTT_SERVER_ACTIVITY_OTHER;
    } else if (entry->activity == MQTT_SERVER_ACTIVITY_NONE) {
        entry->activity = MQTT_SERVER_ACTIVITY_PINGREQ;
    } else {
        /* It already holds as much as a PINGREQ says. */
    }
    if (entry->state == MQTT_CONNECTION_WAIT_CONNECT) {
        if ((uint32_t)header->type != MQTT_CONNECT) {
            status = MQTT_PROTOCOL_ERROR;
        }
    } else {
        status = mqtt_fixed_header_check(header, entry->version);
    }
    /* Memory is only held for a large packet from a Client that has been
     * accepted. */
    if ((status == MQTT_OK) && (entry->state != MQTT_CONNECTION_ACTIVE) &&
        (((uint64_t)header->size + (uint64_t)header->remaining_length) >
         (uint64_t)server->config.maximum_connect_size)) {
        status = MQTT_PACKET_TOO_LARGE;
    }
    if ((status == MQTT_OK) &&
        ((uint64_t)header->size + (uint64_t)header->remaining_length >
         (uint64_t)server->config.maximum_packet_size)) {
        status = MQTT_PACKET_TOO_LARGE;
    }
    return status;
}

/**
 * @brief Decide whether a packet can hold a secret, the Password of a CONNECT
 * or the Authentication Data of a CONNECT or an AUTH, so that its bytes are
 * wiped once it has been handled.
 * @param[in] header The packet's Fixed Header.
 * @return true if it can.
 */
static bool receive_secret(const mqtt_fixed_header *header)
{
    return ((uint32_t)header->type == MQTT_CONNECT) ||
           ((uint32_t)header->type == MQTT5_AUTH);
}

/**
 * @brief Handle a packet that has arrived whole in the arena.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @return MQTT_OK, or the reason the connection is closing.
 */
static mqtt_status receive_finish_assembly(mqtt_server *server,
                                           uint32_t connection)
{
    /** @brief The connection. */
    mqtt_connection *entry = &server->storage.connections[connection];
    /** @brief The bytes of the packet. */
    const uint8_t *bytes =
        mqtt_page_arena_bytes(&server->arena, entry->assembly_block);
    /** @brief The Fixed Header. */
    mqtt_fixed_header header = {0U, 0U, 0U, 0U, true};
    /** @brief The outcome. */
    mqtt_status status =
        mqtt_fixed_header_decode(bytes, entry->assembly_used, &header);

    if (status == MQTT_OK) {
        status = mqtt_server_handle_packet(server, connection, &header,
                                           &bytes[header.size]);
    }
    if (receive_secret(&header)) {
        mqtt_page_arena_wipe(&server->arena, entry->assembly_block);
    }
    mqtt_page_arena_give(&server->arena, entry->assembly_block);
    entry->assembly_block = MQTT_PAGE_ARENA_NONE;
    entry->assembly_used = 0U;
    entry->assembly_size = 0U;
    return status;
}

/**
 * @brief Start holding a packet in an arena block, and move the bytes of it
 * already in the receive ring there.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] size Number of bytes in the whole packet.
 * @return MQTT_OK, or the reason the connection is closing.
 */
static mqtt_status receive_start_assembly(mqtt_server *server,
                                          uint32_t connection, uint32_t size)
{
    /** @brief The connection. */
    mqtt_connection *entry = &server->storage.connections[connection];
    /** @brief The block. */
    const uint32_t block = mqtt_page_arena_take(&server->arena, size);
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (block == MQTT_PAGE_ARENA_NONE) {
        status = MQTT_QUOTA_EXCEEDED;
        mqtt_server_fail(server, connection, status);
    } else {
        /** @brief Number of bytes of the packet in the ring. */
        const uint32_t moved =
            mqtt_ring_peek(&entry->receive, 0U,
                           mqtt_page_arena_bytes(&server->arena, block), size);

        entry->assembly_block = block;
        entry->assembly_size = size;
        entry->assembly_used = moved;
        mqtt_server_blocks_returned(server,
                                    mqtt_ring_consume(&entry->receive, moved));
        if (moved == size) {
            status = receive_finish_assembly(server, connection);
        }
    }
    return status;
}

/**
 * @brief Work out how many bytes of a PUBLISH body come before its Payload,
 * from the receive ring.
 * @param[in] entry The connection, with the whole packet in its ring.
 * @param[in] header The Fixed Header.
 * @param[out] head_length The number of bytes.
 * @return true if they could be worked out and fit in the body. If not, the
 * decoder finds what is wrong.
 */
static bool receive_publish_head(const mqtt_connection *entry,
                                 const mqtt_fixed_header *header,
                                 uint32_t *head_length)
{
    /** @brief Bytes copied out of the ring. */
    uint8_t bytes[4] = {0U, 0U, 0U, 0U};
    /** @brief Number of bytes up to the Payload, as far as known. */
    uint32_t length = 0U;
    /** @brief The answer. */
    bool found = false;

    /* The Topic Name, then a Packet Identifier above QoS 0: section 3.3.2. */
    if (mqtt_ring_peek(&entry->receive, header->size, bytes, 2U) == 2U) {
        length = 2U + (((uint32_t)bytes[0] << 8U) | (uint32_t)bytes[1]);
        if (((uint32_t)header->flags & MQTT_QOS_MASK) != 0U) {
            length += 2U;
        }
        found = true;
    }
    /* MQTT 5.0 adds the Properties, after their Variable Byte Integer
     * length. */
    if (found && (entry->version == MQTT_VERSION_5)) {
        /** @brief Number of bytes copied. */
        const uint32_t copied =
            mqtt_ring_peek(&entry->receive, header->size + length, bytes, 4U);
        /** @brief Length of the Properties. */
        uint32_t properties = 0U;
        /** @brief Number of bytes of that length. */
        uint32_t used = 0U;

        found = (mqtt_variable_byte_integer_decode(bytes, copied, &properties,
                                                   &used) == MQTT_OK);
        length += used + properties;
    }
    *head_length = length;
    return found && (length <= header->remaining_length);
}

/**
 * @brief Handle a whole PUBLISH packet from the receive ring whose body is not
 * in one block, passing its Payload on as runs.
 * @param[in,out] server The server.
 * @param[in] connection The connection, which is active.
 * @param[in] header The Fixed Header.
 * @param[out] status The outcome.
 * @return true if it was handled, false if its head does not make sense, for
 * the caller to have it decoded whole.
 */
static bool receive_publish_runs(mqtt_server *server, uint32_t connection,
                                 const mqtt_fixed_header *header,
                                 mqtt_status *status)
{
    /** @brief The connection. */
    mqtt_connection *entry = &server->storage.connections[connection];
    /** @brief Number of bytes up to the Payload. */
    uint32_t head_length = 0U;
    /** @brief The answer. */
    const bool handled = receive_publish_head(entry, header, &head_length);

    if (handled) {
        /** @brief The head, if it is in one block. */
        mqtt_run head = {NULL, 0U};
        /** @brief The Payload. */
        mqtt_run_list payload = {server->receive_runs, 0U, 0U};

        if ((mqtt_ring_view_range(&entry->receive, header->size, head_length,
                                  &head, 1U) != 1U) ||
            (head.length != head_length)) {
            (void)mqtt_ring_peek(&entry->receive, header->size,
                                 server->storage.receive_scratch,
                                 head_length);
            head.data = server->storage.receive_scratch;
        }
        payload.length = header->remaining_length - head_length;
        payload.count = mqtt_ring_view_range(
            &entry->receive, header->size + head_length, payload.length,
            server->receive_runs, MQTT_SERVER_RECEIVE_RUNS);
        entry->last_received_ms = server->now_ms;
        *status = mqtt_publish_handle_runs(server, connection, header,
                                           head.data, head_length, &payload);
        if (*status != MQTT_OK) {
            mqtt_server_fail(server, connection, *status);
        }
    }
    return handled;
}

/**
 * @brief Handle the whole packet at the front of the receive ring, and remove
 * it.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] header The Fixed Header.
 * @param[in] first The ring's first run.
 * @return MQTT_OK, or the reason the connection is closing.
 */
static mqtt_status receive_handle(mqtt_server *server, uint32_t connection,
                                  const mqtt_fixed_header *header,
                                  const mqtt_run *first)
{
    /** @brief The connection. */
    mqtt_connection *entry = &server->storage.connections[connection];
    /** @brief The body, if it is in one block. */
    mqtt_run body = {NULL, 0U};
    /** @brief Number of runs found for the body, up to one. */
    uint32_t runs = 0U;
    /** @brief true once the packet has been handled. */
    bool handled = false;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    /* Most packets lie in the first run, so look no further for them. */
    if ((header->size + header->remaining_length) <= first->length) {
        body.data = &first->data[header->size];
        body.length = header->remaining_length;
        runs = 1U;
    } else {
        runs = mqtt_ring_view_range(&entry->receive, header->size,
                                    header->remaining_length, &body, 1U);
    }
    if ((runs == 1U) && (body.length == header->remaining_length)) {
        status = mqtt_server_handle_packet(server, connection, header,
                                           body.data);
        handled = true;
    } else if (((uint32_t)header->type == MQTT_PUBLISH) &&
               (header->remaining_length > 0U) &&
               (entry->state == MQTT_CONNECTION_ACTIVE)) {
        handled = receive_publish_runs(server, connection, header, &status);
    } else {
        /* It is gathered below. */
    }
    if (!handled) {
        (void)mqtt_ring_peek(&entry->receive, header->size,
                             server->storage.receive_scratch,
                             header->remaining_length);
        status = mqtt_server_handle_packet(server, connection, header,
                                           server->storage.receive_scratch);
        if (receive_secret(header)) {
            mqtt_bytes_wipe(server->storage.receive_scratch,
                            header->remaining_length);
        }
    }
    if (receive_secret(header)) {
        mqtt_ring_wipe_front(&entry->receive,
                             header->size + header->remaining_length);
    }
    mqtt_server_blocks_returned(
        server, mqtt_ring_consume(&entry->receive,
                                  header->size + header->remaining_length));
    return status;
}

/**
 * @brief Find what the bytes at the front of the receive ring hold.
 * @param[in] server The server.
 * @param[in,out] entry The connection.
 * @param[out] header The Fixed Header, as far as it has arrived.
 * @param[out] first The ring's first run.
 * @return MQTT_OK for a whole Fixed Header, MQTT_INCOMPLETE for part of one
 * with nothing wrong so far, or the reason to close the connection.
 */
static mqtt_status receive_front(const mqtt_server *server,
                                 mqtt_connection *entry,
                                 mqtt_fixed_header *header, mqtt_run *first)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    first->data = NULL;
    first->length = 0U;
    (void)mqtt_ring_view(&entry->receive, first, 1U);
    /* A Fixed Header that lies in the first run is read from where it is. */
    if ((first->length >= MQTT_FIXED_HEADER_BYTES) ||
        (first->length == entry->receive.used)) {
        status = mqtt_fixed_header_decode(
            first->data,
            (first->length < MQTT_FIXED_HEADER_BYTES) ?
                first->length :
                MQTT_FIXED_HEADER_BYTES,
            header);
    } else {
        /** @brief The first bytes, gathered from the blocks they span. */
        uint8_t bytes[MQTT_FIXED_HEADER_BYTES] = {0U, 0U, 0U, 0U, 0U};
        /** @brief Number of them. */
        const uint32_t count =
            mqtt_ring_peek(&entry->receive, 0U, bytes, MQTT_FIXED_HEADER_BYTES);

        status = mqtt_fixed_header_decode(bytes, count, header);
    }

    /* The first byte alone can show this is no packet, so don't wait for the
     * rest of it. */
    if ((status == MQTT_OK) || (status == MQTT_INCOMPLETE)) {
        /** @brief What is wrong with what has arrived so far. */
        const mqtt_status check = receive_check_header(server, entry, header);

        if (check != MQTT_OK) {
            status = check;
        }
    }
    return status;
}

/**
 * @brief Handle every whole packet in the receive ring.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @return MQTT_OK, or the reason the connection is closing.
 */
static mqtt_status receive_take(mqtt_server *server, uint32_t connection)
{
    /** @brief The connection. */
    mqtt_connection *entry = &server->storage.connections[connection];
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief true while a whole packet may be waiting. */
    bool going = true;

    while (going && (status == MQTT_OK) &&
           (entry->state != MQTT_CONNECTION_CLOSING) &&
           (entry->assembly_block == MQTT_PAGE_ARENA_NONE) &&
           (entry->receive.used > 0U)) {
        /** @brief The Fixed Header. */
        mqtt_fixed_header header = {0U, 0U, 0U, 0U, true};
        /** @brief The ring's first run. */
        mqtt_run first = {NULL, 0U};
        /** @brief What the front holds. */
        const mqtt_status front = receive_front(server, entry, &header, &first);

        if (front == MQTT_INCOMPLETE) {
            going = false;
        } else if (front != MQTT_OK) {
            status = front;
            mqtt_server_fail(server, connection, status);
        } else if ((header.size + header.remaining_length) >
                   server->receive_inline) {
            status = receive_start_assembly(
                server, connection, header.size + header.remaining_length);
        } else if (entry->receive.used <
                   (header.size + header.remaining_length)) {
            going = false;
        } else {
            status = receive_handle(server, connection, &header, &first);
        }
    }
    return status;
}

/**
 * @brief Count the bytes a connection may be given now.
 *
 * Bytes received and the replies to them come from the same blocks, so each
 * gets about half of what may still be taken. The bytes of a packet that has
 * only partly arrived count against the room for replies too, as its reply is
 * still to come.
 *
 * @param[in] server The server.
 * @param[in] entry The connection.
 * @return The number of bytes.
 */
static uint32_t receive_room(const mqtt_server *server,
                             const mqtt_connection *entry)
{
    /** @brief Blocks replies may take: what is left of the hard quota. */
    const uint32_t reply =
        mqtt_server_grant(server, entry, MQTT_SERVER_FOR_REPLY);
    /** @brief The answer. */
    uint32_t room = 0U;

    if (entry->assembly_block != MQTT_PAGE_ARENA_NONE) {
        /** @brief Number of bytes the packet still needs. */
        const uint32_t left = entry->assembly_size - entry->assembly_used;
        /** @brief Room for replies. */
        const uint32_t replies = mqtt_ring_room(&entry->ring, reply);

        room = (left < replies) ? left : replies;
    } else {
        /** @brief Blocks the bytes received may take. */
        const uint32_t grant =
            mqtt_server_grant(server, entry, MQTT_SERVER_FOR_RECEIVE);
        /** @brief Their share. */
        const uint32_t half = (reply + 1U) / 2U;
        /** @brief Blocks taken for bytes received. */
        const uint32_t taken = (grant < half) ? grant : half;
        /** @brief Room for bytes received. */
        const uint32_t bytes = mqtt_ring_room(&entry->receive, taken);
        /** @brief Room for replies, with the blocks left. */
        const uint32_t replies = mqtt_ring_room(&entry->ring, reply - taken);
        /**
         * @brief What is left of it once the part of a packet already
         * received has its share.
         */
        const uint32_t left = (replies > entry->receive.used) ?
                                  (replies - entry->receive.used) :
                                  0U;

        room = (bytes < left) ? bytes : left;
    }
    return room;
}

uint32_t mqtt_server_receive_capacity(const mqtt_server *server,
                                      uint32_t connection)
{
    /**
     * @brief The answer: nothing for a connection that is not open, or is
     * closing.
     */
    uint32_t capacity = 0U;
    /** @brief The connection. */
    const mqtt_connection *entry = receive_connection(server, connection);

    if ((entry != NULL) && (entry->state != MQTT_CONNECTION_CLOSING)) {
        capacity = receive_room(server, entry);
    }
    return capacity;
}

/**
 * @brief Act on a connection that has no room to receive: move part of a
 * packet to the arena, or wait for blocks.
 * @param[in,out] server The server.
 * @param[in] connection The connection, which is in use and not closing.
 */
static void receive_throttle(mqtt_server *server, uint32_t connection)
{
    /** @brief The connection. */
    mqtt_connection *entry = &server->storage.connections[connection];
    /** @brief The Fixed Header at the front, if it has come. */
    mqtt_fixed_header header = {0U, 0U, 0U, 0U, true};
    /** @brief The ring's first run. */
    mqtt_run first = {NULL, 0U};
    /** @brief true if part of a packet whose size is known is waiting. */
    bool known = false;

    if (!entry->throttled) {
        entry->throttled = true;
        server->statistics.receive_throttled += 1U;
    }
    if ((entry->assembly_block == MQTT_PAGE_ARENA_NONE) &&
        (entry->receive.used > 0U)) {
        known = (receive_front(server, entry, &header, &first) == MQTT_OK);
    }
    /* Waiting for the rest of this packet would hold its blocks, so it waits
     * in the arena instead. */
    if (known) {
        if (receive_start_assembly(server, connection,
                                   header.size + header.remaining_length) !=
            MQTT_OK) {
            /* The arena is full too, and the connection is closing, which
             * waits for nothing. */
            entry->throttled = false;
        }
    } else if ((entry->ring.used == 0U) && (entry->direct_left == 0U)) {
        mqtt_server_wait_blocks(server, connection);
    } else {
        /* Sending what is waiting gives blocks back, and brings it back. */
    }
}

bool mqtt_server_receive_wanted(mqtt_server *server, uint32_t connection)
{
    /** @brief The room, which is zero for a connection not in use. */
    uint32_t room = mqtt_server_receive_capacity(server, connection);
    /** @brief The connection, if it is in use. */
    const mqtt_connection *entry = receive_connection(server, connection);

    if ((room == 0U) && (entry != NULL)) {
        if (entry->state != MQTT_CONNECTION_CLOSING) {
            receive_throttle(server, connection);
            room = mqtt_server_receive_capacity(server, connection);
        }
    }
    if (room > 0U) {
        server->storage.connections[connection].throttled = false;
    }
    return room > 0U;
}

/**
 * @brief Cut a list of places so that it holds no more than a number of bytes.
 * @param[in,out] spaces The places.
 * @param[in] count Number of places.
 * @param[in] room The most bytes.
 * @return Number of places left.
 */
static uint32_t receive_clip(mqtt_space *spaces, uint32_t count, uint32_t room)
{
    /** @brief Index of the place. */
    uint32_t index = 0U;
    /** @brief Number of bytes in the places so far. */
    uint32_t total = 0U;

    while ((index < count) && (total < room)) {
        if (spaces[index].length > (room - total)) {
            spaces[index].length = room - total;
        }
        total += spaces[index].length;
        index += 1U;
    }
    return index;
}

uint32_t mqtt_server_receive_space(mqtt_server *server, uint32_t connection,
                                   mqtt_space *spaces, uint32_t max_spaces)
{
    /** @brief Number of places. */
    uint32_t count = 0U;
    /** @brief true if there is room. */
    bool wanted = false;

    if ((spaces != NULL) || (max_spaces == 0U)) {
        wanted = mqtt_server_receive_wanted(server, connection);
    }
    if ((max_spaces > 0U) && wanted) {
        /** @brief The connection. */
        mqtt_connection *entry = &server->storage.connections[connection];
        /** @brief The room. */
        const uint32_t room = receive_room(server, entry);

        if (entry->assembly_block != MQTT_PAGE_ARENA_NONE) {
            spaces[0].data = &mqtt_page_arena_bytes(
                &server->arena, entry->assembly_block)[entry->assembly_used];
            spaces[0].length = room;
            count = 1U;
        } else {
            (void)mqtt_ring_reserve(&entry->receive,
                                    mqtt_ring_blocks_for(&entry->receive,
                                                         room));
            count = receive_clip(
                spaces, mqtt_ring_space(&entry->receive, spaces, max_spaces),
                room);
        }
    }
    return count;
}

/**
 * @brief Do what follows bytes received: send what is due, or report a close.
 * @param[in,out] server The server.
 * @param[in] connection The connection.
 * @param[in] status The outcome so far.
 */
static void receive_after(mqtt_server *server, uint32_t connection,
                          mqtt_status status)
{
    /** @brief The connection. */
    const mqtt_connection *entry = &server->storage.connections[connection];

    if (entry->state == MQTT_CONNECTION_ACTIVE) {
        mqtt_route_pump(server, connection);
    }
    if ((status == MQTT_OK) && (entry->state == MQTT_CONNECTION_CLOSING)) {
        mqtt_route_mark_pending(server, connection);
    }
}

mqtt_status mqtt_server_receive_commit(mqtt_server *server, uint32_t connection,
                                       uint32_t count, uint64_t now_ms)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_INVALID_ARGUMENT;

    if (mqtt_server_ready(server)) {
        server->now_ms = now_ms;
    }
    if (receive_connection(server, connection) != NULL) {
        /** @brief The connection. */
        mqtt_connection *entry = &server->storage.connections[connection];

        status = MQTT_OK;
        if (entry->assembly_block != MQTT_PAGE_ARENA_NONE) {
            /** @brief Number of bytes the packet still needs. */
            const uint32_t left = entry->assembly_size - entry->assembly_used;

            entry->assembly_used += (count < left) ? count : left;
            if (entry->assembly_used == entry->assembly_size) {
                status = receive_finish_assembly(server, connection);
            }
        } else {
            mqtt_server_blocks_returned(
                server, mqtt_ring_commit(&entry->receive, count));
        }
        if (status == MQTT_OK) {
            status = receive_take(server, connection);
        }
        receive_after(server, connection, status);
    }
    return status;
}

/**
 * @brief Copy bytes into places, in order.
 * @param[in] spaces The places.
 * @param[in] count Number of places.
 * @param[in] data The bytes.
 * @param[in] length Number of bytes.
 * @return Number of bytes copied.
 */
static uint32_t receive_copy(const mqtt_space *spaces, uint32_t count,
                             const uint8_t *data, uint32_t length)
{
    /** @brief Index of the place. */
    uint32_t place = 0U;
    /** @brief Number of bytes copied. */
    uint32_t copied = 0U;

    while ((place < count) && (copied < length)) {
        /** @brief Number of bytes the place takes. */
        const uint32_t room = (spaces[place].length < (length - copied)) ?
                                  spaces[place].length :
                                  (length - copied);

        /* room is at most the place's length. */
        mqtt_bytes_put(spaces[place].data, &data[copied], room);
        copied += room;
        place += 1U;
    }
    return copied;
}

mqtt_status mqtt_server_receive(mqtt_server *server, uint32_t connection,
                                const uint8_t *data, uint32_t length,
                                uint64_t now_ms)
{
    /** @brief Index of the next byte to take. */
    uint32_t position = 0U;
    /** @brief The outcome. */
    mqtt_status status = ((receive_connection(server, connection) != NULL) &&
                          ((data != NULL) || (length == 0U))) ?
                             MQTT_OK :
                             MQTT_INVALID_ARGUMENT;

    if (mqtt_server_ready(server)) {
        server->now_ms = now_ms;
    }
    while ((status == MQTT_OK) && (position < length) &&
           (server->storage.connections[connection].state !=
            MQTT_CONNECTION_CLOSING)) {
        /** @brief The places. */
        mqtt_space spaces[RECEIVE_SPACES];
        /** @brief Number of places. */
        const uint32_t count = mqtt_server_receive_space(server, connection,
                                                         spaces,
                                                         RECEIVE_SPACES);

        if (count == 0U) {
            status = MQTT_RING_FULL;
        } else {
            /** @brief Number of bytes copied in. */
            const uint32_t copied = receive_copy(
                spaces, count, &data[position], length - position);

            position += copied;
            status =
                mqtt_server_receive_commit(server, connection, copied, now_ms);
        }
    }
    return status;
}
