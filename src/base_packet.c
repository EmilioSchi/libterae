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
 * @file base_packet.c
 * @brief Decoding the MQTT Control Packets.
 *
 * Fields are read in the order they appear, and each packet must use up its
 * Remaining Length exactly. A reader remembers its first failure, so most
 * fields need no check of their own.
 *
 * [V31] MQTT V3.1 Protocol Specification.
 * @li Cited: 3.1, the note that Remaining Length takes precedence over the User
 * Name and Password flags.
 * @li Source: public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015.
 * @li Cited: 2.2, 3.1, 3.2, 3.3, 3.4-3.7, 3.8-3.11, 3.14.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 2.1, 2.4, 3.1-3.15, p.22-92.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */

#include <stddef.h>

#include "base_connect.h"
#include "base_fixed.h"
#include "base_flags.h"
#include "base_packet.h"
#include "base_qos.h"
#include "base_remaining.h"
#include "base_topic_check.h"
#include "base_type.h"
#include "base_utf8_check.h"
#include "v311_connack.h"
#include "v311_connect.h"
#include "v311_flags.h"
#include "v311_protocol.h"
#include "v311_rc.h"
#include "v311_remaining.h"
#include "v311_suback.h"
#include "v311_subscribe.h"
#include "v31_connect.h"
#include "v31_protocol.h"
#include "v31_remaining.h"
#include "v31_subscribe.h"
#include "v5_connack.h"
#include "v5_prop.h"
#include "v5_protocol.h"
#include "v5_rc.h"
#include "v5_remaining.h"
#include "v5_subscribe.h"
#include "v5_type.h"

/** @brief The Reason Codes one packet type may carry. */
typedef struct {
    /** @brief The Reason Codes, or NULL if the packet has none. */
    const uint8_t *codes;
    /** @brief Number of Reason Codes. */
    uint32_t count;
} packet_reason_codes;

/** @brief A status, and the Reason Code that reports it. */
typedef struct {
    /** @brief The status. */
    mqtt_status status;
    /** @brief The Reason Code. */
    uint8_t code;
} packet_reason_pair;

/**
 * @brief Read a string and check its characters.
 * @param[in,out] reader The reader, which fails if the string is not valid.
 * @param[in] version The protocol in use. MQTT 3.1 can use all of Unicode in a
 * string.
 * @return The string.
 */
static mqtt_utf8 packet_read_string(mqtt_reader *reader, mqtt_version version)
{
    /** @brief The string. */
    const mqtt_utf8 text = mqtt_read_utf8(reader);

    if (reader->status == MQTT_OK) {
        /** @brief true if the characters are allowed. */
        const bool valid =
            (version == MQTT_VERSION_31) ?
                mqtt_utf8_is_well_formed(text.data, (uint32_t)text.length) :
                mqtt_utf8_is_valid(text.data, (uint32_t)text.length);

        if (!valid) {
            reader->status = MQTT_MALFORMED_PACKET;
        }
    }
    return text;
}

/**
 * @brief Read a Packet Identifier, which may not be zero.
 * @param[in,out] reader The reader, which fails if it is zero.
 * @return The Packet Identifier.
 */
static uint16_t packet_read_identifier(mqtt_reader *reader)
{
    /** @brief The Packet Identifier. */
    const uint16_t identifier = mqtt_read_two_byte_integer(reader);

    /* It reads correctly, and breaks a rule: [MQTT-2.2.1-3]. */
    if ((reader->status == MQTT_OK) && (identifier == 0U)) {
        reader->status = MQTT_PROTOCOL_ERROR;
    }
    return identifier;
}

/**
 * @brief Check that a packet's body has been used up.
 * @param[in] reader The reader.
 * @return The reader's status, or MQTT_MALFORMED_PACKET if bytes are left over.
 */
static mqtt_status packet_finish(const mqtt_reader *reader)
{
    /** @brief The outcome. */
    mqtt_status status = reader->status;

    if ((status == MQTT_OK) && (mqtt_reader_left(reader) != 0U)) {
        status = MQTT_MALFORMED_PACKET;
    }
    return status;
}

/**
 * @brief Read and identify the Protocol Name and its version.
 * @param[in,out] reader The reader.
 * @param[out] packet Its version is set if the two fields name a supported
 * version.
 * @return MQTT_OK, MQTT_MALFORMED_PACKET, or MQTT_UNSUPPORTED_PROTOCOL_VERSION.
 */
static mqtt_status packet_read_protocol(mqtt_reader *reader,
                                        mqtt_connect *packet)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief Protocol Name. */
    const mqtt_utf8 name = mqtt_read_utf8(reader);
    /** @brief Protocol Version, or Protocol Level. */
    const uint32_t number = (uint32_t)mqtt_read_byte(reader);

    if (reader->status != MQTT_OK) {
        status = MQTT_MALFORMED_PACKET;
    } else if (mqtt_utf8_equal_text(name.data, (uint32_t)name.length,
                                    MQTT31_PROTOCOL_NAME) &&
               ((uint32_t)name.length == MQTT31_PROTOCOL_NAME_LENGTH) &&
               (number == MQTT31_PROTOCOL_VERSION)) {
        packet->version = MQTT_VERSION_31;
    } else if (mqtt_utf8_equal_text(name.data, (uint32_t)name.length,
                                    MQTT311_PROTOCOL_NAME) &&
               ((uint32_t)name.length == MQTT311_PROTOCOL_NAME_LENGTH) &&
               (number == MQTT311_PROTOCOL_LEVEL)) {
        packet->version = MQTT_VERSION_311;
    } else if (mqtt_utf8_equal_text(name.data, (uint32_t)name.length,
                                    MQTT5_PROTOCOL_NAME) &&
               ((uint32_t)name.length == MQTT5_PROTOCOL_NAME_LENGTH) &&
               (number == MQTT5_PROTOCOL_VERSION)) {
        packet->version = MQTT_VERSION_5;
    } else {
        status = MQTT_UNSUPPORTED_PROTOCOL_VERSION;
    }
    return status;
}

/**
 * @brief Read and check the Connect Flags.
 * @param[in,out] reader The reader.
 * @param[in,out] packet Its version is read and its flag fields are set.
 * @return MQTT_OK, MQTT_MALFORMED_PACKET, or MQTT_PROTOCOL_ERROR.
 */
static mqtt_status packet_read_connect_flags(mqtt_reader *reader,
                                             mqtt_connect *packet)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief Connect Flags. */
    const uint32_t flags = (uint32_t)mqtt_read_byte(reader);
    /**
     * @brief true for the versions that say what a receiver does with bad
     * flags.
     */
    const bool strict = (packet->version != MQTT_VERSION_31);

    packet->user_name_flag = (flags & MQTT_USER_NAME_FLAG) != 0U;
    packet->password_flag = (flags & MQTT_PASSWORD_FLAG) != 0U;
    packet->will_retain = (flags & MQTT_WILL_RETAIN) != 0U;
    packet->will_qos =
        (uint8_t)((flags & MQTT_WILL_QOS_MASK) >> MQTT_WILL_QOS_POS);
    packet->will_flag = (flags & MQTT_WILL_FLAG) != 0U;
    packet->clean_start = (flags & MQTT311_CLEAN_SESSION) != 0U;
    if (reader->status != MQTT_OK) {
        status = MQTT_MALFORMED_PACKET;
    } else if (strict && ((flags & MQTT_CONNECT_RESERVED) != 0U)) {
        status = MQTT_MALFORMED_PACKET;
    } else if ((packet->will_qos == MQTT_QOS_RESERVED) &&
               (strict || packet->will_flag)) {
        status = MQTT_MALFORMED_PACKET;
    } else if (strict && (!packet->will_flag) &&
               (packet->will_retain || (packet->will_qos != 0U))) {
        /* It reads correctly, and breaks a rule: [MQTT-3.1.2-11],
         * [MQTT-3.1.2-13]. */
        status = MQTT_PROTOCOL_ERROR;
    } else if ((packet->version != MQTT_VERSION_5) && packet->password_flag &&
               (!packet->user_name_flag)) {
        status = MQTT_MALFORMED_PACKET;
    } else {
        /* Nothing else in the flags can be wrong. */
    }
    /* MQTT 3.1 disregards these two without a Will Flag, whatever they hold. */
    if (!packet->will_flag) {
        packet->will_qos = 0U;
        packet->will_retain = false;
    }
    return status;
}

