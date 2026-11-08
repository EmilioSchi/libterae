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
 * @file platform_net_server.c
 * @brief Running a server on real network connections.
 *
 * The round of a server: wait for readiness, read from what is readable, close
 * and sync what that wrote to the store, then write out everything the server
 * has for any connection. All of this is on one thread.
 *
 * Readiness comes from the wait functions of base_platform.h, which wait in
 * tiers. After each read the server says what the Client sent, and this file
 * tells the tier set: only traffic other than PINGREQ makes a socket HOT, an
 * accepted CONNECT unpins it and sets its Keep Alive, and bytes held for or by
 * it keep it from COLD.
 */

#include <stddef.h>
#include <string.h>

#include "base_net.h"
#include "base_platform.h"
#include "base_tls.h"
#include "base_tier.h"
#include "base_websocket.h"
#include "platform_net.h"

/** @brief The tag of the first listener; a tag below this is a connection. */
#define NET_LISTENER_TAG 0xFFFFFF00U
/** @brief The number of listeners. */
#define NET_LISTENERS 3U
/** @brief The tag of the pipe that stop signals write to. */
#define NET_WAKE_TAG (NET_LISTENER_TAG + NET_LISTENERS)
#if !defined(MQTT_NET_EVENTS)
/**
 * @brief Number of readiness events handled in one round, unless the build
 * sets another.
 */
#define MQTT_NET_EVENTS 1024U
#endif
/** @brief Number of runs written in one go. */
#define NET_RUNS PLATFORM_NET_RUNS
/**
 * @brief Number of runs of a connection's bytes in one WebSocket write, which
 * leaves room for a Pong frame and a frame header.
 */
#define NET_PAYLOAD_RUNS (NET_RUNS - 2U)
/**
 * @brief How long to pause, in milliseconds, when a listener can't accept what
 * is waiting.
 */
#define NET_ACCEPT_PAUSE_MS 10U
/**
 * @brief Number of bytes one connection may read in one round, so a fast
 * sender can't keep the others waiting.
 */
#define NET_READ_BUDGET (256U * 1024U)

/** @brief Where the MQTT bytes of a WebSocket read are being moved to. */
typedef struct {
    /** @brief The places the bytes were read into. */
    const mqtt_space *spaces;
    /** @brief The place the next MQTT byte goes to. */
    uint32_t place;
    /** @brief Index in it. */
    uint32_t offset;
    /** @brief Number of MQTT bytes so far. */
    uint32_t written;
} net_websocket_writer;

/**
 * @brief The first of the connections whose bytes TLS holds and that have room
 * for them again, or MQTT_SERVER_NONE.
 */
static uint32_t net_again_head = MQTT_SERVER_NONE;
/** @brief Counts of the server that runs. */
static mqtt_net_counters net_counters;
/** @brief The send buffer to give each connection accepted, or zero. */
static uint32_t net_send_buffer;
/**
 * @brief Where small runs are put together into one TLS record, shared as all
 * of a server's connections are on one thread. TLS copies a record's bytes in
 * when it is first given them, so a write it has to finish later doesn't read
 * this again.
 */
static uint8_t net_tls_scratch[PLATFORM_NET_TLS_RECORD];

/**
 * @brief Read through TLS into places, in order, until one is not filled.
 * @param[in] entry The connection, which uses TLS.
 * @param[in] spaces The places.
 * @param[in] count Number of places.
 * @param[out] count_read Number of bytes read.
 * @return MQTT_OK once any bytes came, MQTT_INCOMPLETE, or
 * MQTT_CONNECTION_CLOSED.
 */
static mqtt_status net_tls_read_spaces(const mqtt_net_connection *entry,
                                       const mqtt_space *spaces,
                                       uint32_t count, uint32_t *count_read)
{
    /** @brief Index of the place. */
    uint32_t place = 0U;
    /** @brief Number of bytes read. */
    uint32_t total = 0U;
    /** @brief true while each place has been filled. */
    bool going = true;
    /** @brief The outcome of the last read. */
    mqtt_status status = MQTT_OK;

    while (going && (place < count)) {
        /** @brief Number of bytes read into this place. */
        uint32_t got = 0U;

        status = mqtt_tls_read(entry->tls_session, spaces[place].data,
                               spaces[place].length, &got);
        if (status == MQTT_OK) {
            total += got;
            going = (got == spaces[place].length);
        } else {
            going = false;
        }
        place += 1U;
    }
    /* Bytes that came before a failure are handed on, and the next read
     * finds it again. */
    if (total > 0U) {
        status = MQTT_OK;
    }
    *count_read = total;
    return status;
}

/**
 * @brief Read from a socket into places, through TLS if it uses TLS.
 * @param[in] entry The connection.
 * @param[in] spaces The places.
 * @param[in] count Number of places.
 * @param[out] count_read Number of bytes read.
 * @return MQTT_OK, MQTT_INCOMPLETE, or MQTT_CONNECTION_CLOSED.
 */
static mqtt_status net_stream_read(const mqtt_net_connection *entry,
                                   const mqtt_space *spaces, uint32_t count,
                                   uint32_t *count_read)
{
    return (entry->kind == MQTT_NET_TLS) ?
               net_tls_read_spaces(entry, spaces, count, count_read) :
               mqtt_platform_read_spaces(entry->handle, spaces, count,
                                         count_read);
}

/**
 * @brief Keep no more than a number of bytes of runs.
 * @param[in,out] runs The runs.
 * @param[in] count Number of runs.
 * @param[in] limit Number of bytes to keep.
 * @return Number of runs kept.
 */
static uint32_t net_runs_cap(mqtt_run *runs, uint32_t count, uint32_t limit)
{
    /** @brief Number of runs kept. */
    uint32_t kept = 0U;
    /** @brief Number of bytes kept. */
    uint32_t total = 0U;

    while ((kept < count) && (total < limit)) {
        if (runs[kept].length > (limit - total)) {
            runs[kept].length = limit - total;
        }
        total += runs[kept].length;
        kept += 1U;
    }
    return kept;
}

