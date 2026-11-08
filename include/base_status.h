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
 * @file base_status.h
 * @brief Outcome of a library call.
 *
 * Failures that an MQTT 5.0 Reason Code can describe take the name of that
 * Reason Code, so a caller can report them with mqtt5_reason_code. The
 * specifications give no name for the others, which are about the state of this
 * library and not about a packet.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 2.4, Table 2-6, p.32-34.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */
#ifndef MQTT_BASE_STATUS_H
#define MQTT_BASE_STATUS_H

/** @brief Outcome of a library call. */
typedef enum {
    /** @brief The call did what it was asked to do. */
    MQTT_OK = 0,
    /** @brief The bytes so far are valid but do not yet hold a whole item. */
    MQTT_INCOMPLETE,
    /** @brief The bytes break the rules of the packet format. */
    MQTT_MALFORMED_PACKET,
    /**
     * @brief The packet is well-formed but breaks the rules of the protocol.
     */
    MQTT_PROTOCOL_ERROR,
    /**
     * @brief The CONNECT packet asks for a protocol this library does not
     * support.
     */
    MQTT_UNSUPPORTED_PROTOCOL_VERSION,
    /** @brief The Client Identifier is one the Server will not accept. */
    MQTT_CLIENT_IDENTIFIER_NOT_VALID,
    /** @brief The User Name or Password was refused. */
    MQTT_BAD_USER_NAME_OR_PASSWORD,
    /** @brief The operation was refused by authorization. */
    MQTT_NOT_AUTHORIZED,
    /** @brief The Authentication Method is not one the Server supports. */
    MQTT_BAD_AUTHENTICATION_METHOD,
    /** @brief The Topic Name is not valid. */
    MQTT_TOPIC_NAME_INVALID,
    /** @brief The Topic Filter is not valid. */
    MQTT_TOPIC_FILTER_INVALID,
    /** @brief The Topic Alias is not valid. */
    MQTT_TOPIC_ALIAS_INVALID,
    /** @brief The packet is larger than the limit that applies to it. */
    MQTT_PACKET_TOO_LARGE,
    /**
     * @brief More QoS 1 and QoS 2 messages are in flight than Receive Maximum
     * allows.
     */
    MQTT_RECEIVE_MAXIMUM_EXCEEDED,
    /** @brief The Packet Identifier is already in use. */
    MQTT_PACKET_IDENTIFIER_IN_USE,
    /** @brief The Packet Identifier is not in use. */
    MQTT_PACKET_IDENTIFIER_NOT_FOUND,
    /** @brief The Payload does not match the Payload Format Indicator. */
    MQTT_PAYLOAD_FORMAT_INVALID,
    /** @brief The destination buffer cannot hold the result. */
    MQTT_BUFFER_TOO_SMALL,
    /** @brief A fixed pool of the library is exhausted. */
    MQTT_QUOTA_EXCEEDED,
    /** @brief An argument is out of range. */
    MQTT_INVALID_ARGUMENT,
    /** @brief The call is not allowed in the current state. */
    MQTT_INVALID_STATE,
    /** @brief The item looked for does not exist. */
    MQTT_NOT_FOUND,
    /** @brief The network connection has ended, or could not be made. */
    MQTT_CONNECTION_CLOSED,
    /** @brief The TLS library reported a failure. */
    MQTT_TLS_ERROR,
    /**
     * @brief A ring may not take the blocks the bytes need, so none were
     * added.
     */
    MQTT_RING_FULL,
    /**
     * @brief Stored state is damaged, or doesn't fit, so nothing of it was
     * taken.
     */
    MQTT_STORE_DAMAGED
} mqtt_status;

#endif /* MQTT_BASE_STATUS_H */