/**
 * @brief Read the fields of a CONNECT Payload.
 * @param[in,out] reader The reader.
 * @param[in,out] packet The packet.
 * @return MQTT_OK, or the reason to refuse the packet.
 */
static mqtt_status packet_read_connect_payload(mqtt_reader *reader,
                                               mqtt_connect *packet)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    packet->client_identifier = packet_read_string(reader, packet->version);
    if (packet->will_flag) {
        if (packet->version == MQTT_VERSION_5) {
            status = mqtt5_properties_decode(reader, MQTT5_PROPERTIES_WILL,
                                             &packet->will_properties);
        }
        if (status == MQTT_OK) {
            packet->will_topic = mqtt_read_utf8(reader);
            packet->will_payload = mqtt_read_binary(reader);
            status = reader->status;
        }
        if ((status == MQTT_OK) && (packet->will_topic.length == 0U)) {
            /* [MQTT-4.7.3-1] */
            status = MQTT_PROTOCOL_ERROR;
        }
        if (status == MQTT_OK) {
            status =
                mqtt_topic_name_check(&packet->will_topic, packet->version);
        }
        if ((status == MQTT_OK) && (packet->version == MQTT_VERSION_31)) {
            /* It goes out without its length, so MQTT 3.1 keeps it to 7-bit
             * ASCII. */
            /** @brief Index of the byte being looked at. */
            uint32_t index = 0U;

            while ((status == MQTT_OK) &&
                   (index < packet->will_payload.length)) {
                if ((uint32_t)packet->will_payload.data[index] >
                    MQTT31_WILL_MESSAGE_ASCII_MAX) {
                    status = MQTT_MALFORMED_PACKET;
                }
                index += 1U;
            }
        }
    }
    /* In MQTT 3.1 the Remaining Length takes precedence over these two
     * flags. */
    if ((status == MQTT_OK) && packet->user_name_flag) {
        if ((packet->version == MQTT_VERSION_31) &&
            (mqtt_reader_left(reader) == 0U)) {
            packet->user_name_flag = false;
        } else {
            packet->user_name = packet_read_string(reader, packet->version);
        }
    }
    if ((status == MQTT_OK) && packet->password_flag) {
        if ((packet->version == MQTT_VERSION_31) &&
            (mqtt_reader_left(reader) == 0U)) {
            packet->password_flag = false;
        } else {
            packet->password = mqtt_read_binary(reader);
            /* A password is binary from MQTT 3.1.1 on, and a string before
             * that. */
            if ((packet->version == MQTT_VERSION_31) &&
                (reader->status == MQTT_OK) &&
                (!mqtt_utf8_is_well_formed(packet->password.data,
                                           packet->password.length))) {
                status = MQTT_MALFORMED_PACKET;
            }
        }
    }
    if (status == MQTT_OK) {
        /* MQTT 3.1 disregards what the flags don't ask for, and a server has to
         * allow for this. */
        status = (packet->version == MQTT_VERSION_31) ? reader->status :
                                                        packet_finish(reader);
    }
    return status;
}

mqtt_status mqtt_fixed_header_decode(const uint8_t *data, uint32_t length,
                                     mqtt_fixed_header *header)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_INCOMPLETE;

    if ((header == NULL) || ((data == NULL) && (length > 0U))) {
        status = MQTT_INVALID_ARGUMENT;
    } else if (length > 0U) {
        /** @brief Number of bytes of the Remaining Length. */
        uint32_t used = 0U;

        header->type = (uint8_t)((uint32_t)data[0] >> MQTT_TYPE_POS);
        header->flags = (uint8_t)((uint32_t)data[0] & MQTT_FLAGS_MASK);
        status = mqtt_remaining_length_decode(&data[1], length - 1U,
                                              &header->remaining_length, &used,
                                              &header->shortest);
        if (status == MQTT_OK) {
            header->size = used + 1U;
        }
    } else {
        /* No bytes have arrived yet. */
    }
    return status;
}

/**
 * @brief Do what mqtt_fixed_header_check does, for arguments known to be valid.
 * @param[in] header The Fixed Header. A CONNECT packet is checked for the
 * version it asks for.
 * @param[in] version The protocol in use.
 * @return MQTT_OK or MQTT_MALFORMED_PACKET.
 */
static mqtt_status packet_fixed_header_check_body(
    const mqtt_fixed_header *header, mqtt_version version)
{
    /** @brief The flags this type must carry. */
    uint32_t expected = MQTT311_RESERVED_FLAGS;
    /** @brief The bits that are checked. */
    uint32_t mask = MQTT_FLAGS_MASK;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    /* [MQTT-1.5.5-1] is a rule of MQTT 5.0 only. */
    if ((version == MQTT_VERSION_5) && (!header->shortest)) {
        status = MQTT_MALFORMED_PACKET;
    }
    switch ((uint32_t)header->type) {
    case MQTT_PUBLISH:
        mask = 0U;
        break;
    case MQTT_PUBREL:
        expected = MQTT311_PUBREL_FLAGS;
        break;
    case MQTT_SUBSCRIBE:
        expected = MQTT311_SUBSCRIBE_FLAGS;
        break;
    case MQTT_UNSUBSCRIBE:
        expected = MQTT311_UNSUBSCRIBE_FLAGS;
        break;
    case MQTT_CONNECT:
    case MQTT_CONNACK:
    case MQTT_PUBACK:
    case MQTT_PUBREC:
    case MQTT_PUBCOMP:
    case MQTT_SUBACK:
    case MQTT_UNSUBACK:
    case MQTT_PINGREQ:
    case MQTT_PINGRESP:
    case MQTT_DISCONNECT:
        break;
    case MQTT5_AUTH:
        if (version != MQTT_VERSION_5) {
            status = MQTT_MALFORMED_PACKET;
        }
        break;
    default:
        status = MQTT_MALFORMED_PACKET;
        break;
    }
    /* MQTT 3.1 gives no rule for a receiver of other flags, and a retry may set
     * DUP. */
    if ((version != MQTT_VERSION_31) &&
        (((uint32_t)header->flags & mask) != expected)) {
        status = MQTT_MALFORMED_PACKET;
    }
    return status;
}

mqtt_status mqtt_fixed_header_check(const mqtt_fixed_header *header,
                                    mqtt_version version)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if (header != NULL) {
        answer = packet_fixed_header_check_body(header, version);
    }
    return answer;
}

/**
 * @brief Do what mqtt_connect_clear does, for arguments known to be valid.
 * @param[out] packet The packet.
 */
static void packet_connect_clear_body(mqtt_connect *packet)
{
    /** @brief A string with nothing in it. */
    const mqtt_utf8 none = {0U, NULL};
    /** @brief Binary Data with nothing in it. */
    const mqtt_binary no_data = {0U, NULL};

    packet->version = MQTT_VERSION_311;
    packet->clean_start = false;
    packet->will_flag = false;
    packet->will_retain = false;
    packet->user_name_flag = false;
    packet->password_flag = false;
    packet->will_qos = 0U;
    packet->keep_alive = 0U;
    packet->client_identifier = none;
    packet->will_topic = none;
    packet->will_payload = no_data;
    packet->user_name = none;
    packet->password = no_data;
    mqtt5_properties_clear(&packet->properties);
    mqtt5_properties_clear(&packet->will_properties);
}

void mqtt_connect_clear(mqtt_connect *packet)
{
    if (packet != NULL) {
        packet_connect_clear_body(packet);
    }
}

/**
 * @brief Do what mqtt_connect_decode does, for arguments known to be valid.
 * @param[in] body The bytes after the Fixed Header.
 * @param[in] length Remaining Length.
 * @param[out] packet The result. Its version is set as soon as the version is
 * known.
 * @return MQTT_OK, or the reason to refuse it.
 */
static mqtt_status packet_connect_decode_body(const uint8_t *body,
                                              uint32_t length,
                                              mqtt_connect *packet)
{
    /** @brief Reader over the body. */
    mqtt_reader reader = {NULL, 0U, 0U, MQTT_OK};
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    mqtt_connect_clear(packet);
    mqtt_reader_init(&reader, body, length);
    status = packet_read_protocol(&reader, packet);
    if (status == MQTT_OK) {
        status = packet_read_connect_flags(&reader, packet);
    }
    if (status == MQTT_OK) {
        packet->keep_alive = mqtt_read_two_byte_integer(&reader);
        if ((packet->version == MQTT_VERSION_5) && (reader.status == MQTT_OK)) {
            status = mqtt5_properties_decode(&reader, MQTT_CONNECT,
                                             &packet->properties);
        }
    }
    if (status == MQTT_OK) {
        status = packet_read_connect_payload(&reader, packet);
    }
    return status;
}