/**
 * @brief Say what to wait for on a connection, if that has changed.
 * @param[in,out] entry The connection.
 * @param[in] index Its index.
 * @param[in] want_read Whether to wait for read readiness.
 * @param[in] want_write Whether to wait for write readiness.
 */
static void net_interest(mqtt_net_connection *entry, uint32_t index,
                         bool want_read, bool want_write)
{
    (void)index;
    if ((entry->want_read != want_read) || (entry->want_write != want_write)) {
        mqtt_platform_wait_interest(entry->wait, want_read, want_write);
        entry->want_read = want_read;
        entry->want_write = want_write;
    }
}

/**
 * @brief Put a connection on the list of those to read again, unless it is on
 * it already.
 * @param[in,out] connections The connections.
 * @param[in] index The connection.
 */
static void net_again_push(mqtt_net_connection *connections, uint32_t index)
{
    if (!connections[index].again_listed) {
        connections[index].again_listed = true;
        connections[index].again_next = net_again_head;
        net_again_head = index;
    }
}

/**
 * @brief Close a connection, and tell the server it has gone.
 * @param[in,out] server The server.
 * @param[in,out] connections The connections.
 * @param[in] index The connection.
 */
static void net_close(mqtt_server *server, mqtt_net_connection *connections,
                      uint32_t index)
{
    /** @brief The connection. */
    mqtt_net_connection *entry = &connections[index];

    if (entry->handle != MQTT_SOCKET_NONE) {
        mqtt_platform_wait_remove(entry->wait);
        entry->wait = MQTT_TIER_NONE;
        entry->tls_stranded = false;
        if (entry->kind == MQTT_NET_TLS) {
            mqtt_tls_session_close(entry->tls_session);
        }
        mqtt_platform_close(entry->handle);
        entry->handle = MQTT_SOCKET_NONE;
        mqtt_server_connection_closed(server, index, mqtt_platform_time_ms());
    }
}

/**
 * @brief Put together one WebSocket write: a Pong frame waiting between frames,
 * the rest of a frame's header, and as much of its payload as is waiting.
 *
 * Each frame carries one batch, as much as was waiting when it began: MQTT does
 * not care where frames end. A Pong only goes between frames, as it must not
 * land inside one: RFC 6455 sections 5.4 and 5.5.2.
 *
 * @param[in,out] server The server.
 * @param[in,out] entry The connection.
 * @param[in] index Its index.
 * @param[out] runs The runs, with room for NET_RUNS.
 * @param[out] pong Number of bytes of the Pong frame in them.
 * @param[out] header Number of bytes of the frame header in them.
 * @return Number of runs.
 */
static uint32_t net_websocket_runs(mqtt_server *server,
                                   mqtt_net_connection *entry, uint32_t index,
                                   mqtt_run *runs, uint32_t *pong,
                                   uint32_t *header)
{
    /** @brief The connection's bytes. */
    mqtt_run payload[NET_PAYLOAD_RUNS];
    /** @brief Number of runs of them. */
    uint32_t payload_count =
        mqtt_server_transmit_view(server, index, payload, NET_PAYLOAD_RUNS);
    /** @brief Number of runs. */
    uint32_t count = 0U;
    /** @brief Index of a run of the connection's bytes. */
    uint32_t run = 0U;

    *pong = 0U;
    *header = 0U;
    if ((entry->websocket_header_left == 0U) &&
        (entry->websocket_payload_left == 0U)) {
        if (entry->websocket_pong_left > 0U) {
            runs[0].data = &entry->websocket_pong[entry->websocket_pong_size -
                                                  entry->websocket_pong_left];
            runs[0].length = entry->websocket_pong_left;
            *pong = entry->websocket_pong_left;
            count = 1U;
        }
        if (payload_count > 0U) {
            entry->websocket_payload_left =
                platform_net_runs_length(payload, payload_count);
            entry->websocket_header_size = mqtt_websocket_frame_write(
                entry->websocket_header, (uint8_t)MQTT_WEBSOCKET_OPCODE_BINARY,
                (uint64_t)entry->websocket_payload_left);
            entry->websocket_header_left = entry->websocket_header_size;
        }
    }
    if (entry->websocket_header_left > 0U) {
        runs[count].data =
            &entry->websocket_header[entry->websocket_header_size -
                                     entry->websocket_header_left];
        runs[count].length = entry->websocket_header_left;
        *header = entry->websocket_header_left;
        count += 1U;
    }
    payload_count =
        net_runs_cap(payload, payload_count, entry->websocket_payload_left);
    while (run < payload_count) {
        runs[count] = payload[run];
        count += 1U;
        run += 1U;
    }
    return count;
}

/**
 * @brief Write frames for as long as the socket takes them, for WebSocket.
 * @param[in,out] server The server.
 * @param[in,out] entry The connection.
 * @param[in] index Its index.
 * @return MQTT_OK, MQTT_INCOMPLETE if the socket is full, or
 * MQTT_CONNECTION_CLOSED.
 */
