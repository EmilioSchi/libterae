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
 * @file base_packet.h
 * @brief The MQTT Control Packets: decoding and encoding, for all three
 * versions.
 *
 * A decoded packet is a set of views of the received bytes, so those bytes must
 * stay in place while it is used. Decoding checks everything the packet alone
 * can show: the rest needs a session.
 *
 * Encoding takes the same structures. Each encode function writes a whole
 * packet, Fixed Header and all, and leaves the writer's status at
 * MQTT_BUFFER_TOO_SMALL if it does not fit.
 *
 * [V31] MQTT V3.1 Protocol Specification.
 * @li Cited: 2, 3.
 * @li Source: public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015.
 * @li Cited: 2, 3.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 2, 3, p.22-92.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */
#ifndef MQTT_BASE_PACKET_H
#define MQTT_BASE_PACKET_H

#include <stdbool.h>
#include <stdint.h>

#include "base_binary_t.h"
#include "base_payload_t.h"
#include "base_status.h"
#include "base_utf8_t.h"
#include "base_version.h"
#include "base_wire.h"
#include "v5_properties.h"

/** @brief A decoded Fixed Header. */
typedef struct {
    /** @brief The MQTT Control Packet type. */
    uint8_t type;
    /** @brief The flags bits. */
    uint8_t flags;
    /** @brief Number of bytes in the Fixed Header. */
    uint32_t size;
    /** @brief Remaining Length. */
    uint32_t remaining_length;
    /** @brief true if the Remaining Length took the fewest bytes it can. */
    bool shortest;
} mqtt_fixed_header;

/** @brief A CONNECT packet. */
typedef struct {
    /** @brief The protocol asked for. */
    mqtt_version version;
    /** @brief Clean Start in MQTT 5.0, Clean Session before it. */
    bool clean_start;
    /** @brief Will Flag. */
    bool will_flag;
    /** @brief Will Retain. */
    bool will_retain;
    /** @brief User Name Flag. */
    bool user_name_flag;
    /** @brief Password Flag. */
    bool password_flag;
    /** @brief Will QoS. */
    uint8_t will_qos;
    /** @brief Keep Alive. */
    uint16_t keep_alive;
    /** @brief Client Identifier. */
    mqtt_utf8 client_identifier;
    /** @brief Will Topic. */
    mqtt_utf8 will_topic;
    /** @brief Will Payload. */
    mqtt_binary will_payload;
    /** @brief User Name. */
    mqtt_utf8 user_name;
    /** @brief Password. */
    mqtt_binary password;
    /** @brief CONNECT Properties. */
    mqtt5_properties properties;
    /** @brief Will Properties. */
    mqtt5_properties will_properties;
} mqtt_connect;

/** @brief A CONNACK packet. */
typedef struct {
    /** @brief Session Present. */
    bool session_present;
    /**
     * @brief Connect Reason Code, or the Connect Return Code before MQTT 5.0.
     */
    uint8_t reason_code;
    /** @brief CONNACK Properties. */
    mqtt5_properties properties;
} mqtt_connack;

/** @brief A PUBLISH packet. */
typedef struct {
    /** @brief DUP flag. */
    bool dup;
    /** @brief RETAIN flag. */
    bool retain;
    /** @brief QoS level. */
    uint8_t qos;
    /** @brief Packet Identifier, if the QoS level is above 0. */
    uint16_t packet_identifier;
    /** @brief Topic Name, which may be empty if a Topic Alias stands for it. */
    mqtt_utf8 topic_name;
    /** @brief Payload. */
    mqtt_payload payload;
    /** @brief PUBLISH Properties. */
    mqtt5_properties properties;
} mqtt_publish;

/** @brief A PUBACK, PUBREC, PUBREL or PUBCOMP packet. */
typedef struct {
    /** @brief The MQTT Control Packet type. */
    uint8_t type;
    /** @brief true if this is a resend. It is only written for MQTT 3.1. */
    bool dup;
    /** @brief Reason Code, which is success if the packet has none. */
    uint8_t reason_code;
    /** @brief Packet Identifier. */
    uint16_t packet_identifier;
    /** @brief Properties. */
    mqtt5_properties properties;
} mqtt_publish_response;

/** @brief A SUBSCRIBE packet. Its entries are read with mqtt_subscribe_next. */
typedef struct {
    /** @brief Packet Identifier. */
    uint16_t packet_identifier;
    /**
     * @brief The Payload, which has passed the checks in mqtt_subscribe_decode.
     */
    mqtt_payload payload;
    /** @brief SUBSCRIBE Properties. */
    mqtt5_properties properties;
} mqtt_subscribe;