mqtt_status mqtt_connect_decode(const uint8_t *body, uint32_t length,
                                mqtt_connect *packet)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if ((packet != NULL) && ((body != NULL) || (length == 0U))) {
        answer = packet_connect_decode_body(body, length, packet);
    }
    return answer;
}

/**
 * @brief Do what mqtt_connack_decode does, for arguments known to be valid.
 * @param[in] body The bytes after the Fixed Header.
 * @param[in] length Remaining Length.
 * @param[in] version The protocol in use.
 * @param[out] packet The result.
 * @return MQTT_OK, MQTT_MALFORMED_PACKET, or MQTT_PROTOCOL_ERROR.
 */
static mqtt_status packet_connack_decode_body(const uint8_t *body,
                                              uint32_t length,
                                              mqtt_version version,
                                              mqtt_connack *packet)
{
    /** @brief Reader over the body. */
    mqtt_reader reader = {NULL, 0U, 0U, MQTT_OK};
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    mqtt_connack_clear(packet);
    mqtt_reader_init(&reader, body, length);
    {
        /** @brief Connect Acknowledge Flags. */
        const uint32_t flags = (uint32_t)mqtt_read_byte(&reader);

        packet->reason_code = mqtt_read_byte(&reader);
        if (version == MQTT_VERSION_31) {
            /* This byte is not used in MQTT 3.1, and so there is no Session
             * Present. */
            if (length != MQTT31_CONNACK_REMAINING_LENGTH) {
                status = MQTT_MALFORMED_PACKET;
            }
        } else if ((flags & MQTT311_CONNACK_RESERVED) != 0U) {
            status = MQTT_MALFORMED_PACKET;
        } else {
            packet->session_present = (flags & MQTT311_SESSION_PRESENT) != 0U;
            if ((packet->session_present) && (reader.status == MQTT_OK) &&
                (packet->reason_code != 0U)) {
                status = MQTT_PROTOCOL_ERROR;
            }
        }
    }
    if ((status == MQTT_OK) && (reader.status == MQTT_OK)) {
        if (version == MQTT_VERSION_5) {
            status =
                mqtt5_reason_code_is_valid(MQTT_CONNACK, packet->reason_code) ?
                    MQTT_OK :
                    MQTT_PROTOCOL_ERROR;
            if (status == MQTT_OK) {
                status = mqtt5_properties_decode(&reader, MQTT_CONNACK,
                                                 &packet->properties);
            }
        }
    }
    if (status == MQTT_OK) {
        status = packet_finish(&reader);
    }
    return status;
}

mqtt_status mqtt_connack_decode(const uint8_t *body, uint32_t length,
                                mqtt_version version, mqtt_connack *packet)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if ((packet != NULL) && ((body != NULL) || (length == 0U))) {
        answer = packet_connack_decode_body(body, length, version, packet);
    }
    return answer;
}

mqtt_status mqtt_publish_decode(const mqtt_fixed_header *header,
                                const uint8_t *body, mqtt_version version,
                                mqtt_publish *packet)
{
    return (header != NULL) ?
               mqtt_publish_decode_head(header, body, header->remaining_length,
                                        version, packet) :
               MQTT_INVALID_ARGUMENT;
}

/**
 * @brief Do what mqtt_publish_decode_head does, for arguments known to be
 * valid.
 * @param[in] header The Fixed Header.
 * @param[in] head The bytes after the Fixed Header up to the Payload.
 * @param[in] head_length Number of bytes in head.
 * @param[in] version The protocol in use.
 * @param[out] packet The result. Its Payload is what head holds after the
 * Properties, which is nothing if head_length was right.
 * @return MQTT_OK, or the reason to refuse it.
 */
static mqtt_status packet_publish_decode_head_body(
    const mqtt_fixed_header *header, const uint8_t *head, uint32_t head_length,
    mqtt_version version, mqtt_publish *packet)
{
    /** @brief Reader over the body. */
    mqtt_reader reader = {NULL, 0U, 0U, MQTT_OK};
    /** @brief The flags bits. */
    const uint32_t flags = (uint32_t)header->flags;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    mqtt_reader_init(&reader, head, head_length);
    packet->dup = (flags & MQTT_DUP) != 0U;
    packet->retain = (flags & MQTT_RETAIN) != 0U;
    packet->qos = (uint8_t)((flags & MQTT_QOS_MASK) >> MQTT_QOS_POS);
    packet->packet_identifier = 0U;
    mqtt5_properties_clear(&packet->properties);
    if (packet->qos == MQTT_QOS_RESERVED) {
        status = MQTT_MALFORMED_PACKET;
    } else if ((version != MQTT_VERSION_31) && packet->dup &&
               (packet->qos == MQTT_QOS_0)) {
        status = MQTT_PROTOCOL_ERROR;
    } else {
        packet->topic_name = mqtt_read_utf8(&reader);
        if (packet->qos != MQTT_QOS_0) {
            packet->packet_identifier = packet_read_identifier(&reader);
        }
        status = reader.status;
    }
    if ((status == MQTT_OK) && (version == MQTT_VERSION_5)) {
        status =
            mqtt5_properties_decode(&reader, MQTT_PUBLISH, &packet->properties);
    }
    /* [MQTT-3.3.2-14] holds in both directions. */
    if ((status == MQTT_OK) &&
        mqtt5_properties_has(&packet->properties, MQTT5_PROP_RESPONSE_TOPIC) &&
        (mqtt_topic_name_check(&packet->properties.response_topic, version) !=
         MQTT_OK)) {
        status = MQTT_PROTOCOL_ERROR;
    }
    if (status == MQTT_OK) {
        if (packet->topic_name.length > 0U) {
            status = mqtt_topic_name_check(&packet->topic_name, version);
        } else if (mqtt5_properties_has(&packet->properties,
                                        MQTT5_PROP_TOPIC_ALIAS)) {
            /* The Topic Alias stands for the Topic Name. */
        } else {
            status = (version == MQTT_VERSION_5) ? MQTT_PROTOCOL_ERROR :
                                                   MQTT_TOPIC_NAME_INVALID;
        }
    }
    if (status == MQTT_OK) {
        packet->payload.data = &reader.data[reader.position];
        packet->payload.length = mqtt_reader_left(&reader);
    }
    return status;
}

mqtt_status mqtt_publish_decode_head(const mqtt_fixed_header *header,
                                     const uint8_t *head, uint32_t head_length,
                                     mqtt_version version, mqtt_publish *packet)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if ((header != NULL) && (packet != NULL) && ((head != NULL) ||
        (head_length == 0U))) {
        answer = packet_publish_decode_head_body(header, head, head_length,
                                                 version, packet);
    }
    return answer;
}

/**
 * @brief Do what mqtt_publish_response_decode does, for arguments known to be
 * valid.
 * @param[in] header The Fixed Header.
 * @param[in] body The bytes after the Fixed Header.
 * @param[in] version The protocol in use.
 * @param[out] packet The result.
 * @return MQTT_OK, MQTT_MALFORMED_PACKET, or MQTT_PROTOCOL_ERROR.
 */
static mqtt_status packet_publish_response_decode_body(
    const mqtt_fixed_header *header, const uint8_t *body, mqtt_version version,
    mqtt_publish_response *packet)
{
    /** @brief Reader over the body. */
    mqtt_reader reader = {NULL, 0U, 0U, MQTT_OK};
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    mqtt_reader_init(&reader, body, header->remaining_length);
    packet->type = header->type;
    packet->dup = ((uint32_t)header->flags & MQTT_DUP) != 0U;
    packet->reason_code = MQTT5_RC_SUCCESS;
    mqtt5_properties_clear(&packet->properties);
    packet->packet_identifier = packet_read_identifier(&reader);
    if (reader.status != MQTT_OK) {
        status = reader.status;
    } else if (version == MQTT_VERSION_5) {
        if (mqtt_reader_left(&reader) > 0U) {
            packet->reason_code = mqtt_read_byte(&reader);
            if (!mqtt5_reason_code_is_valid((uint32_t)header->type,
                                            packet->reason_code)) {
                status = MQTT_PROTOCOL_ERROR;
            }
        }
        if ((status == MQTT_OK) && (mqtt_reader_left(&reader) > 0U)) {
            status = mqtt5_properties_decode(&reader, (uint32_t)header->type,
                                             &packet->properties);
        }
    } else {
        /* There is nothing else in these packets before MQTT 5.0. */
    }
    if (status == MQTT_OK) {
        status = packet_finish(&reader);
    }
    return status;
}