static mqtt_status net_flush_websocket(mqtt_server *server,
                                       mqtt_net_connection *entry,
                                       uint32_t index)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief true while there may be more to write. */
    bool more = true;

    while ((status == MQTT_OK) && more) {
        /** @brief The runs of one write. */
        mqtt_run runs[NET_RUNS];
        /** @brief Number of bytes of the Pong frame in them. */
        uint32_t pong = 0U;
        /** @brief Number of bytes of the frame header in them. */
        uint32_t header = 0U;
        /** @brief Number of runs. */
        const uint32_t count =
            net_websocket_runs(server, entry, index, runs, &pong, &header);
        /** @brief Number of bytes taken. */
        uint32_t taken = 0U;

        more = count > 0U;
        if (more) {
            net_counters.batches += 1U;
            net_counters.write_calls += 1U;
            status = mqtt_platform_write_runs(entry->handle, runs, count,
                                              &taken);
        }
        if (more && (status == MQTT_OK)) {
            /** @brief Number of bytes the socket did not take. */
            const uint32_t short_by =
                platform_net_runs_length(runs, count) - taken;
            /** @brief Number of them that were the Pong's. */
            const uint32_t pong_taken = (taken < pong) ? taken : pong;
            /** @brief Number of them that were the header's. */
            const uint32_t header_taken =
                ((taken - pong_taken) < header) ? (taken - pong_taken) : header;
            /** @brief Number of them that were the connection's bytes. */
            const uint32_t payload_taken = taken - pong_taken - header_taken;

            entry->websocket_pong_left -= pong_taken;
            entry->websocket_header_left -= header_taken;
            entry->websocket_payload_left -= payload_taken;
            mqtt_server_transmit_consume(server, index, payload_taken);
            if (short_by > 0U) {
                status = MQTT_INCOMPLETE;
            }
        }
    }
    return status;
}

/**
 * @brief Write what the server has for a connection, one batch to a write, for
 * as long as the socket takes it, directly or through TLS.
 *
 * A batch is what one view gives: on a plain socket, what is waiting goes out
 * in one system call, however it is split. A socket that takes less is full, so
 * no write is tried just to be told so.
 *
 * @param[in,out] server The server.
 * @param[in] entry The connection.
 * @param[in] index Its index.
 * @return MQTT_OK, MQTT_INCOMPLETE if the socket is full, or
 * MQTT_CONNECTION_CLOSED.
 */
static mqtt_status net_flush_stream(mqtt_server *server,
                                    const mqtt_net_connection *entry,
                                    uint32_t index)
{
    /** @brief The runs of one write. */
    mqtt_run runs[NET_RUNS];
    /** @brief Number of runs. */
    uint32_t count = mqtt_server_transmit_view(server, index, runs, NET_RUNS);
    /** @brief The TLS session, or MQTT_TLS_NONE for a plain socket. */
    const uint32_t session = (entry->kind == MQTT_NET_TLS) ?
                                 entry->tls_session :
                                 MQTT_TLS_NONE;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    while ((status == MQTT_OK) && (count > 0U)) {
        /** @brief Number of bytes taken. */
        uint32_t taken = 0U;

        net_counters.batches += 1U;
        net_counters.write_calls += 1U;
        status = platform_net_write_runs(entry->handle, session, runs, count,
                                         net_tls_scratch, &taken);
        if (status == MQTT_OK) {
            mqtt_server_transmit_consume(server, index, taken);
            if ((session == MQTT_TLS_NONE) &&
                (taken < platform_net_runs_length(runs, count))) {
                status = MQTT_INCOMPLETE;
            } else {
                count = mqtt_server_transmit_view(server, index, runs,
                                                  NET_RUNS);
            }
        }
    }
    return status;
}

/**
 * @brief Write out what the server has for a connection, for as long as the
 * socket takes it.
 * @param[in,out] server The server.
 * @param[in,out] connections The connections.
 * @param[in] index The connection.
 */
static void net_flush(mqtt_server *server, mqtt_net_connection *connections,
                      uint32_t index)
{
    /** @brief The connection. */
    mqtt_net_connection *entry = &connections[index];
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if ((entry->kind == MQTT_NET_WEBSOCKET) && entry->websocket_open) {
        status = net_flush_websocket(server, entry, index);
    } else if (entry->kind != MQTT_NET_WEBSOCKET) {
        status = net_flush_stream(server, entry, index);
    } else {
        /* The opening handshake is still going on, and it writes its own
         * reply. */
    }
    if (status == MQTT_CONNECTION_CLOSED) {
        net_close(server, connections, index);
    } else if (mqtt_server_should_close(server, index)) {
        net_close(server, connections, index);
    } else {
        /** @brief true if the socket could not take everything. */
        const bool blocked = (status == MQTT_INCOMPLETE) ||
                             ((entry->kind == MQTT_NET_TLS) &&
                              mqtt_tls_wants_write(entry->tls_session));
        /**
         * @brief true if there is room for what reading more would make the
         * server send.
         */
        const bool can_read = mqtt_server_receive_wanted(server, index);

        net_interest(entry, index, can_read, blocked);
        mqtt_tier_busy(mqtt_platform_wait_tier(), entry->wait,
                       !mqtt_server_idle(server, index));
        if (can_read && entry->tls_stranded) {
            net_again_push(connections, index);
        }
    }
}

/**
 * @brief Act on a whole control frame, whose payload is in the buffer: a Ping
 * gets a Pong.
 *
 * The Pong goes out with the next flush, as it must not land inside a frame
 * being sent: RFC 6455 sections 5.4 and 5.5.2.
 *
 * @param[in,out] entry The connection.
 */
static void net_websocket_control(mqtt_net_connection *entry)
{
    /* A Pong that has started to go has to finish, and this one's answer can be
     * left out. */
    if ((entry->websocket_control_opcode ==
         (uint8_t)MQTT_WEBSOCKET_OPCODE_PING) &&
        ((entry->websocket_pong_left == 0U) ||
         (entry->websocket_pong_left == entry->websocket_pong_size))) {
        /** @brief Number of bytes in the payload. */
        const uint32_t length = entry->websocket_used;
        /** @brief Where in the mask the bytes start. */
        uint32_t mask_at = 0U;
        /** @brief Number of bytes in the header. */
        const uint32_t size = mqtt_websocket_frame_write(
            entry->websocket_pong, (uint8_t)MQTT_WEBSOCKET_OPCODE_PONG,
            (uint64_t)length);

        mqtt_websocket_unmask(entry->websocket_buffer, length,
                              entry->websocket_mask, &mask_at);
        (void)memcpy(&entry->websocket_pong[size], entry->websocket_buffer,
                     (size_t)length);
        entry->websocket_pong_size = size + length;
        entry->websocket_pong_left = size + length;
    }
    entry->websocket_used = 0U;
}