/** @brief A SUBACK or UNSUBACK packet. */
typedef struct {
    /** @brief Packet Identifier. */
    uint16_t packet_identifier;
    /**
     * @brief The Reason Codes, one byte per filter. Empty for an UNSUBACK
     * before MQTT 5.0.
     */
    mqtt_payload reason_codes;
    /** @brief Properties. */
    mqtt5_properties properties;
} mqtt_subscribe_response;

/** @brief A DISCONNECT or AUTH packet. */
typedef struct {
    /** @brief Reason Code, which is success if the packet has none. */
    uint8_t reason_code;
    /** @brief Properties. */
    mqtt5_properties properties;
} mqtt_reason;

/**
 * @brief Set every field of a CONNECT packet to what it is when the packet
 * holds none of its optional parts. The memory may hold anything before, and
 * this is cheaper than setting it all to zero.
 * @param[out] packet The packet.
 */
void mqtt_connect_clear(mqtt_connect *packet);

/**
 * @brief Set every field of a CONNACK packet to zero, with no Properties.
 * @param[out] packet The packet.
 */
void mqtt_connack_clear(mqtt_connack *packet);

/**
 * @brief Set every field of a PUBACK, PUBREC, PUBREL or PUBCOMP packet to
 * zero, with no Properties.
 * @param[out] packet The packet.
 */
void mqtt_publish_response_clear(mqtt_publish_response *packet);

/**
 * @brief Set every field of a SUBSCRIBE or UNSUBSCRIBE packet to zero, with no
 * payload and no Properties.
 * @param[out] packet The packet.
 */
void mqtt_subscribe_clear(mqtt_subscribe *packet);

/**
 * @brief Set every field of a SUBACK or UNSUBACK packet to zero, with no
 * Reason Codes and no Properties.
 * @param[out] packet The packet.
 */
void mqtt_subscribe_response_clear(mqtt_subscribe_response *packet);

/**
 * @brief Set every field of a DISCONNECT or AUTH packet to zero, with no
 * Properties.
 * @param[out] packet The packet.
 */
void mqtt_reason_clear(mqtt_reason *packet);

/**
 * @brief Decode a Fixed Header from bytes that may be incomplete.
 * @param[in] data The bytes.
 * @param[in] length Number of bytes in data.
 * @param[out] header The result, on MQTT_OK. The type and flags are also set on
 * MQTT_INCOMPLETE if there was at least one byte, which is enough to refuse a
 * packet before the rest of it arrives.
 * @return MQTT_OK, MQTT_INCOMPLETE, or MQTT_MALFORMED_PACKET.
 */
mqtt_status mqtt_fixed_header_decode(const uint8_t *data, uint32_t length,
                                     mqtt_fixed_header *header);

/**
 * @brief Check a Fixed Header against the type's rules for flags.
 * @param[in] header The Fixed Header. A CONNECT packet is checked for the
 * version it asks for.
 * @param[in] version The protocol in use.
 * @return MQTT_OK or MQTT_MALFORMED_PACKET.
 */
mqtt_status mqtt_fixed_header_check(const mqtt_fixed_header *header,
                                    mqtt_version version);

/**
 * @brief Decode the body of a CONNECT packet.
 * @param[in] body The bytes after the Fixed Header.
 * @param[in] length Remaining Length.
 * @param[out] packet The result. Its version is set as soon as the version is
 * known.
 * @return MQTT_OK, or the reason to refuse it.
 */
mqtt_status mqtt_connect_decode(const uint8_t *body, uint32_t length,
                                mqtt_connect *packet);

/**
 * @brief Decode the body of a CONNACK packet.
 * @param[in] body The bytes after the Fixed Header.
 * @param[in] length Remaining Length.
 * @param[in] version The protocol in use.
 * @param[out] packet The result.
 * @return MQTT_OK, MQTT_MALFORMED_PACKET, or MQTT_PROTOCOL_ERROR.
 */
mqtt_status mqtt_connack_decode(const uint8_t *body, uint32_t length,
                                mqtt_version version, mqtt_connack *packet);

/**
 * @brief Decode the start of the body of a PUBLISH packet, whose Payload is
 * elsewhere: its Topic Name, Packet Identifier and Properties.
 * @param[in] header The Fixed Header.
 * @param[in] head The bytes after the Fixed Header up to the Payload.
 * @param[in] head_length Number of bytes in head.
 * @param[in] version The protocol in use.
 * @param[out] packet The result. Its Payload is what head holds after the
 * Properties, which is nothing if head_length was right.
 * @return MQTT_OK, or the reason to refuse it.
 */
mqtt_status mqtt_publish_decode_head(const mqtt_fixed_header *header,
                                     const uint8_t *head, uint32_t head_length,
                                     mqtt_version version,
                                     mqtt_publish *packet);

/**
 * @brief Decode the body of a PUBLISH packet.
 * @param[in] header The Fixed Header.
 * @param[in] body The bytes after the Fixed Header.
 * @param[in] version The protocol in use.
 * @param[out] packet The result.
 * @return MQTT_OK, or the reason to refuse it.
 */
