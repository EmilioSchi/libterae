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
 * @file v5_properties.h
 * @brief Decoding and encoding Properties.
 *
 * Decoding checks each Property against the packet it arrived in, which Table
 * 2-4 sets, and against the rules for its value. It does not copy anything: the
 * strings are views of the received bytes, and raw keeps the whole block so
 * that repeated Properties can be read with a cursor.
 *
 * Only a Property marked present has a value. Clearing a set writes the mask,
 * raw and the User Properties, and leaves the values as they were, so a value
 * is read only after mqtt5_properties_has. A set that is all zeros is empty. A
 * mask that something overwrote marks nothing, and encoding it fails.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 2.2.2, Table 2-4, p.24-26. 3.1.2.11, 3.2.2.3, 3.3.2.3, p.45-68.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */
#ifndef MQTT_V5_PROPERTIES_H
#define MQTT_V5_PROPERTIES_H

#include <stdbool.h>
#include <stdint.h>

#include "base_binary_t.h"
#include "base_bool.h"
#include "base_payload_t.h"
#include "base_status.h"
#include "base_utf8_t.h"
#include "base_wire.h"
#include "v5_pair_t.h"

/**
 * @brief Stands for the Will Properties of a CONNECT packet where a packet type
 * is asked for.
 */
#define MQTT5_PROPERTIES_WILL 16U

/** @brief The Properties of a packet, or of a Will Message. */
typedef struct {
    /**
     * @brief One bit for each Property present: bit n is the Property with
     * Identifier n.
     */
    mqtt_bool_mask present;
    /** @brief The whole block, not counting its length. Set when decoding. */
    mqtt_payload raw;
    /** @brief User Properties to send. Only read when encoding. */
    const mqtt5_utf8_pair *user_pairs;
    /**
     * @brief Number of entries in user_pairs. Set to the number received when
     * decoding.
     */
    uint32_t user_pair_count;
    /** @brief Message Expiry Interval. */
    uint32_t message_expiry_interval;
    /** @brief Session Expiry Interval. */
    uint32_t session_expiry_interval;
    /** @brief Will Delay Interval. */
    uint32_t will_delay_interval;
    /** @brief Maximum Packet Size. */
    uint32_t maximum_packet_size;
    /** @brief Subscription Identifier, the first one if there are several. */
    uint32_t subscription_identifier;
    /** @brief Content Type. */
    mqtt_utf8 content_type;
    /** @brief Response Topic. */
    mqtt_utf8 response_topic;
    /** @brief Assigned Client Identifier. */
    mqtt_utf8 assigned_client_identifier;
    /** @brief Authentication Method. */
    mqtt_utf8 authentication_method;
    /** @brief Response Information. */
    mqtt_utf8 response_information;
    /** @brief Server Reference. */
    mqtt_utf8 server_reference;
    /** @brief Reason String. */
    mqtt_utf8 reason_string;
    /** @brief Correlation Data. */
    mqtt_binary correlation_data;
    /** @brief Authentication Data. */
    mqtt_binary authentication_data;
    /** @brief Server Keep Alive. */
    uint16_t server_keep_alive;
    /** @brief Receive Maximum. */
    uint16_t receive_maximum;
    /** @brief Topic Alias Maximum. */
    uint16_t topic_alias_maximum;
    /** @brief Topic Alias. */
    uint16_t topic_alias;
    /** @brief Payload Format Indicator. */
    uint8_t payload_format_indicator;
    /** @brief Request Problem Information. */
    uint8_t request_problem_information;
    /** @brief Request Response Information. */
    uint8_t request_response_information;
    /** @brief Maximum QoS. */
    uint8_t maximum_qos;
    /** @brief Retain Available. */
    uint8_t retain_available;
    /** @brief Wildcard Subscription Available. */
    uint8_t wildcard_subscription_available;
    /** @brief Subscription Identifier Available. */
    uint8_t subscription_identifier_available;
    /** @brief Shared Subscription Available. */
    uint8_t shared_subscription_available;
} mqtt5_properties;

/** @brief One Property read with a cursor. */
typedef struct {
    /** @brief Its Identifier. */
    uint32_t identifier;
    /**
     * @brief The value of a Byte, Two Byte, Four Byte or Variable Byte Integer
     * Property.
     */
    uint32_t integer;
    /**
     * @brief The value of a string Property, or the name of a User Property.
     */
    mqtt_utf8 text;
    /** @brief The value of a User Property. */
    mqtt_utf8 value;
    /** @brief The value of a Binary Data Property. */
    mqtt_binary binary;
} mqtt5_property;

/**
 * @brief Clear a set of Properties: mark none present, and drop raw and the
 * User Properties. The values are left as they were, as none is read now.
 * @param[out] properties The Properties.
 */
void mqtt5_properties_clear(mqtt5_properties *properties);

/**
 * @brief Set the bit that marks a Property as present.
 * @param[in,out] properties The Properties.
 * @param[in] identifier The Identifier of the Property.
 */
void mqtt5_properties_mark(mqtt5_properties *properties, uint32_t identifier);

/**
 * @brief Report whether a Property is present.
 * @param[in] properties The Properties.
 * @param[in] identifier The Identifier of the Property.
 * @return true if it is present.
 */
bool mqtt5_properties_has(const mqtt5_properties *properties,
                          uint32_t identifier);

/**
 * @brief Report whether a set marks no Property. A set whose mask was
 * overwritten is not empty, so a packet's encoder goes on to
 * mqtt5_properties_encode, which refuses it.
 * @param[in] properties The Properties.
 * @return true if the mask is valid and marks nothing.
 */
bool mqtt5_properties_empty(const mqtt5_properties *properties);

/**
 * @brief Read and check a Property Length and the block that follows it.
 * @param[in,out] reader Reader placed at the Property Length.
 * @param[in] packet_type The packet, or MQTT5_PROPERTIES_WILL.
 * @param[out] properties The result.
 * @return MQTT_OK, MQTT_MALFORMED_PACKET, or MQTT_PROTOCOL_ERROR.
 */
mqtt_status mqtt5_properties_decode(mqtt_reader *reader, uint32_t packet_type,
                                    mqtt5_properties *properties);

/**
 * @brief Read and check a block of Properties kept without its Property
 * Length, as a stored message keeps them.
 * @param[in] data The block.
 * @param[in] length Number of bytes in it.
 * @param[in] packet_type The packet, or MQTT5_PROPERTIES_WILL.
 * @param[out] properties The result.
 * @return MQTT_OK, MQTT_MALFORMED_PACKET, or MQTT_PROTOCOL_ERROR.
 */
mqtt_status mqtt5_properties_decode_block(const uint8_t *data, uint32_t length,
                                          uint32_t packet_type,
                                          mqtt5_properties *properties);

/**
 * @brief Read the next Property of a checked block.
 * @param[in,out] cursor Reader over the block in raw, started with
 * mqtt_reader_init.
 * @param[out] property The Property.
 * @return true if there was one, false at the end of the block.
 */
bool mqtt5_property_next(mqtt_reader *cursor, mqtt5_property *property);

/**
 * @brief Write a Property Length and the block that follows it. A mask that
 * isn't valid sets the writer's status to MQTT_INVALID_ARGUMENT.
 * @param[in,out] writer The writer.
 * @param[in] properties The Properties that are marked present, and any User
 * Properties.
 */
void mqtt5_properties_encode(mqtt_writer *writer,
                             const mqtt5_properties *properties);

#endif /* MQTT_V5_PROPERTIES_H */