/**
 * @brief Take bytes of a WebSocket connection's opening handshake, and answer
 * the request once it is whole.
 * @param[in,out] entry The connection.
 * @param[in] bytes The bytes read.
 * @param[in] count Number of bytes in bytes.
 * @param[in,out] position Index of the next byte in bytes.
 * @return MQTT_OK or MQTT_CONNECTION_CLOSED.
 */
static mqtt_status net_websocket_handshake(mqtt_net_connection *entry,
                                           const uint8_t *bytes, uint32_t count,
                                           uint32_t *position)
{
    /** @brief The reply. */
    uint8_t reply[512];
    /** @brief Number of bytes in the reply. */
    uint32_t reply_length = 0U;
    /** @brief Number of bytes the request took. */
    uint32_t used = 0U;
    /** @brief Number of bytes to keep. */
    const uint32_t take =
        ((count - *position) < (1024U - entry->websocket_used)) ?
            (count - *position) :
            (1024U - entry->websocket_used);
    /** @brief The outcome of looking at the request. */
    mqtt_status accepted = MQTT_OK;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    (void)memcpy(&entry->websocket_buffer[entry->websocket_used],
                 &bytes[*position], (size_t)take);
    entry->websocket_used += take;
    *position += take;
    accepted = mqtt_websocket_accept(entry->websocket_buffer,
                                     entry->websocket_used, reply, 512U,
                                     &reply_length, &used);
    if (accepted == MQTT_OK) {
        /** @brief Number of bytes taken by the socket. */
        uint32_t written = 0U;

        if ((mqtt_platform_write(entry->handle, reply, reply_length,
                                 &written) != MQTT_OK) ||
            (written != reply_length)) {
            status = MQTT_CONNECTION_CLOSED;
        } else {
            /* What came after the request is the start of the frames. */
            *position -= entry->websocket_used - used;
            entry->websocket_open = true;
            entry->websocket_used = 0U;
        }
    } else if ((accepted != MQTT_INCOMPLETE) ||
               (entry->websocket_used == 1024U)) {
        /**
         * @brief The answer to what is not a valid opening: RFC 6455 section
         * 4.2.1. It names the version spoken here, which section 4.2.2 asks
         * for when the version is what is wrong. The reply is the same for
         * every fault, as the library doesn't say which it found.
         */
        static const char refusal[] =
            "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n"
            "Sec-WebSocket-Version: " MQTT_WEBSOCKET_VERSION "\r\n"
            "Content-Length: 0\r\n\r\n";
        /** @brief Number of bytes taken by the socket. */
        uint32_t written = 0U;

        (void)mqtt_platform_write(entry->handle, (const uint8_t *)refusal,
                                  (uint32_t)(sizeof(refusal) - 1U), &written);
        status = MQTT_CONNECTION_CLOSED;
    } else {
        /* The request goes on in the next read. */
    }
    return status;
}

/**
 * @brief Take the part of a control frame's payload that is in the bytes read,
 * and act on the frame once it is whole.
 * @param[in,out] entry The connection.
 * @param[in] bytes The bytes read.
 * @param[in] count Number of bytes in bytes.
 * @param[in,out] position Index of the next byte in bytes.
 */
static void net_websocket_control_payload(mqtt_net_connection *entry,
                                          const uint8_t *bytes, uint32_t count,
                                          uint32_t *position)
{
    /** @brief Number of bytes of the control frame here. */
    const uint32_t here =
        ((count - *position) < entry->websocket_control_left) ?
            (count - *position) :
            entry->websocket_control_left;

    (void)memcpy(&entry->websocket_buffer[entry->websocket_used],
                 &bytes[*position], (size_t)here);
    entry->websocket_used += here;
    entry->websocket_control_left -= here;
    *position += here;
    if (entry->websocket_control_left == 0U) {
        net_websocket_control(entry);
    }
}

/**
 * @brief Keep the part of a data frame's payload that is in the bytes read:
 * unmask it where it is, and move it down to where the MQTT bytes so far end.
 * @param[in,out] entry The connection.
 * @param[in,out] writer Where the MQTT bytes so far end.
 * @param[in,out] bytes The bytes read.
 * @param[in] count Number of bytes in bytes.
 * @param[in,out] position Index of the next byte in bytes.
 */
static void net_websocket_data_payload(mqtt_net_connection *entry,
                                       net_websocket_writer *writer,
                                       uint8_t *bytes, uint32_t count,
                                       uint32_t *position)
{
    /** @brief Number of bytes the buffer still holds. */
    const uint32_t buffered = count - *position;
    /** @brief Number of bytes of this frame in the buffer. */
    const uint32_t here = ((uint64_t)buffered < entry->websocket_left) ?
                              buffered :
                              (uint32_t)entry->websocket_left;
    /** @brief Index of the byte being moved. */
    uint32_t index = 0U;

    mqtt_websocket_unmask(&bytes[*position], here, entry->websocket_mask,
                          &entry->websocket_mask_at);
    /* Bytes only move towards the start, to places already read, so moving
     * them one at a time from the first overwrites none still to move. */
    while (index < here) {
        while (writer->offset == writer->spaces[writer->place].length) {
            writer->place += 1U;
            writer->offset = 0U;
        }
        writer->spaces[writer->place].data[writer->offset] =
            bytes[*position + index];
        writer->offset += 1U;
        writer->written += 1U;
        index += 1U;
    }
    entry->websocket_left -= (uint64_t)here;
    *position += here;
}

/**
 * @brief Act on a whole frame header: get ready for the payload, or close.
 * @param[in,out] entry The connection.
 * @param[in] frame The frame header.
 * @return MQTT_OK or MQTT_CONNECTION_CLOSED.
 */