mqtt_status mqtt_publish_response_decode(const mqtt_fixed_header *header,
                                         const uint8_t *body,
                                         mqtt_version version,
                                         mqtt_publish_response *packet)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if ((header != NULL) && (packet != NULL) && ((body != NULL) ||
        (header->remaining_length == 0U))) {
        answer = packet_publish_response_decode_body(header, body, version,
                                                     packet);
    }
    return answer;
}

/**
 * @brief Check the Subscription Options of one entry of a SUBSCRIBE Payload.
 * @param[in] options The byte.
 * @param[in] filter The Topic Filter of the entry.
 * @param[in] version The protocol in use.
 * @return MQTT_OK, MQTT_MALFORMED_PACKET, or MQTT_PROTOCOL_ERROR.
 */
static mqtt_status packet_check_options(uint32_t options,
                                        const mqtt_utf8 *filter,
                                        mqtt_version version)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (filter->length == 0U) {
        /* [MQTT-4.7.3-1], and a Client that does this can't be expected to read
         * a failure code. */
        status = MQTT_MALFORMED_PACKET;
    } else if (version == MQTT_VERSION_5) {
        if ((options & MQTT5_SUBSCRIPTION_OPTIONS_RESERVED) != 0U) {
            status = MQTT_MALFORMED_PACKET;
        } else if ((options & MQTT5_MAXIMUM_QOS_MASK) == MQTT_QOS_RESERVED) {
            /* Section 3.8.3.1 */
            status = MQTT_PROTOCOL_ERROR;
        } else if (((options & MQTT5_RETAIN_HANDLING_MASK) >>
                    MQTT5_RETAIN_HANDLING_POS) ==
                   MQTT5_RETAIN_HANDLING_RESERVED) {
            status = MQTT_PROTOCOL_ERROR;
        } else if ((options & MQTT5_NO_LOCAL) != 0U) {
            /** @brief Unused parts of the split. */
            mqtt_utf8 part = {0U, NULL};
            /** @brief Unused parts of the split. */
            mqtt_utf8 rest = {0U, NULL};

            if (mqtt_topic_share_split(filter, &part, &rest)) {
                status = MQTT_PROTOCOL_ERROR;
            }
        } else {
            /* The other bits have no rule to check here. */
        }
    } else if (version == MQTT_VERSION_311) {
        if (((options & MQTT311_REQUESTED_QOS_RESERVED) != 0U) ||
            ((options & MQTT311_REQUESTED_QOS_MASK) == MQTT_QOS_RESERVED)) {
            status = MQTT_MALFORMED_PACKET;
        }
    } else {
        if ((options & MQTT31_REQUESTED_QOS_MASK) == MQTT_QOS_RESERVED) {
            status = MQTT_MALFORMED_PACKET;
        }
    }
    return status;
}

/**
 * @brief Do what mqtt_subscribe_decode does, for arguments known to be valid.
 * @param[in] body The bytes after the Fixed Header.
 * @param[in] length Remaining Length.
 * @param[in] version The protocol in use.
 * @param[out] packet The result.
 * @return MQTT_OK, MQTT_MALFORMED_PACKET, or MQTT_PROTOCOL_ERROR.
 */
static mqtt_status packet_subscribe_decode_body(const uint8_t *body,
                                                uint32_t length,
                                                mqtt_version version,
                                                mqtt_subscribe *packet)
{
    /** @brief Reader over the body. */
    mqtt_reader reader = {NULL, 0U, 0U, MQTT_OK};
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    mqtt_reader_init(&reader, body, length);
    mqtt5_properties_clear(&packet->properties);
    packet->packet_identifier = packet_read_identifier(&reader);
    if ((reader.status == MQTT_OK) && (version == MQTT_VERSION_5)) {
        status = mqtt5_properties_decode(&reader, MQTT_SUBSCRIBE,
                                         &packet->properties);
    }
    if ((status == MQTT_OK) && (reader.status != MQTT_OK)) {
        status = reader.status;
    }
    if (status == MQTT_OK) {
        packet->payload.data = &reader.data[reader.position];
        packet->payload.length = mqtt_reader_left(&reader);
        if (packet->payload.length == 0U) {
            status = (version == MQTT_VERSION_5) ? MQTT_PROTOCOL_ERROR :
                                                   MQTT_MALFORMED_PACKET;
        }
    }
    while ((status == MQTT_OK) && (mqtt_reader_left(&reader) > 0U)) {
        /** @brief Topic Filter. */
        const mqtt_utf8 filter = packet_read_string(&reader, version);
        /** @brief Subscription Options. */
        const uint32_t options = (uint32_t)mqtt_read_byte(&reader);

        status = reader.status;
        if (status == MQTT_OK) {
            status = packet_check_options(options, &filter, version);
        }
    }
    return status;
}

mqtt_status mqtt_subscribe_decode(const uint8_t *body, uint32_t length,
                                  mqtt_version version, mqtt_subscribe *packet)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if ((packet != NULL) && ((body != NULL) || (length == 0U))) {
        answer = packet_subscribe_decode_body(body, length, version, packet);
    }
    return answer;
}

bool mqtt_subscribe_next(mqtt_reader *cursor, mqtt_utf8 *topic_filter,
                         uint8_t *options)
{
    /** @brief The answer. */
    bool found = false;

    if ((cursor != NULL) && (topic_filter != NULL) && (options != NULL) &&
        (cursor->status == MQTT_OK) && (mqtt_reader_left(cursor) > 0U)) {
        *topic_filter = mqtt_read_utf8(cursor);
        *options = mqtt_read_byte(cursor);
        found = (cursor->status == MQTT_OK);
    }
    return found;
}

/**
 * @brief Do what mqtt_unsubscribe_decode does, for arguments known to be valid.
 * @param[in] body The bytes after the Fixed Header.
 * @param[in] length Remaining Length.
 * @param[in] version The protocol in use.
 * @param[out] packet The result.
 * @return MQTT_OK, MQTT_MALFORMED_PACKET, or MQTT_PROTOCOL_ERROR.
 */
static mqtt_status packet_unsubscribe_decode_body(const uint8_t *body,
                                                  uint32_t length,
                                                  mqtt_version version,
                                                  mqtt_subscribe *packet)
{
    /** @brief Reader over the body. */
    mqtt_reader reader = {NULL, 0U, 0U, MQTT_OK};
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    mqtt_reader_init(&reader, body, length);
    mqtt5_properties_clear(&packet->properties);
    packet->packet_identifier = packet_read_identifier(&reader);
    if ((reader.status == MQTT_OK) && (version == MQTT_VERSION_5)) {
        status = mqtt5_properties_decode(&reader, MQTT_UNSUBSCRIBE,
                                         &packet->properties);
    }
    if ((status == MQTT_OK) && (reader.status != MQTT_OK)) {
        status = reader.status;
    }
    if (status == MQTT_OK) {
        packet->payload.data = &reader.data[reader.position];
        packet->payload.length = mqtt_reader_left(&reader);
        if (packet->payload.length == 0U) {
            status = (version == MQTT_VERSION_5) ? MQTT_PROTOCOL_ERROR :
                                                   MQTT_MALFORMED_PACKET;
        }
    }
    while ((status == MQTT_OK) && (mqtt_reader_left(&reader) > 0U)) {
        /** @brief Topic Filter. */
        const mqtt_utf8 filter = packet_read_string(&reader, version);

        status = reader.status;
        if ((status == MQTT_OK) && (filter.length == 0U)) {
            /* [MQTT-4.7.3-1] */
            status = MQTT_MALFORMED_PACKET;
        }
    }
    return status;
}

mqtt_status mqtt_unsubscribe_decode(const uint8_t *body, uint32_t length,
                                    mqtt_version version,
                                    mqtt_subscribe *packet)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if ((packet != NULL) && ((body != NULL) || (length == 0U))) {
        answer = packet_unsubscribe_decode_body(body, length, version, packet);
    }
    return answer;
}

bool mqtt_unsubscribe_next(mqtt_reader *cursor, mqtt_utf8 *topic_filter)
{
    /** @brief The answer. */
    bool found = false;

    if ((cursor != NULL) && (topic_filter != NULL) &&
        (cursor->status == MQTT_OK) && (mqtt_reader_left(cursor) > 0U)) {
        *topic_filter = mqtt_read_utf8(cursor);
        found = (cursor->status == MQTT_OK);
    }
    return found;
}