mqtt_status mqtt_publish_decode(const mqtt_fixed_header *header,
                                const uint8_t *body, mqtt_version version,
                                mqtt_publish *packet);

/**
 * @brief Decode the body of a PUBACK, PUBREC, PUBREL or PUBCOMP packet.
 * @param[in] header The Fixed Header.
 * @param[in] body The bytes after the Fixed Header.
 * @param[in] version The protocol in use.
 * @param[out] packet The result.
 * @return MQTT_OK, MQTT_MALFORMED_PACKET, or MQTT_PROTOCOL_ERROR.
 */
mqtt_status mqtt_publish_response_decode(const mqtt_fixed_header *header,
                                         const uint8_t *body,
                                         mqtt_version version,
                                         mqtt_publish_response *packet);

/**
 * @brief Decode the body of a SUBSCRIBE packet and check all its entries.
 * @param[in] body The bytes after the Fixed Header.
 * @param[in] length Remaining Length.
 * @param[in] version The protocol in use.
 * @param[out] packet The result.
 * @return MQTT_OK, MQTT_MALFORMED_PACKET, or MQTT_PROTOCOL_ERROR.
 */
mqtt_status mqtt_subscribe_decode(const uint8_t *body, uint32_t length,
                                  mqtt_version version, mqtt_subscribe *packet);

/**
 * @brief Read the next entry of a checked SUBSCRIBE Payload.
 * @param[in,out] cursor Reader over the Payload, started with mqtt_reader_init.
 * @param[out] topic_filter The Topic Filter, which may still be refused by
 * mqtt_topic_filter_check.
 * @param[out] options The Subscription Options, or the Requested QoS before
 * MQTT 5.0.
 * @return true if there was one, false at the end.
 */
bool mqtt_subscribe_next(mqtt_reader *cursor, mqtt_utf8 *topic_filter,
                         uint8_t *options);

/**
 * @brief Decode the body of an UNSUBSCRIBE packet and check all its entries.
 * @param[in] body The bytes after the Fixed Header.
 * @param[in] length Remaining Length.
 * @param[in] version The protocol in use.
 * @param[out] packet The result.
 * @return MQTT_OK, MQTT_MALFORMED_PACKET, or MQTT_PROTOCOL_ERROR.
 */
mqtt_status mqtt_unsubscribe_decode(const uint8_t *body, uint32_t length,
                                    mqtt_version version,
                                    mqtt_subscribe *packet);

/**
 * @brief Read the next entry of a checked UNSUBSCRIBE Payload.
 * @param[in,out] cursor Reader over the Payload, started with mqtt_reader_init.
 * @param[out] topic_filter The Topic Filter.
 * @return true if there was one, false at the end.
 */
bool mqtt_unsubscribe_next(mqtt_reader *cursor, mqtt_utf8 *topic_filter);

/**
 * @brief Decode the body of a SUBACK packet.
 * @param[in] body The bytes after the Fixed Header.
 * @param[in] length Remaining Length.
 * @param[in] version The protocol in use.
 * @param[out] packet The result.
 * @return MQTT_OK, MQTT_MALFORMED_PACKET, or MQTT_PROTOCOL_ERROR.
 */
mqtt_status mqtt_suback_decode(const uint8_t *body, uint32_t length,
                               mqtt_version version,
                               mqtt_subscribe_response *packet);

/**
 * @brief Decode the body of an UNSUBACK packet.
 * @param[in] body The bytes after the Fixed Header.
 * @param[in] length Remaining Length.
 * @param[in] version The protocol in use.
 * @param[out] packet The result.
 * @return MQTT_OK, MQTT_MALFORMED_PACKET, or MQTT_PROTOCOL_ERROR.
 */
mqtt_status mqtt_unsuback_decode(const uint8_t *body, uint32_t length,
                                 mqtt_version version,
                                 mqtt_subscribe_response *packet);

/**
 * @brief Decode the body of a DISCONNECT packet.
 * @param[in] body The bytes after the Fixed Header.
 * @param[in] length Remaining Length.
 * @param[in] version The protocol in use.
 * @param[out] packet The result.
 * @return MQTT_OK, MQTT_MALFORMED_PACKET, or MQTT_PROTOCOL_ERROR.
 */
mqtt_status mqtt_disconnect_decode(const uint8_t *body, uint32_t length,
                                   mqtt_version version, mqtt_reason *packet);

/**
 * @brief Decode the body of an AUTH packet.
 * @param[in] body The bytes after the Fixed Header.
 * @param[in] length Remaining Length.
 * @param[out] packet The result.
 * @return MQTT_OK, MQTT_MALFORMED_PACKET, or MQTT_PROTOCOL_ERROR.
 */