static mqtt_status net_websocket_frame_start(mqtt_net_connection *entry,
                                             const mqtt_websocket_frame *frame)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (!frame->masked) {
        /* Frames from a Client must be masked: RFC 6455 section 5.1. */
        status = MQTT_CONNECTION_CLOSED;
    } else if (frame->opcode == (uint8_t)MQTT_WEBSOCKET_OPCODE_CLOSE) {
        /* It gets one in return, unless that would land inside a frame:
         * section 5.5.1. */
        if ((entry->websocket_header_left == 0U) &&
            (entry->websocket_payload_left == 0U)) {
            /** @brief The Close frame. */
            uint8_t closing[2] = {0U, 0U};
            /** @brief Number of bytes taken by the socket. */
            uint32_t written = 0U;
            /** @brief Number of bytes in the frame. */
            const uint32_t size = mqtt_websocket_frame_write(
                closing, (uint8_t)MQTT_WEBSOCKET_OPCODE_CLOSE, 0U);

            (void)mqtt_platform_write(entry->handle, closing, size, &written);
        }
        status = MQTT_CONNECTION_CLOSED;
    } else if (frame->opcode >= (uint8_t)MQTT_WEBSOCKET_OPCODE_CLOSE) {
        /* Its payload may be split across reads, so it is gathered first. A
         * control frame is short, is never fragmented, and only two more are
         * defined: section 5.5. */
        if ((frame->payload_length > 125U) || (!frame->final) ||
            (frame->opcode > (uint8_t)MQTT_WEBSOCKET_OPCODE_PONG)) {
            status = MQTT_CONNECTION_CLOSED;
        } else {
            (void)memcpy(entry->websocket_mask, frame->mask, 4U);
            entry->websocket_mask_at = 0U;
            entry->websocket_control_opcode = frame->opcode;
            entry->websocket_control_left = (uint32_t)frame->payload_length;
            if (entry->websocket_control_left == 0U) {
                net_websocket_control(entry);
            }
        }
    } else if ((frame->opcode != (uint8_t)MQTT_WEBSOCKET_OPCODE_BINARY) &&
               (frame->opcode !=
                (uint8_t)MQTT_WEBSOCKET_OPCODE_CONTINUATION)) {
        /* MQTT only travels in binary frames: [MQTT-6.0.0-1]. */
        status = MQTT_CONNECTION_CLOSED;
    } else if (((frame->opcode == (uint8_t)MQTT_WEBSOCKET_OPCODE_BINARY) &&
                entry->websocket_fragmented) ||
               ((frame->opcode ==
                 (uint8_t)MQTT_WEBSOCKET_OPCODE_CONTINUATION) &&
                (!entry->websocket_fragmented))) {
        /* A message starts only after the last one ends, and only a started
         * one can be continued: section 5.4. Control frames may come between
         * the fragments, so they leave this as it is. */
        status = MQTT_CONNECTION_CLOSED;
    } else {
        entry->websocket_fragmented = !frame->final;
        (void)memcpy(entry->websocket_mask, frame->mask, 4U);
        entry->websocket_mask_at = 0U;
        entry->websocket_left = frame->payload_length;
    }
    return status;
}

/**
 * @brief Take bytes of a frame header, and act on the header once it is whole.
 * @param[in,out] entry The connection.
 * @param[in] bytes The bytes read.
 * @param[in] count Number of bytes in bytes.
 * @param[in,out] position Index of the next byte in bytes.
 * @return MQTT_OK or MQTT_CONNECTION_CLOSED.
 */
static mqtt_status net_websocket_frame_header(mqtt_net_connection *entry,
                                              const uint8_t *bytes,
                                              uint32_t count,
                                              uint32_t *position)
{
    /** @brief The frame header. */
    mqtt_websocket_frame frame;
    /** @brief Number of bytes to keep. */
    const uint32_t take =
        ((count - *position) < 16U) ? (count - *position) : 16U;
    /** @brief The outcome of reading it. */
    mqtt_status read_status = MQTT_OK;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    (void)memcpy(&entry->websocket_buffer[entry->websocket_used],
                 &bytes[*position], (size_t)take);
    read_status = mqtt_websocket_frame_read(
        entry->websocket_buffer, entry->websocket_used + take, &frame);
    if (read_status == MQTT_OK) {
        /** @brief Number of the header's bytes that are in these bytes. */
        const uint32_t in_buffer = frame.size - entry->websocket_used;

        *position += in_buffer;
        entry->websocket_used = 0U;
        status = net_websocket_frame_start(entry, &frame);
    } else if (read_status == MQTT_INCOMPLETE) {
        entry->websocket_used += take;
        *position += take;
    } else {
        status = MQTT_CONNECTION_CLOSED;
    }
    return status;
}

/**
 * @brief Take the bytes of one place read on a WebSocket connection: the
 * opening handshake, then frames, keeping the MQTT bytes.
 * @param[in,out] entry The connection.
 * @param[in,out] writer Where the MQTT bytes so far end.
 * @param[in,out] bytes The bytes.
 * @param[in] count Number of bytes.
 * @return MQTT_OK or MQTT_CONNECTION_CLOSED.
 */
static mqtt_status net_websocket_take(mqtt_net_connection *entry,
                                      net_websocket_writer *writer,
                                      uint8_t *bytes, uint32_t count)
{
    /** @brief Index of the next byte. */
    uint32_t position = 0U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    while ((status == MQTT_OK) && (position < count)) {
        if (!entry->websocket_open) {
            status = net_websocket_handshake(entry, bytes, count, &position);
        } else if (entry->websocket_control_left > 0U) {
            net_websocket_control_payload(entry, bytes, count, &position);
        } else if (entry->websocket_left > 0U) {
            net_websocket_data_payload(entry, writer, bytes, count, &position);
        } else {
            status =
                net_websocket_frame_header(entry, bytes, count, &position);
        }
    }
    return status;
}

/**
 * @brief Take the bytes read on a WebSocket connection, which are in the places
 * the server gave, and hand the server the MQTT bytes among them.
 *
 * The frame headers, the handshake and control frames are taken out, and the
 * data is moved down over them, so the server is given its bytes where they
 * are.
 *
 * @param[in,out] server The server.
 * @param[in,out] entry The connection.
 * @param[in] index Its index.
 * @param[in] spaces The places.
 * @param[in] count_read Number of bytes read into them.
 * @param[in] now_ms The time.
 * @return MQTT_OK or MQTT_CONNECTION_CLOSED.
 */