/**
 * @brief Do what mqtt_suback_decode does, for arguments known to be valid.
 * @param[in] body The bytes after the Fixed Header.
 * @param[in] length Remaining Length.
 * @param[in] version The protocol in use.
 * @param[out] packet The result.
 * @return MQTT_OK, MQTT_MALFORMED_PACKET, or MQTT_PROTOCOL_ERROR.
 */
static mqtt_status packet_suback_decode_body(const uint8_t *body,
                                             uint32_t length,
                                             mqtt_version version,
                                             mqtt_subscribe_response *packet)
{
    /** @brief Reader over the body. */
    mqtt_reader reader = {NULL, 0U, 0U, MQTT_OK};
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    mqtt_reader_init(&reader, body, length);
    mqtt5_properties_clear(&packet->properties);
    packet->packet_identifier = packet_read_identifier(&reader);
    if ((reader.status == MQTT_OK) && (version == MQTT_VERSION_5)) {
        status =
            mqtt5_properties_decode(&reader, MQTT_SUBACK, &packet->properties);
    }
    if ((status == MQTT_OK) && (reader.status != MQTT_OK)) {
        status = reader.status;
    }
    if (status == MQTT_OK) {
        /** @brief Index of the Reason Code being checked. */
        uint32_t index = 0U;

        packet->reason_codes.data = &reader.data[reader.position];
        packet->reason_codes.length = mqtt_reader_left(&reader);
        if (packet->reason_codes.length == 0U) {
            status = MQTT_MALFORMED_PACKET;
        }
        while ((status == MQTT_OK) && (index < packet->reason_codes.length)) {
            /** @brief The Reason Code. */
            const uint8_t code = packet->reason_codes.data[index];

            if (version == MQTT_VERSION_5) {
                status = mqtt5_reason_code_is_valid(MQTT_SUBACK, code) ?
                             MQTT_OK : MQTT_PROTOCOL_ERROR;
            } else if (version == MQTT_VERSION_31) {
                /* The top six bits of a granted QoS aren't used, so nothing
                 * here can be wrong. */
            } else if (((uint32_t)code > MQTT311_RC_MAXIMUM_QOS_2) &&
                       ((uint32_t)code != MQTT311_RC_FAILURE)) {
                status = MQTT_PROTOCOL_ERROR;
            } else {
                /* A Return Code this version allows. */
            }
            index += 1U;
        }
    }
    return status;
}

mqtt_status mqtt_suback_decode(const uint8_t *body, uint32_t length,
                               mqtt_version version,
                               mqtt_subscribe_response *packet)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if ((packet != NULL) && ((body != NULL) || (length == 0U))) {
        answer = packet_suback_decode_body(body, length, version, packet);
    }
    return answer;
}

/**
 * @brief Do what mqtt_unsuback_decode does, for arguments known to be valid.
 * @param[in] body The bytes after the Fixed Header.
 * @param[in] length Remaining Length.
 * @param[in] version The protocol in use.
 * @param[out] packet The result.
 * @return MQTT_OK, MQTT_MALFORMED_PACKET, or MQTT_PROTOCOL_ERROR.
 */
static mqtt_status packet_unsuback_decode_body(const uint8_t *body,
                                               uint32_t length,
                                               mqtt_version version,
                                               mqtt_subscribe_response *packet)
{
    /** @brief Reader over the body. */
    mqtt_reader reader = {NULL, 0U, 0U, MQTT_OK};
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    mqtt_reader_init(&reader, body, length);
    mqtt5_properties_clear(&packet->properties);
    packet->reason_codes.data = NULL;
    packet->reason_codes.length = 0U;
    packet->packet_identifier = packet_read_identifier(&reader);
    if (reader.status != MQTT_OK) {
        status = reader.status;
    } else if (version == MQTT_VERSION_5) {
        status = mqtt5_properties_decode(&reader, MQTT_UNSUBACK,
                                         &packet->properties);
        if (status == MQTT_OK) {
            /** @brief Index of the Reason Code being checked. */
            uint32_t index = 0U;

            packet->reason_codes.data = &reader.data[reader.position];
            packet->reason_codes.length = mqtt_reader_left(&reader);
            if (packet->reason_codes.length == 0U) {
                status = MQTT_MALFORMED_PACKET;
            }
            while ((status == MQTT_OK) &&
                   (index < packet->reason_codes.length)) {
                status = mqtt5_reason_code_is_valid(
                             MQTT_UNSUBACK, packet->reason_codes.data[index]) ?
                             MQTT_OK :
                             MQTT_PROTOCOL_ERROR;
                index += 1U;
            }
        }
    } else {
        status = packet_finish(&reader);
    }
    return status;
}

mqtt_status mqtt_unsuback_decode(const uint8_t *body, uint32_t length,
                                 mqtt_version version,
                                 mqtt_subscribe_response *packet)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if ((packet != NULL) && ((body != NULL) || (length == 0U))) {
        answer = packet_unsuback_decode_body(body, length, version, packet);
    }
    return answer;
}

/**
 * @brief Do what mqtt_disconnect_decode does, for arguments known to be valid.
 * @param[in] body The bytes after the Fixed Header.
 * @param[in] length Remaining Length.
 * @param[in] version The protocol in use.
 * @param[out] packet The result.
 * @return MQTT_OK, MQTT_MALFORMED_PACKET, or MQTT_PROTOCOL_ERROR.
 */
static mqtt_status packet_disconnect_decode_body(const uint8_t *body,
                                                 uint32_t length,
                                                 mqtt_version version,
                                                 mqtt_reason *packet)
{
    /** @brief Reader over the body. */
    mqtt_reader reader = {NULL, 0U, 0U, MQTT_OK};
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    mqtt_reader_init(&reader, body, length);
    packet->reason_code = MQTT5_RC_NORMAL_DISCONNECTION;
    mqtt5_properties_clear(&packet->properties);
    if (version == MQTT_VERSION_5) {
        if (length > 0U) {
            packet->reason_code = mqtt_read_byte(&reader);
            if (!mqtt5_reason_code_is_valid(MQTT_DISCONNECT,
                                            packet->reason_code)) {
                status = MQTT_PROTOCOL_ERROR;
            }
        }
        if ((status == MQTT_OK) && (mqtt_reader_left(&reader) > 0U)) {
            status = mqtt5_properties_decode(&reader, MQTT_DISCONNECT,
                                             &packet->properties);
        }
    }
    if (status == MQTT_OK) {
        status = packet_finish(&reader);
    }
    return status;
}

mqtt_status mqtt_disconnect_decode(const uint8_t *body, uint32_t length,
                                   mqtt_version version, mqtt_reason *packet)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if ((packet != NULL) && ((body != NULL) || (length == 0U))) {
        answer = packet_disconnect_decode_body(body, length, version, packet);
    }
    return answer;
}

/**
 * @brief Do what mqtt_auth_decode does, for arguments known to be valid.
 * @param[in] body The bytes after the Fixed Header.
 * @param[in] length Remaining Length.
 * @param[out] packet The result.
 * @return MQTT_OK, MQTT_MALFORMED_PACKET, or MQTT_PROTOCOL_ERROR.
 */
static mqtt_status packet_auth_decode_body(const uint8_t *body, uint32_t length,
                                           mqtt_reason *packet)
{
    /** @brief Reader over the body. */
    mqtt_reader reader = {NULL, 0U, 0U, MQTT_OK};
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    mqtt_reader_init(&reader, body, length);
    packet->reason_code = MQTT5_RC_SUCCESS;
    mqtt5_properties_clear(&packet->properties);
    if (length > 0U) {
        packet->reason_code = mqtt_read_byte(&reader);
        if (!mqtt5_reason_code_is_valid(MQTT5_AUTH, packet->reason_code)) {
            status = MQTT_PROTOCOL_ERROR;
        }
        if ((status == MQTT_OK) && (mqtt_reader_left(&reader) > 0U)) {
            status = mqtt5_properties_decode(&reader, MQTT5_AUTH,
                                             &packet->properties);
        }
        if (status == MQTT_OK) {
            status = packet_finish(&reader);
        }
    }
    return status;
}

mqtt_status mqtt_auth_decode(const uint8_t *body, uint32_t length,
                             mqtt_reason *packet)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if ((packet != NULL) && ((body != NULL) || (length == 0U))) {
        answer = packet_auth_decode_body(body, length, packet);
    }
    return answer;
}