mqtt_status mqtt_auth_decode(const uint8_t *body, uint32_t length,
                             mqtt_reason *packet);

/**
 * @brief Decide whether a Reason Code may be used in a packet.
 * @param[in] type The MQTT Control Packet type.
 * @param[in] reason_code The Reason Code.
 * @return true if the packet's table lists it.
 */
bool mqtt5_reason_code_is_valid(uint32_t type, uint8_t reason_code);

/**
 * @brief Find the Reason Code that reports a status.
 * @param[in] status A status that is not MQTT_OK.
 * @return The Reason Code, MQTT5_RC_UNSPECIFIED_ERROR if none is closer.
 */
uint8_t mqtt5_reason_code(mqtt_status status);

/**
 * @brief Write a Fixed Header.
 * @param[in,out] writer The writer.
 * @param[in] first_byte The packet type and flags.
 * @param[in] remaining_length Remaining Length.
 */
void mqtt_fixed_header_encode(mqtt_writer *writer, uint8_t first_byte,
                              uint32_t remaining_length);

/**
 * @brief Write a CONNECT packet.
 * @param[in,out] writer The writer.
 * @param[in] packet The packet.
 */
void mqtt_connect_encode(mqtt_writer *writer, const mqtt_connect *packet);

/**
 * @brief Write a CONNACK packet.
 * @param[in,out] writer The writer.
 * @param[in] version The protocol in use.
 * @param[in] packet The packet.
 */
void mqtt_connack_encode(mqtt_writer *writer, mqtt_version version,
                         const mqtt_connack *packet);

/**
 * @brief Write a PUBLISH packet.
 * @param[in,out] writer The writer.
 * @param[in] version The protocol in use.
 * @param[in] packet The packet.
 */
void mqtt_publish_encode(mqtt_writer *writer, mqtt_version version,
                         const mqtt_publish *packet);

/**
 * @brief Count the bytes of a PUBLISH packet.
 * @param[in] version The protocol in use.
 * @param[in] packet The packet.
 * @return The number of bytes in the whole packet, or zero if it could never be
 * sent.
 */
uint32_t mqtt_publish_size(mqtt_version version, const mqtt_publish *packet);

/**
 * @brief Write a PUBACK, PUBREC, PUBREL or PUBCOMP packet, in its shortest
 * form.
 * @param[in,out] writer The writer.
 * @param[in] version The protocol in use.
 * @param[in] packet The packet.
 */
void mqtt_publish_response_encode(mqtt_writer *writer, mqtt_version version,
                                  const mqtt_publish_response *packet);

/**
 * @brief Write a SUBSCRIBE packet whose Payload is already encoded.
 * @param[in,out] writer The writer.
 * @param[in] version The protocol in use.
 * @param[in] packet The packet.
 */
void mqtt_subscribe_encode(mqtt_writer *writer, mqtt_version version,
                           const mqtt_subscribe *packet);

/**
 * @brief Write an UNSUBSCRIBE packet whose Payload is already encoded.
 * @param[in,out] writer The writer.
 * @param[in] version The protocol in use.
 * @param[in] packet The packet.
 */
void mqtt_unsubscribe_encode(mqtt_writer *writer, mqtt_version version,
                             const mqtt_subscribe *packet);

/**
 * @brief Write a SUBACK packet.
 * @param[in,out] writer The writer.
 * @param[in] version The protocol in use.
 * @param[in] packet The packet.
 */
void mqtt_suback_encode(mqtt_writer *writer, mqtt_version version,
                        const mqtt_subscribe_response *packet);

/**
 * @brief Write an UNSUBACK packet.
 * @param[in,out] writer The writer.
 * @param[in] version The protocol in use.
 * @param[in] packet The packet.
 */
void mqtt_unsuback_encode(mqtt_writer *writer, mqtt_version version,
                          const mqtt_subscribe_response *packet);

/**
 * @brief Write a PINGREQ packet.
 * @param[in,out] writer The writer.
 */
void mqtt_pingreq_encode(mqtt_writer *writer);

/**
 * @brief Write a PINGRESP packet.
 * @param[in,out] writer The writer.
 */
void mqtt_pingresp_encode(mqtt_writer *writer);

/**
 * @brief Write a DISCONNECT packet, in its shortest form.
 * @param[in,out] writer The writer.
 * @param[in] version The protocol in use.
 * @param[in] packet The packet.
 */
void mqtt_disconnect_encode(mqtt_writer *writer, mqtt_version version,
                            const mqtt_reason *packet);

/**
 * @brief Write an AUTH packet.
 * @param[in,out] writer The writer.
 * @param[in] packet The packet.
 */
void mqtt_auth_encode(mqtt_writer *writer, const mqtt_reason *packet);

#endif /* MQTT_BASE_PACKET_H */