static mqtt_status net_receive_websocket(mqtt_server *server,
                                         mqtt_net_connection *entry,
                                         uint32_t index,
                                         const mqtt_space *spaces,
                                         uint32_t count_read, uint64_t now_ms)
{
    /** @brief Where the MQTT bytes go. */
    net_websocket_writer writer = {spaces, 0U, 0U, 0U};
    /** @brief Number of bytes read not yet looked at. */
    uint32_t left = count_read;
    /** @brief Index of the place being looked at. */
    uint32_t place = 0U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    while ((status == MQTT_OK) && (left > 0U)) {
        /** @brief Number of bytes read into this place. */
        const uint32_t here =
            (left < spaces[place].length) ? left : spaces[place].length;

        status = net_websocket_take(entry, &writer, spaces[place].data, here);
        left -= here;
        place += 1U;
    }
    (void)mqtt_server_receive_commit(server, index, writer.written, now_ms);
    return status;
}

/**
 * @brief Tell the tier set what a read brought.
 * @param[in,out] server The server.
 * @param[in] entry The connection.
 * @param[in] index Its index.
 * @param[in] received true if any bytes came.
 */
static void net_tier_update(mqtt_server *server,
                            const mqtt_net_connection *entry, uint32_t index,
                            bool received)
{
    /** @brief The tier set. */
    mqtt_tier *tier = mqtt_platform_wait_tier();
    /** @brief The socket's record. */
    const mqtt_tier_socket *record = mqtt_tier_get(tier, entry->wait);

    if (received) {
        /* A WebSocket Ping, or TLS that brought no MQTT bytes, starts no
         * packet, which counts as keeping the connection alive. */
        mqtt_tier_received(tier, entry->wait, mqtt_platform_time_ms(),
                           mqtt_server_take_activity(server, index) !=
                               MQTT_SERVER_ACTIVITY_OTHER);
    }
    if ((record != NULL) &&
        ((record->pins & MQTT_TIER_PIN_CONNECTING) != 0U) &&
        mqtt_server_accepted(server, index)) {
        mqtt_tier_unpin(tier, entry->wait, MQTT_TIER_PIN_CONNECTING);
        mqtt_tier_keep_alive(tier, entry->wait,
                             mqtt_server_keep_alive(server, index));
    }
    if (entry->tls_stranded) {
        mqtt_tier_pin(tier, entry->wait, MQTT_TIER_PIN_TLS_PENDING);
    } else {
        mqtt_tier_unpin(tier, entry->wait, MQTT_TIER_PIN_TLS_PENDING);
    }
}

/**
 * @brief Count the bytes of some places.
 * @param[in] spaces The places.
 * @param[in] count Number of places.
 * @return Number of bytes.
 */
static uint32_t net_spaces_length(const mqtt_space *spaces, uint32_t count)
{
    /** @brief The answer. */
    uint32_t length = 0U;
    /** @brief The place being counted. */
    uint32_t index;

    for (index = 0U; index < count; index += 1U) {
        length += spaces[index].length;
    }
    return length;
}

/**
 * @brief Read from a connection straight into the places the server gives, and
 * hand what arrived to the server. A read that fills every place may have left
 * more in the socket, so it reads again, until a read comes up short, the
 * server has no room or NET_READ_BUDGET bytes have come.
 * @param[in,out] server The server.
 * @param[in,out] connections The connections.
 * @param[in] index The connection.
 */
static void net_read(mqtt_server *server, mqtt_net_connection *connections,
                     uint32_t index)
{
    /** @brief The connection. */
    mqtt_net_connection *entry = &connections[index];
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /**
     * @brief true to go round again, as TLS may hold more than the socket
     * shows.
     */
    bool again = true;
    /** @brief Number of bytes read in this round. */
    uint32_t total = 0U;

    while (again && (status == MQTT_OK)) {
        /** @brief The places. */
        mqtt_space spaces[MQTT_PLATFORM_READ_SPACES];
        /** @brief Number of places. */
        const uint32_t count = mqtt_server_receive_space(
            server, index, spaces, MQTT_PLATFORM_READ_SPACES);
        /** @brief Number of bytes read. */
        uint32_t count_read = 0U;

        again = false;
        if (count > 0U) {
            /** @brief Number of bytes the places hold. */
            const uint32_t offered = net_spaces_length(spaces, count);

            status = net_stream_read(entry, spaces, count, &count_read);
            if (status != MQTT_OK) {
                count_read = 0U;
            }
            if ((status == MQTT_OK) && (entry->kind == MQTT_NET_WEBSOCKET)) {
                status = net_receive_websocket(server, entry, index, spaces,
                                               count_read,
                                               mqtt_platform_time_ms());
            } else {
                (void)mqtt_server_receive_commit(server, index, count_read,
                                                 mqtt_platform_time_ms());
            }
            total += count_read;
            again = (status == MQTT_OK) &&
                    (((count_read == offered) && (total < NET_READ_BUDGET)) ||
                     ((entry->kind == MQTT_NET_TLS) &&
                      mqtt_tls_has_pending(entry->tls_session)));
        }
    }
    if (status == MQTT_INCOMPLETE) {
        status = MQTT_OK;
    }
    entry->tls_stranded =
        (entry->kind == MQTT_NET_TLS) &&
        (mqtt_server_receive_capacity(server, index) == 0U) &&
        mqtt_tls_has_pending(entry->tls_session);
    if (status == MQTT_CONNECTION_CLOSED) {
        net_close(server, connections, index);
    } else {
        net_tier_update(server, entry, index, total > 0U);
        net_flush(server, connections, index);
    }
}