bool mqtt5_reason_code_is_valid(uint32_t type, uint8_t reason_code)
{
    /** @brief The Reason Code tables of the PUBACK and PUBREC packets. */
    static const uint8_t publish_codes[] = {0x00U, 0x10U, 0x80U, 0x83U, 0x87U,
                                            0x90U, 0x91U, 0x97U, 0x99U};
    /** @brief The Reason Code tables of the PUBREL and PUBCOMP packets. */
    static const uint8_t release_codes[] = {0x00U, 0x92U};
    /** @brief The Reason Code table of the SUBACK packet. */
    static const uint8_t suback_codes[] = {0x00U, 0x01U, 0x02U, 0x80U,
                                           0x83U, 0x87U, 0x8FU, 0x91U,
                                           0x97U, 0x9EU, 0xA1U, 0xA2U};
    /** @brief The Reason Code table of the UNSUBACK packet. */
    static const uint8_t unsuback_codes[] = {0x00U, 0x11U, 0x80U, 0x83U,
                                             0x87U, 0x8FU, 0x91U};
    /** @brief The Reason Code table of the CONNACK packet. */
    static const uint8_t connack_codes[] = {
        0x00U, 0x80U, 0x81U, 0x82U, 0x83U, 0x84U, 0x85U, 0x86U,
        0x87U, 0x88U, 0x89U, 0x8AU, 0x8CU, 0x90U, 0x95U, 0x97U,
        0x99U, 0x9AU, 0x9BU, 0x9CU, 0x9DU, 0x9FU};
    /** @brief The Reason Code table of the DISCONNECT packet. */
    static const uint8_t disconnect_codes[] = {
        0x00U, 0x04U, 0x80U, 0x81U, 0x82U, 0x83U, 0x87U, 0x89U, 0x8BU, 0x8DU,
        0x8EU, 0x8FU, 0x90U, 0x93U, 0x94U, 0x95U, 0x96U, 0x97U, 0x98U, 0x99U,
        0x9AU, 0x9BU, 0x9CU, 0x9DU, 0x9EU, 0x9FU, 0xA0U, 0xA1U, 0xA2U};
    /** @brief The Reason Code table of the AUTH packet. */
    static const uint8_t auth_codes[] = {0x00U, 0x18U, 0x19U};
    /**
     * @brief The Reason Code table of each packet type, by type. No other
     * packet has a Reason Code.
     */
    static const packet_reason_codes tables[MQTT5_AUTH + 1U] = {
        [MQTT_CONNACK] = {connack_codes, (uint32_t)sizeof(connack_codes)},
        [MQTT_PUBACK] = {publish_codes, (uint32_t)sizeof(publish_codes)},
        [MQTT_PUBREC] = {publish_codes, (uint32_t)sizeof(publish_codes)},
        [MQTT_PUBREL] = {release_codes, (uint32_t)sizeof(release_codes)},
        [MQTT_PUBCOMP] = {release_codes, (uint32_t)sizeof(release_codes)},
        [MQTT_SUBACK] = {suback_codes, (uint32_t)sizeof(suback_codes)},
        [MQTT_UNSUBACK] = {unsuback_codes, (uint32_t)sizeof(unsuback_codes)},
        [MQTT_DISCONNECT] = {disconnect_codes,
                             (uint32_t)sizeof(disconnect_codes)},
        [MQTT5_AUTH] = {auth_codes, (uint32_t)sizeof(auth_codes)},
    };
    /** @brief The table to search. */
    const uint8_t *table = NULL;
    /** @brief Number of entries in the table. */
    uint32_t count = 0U;
    /** @brief The answer. */
    bool found = false;
    /** @brief Index of the entry being looked at. */
    uint32_t index = 0U;

    if (type <= MQTT5_AUTH) {
        table = tables[type].codes;
        count = tables[type].count;
    }
    while ((!found) && (index < count)) {
        found = (table[index] == reason_code);
        index += 1U;
    }
    return found;
}

uint8_t mqtt5_reason_code(mqtt_status status)
{
    /**
     * @brief Each status that has a Reason Code. The rest are about the
     * library's state, and have no closer Reason Code.
     */
    static const packet_reason_pair pairs[] = {
        {MQTT_OK, MQTT5_RC_SUCCESS},
        {MQTT_MALFORMED_PACKET, MQTT5_RC_MALFORMED_PACKET},
        {MQTT_PROTOCOL_ERROR, MQTT5_RC_PROTOCOL_ERROR},
        {MQTT_UNSUPPORTED_PROTOCOL_VERSION,
         MQTT5_RC_UNSUPPORTED_PROTOCOL_VERSION},
        {MQTT_CLIENT_IDENTIFIER_NOT_VALID,
         MQTT5_RC_CLIENT_IDENTIFIER_NOT_VALID},
        {MQTT_BAD_USER_NAME_OR_PASSWORD, MQTT5_RC_BAD_USER_NAME_OR_PASSWORD},
        {MQTT_NOT_AUTHORIZED, MQTT5_RC_NOT_AUTHORIZED},
        {MQTT_BAD_AUTHENTICATION_METHOD, MQTT5_RC_BAD_AUTHENTICATION_METHOD},
        {MQTT_TOPIC_NAME_INVALID, MQTT5_RC_TOPIC_NAME_INVALID},
        {MQTT_TOPIC_FILTER_INVALID, MQTT5_RC_TOPIC_FILTER_INVALID},
        {MQTT_TOPIC_ALIAS_INVALID, MQTT5_RC_TOPIC_ALIAS_INVALID},
        {MQTT_PACKET_TOO_LARGE, MQTT5_RC_PACKET_TOO_LARGE},
        {MQTT_RECEIVE_MAXIMUM_EXCEEDED, MQTT5_RC_RECEIVE_MAXIMUM_EXCEEDED},
        {MQTT_PACKET_IDENTIFIER_IN_USE, MQTT5_RC_PACKET_IDENTIFIER_IN_USE},
        {MQTT_PACKET_IDENTIFIER_NOT_FOUND,
         MQTT5_RC_PACKET_IDENTIFIER_NOT_FOUND},
        {MQTT_PAYLOAD_FORMAT_INVALID, MQTT5_RC_PAYLOAD_FORMAT_INVALID},
        {MQTT_QUOTA_EXCEEDED, MQTT5_RC_QUOTA_EXCEEDED},
        {MQTT_RING_FULL, MQTT5_RC_QUOTA_EXCEEDED}};
    /** @brief Number of pairs. */
    const uint32_t count = (uint32_t)(sizeof(pairs) / sizeof(pairs[0]));
    /** @brief The Reason Code. */
    uint32_t code = MQTT5_RC_UNSPECIFIED_ERROR;
    /** @brief true once the status is found. */
    bool found = false;
    /** @brief Index of the pair being looked at. */
    uint32_t index = 0U;

    while ((!found) && (index < count)) {
        if (pairs[index].status == status) {
            code = pairs[index].code;
            found = true;
        }
        index += 1U;
    }
    return (uint8_t)code;
}

/**
 * @brief Pick the flags of a packet other than PUBLISH. They are the same in
 * all three versions: those of PUBREL, SUBSCRIBE and UNSUBSCRIBE are 0x02, and
 * the others' are zero.
 * @param[in] type The MQTT Control Packet type.
 * @return The first byte of the packet.
 */
static uint8_t packet_first_byte(uint32_t type)
{
    /** @brief The flags. */
    uint32_t flags = MQTT311_RESERVED_FLAGS;

    if (type == MQTT_PUBREL) {
        flags = MQTT311_PUBREL_FLAGS;
    } else if (type == MQTT_SUBSCRIBE) {
        flags = MQTT311_SUBSCRIBE_FLAGS;
    } else if (type == MQTT_UNSUBSCRIBE) {
        flags = MQTT311_UNSUBSCRIBE_FLAGS;
    } else {
        /* The rest have reserved flags of zero. */
    }
    return (uint8_t)((type << MQTT_TYPE_POS) | flags);
}

void mqtt_fixed_header_encode(mqtt_writer *writer, uint8_t first_byte,
                              uint32_t remaining_length)
{
    mqtt_write_byte(writer, first_byte);
    mqtt_write_variable_byte_integer(writer, remaining_length);
}

/**
 * @brief Write the body of a CONNECT packet.
 * @param[in,out] writer The writer.
 * @param[in] packet The packet.
 */
