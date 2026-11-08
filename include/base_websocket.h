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
 * @file base_websocket.h
 * @brief Carrying MQTT over WebSocket.
 *
 * The specifications allow MQTT to be carried over WebSocket, in binary frames,
 * with a subprotocol of "mqtt". This file has only what that needs, with no
 * input or output of its own: the server's side of the opening handshake, and
 * the layout of frames. The SHA-1 that the opening handshake calls for is here,
 * and it has no other use.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 6, p.116.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015.
 * @li Cited: 6.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 *
 * [RFC6455] The WebSocket Protocol.
 * @li Cited: 1.3, 4.2.1, 4.2.2, 5.2, 5.3, 5.5.
 * @li Source: www.rfc-editor.org/rfc/rfc6455
 */
#ifndef MQTT_BASE_WEBSOCKET_H
#define MQTT_BASE_WEBSOCKET_H

#include <stdbool.h>
#include <stdint.h>

#include "base_status.h"

/** @brief The largest header a frame this server sends can have. */
#define MQTT_WEBSOCKET_HEADER_MAX 10U
/** @brief The opcode of a frame of binary data. */
#define MQTT_WEBSOCKET_OPCODE_BINARY 2U
/** @brief The opcode of a frame that continues the last one. */
#define MQTT_WEBSOCKET_OPCODE_CONTINUATION 0U
/** @brief The opcode of a closing frame. */
#define MQTT_WEBSOCKET_OPCODE_CLOSE 8U
/** @brief The opcode of a ping. */
#define MQTT_WEBSOCKET_OPCODE_PING 9U
/** @brief The opcode of a pong. */
#define MQTT_WEBSOCKET_OPCODE_PONG 10U
/**
 * @brief The version of the WebSocket Protocol spoken here, which an opening
 * has to ask for and a refusal names: RFC 6455 sections 4.2.1 and 4.2.2.
 */
#define MQTT_WEBSOCKET_VERSION "13"

/** @brief A frame header that has been read. */
typedef struct {
    /** @brief The opcode. */
    uint8_t opcode;
    /** @brief true if this is the last frame of its message. */
    bool final;
    /**
     * @brief true if the frame's payload is masked, as every frame from a
     * Client must be.
     */
    bool masked;
    /** @brief The mask. */
    uint8_t mask[4];
    /** @brief Number of bytes in the header. */
    uint32_t size;
    /** @brief Number of bytes in the payload. */
    uint64_t payload_length;
} mqtt_websocket_frame;

/**
 * @brief Work out the reply to the opening handshake of a WebSocket connection.
 *
 * The request has to be what RFC 6455 section 4.2.1 describes: a GET of
 * HTTP/1.1 or later, with a Host, an Upgrade to websocket, a Connection with
 * Upgrade, version 13 and one key of 16 bytes in base64. It also has to offer
 * the subprotocol of MQTT. Any Request-URI is taken.
 *
 * @param[in] request The bytes of the HTTP request.
 * @param[in] length Number of bytes in request.
 * @param[out] reply Where to put the reply.
 * @param[in] capacity Number of bytes reply can hold.
 * @param[out] reply_length Number of bytes in the reply, on MQTT_OK.
 * @param[out] used Number of bytes the request took, on MQTT_OK.
 * @return MQTT_OK, MQTT_INCOMPLETE if the request has not all arrived, or
 * MQTT_MALFORMED_PACKET.
 */
mqtt_status mqtt_websocket_accept(const uint8_t *request, uint32_t length,
                                  uint8_t *reply, uint32_t capacity,
                                  uint32_t *reply_length, uint32_t *used);

/**
 * @brief Read a frame header.
 *
 * Only the reserved bits and the encoding of the length are checked. The
 * caller checks the rest of RFC 6455 sections 5.1 to 5.5: that a Client's
 * frames are masked, that the opcode is one it takes, that fragments come in
 * order, and that a control frame is short and whole.
 *
 * @param[in] data The bytes.
 * @param[in] length Number of bytes in data.
 * @param[out] frame The result, on MQTT_OK.
 * @return MQTT_OK, MQTT_INCOMPLETE, or MQTT_MALFORMED_PACKET.
 */
mqtt_status mqtt_websocket_frame_read(const uint8_t *data, uint32_t length,
                                      mqtt_websocket_frame *frame);

/**
 * @brief Write the header of an unmasked frame, as a server's frames must be.
 * @param[out] header Where to put it, with room for MQTT_WEBSOCKET_HEADER_MAX
 * bytes.
 * @param[in] opcode The opcode.
 * @param[in] payload_length Number of bytes in the payload.
 * @return Number of bytes in the header.
 */
uint32_t mqtt_websocket_frame_write(uint8_t *header, uint8_t opcode,
                                    uint64_t payload_length);

/**
 * @brief Remove the mask from bytes of a frame's payload.
 * @param[in,out] data The bytes.
 * @param[in] length Number of bytes.
 * @param[in] mask The frame's mask.
 * @param[in,out] position Where in the payload these bytes start. It moves on
 * by length.
 */
void mqtt_websocket_unmask(uint8_t *data, uint32_t length, const uint8_t *mask,
                           uint32_t *position);

#endif /* MQTT_BASE_WEBSOCKET_H */