/**
 * @brief Read again from the connections whose bytes TLS holds, now that they
 * have room.
 *
 * Readiness can't report those, as they have left the socket already. Each is
 * on a list, so nothing else is looked at. A connection that goes back on it
 * meanwhile waits for the next round.
 *
 * @param[in,out] server The server.
 * @param[in,out] connections The connections.
 */
static void net_read_stranded(mqtt_server *server,
                              mqtt_net_connection *connections)
{
    /** @brief The connection being taken off the list. */
    uint32_t index = net_again_head;

    net_again_head = MQTT_SERVER_NONE;
    while (index != MQTT_SERVER_NONE) {
        /** @brief The next one. */
        const uint32_t next = connections[index].again_next;

        connections[index].again_listed = false;
        if ((connections[index].handle != MQTT_SOCKET_NONE) &&
            connections[index].tls_stranded &&
            mqtt_server_receive_wanted(server, index)) {
            net_read(server, connections, index);
        }
        index = next;
    }
}

/**
 * @brief Accept every connection waiting at a listener.
 * @param[in,out] server The server.
 * @param[in,out] connections The connections.
 * @param[in] listener The listening socket.
 * @param[in] kind How its connections carry their bytes.
 */
static void net_accept(mqtt_server *server, mqtt_net_connection *connections,
                       int32_t listener, mqtt_net_kind kind)
{
    /** @brief true while more may be waiting. */
    bool more = true;

    while (more) {
        /** @brief The new socket. */
        int32_t handle = MQTT_SOCKET_NONE;

        /** @brief The outcome of accepting. */
        const mqtt_status accepted = mqtt_platform_accept(listener, &handle);

        if (accepted == MQTT_CONNECTION_CLOSED) {
            /* Out of descriptors, say. The connection stays queued and the
             * listener stays ready, so without a pause this would take a whole
             * CPU until one is free. */
            mqtt_platform_sleep_ms(NET_ACCEPT_PAUSE_MS);
            more = false;
        } else if (accepted != MQTT_OK) {
            more = false;
        } else {
            /** @brief The connection's index. */
            uint32_t index = 0U;

            if (net_send_buffer != 0U) {
                mqtt_platform_send_buffer(handle, net_send_buffer);
            }
            if (mqtt_server_connection_open(server, mqtt_platform_time_ms(),
                                            &index) != MQTT_OK) {
                /* No room: with no CONNECT packet yet, there is no way to say
                 * so politely. */
                mqtt_platform_close(handle);
            } else {
                /** @brief The connection. */
                mqtt_net_connection *entry = &connections[index];
                /** @brief Whether it is on the list to read again. */
                const bool listed = entry->again_listed;
                /** @brief The next one on that list. */
                const uint32_t next = entry->again_next;

                (void)memset(entry, 0, sizeof(*entry));
                entry->again_listed = listed;
                entry->again_next = next;
                entry->handle = handle;
                entry->kind = kind;
                entry->tls_session = MQTT_TLS_NONE;
                if (kind == MQTT_NET_TLS) {
                    entry->tls_session =
                        mqtt_tls_session_open(handle, true, NULL);
                }
                entry->wait = mqtt_platform_wait_add(
                    handle, index, MQTT_TIER_PIN_CONNECTING);
                if (((kind == MQTT_NET_TLS) &&
                     (entry->tls_session == MQTT_TLS_NONE)) ||
                    (entry->wait == MQTT_TIER_NONE)) {
                    mqtt_platform_wait_remove(entry->wait);
                    if (entry->tls_session != MQTT_TLS_NONE) {
                        mqtt_tls_session_close(entry->tls_session);
                    }
                    mqtt_platform_close(handle);
                    entry->handle = MQTT_SOCKET_NONE;
                    entry->wait = MQTT_TIER_NONE;
                    mqtt_server_connection_closed(server, index,
                                                  mqtt_platform_time_ms());
                } else {
                    net_interest(entry, index, true, false);
                }
            }
        }
    }
}

/**
 * @brief Get ready to run a server: no connection is open yet, then
 * readiness, the store, TLS and the listeners.
 * @param[in,out] server The server, whose store gets a whole snapshot.
 * @param[out] connections One entry for each connection of the server.
 * @param[in] options Where to listen.
 * @param[in,out] listeners The sockets that accept connections, each
 * MQTT_SOCKET_NONE until it listens.
 * @return MQTT_OK, or why it could not start.
 */
static mqtt_status net_server_start(mqtt_server *server,
                                    mqtt_net_connection *connections,
                                    const mqtt_net_server_options *options,
                                    int32_t *listeners)
{
    /** @brief Index of the entry being looked at. */
    uint32_t index = 0U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    while (index < server->storage.connection_count) {
        connections[index].handle = MQTT_SOCKET_NONE;
        connections[index].wait = MQTT_TIER_NONE;
        connections[index].again_listed = false;
        index += 1U;
    }
    net_again_head = MQTT_SERVER_NONE;
    net_send_buffer = options->send_buffer;
    (void)memset(&net_counters, 0, sizeof(net_counters));
    if ((server->storage.connection_count >
         (MQTT_PLATFORM_WAIT_SOCKETS - NET_LISTENERS - 1U)) ||
        (mqtt_platform_wait_start(options->tier) != MQTT_OK)) {
        status = MQTT_INVALID_ARGUMENT;
    }
    if (status == MQTT_OK) {
        status = platform_net_store_start(server, options->store);
    }
    if ((status == MQTT_OK) && (options->tls_port != NULL)) {
        status =
            mqtt_tls_server_setup(options->certificate_file, options->key_file,
                                  options->key_password, options->ca_file);
    }
    if ((status == MQTT_OK) && (options->port != NULL)) {
        status =
            mqtt_platform_listen(options->host, options->port, &listeners[0]);
    }
    if ((status == MQTT_OK) && (options->tls_port != NULL)) {
        status = mqtt_platform_listen(options->host, options->tls_port,
                                      &listeners[1]);
    }
    if ((status == MQTT_OK) && (options->websocket_port != NULL)) {
        status = mqtt_platform_listen(options->host, options->websocket_port,
                                      &listeners[2]);
    }
    index = 0U;
    while ((status == MQTT_OK) && (index < NET_LISTENERS)) {
        if (listeners[index] != MQTT_SOCKET_NONE) {
            (void)mqtt_platform_wait_add(listeners[index],
                                         NET_LISTENER_TAG + index,
                                         MQTT_TIER_PIN_ALWAYS);
        }
        index += 1U;
    }
    if ((status == MQTT_OK) &&
        (mqtt_platform_wake_handle() != MQTT_SOCKET_NONE)) {
        (void)mqtt_platform_wait_add(mqtt_platform_wake_handle(), NET_WAKE_TAG,
                                     MQTT_TIER_PIN_ALWAYS);
    }
    return status;
}