static void packet_connect_body(mqtt_writer *writer, const mqtt_connect *packet)
{
    /** @brief The Connect Flags. */
    uint32_t flags = (uint32_t)packet->will_qos << MQTT_WILL_QOS_POS;

    if (packet->version == MQTT_VERSION_31) {
        /** @brief The Protocol Name. */
        const mqtt_utf8 name = mqtt_utf8_from_text(MQTT31_PROTOCOL_NAME);

        mqtt_write_utf8(writer, &name);
        mqtt_write_byte(writer, (uint8_t)MQTT31_PROTOCOL_VERSION);
    } else {
        /** @brief The Protocol Name, the same in both of these versions. */
        const mqtt_utf8 name = mqtt_utf8_from_text(MQTT311_PROTOCOL_NAME);

        mqtt_write_utf8(writer, &name);
        mqtt_write_byte(writer, (uint8_t)packet->version);
    }
    if (packet->clean_start) {
        flags |= MQTT311_CLEAN_SESSION;
    }
    if (packet->will_flag) {
        flags |= MQTT_WILL_FLAG;
    }
    if (packet->will_retain) {
        flags |= MQTT_WILL_RETAIN;
    }
    if (packet->password_flag) {
        flags |= MQTT_PASSWORD_FLAG;
    }
    if (packet->user_name_flag) {
        flags |= MQTT_USER_NAME_FLAG;
    }
    mqtt_write_byte(writer, (uint8_t)flags);
    mqtt_write_two_byte_integer(writer, packet->keep_alive);
    if (packet->version == MQTT_VERSION_5) {
        mqtt5_properties_encode(writer, &packet->properties);
    }
    mqtt_write_utf8(writer, &packet->client_identifier);
    if (packet->will_flag) {
        if (packet->version == MQTT_VERSION_5) {
            mqtt5_properties_encode(writer, &packet->will_properties);
        }
        mqtt_write_utf8(writer, &packet->will_topic);
        mqtt_write_binary(writer, &packet->will_payload);
    }
    if (packet->user_name_flag) {
        mqtt_write_utf8(writer, &packet->user_name);
    }
    if (packet->password_flag) {
        mqtt_write_binary(writer, &packet->password);
    }
}

/**
 * @brief Do what mqtt_connect_encode does, for arguments known to be valid.
 * @param[in,out] writer The writer.
 * @param[in] packet The packet.
 */
static void packet_connect_encode_body(mqtt_writer *writer,
                                       const mqtt_connect *packet)
{
    /** @brief A writer that only counts. */
    mqtt_writer counter = {NULL, 0U, 0U, MQTT_OK};

    mqtt_writer_init(&counter, NULL, 0U);
    packet_connect_body(&counter, packet);
    mqtt_fixed_header_encode(writer,
                             packet_first_byte(MQTT_CONNECT),
                             counter.position);
    packet_connect_body(writer, packet);
}

void mqtt_connect_encode(mqtt_writer *writer, const mqtt_connect *packet)
{
    if (packet != NULL) {
        packet_connect_encode_body(writer, packet);
    } else {
        mqtt_writer_refuse(writer);
    }
}

void mqtt_connack_encode(mqtt_writer *writer, mqtt_version version,
                         const mqtt_connack *packet)
{
    if (packet != NULL) {
        if (version == MQTT_VERSION_5) {
            /** @brief A writer that only counts. */
            mqtt_writer counter = {NULL, 0U, 0U, MQTT_OK};

            mqtt_writer_init(&counter, NULL, 0U);
            mqtt5_properties_encode(&counter, &packet->properties);
            mqtt_fixed_header_encode(writer,
                                     packet_first_byte(MQTT_CONNACK),
                                     counter.position + 2U);
        } else {
            mqtt_fixed_header_encode(writer,
                                     packet_first_byte(MQTT_CONNACK),
                                     MQTT311_CONNACK_REMAINING_LENGTH);
        }
        mqtt_write_byte(
            writer,
            (uint8_t)(packet->session_present ? MQTT311_SESSION_PRESENT : 0U));
        mqtt_write_byte(writer, packet->reason_code);
        if (version == MQTT_VERSION_5) {
            mqtt5_properties_encode(writer, &packet->properties);
        }
    } else {
        mqtt_writer_refuse(writer);
    }
}

/**
 * @brief Write the body of a PUBLISH packet.
 * @param[in,out] writer The writer.
 * @param[in] version The protocol in use.
 * @param[in] packet The packet.
 */
static void packet_publish_body(mqtt_writer *writer, mqtt_version version,
                                const mqtt_publish *packet)
{
    mqtt_write_utf8(writer, &packet->topic_name);
    if (packet->qos != MQTT_QOS_0) {
        mqtt_write_two_byte_integer(writer, packet->packet_identifier);
    }
    if (version == MQTT_VERSION_5) {
        mqtt5_properties_encode(writer, &packet->properties);
    }
    mqtt_write_bytes(writer, packet->payload.data, packet->payload.length);
}

/**
 * @brief Do what mqtt_publish_encode does, for arguments known to be valid.
 * @param[in,out] writer The writer.
 * @param[in] version The protocol in use.
 * @param[in] packet The packet.
 */
static void packet_publish_encode_body(mqtt_writer *writer,
                                       mqtt_version version,
                                       const mqtt_publish *packet)
{
    /** @brief A writer that only counts. */
    mqtt_writer counter = {NULL, 0U, 0U, MQTT_OK};
    /** @brief The first byte. */
    uint32_t first = ((uint32_t)MQTT_PUBLISH << MQTT_TYPE_POS) |
                     ((uint32_t)packet->qos << MQTT_QOS_POS);

    if (packet->dup) {
        first |= MQTT_DUP;
    }
    if (packet->retain) {
        first |= MQTT_RETAIN;
    }
    mqtt_writer_init(&counter, NULL, 0U);
    packet_publish_body(&counter, version, packet);
    mqtt_fixed_header_encode(writer, (uint8_t)first, counter.position);
    packet_publish_body(writer, version, packet);
}

void mqtt_publish_encode(mqtt_writer *writer, mqtt_version version,
                         const mqtt_publish *packet)
{
    if (packet != NULL) {
        packet_publish_encode_body(writer, version, packet);
    } else {
        mqtt_writer_refuse(writer);
    }
}

/**
 * @brief Do what mqtt_publish_size does, for arguments known to be valid.
 * @param[in] version The protocol in use.
 * @param[in] packet The packet.
 * @return The number of bytes in the whole packet, or zero if it could never be
 * sent.
 */
static uint32_t packet_publish_size_body(mqtt_version version,
                                         const mqtt_publish *packet)
{
    /** @brief A writer that only counts. */
    mqtt_writer counter = {NULL, 0U, 0U, MQTT_OK};

    mqtt_writer_init(&counter, NULL, 0U);
    packet_publish_body(&counter, version, packet);
    return (counter.position > MQTT_REMAINING_LENGTH_MAX) ?
               0U :
               (1U + mqtt_variable_byte_integer_size(counter.position) +
                counter.position);
}

uint32_t mqtt_publish_size(mqtt_version version, const mqtt_publish *packet)
{
    /** @brief The answer. */
    uint32_t answer = 0U;

    if (packet != NULL) {
        answer = packet_publish_size_body(version, packet);
    }
    return answer;
}

/**
 * @brief Do what mqtt_publish_response_encode does, for arguments known to be
 * valid.
 * @param[in,out] writer The writer.
 * @param[in] version The protocol in use.
 * @param[in] packet The packet.
 */
static void packet_publish_response_encode_body(
    mqtt_writer *writer, mqtt_version version,
    const mqtt_publish_response *packet)
{
    /** @brief DUP, which only MQTT 3.1 has on these packets. */
    const uint32_t dup =
        (packet->dup && (version == MQTT_VERSION_31)) ? MQTT_DUP : 0U;
    /** @brief The first byte. */
    const uint8_t first =
        (uint8_t)((uint32_t)packet_first_byte((uint32_t)packet->type) |
                  dup);

    if ((version == MQTT_VERSION_5) &&
        (!mqtt5_properties_empty(&packet->properties))) {
        /** @brief A writer that only counts. */
        mqtt_writer counter = {NULL, 0U, 0U, MQTT_OK};

        mqtt_writer_init(&counter, NULL, 0U);
        mqtt5_properties_encode(&counter, &packet->properties);
        mqtt_fixed_header_encode(writer, first, counter.position + 3U);
        mqtt_write_two_byte_integer(writer, packet->packet_identifier);
        mqtt_write_byte(writer, packet->reason_code);
        mqtt5_properties_encode(writer, &packet->properties);
    } else if ((version == MQTT_VERSION_5) &&
               (packet->reason_code != MQTT5_RC_SUCCESS)) {
        mqtt_fixed_header_encode(writer, first, 3U);
        mqtt_write_two_byte_integer(writer, packet->packet_identifier);
        mqtt_write_byte(writer, packet->reason_code);
    } else {
        mqtt_fixed_header_encode(writer, first,
                                 MQTT311_PUBACK_REMAINING_LENGTH);
        mqtt_write_two_byte_integer(writer, packet->packet_identifier);
    }
}