/**
 * @brief Act on one event: accept what waits at a listener, or write and read
 * on a connection.
 * @param[in,out] server The server.
 * @param[in,out] connections The connections.
 * @param[in] listeners The sockets that accept connections.
 * @param[in] event The event.
 */
static void net_server_event(mqtt_server *server,
                             mqtt_net_connection *connections,
                             const int32_t *listeners,
                             const mqtt_wait_event *event)
{
    /** @brief How each listener's connections carry their bytes. */
    static const mqtt_net_kind kinds[NET_LISTENERS] = {
        MQTT_NET_TCP, MQTT_NET_TLS, MQTT_NET_WEBSOCKET};
    /** @brief The tag. */
    const uint32_t tag = event->tag;
    /**
     * @brief Where a view of what a connection has to send goes, when only
     * whether there is any counts.
     */
    mqtt_run unused[1];

    if (tag == NET_WAKE_TAG) {
        mqtt_platform_wake_drain();
    } else if (tag >= NET_LISTENER_TAG) {
        net_accept(server, connections, listeners[tag - NET_LISTENER_TAG],
                   kinds[tag - NET_LISTENER_TAG]);
    } else if (connections[tag].handle != MQTT_SOCKET_NONE) {
        /**
         * @brief true if it can read, or a TLS handshake waits for a writable
         * socket.
         */
        bool read_too = event->readable;

        if (event->writable) {
            read_too =
                read_too ||
                ((connections[tag].kind == MQTT_NET_TLS) &&
                 mqtt_tls_wants_write(connections[tag].tls_session) &&
                 (mqtt_server_transmit_view(server, tag, unused, 1U) == 0U));
            net_flush(server, connections, tag);
        }
        if (read_too && (connections[tag].handle != MQTT_SOCKET_NONE)) {
            net_read(server, connections, tag);
        }
    } else {
        /* It has been closed since this event was reported. */
    }
}

/**
 * @brief Run one round of a server: wait for readiness, act on each event,
 * then write out what the server has for any connection.
 * @param[in,out] server The server.
 * @param[in,out] connections The connections.
 * @param[in] listeners The sockets that accept connections.
 */
static void net_server_round(mqtt_server *server,
                             mqtt_net_connection *connections,
                             const int32_t *listeners)
{
    /** @brief The events. */
    static mqtt_wait_event events[MQTT_NET_EVENTS];
    /** @brief How long the timers and the store allow a wait to be. */
    const uint32_t wait_ms = platform_net_store_wait(
        server, mqtt_server_tick(server, mqtt_platform_time_ms()));
    /** @brief Number of events. */
    const uint32_t count = mqtt_platform_wait(
        events, MQTT_NET_EVENTS,
        (net_again_head != MQTT_SERVER_NONE) ? 0U : wait_ms);
    /** @brief Index of the event. */
    uint32_t event = 0U;
    /** @brief A connection with work to do. */
    uint32_t ready = MQTT_SERVER_NONE;

    while (event < count) {
        net_server_event(server, connections, listeners, &events[event]);
        event += 1U;
    }
    /* The answers that waited for the round's records become pending. */
    platform_net_store_round(server);
    ready = mqtt_server_next_pending(server);
    while (ready != MQTT_SERVER_NONE) {
        if (connections[ready].handle != MQTT_SOCKET_NONE) {
            net_flush(server, connections, ready);
        }
        ready = mqtt_server_next_pending(server);
    }
    if (net_again_head != MQTT_SERVER_NONE) {
        net_read_stranded(server, connections);
    }
}

/**
 * @brief Close the listeners that are open, then every connection.
 * @param[in,out] server The server.
 * @param[in,out] connections The connections.
 * @param[in] listeners The sockets that accept connections.
 */
static void net_server_stop(mqtt_server *server,
                            mqtt_net_connection *connections,
                            const int32_t *listeners)
{
    /** @brief Index of the entry being looked at. */
    uint32_t index = 0U;

    while (index < NET_LISTENERS) {
        if (listeners[index] != MQTT_SOCKET_NONE) {
            mqtt_platform_close(listeners[index]);
        }
        index += 1U;
    }
    index = 0U;
    while (index < server->storage.connection_count) {
        net_close(server, connections, index);
        index += 1U;
    }
    platform_net_store_stop(server);
}

mqtt_status mqtt_net_server_run(mqtt_server *server,
                                mqtt_net_connection *connections,
                                const mqtt_net_server_options *options)
{
    /** @brief The sockets that accept connections. */
    int32_t listeners[NET_LISTENERS] = {MQTT_SOCKET_NONE, MQTT_SOCKET_NONE,
                                        MQTT_SOCKET_NONE};
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    (void)mqtt_platform_stop_requested();
    status = net_server_start(server, connections, options, listeners);
    while ((status == MQTT_OK) && (!mqtt_platform_stop_requested())) {
        net_server_round(server, connections, listeners);
    }
    net_server_stop(server, connections, listeners);
    return status;
}

void mqtt_net_server_counters(mqtt_net_counters *counters)
{
    *counters = net_counters;
    mqtt_tier_counters_get(mqtt_platform_wait_tier(), &counters->tier);
}