void mqtt_publish_response_encode(mqtt_writer *writer, mqtt_version version,
                                  const mqtt_publish_response *packet)
{
    if (packet != NULL) {
        packet_publish_response_encode_body(writer, version, packet);
    } else {
        mqtt_writer_refuse(writer);
    }
}

/**
 * @brief Write the Packet Identifier, Properties and Payload shared by several
 * packets.
 * @param[in,out] writer The writer.
 * @param[in] first The first byte of the packet.
 * @param[in] version The protocol in use.
 * @param[in] identifier Packet Identifier.
 * @param[in] properties Properties.
 * @param[in] payload Payload.
 */
static void packet_identified_body(mqtt_writer *writer, uint8_t first,
                                   mqtt_version version, uint16_t identifier,
                                   const mqtt5_properties *properties,
                                   const mqtt_payload *payload)
{
    /** @brief A writer that only counts. */
    mqtt_writer counter = {NULL, 0U, 0U, MQTT_OK};

    mqtt_writer_init(&counter, NULL, 0U);
    if (version == MQTT_VERSION_5) {
        mqtt5_properties_encode(&counter, properties);
    }
    mqtt_fixed_header_encode(writer, first,
                             counter.position + 2U + payload->length);
    mqtt_write_two_byte_integer(writer, identifier);
    if (version == MQTT_VERSION_5) {
        mqtt5_properties_encode(writer, properties);
    }
    mqtt_write_bytes(writer, payload->data, payload->length);
}

void mqtt_subscribe_encode(mqtt_writer *writer, mqtt_version version,
                           const mqtt_subscribe *packet)
{
    if (packet != NULL) {
        packet_identified_body(writer, packet_first_byte(MQTT_SUBSCRIBE),
                               version, packet->packet_identifier,
                               &packet->properties, &packet->payload);
    } else {
        mqtt_writer_refuse(writer);
    }
}

void mqtt_unsubscribe_encode(mqtt_writer *writer, mqtt_version version,
                             const mqtt_subscribe *packet)
{
    if (packet != NULL) {
        packet_identified_body(writer, packet_first_byte(MQTT_UNSUBSCRIBE),
                               version, packet->packet_identifier,
                               &packet->properties, &packet->payload);
    } else {
        mqtt_writer_refuse(writer);
    }
}

void mqtt_suback_encode(mqtt_writer *writer, mqtt_version version,
                        const mqtt_subscribe_response *packet)
{
    if (packet != NULL) {
        packet_identified_body(writer, packet_first_byte(MQTT_SUBACK),
                               version, packet->packet_identifier,
                               &packet->properties, &packet->reason_codes);
    } else {
        mqtt_writer_refuse(writer);
    }
}

void mqtt_unsuback_encode(mqtt_writer *writer, mqtt_version version,
                          const mqtt_subscribe_response *packet)
{
    if (packet != NULL) {
        packet_identified_body(writer, packet_first_byte(MQTT_UNSUBACK),
                               version, packet->packet_identifier,
                               &packet->properties, &packet->reason_codes);
    } else {
        mqtt_writer_refuse(writer);
    }
}

void mqtt_pingreq_encode(mqtt_writer *writer)
{
    mqtt_fixed_header_encode(writer,
                             packet_first_byte(MQTT_PINGREQ),
                             MQTT311_PINGREQ_REMAINING_LENGTH);
}

void mqtt_pingresp_encode(mqtt_writer *writer)
{
    mqtt_fixed_header_encode(writer,
                             packet_first_byte(MQTT_PINGRESP),
                             MQTT311_PINGRESP_REMAINING_LENGTH);
}

/**
 * @brief Do what mqtt_disconnect_encode does, for arguments known to be valid.
 * @param[in,out] writer The writer.
 * @param[in] version The protocol in use.
 * @param[in] packet The packet.
 */
static void packet_disconnect_encode_body(mqtt_writer *writer,
                                          mqtt_version version,
                                          const mqtt_reason *packet)
{
    /** @brief The first byte. */
    const uint8_t first = packet_first_byte(MQTT_DISCONNECT);

    if ((version == MQTT_VERSION_5) &&
        (!mqtt5_properties_empty(&packet->properties))) {
        /** @brief A writer that only counts. */
        mqtt_writer counter = {NULL, 0U, 0U, MQTT_OK};

        mqtt_writer_init(&counter, NULL, 0U);
        mqtt5_properties_encode(&counter, &packet->properties);
        mqtt_fixed_header_encode(writer, first, counter.position + 1U);
        mqtt_write_byte(writer, packet->reason_code);
        mqtt5_properties_encode(writer, &packet->properties);
    } else if ((version == MQTT_VERSION_5) &&
               (packet->reason_code != MQTT5_RC_NORMAL_DISCONNECTION)) {
        mqtt_fixed_header_encode(writer, first,
                                 MQTT5_DISCONNECT_REMAINING_LENGTH_REASON_CODE);
        mqtt_write_byte(writer, packet->reason_code);
    } else {
        mqtt_fixed_header_encode(writer, first,
                                 MQTT311_DISCONNECT_REMAINING_LENGTH);
    }
}

void mqtt_disconnect_encode(mqtt_writer *writer, mqtt_version version,
                            const mqtt_reason *packet)
{
    if (packet != NULL) {
        packet_disconnect_encode_body(writer, version, packet);
    } else {
        mqtt_writer_refuse(writer);
    }
}

/**
 * @brief Do what mqtt_auth_encode does, for arguments known to be valid.
 * @param[in,out] writer The writer.
 * @param[in] packet The packet.
 */
static void packet_auth_encode_body(mqtt_writer *writer,
                                    const mqtt_reason *packet)
{
    /** @brief The first byte. */
    const uint8_t first = packet_first_byte(MQTT5_AUTH);

    if (mqtt5_properties_empty(&packet->properties) &&
        (packet->reason_code == MQTT5_RC_SUCCESS)) {
        mqtt_fixed_header_encode(writer, first,
                                 MQTT5_AUTH_REMAINING_LENGTH_MIN);
    } else {
        /** @brief A writer that only counts. */
        mqtt_writer counter = {NULL, 0U, 0U, MQTT_OK};

        mqtt_writer_init(&counter, NULL, 0U);
        mqtt5_properties_encode(&counter, &packet->properties);
        mqtt_fixed_header_encode(writer, first, counter.position + 1U);
        mqtt_write_byte(writer, packet->reason_code);
        mqtt5_properties_encode(writer, &packet->properties);
    }
}

void mqtt_auth_encode(mqtt_writer *writer, const mqtt_reason *packet)
{
    if (packet != NULL) {
        packet_auth_encode_body(writer, packet);
    } else {
        mqtt_writer_refuse(writer);
    }
}

void mqtt_connack_clear(mqtt_connack *packet)
{
    if (packet != NULL) {
        packet->session_present = false;
        packet->reason_code = 0U;
        mqtt5_properties_clear(&packet->properties);
    }
}

void mqtt_publish_response_clear(mqtt_publish_response *packet)
{
    if (packet != NULL) {
        packet->type = 0U;
        packet->dup = false;
        packet->reason_code = 0U;
        packet->packet_identifier = 0U;
        mqtt5_properties_clear(&packet->properties);
    }
}

void mqtt_subscribe_clear(mqtt_subscribe *packet)
{
    if (packet != NULL) {
        packet->packet_identifier = 0U;
        packet->payload.data = NULL;
        packet->payload.length = 0U;
        mqtt5_properties_clear(&packet->properties);
    }
}

void mqtt_subscribe_response_clear(mqtt_subscribe_response *packet)
{
    if (packet != NULL) {
        packet->packet_identifier = 0U;
        packet->reason_codes.data = NULL;
        packet->reason_codes.length = 0U;
        mqtt5_properties_clear(&packet->properties);
    }
}

void mqtt_reason_clear(mqtt_reason *packet)
{
    if (packet != NULL) {
        packet->reason_code = 0U;
        mqtt5_properties_clear(&packet->properties);
    }
}
