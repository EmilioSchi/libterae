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
 * @file v5_properties.c
 * @brief Decoding and encoding Properties.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 2.2.2, Table 2-4, p.24-26. 3.1.2.11, 3.2.2.3, 3.3.2.3, p.45-68.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */

#include <stddef.h>

#include "base_type.h"
#include "base_utf8_check.h"
#include "v5_prop.h"
#include "v5_prop_value.h"
#include "v5_properties.h"
#include "v5_type.h"

/** @brief The largest Identifier defined. */
#define V5_PROPERTY_LAST 0x2AU
/** @brief Number of entries in the rule table. */
#define V5_PROPERTY_RULES (V5_PROPERTY_LAST + 1U)

/** @brief Scope bit of the CONNECT packet. */
#define V5_IN_CONNECT ((uint32_t)1U << MQTT_CONNECT)
/** @brief Scope bit of the CONNACK packet. */
#define V5_IN_CONNACK ((uint32_t)1U << MQTT_CONNACK)
/** @brief Scope bit of the PUBLISH packet. */
#define V5_IN_PUBLISH ((uint32_t)1U << MQTT_PUBLISH)
/** @brief Scope bit of the PUBACK packet. */
#define V5_IN_PUBACK ((uint32_t)1U << MQTT_PUBACK)
/** @brief Scope bit of the PUBREC packet. */
#define V5_IN_PUBREC ((uint32_t)1U << MQTT_PUBREC)
/** @brief Scope bit of the PUBREL packet. */
#define V5_IN_PUBREL ((uint32_t)1U << MQTT_PUBREL)
/** @brief Scope bit of the PUBCOMP packet. */
#define V5_IN_PUBCOMP ((uint32_t)1U << MQTT_PUBCOMP)
/** @brief Scope bit of the SUBSCRIBE packet. */
#define V5_IN_SUBSCRIBE ((uint32_t)1U << MQTT_SUBSCRIBE)
/** @brief Scope bit of the SUBACK packet. */
#define V5_IN_SUBACK ((uint32_t)1U << MQTT_SUBACK)
/** @brief Scope bit of the UNSUBSCRIBE packet. */
#define V5_IN_UNSUBSCRIBE ((uint32_t)1U << MQTT_UNSUBSCRIBE)
/** @brief Scope bit of the UNSUBACK packet. */
#define V5_IN_UNSUBACK ((uint32_t)1U << MQTT_UNSUBACK)
/** @brief Scope bit of the DISCONNECT packet. */
#define V5_IN_DISCONNECT ((uint32_t)1U << MQTT_DISCONNECT)
/** @brief Scope bit of the AUTH packet. */
#define V5_IN_AUTH ((uint32_t)1U << MQTT5_AUTH)
/** @brief Scope bit of the Will Properties. */
#define V5_IN_WILL ((uint32_t)1U << MQTT5_PROPERTIES_WILL)
/** @brief Scope of the Properties a message carries. */
#define V5_IN_MESSAGE (V5_IN_PUBLISH | V5_IN_WILL)
/** @brief Scope of the Reason String. */
#define V5_IN_REASON_STRING \
    (V5_IN_CONNACK | V5_IN_PUBACK | V5_IN_PUBREC | V5_IN_PUBREL | \
     V5_IN_PUBCOMP | V5_IN_SUBACK | V5_IN_UNSUBACK | V5_IN_DISCONNECT | \
     V5_IN_AUTH)
/** @brief Scope of the User Property. */
#define V5_IN_USER_PROPERTY \
    (V5_IN_CONNECT | V5_IN_CONNACK | V5_IN_PUBLISH | V5_IN_WILL | \
     V5_IN_PUBACK | V5_IN_PUBREC | V5_IN_PUBREL | V5_IN_PUBCOMP | \
     V5_IN_SUBSCRIBE | V5_IN_SUBACK | V5_IN_UNSUBSCRIBE | V5_IN_UNSUBACK | \
     V5_IN_DISCONNECT | V5_IN_AUTH)

/** @brief The data type of a Property. */
typedef enum {
    /** @brief No Property has this Identifier. */
    V5_KIND_NONE = 0,
    /** @brief Byte. */
    V5_KIND_BYTE,
    /** @brief Two Byte Integer. */
    V5_KIND_TWO,
    /** @brief Four Byte Integer. */
    V5_KIND_FOUR,
    /** @brief Variable Byte Integer. */
    V5_KIND_VARIABLE,
    /** @brief UTF-8 Encoded String. */
    V5_KIND_UTF8,
    /** @brief UTF-8 String Pair. */
    V5_KIND_PAIR,
    /** @brief Binary Data. */
    V5_KIND_BINARY
} v5_property_kind;

/** @brief What Table 2-4 says about one Identifier. */
typedef struct {
    /** @brief The data type. */
    v5_property_kind kind;
    /**
     * @brief One bit for each packet the Property may appear in: bit n is
     * packet type n.
     */
    uint32_t scopes;
} v5_property_rule;

/** @brief Table 2-4, indexed by Identifier. */
static const v5_property_rule v5_property_rules[V5_PROPERTY_RULES] = {
    [MQTT5_PROP_PAYLOAD_FORMAT_INDICATOR] = {V5_KIND_BYTE, V5_IN_MESSAGE},
    [MQTT5_PROP_MESSAGE_EXPIRY_INTERVAL] = {V5_KIND_FOUR, V5_IN_MESSAGE},
    [MQTT5_PROP_CONTENT_TYPE] = {V5_KIND_UTF8, V5_IN_MESSAGE},
    [MQTT5_PROP_RESPONSE_TOPIC] = {V5_KIND_UTF8, V5_IN_MESSAGE},
    [MQTT5_PROP_CORRELATION_DATA] = {V5_KIND_BINARY, V5_IN_MESSAGE},
    [MQTT5_PROP_SUBSCRIPTION_IDENTIFIER] = {V5_KIND_VARIABLE,
                                            V5_IN_PUBLISH | V5_IN_SUBSCRIBE},
    [MQTT5_PROP_SESSION_EXPIRY_INTERVAL] =
        {V5_KIND_FOUR, V5_IN_CONNECT | V5_IN_CONNACK | V5_IN_DISCONNECT},
    [MQTT5_PROP_ASSIGNED_CLIENT_IDENTIFIER] = {V5_KIND_UTF8, V5_IN_CONNACK},
    [MQTT5_PROP_SERVER_KEEP_ALIVE] = {V5_KIND_TWO, V5_IN_CONNACK},
    [MQTT5_PROP_AUTHENTICATION_METHOD] =
        {V5_KIND_UTF8, V5_IN_CONNECT | V5_IN_CONNACK | V5_IN_AUTH},
    [MQTT5_PROP_AUTHENTICATION_DATA] =
        {V5_KIND_BINARY, V5_IN_CONNECT | V5_IN_CONNACK | V5_IN_AUTH},
    [MQTT5_PROP_REQUEST_PROBLEM_INFORMATION] = {V5_KIND_BYTE, V5_IN_CONNECT},
    [MQTT5_PROP_WILL_DELAY_INTERVAL] = {V5_KIND_FOUR, V5_IN_WILL},
    [MQTT5_PROP_REQUEST_RESPONSE_INFORMATION] = {V5_KIND_BYTE, V5_IN_CONNECT},
    [MQTT5_PROP_RESPONSE_INFORMATION] = {V5_KIND_UTF8, V5_IN_CONNACK},
    [MQTT5_PROP_SERVER_REFERENCE] = {V5_KIND_UTF8,
                                     V5_IN_CONNACK | V5_IN_DISCONNECT},
    [MQTT5_PROP_REASON_STRING] = {V5_KIND_UTF8, V5_IN_REASON_STRING},
    [MQTT5_PROP_RECEIVE_MAXIMUM] = {V5_KIND_TWO, V5_IN_CONNECT | V5_IN_CONNACK},
    [MQTT5_PROP_TOPIC_ALIAS_MAXIMUM] = {V5_KIND_TWO,
                                        V5_IN_CONNECT | V5_IN_CONNACK},
    [MQTT5_PROP_TOPIC_ALIAS] = {V5_KIND_TWO, V5_IN_PUBLISH},
    [MQTT5_PROP_MAXIMUM_QOS] = {V5_KIND_BYTE, V5_IN_CONNACK},
    [MQTT5_PROP_RETAIN_AVAILABLE] = {V5_KIND_BYTE, V5_IN_CONNACK},
    [MQTT5_PROP_USER_PROPERTY] = {V5_KIND_PAIR, V5_IN_USER_PROPERTY},
    [MQTT5_PROP_MAXIMUM_PACKET_SIZE] = {V5_KIND_FOUR,
                                        V5_IN_CONNECT | V5_IN_CONNACK},
    [MQTT5_PROP_WILDCARD_SUBSCRIPTION_AVAILABLE] = {V5_KIND_BYTE,
                                                    V5_IN_CONNACK},
    [MQTT5_PROP_SUBSCRIPTION_IDENTIFIER_AVAILABLE] = {V5_KIND_BYTE,
                                                      V5_IN_CONNACK},
    [MQTT5_PROP_SHARED_SUBSCRIPTION_AVAILABLE] = {V5_KIND_BYTE, V5_IN_CONNACK},
};

/**
 * @brief Set every field of a Property to zero, so that the fields another
 * type uses are not left over.
 * @param[out] property The Property.
 */
static void v5_property_clear(mqtt5_property *property)
{
    property->identifier = 0U;
    property->integer = 0U;
    property->text.length = 0U;
    property->text.data = NULL;
    property->value.length = 0U;
    property->value.data = NULL;
    property->binary.length = 0U;
    property->binary.data = NULL;
}

/**
 * @brief Read one Property's value, as its type says.
 * @param[in,out] reader Reader over the block.
 * @param[in] kind The Property's data type.
 * @param[out] property The value.
 */
static void v5_property_read_value(mqtt_reader *reader, v5_property_kind kind,
                                   mqtt5_property *property)
{
    switch (kind) {
    case V5_KIND_BYTE:
        property->integer = (uint32_t)mqtt_read_byte(reader);
        break;
    case V5_KIND_TWO:
        property->integer = (uint32_t)mqtt_read_two_byte_integer(reader);
        break;
    case V5_KIND_FOUR:
        property->integer = mqtt_read_four_byte_integer(reader);
        break;
    case V5_KIND_VARIABLE:
        property->integer = mqtt_read_variable_byte_integer(reader);
        break;
    case V5_KIND_UTF8:
        property->text = mqtt_read_utf8(reader);
        break;
    case V5_KIND_PAIR:
        property->text = mqtt_read_utf8(reader);
        property->value = mqtt_read_utf8(reader);
        break;
    case V5_KIND_BINARY:
        property->binary = mqtt_read_binary(reader);
        break;
    default:
        reader->status = MQTT_MALFORMED_PACKET;
        break;
    }
}

/**
 * @brief Check the rules for a value of a Byte Property that may only be 0 or
 * 1.
 * @param[in] value The value.
 * @return MQTT_OK or MQTT_PROTOCOL_ERROR.
 */
static mqtt_status v5_check_flag(uint32_t value)
{
    return (value <= 1U) ? MQTT_OK : MQTT_PROTOCOL_ERROR;
}

/**
 * @brief Check and store a Property whose value is a Byte.
 * @param[in] property The Property as read.
 * @param[out] properties The destination.
 * @return MQTT_OK, MQTT_MALFORMED_PACKET, or MQTT_PROTOCOL_ERROR.
 */
static mqtt_status v5_property_store_byte(const mqtt5_property *property,
                                          mqtt5_properties *properties)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief The value as a Byte. */
    const uint8_t byte_value = (uint8_t)(property->integer & 0xFFU);

    switch (property->identifier) {
    case MQTT5_PROP_PAYLOAD_FORMAT_INDICATOR:
        status = v5_check_flag(property->integer);
        properties->payload_format_indicator = byte_value;
        break;
    case MQTT5_PROP_REQUEST_PROBLEM_INFORMATION:
        status = v5_check_flag(property->integer);
        properties->request_problem_information = byte_value;
        break;
    case MQTT5_PROP_REQUEST_RESPONSE_INFORMATION:
        status = v5_check_flag(property->integer);
        properties->request_response_information = byte_value;
        break;
    case MQTT5_PROP_MAXIMUM_QOS:
        status = v5_check_flag(property->integer);
        properties->maximum_qos = byte_value;
        break;
    case MQTT5_PROP_RETAIN_AVAILABLE:
        status = v5_check_flag(property->integer);
        properties->retain_available = byte_value;
        break;
    case MQTT5_PROP_WILDCARD_SUBSCRIPTION_AVAILABLE:
        status = v5_check_flag(property->integer);
        properties->wildcard_subscription_available = byte_value;
        break;
    case MQTT5_PROP_SUBSCRIPTION_IDENTIFIER_AVAILABLE:
        status = v5_check_flag(property->integer);
        properties->subscription_identifier_available = byte_value;
        break;
    case MQTT5_PROP_SHARED_SUBSCRIPTION_AVAILABLE:
        status = v5_check_flag(property->integer);
        properties->shared_subscription_available = byte_value;
        break;
    default:
        status = MQTT_MALFORMED_PACKET;
        break;
    }
    return status;
}

/**
 * @brief Check and store a Property whose value is a Two Byte, Four Byte or
 * Variable Byte Integer.
 * @param[in] property The Property as read.
 * @param[in,out] properties The destination.
 * @return MQTT_OK, MQTT_MALFORMED_PACKET, MQTT_PROTOCOL_ERROR, or
 * MQTT_TOPIC_ALIAS_INVALID.
 */
static mqtt_status v5_property_store_integer(const mqtt5_property *property,
                                             mqtt5_properties *properties)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief The value as a Two Byte Integer, where it is one. */
    const uint16_t two_value = (uint16_t)(property->integer & 0xFFFFU);

    switch (property->identifier) {
    case MQTT5_PROP_MESSAGE_EXPIRY_INTERVAL:
        properties->message_expiry_interval = property->integer;
        break;
    case MQTT5_PROP_SUBSCRIPTION_IDENTIFIER:
        if (property->integer < MQTT5_SUBSCRIPTION_IDENTIFIER_MIN) {
            status = MQTT_PROTOCOL_ERROR;
        } else if (!mqtt5_properties_has(properties,
                                         MQTT5_PROP_SUBSCRIPTION_IDENTIFIER)) {
            properties->subscription_identifier = property->integer;
        } else {
            /* Only the first is kept here. A cursor reads the others. */
        }
        break;
    case MQTT5_PROP_SESSION_EXPIRY_INTERVAL:
        properties->session_expiry_interval = property->integer;
        break;
    case MQTT5_PROP_SERVER_KEEP_ALIVE:
        properties->server_keep_alive = two_value;
        break;
    case MQTT5_PROP_WILL_DELAY_INTERVAL:
        properties->will_delay_interval = property->integer;
        break;
    case MQTT5_PROP_RECEIVE_MAXIMUM:
        status = (property->integer < MQTT5_RECEIVE_MAXIMUM_MIN) ?
                     MQTT_PROTOCOL_ERROR :
                     MQTT_OK;
        properties->receive_maximum = two_value;
        break;
    case MQTT5_PROP_TOPIC_ALIAS_MAXIMUM:
        properties->topic_alias_maximum = two_value;
        break;
    case MQTT5_PROP_TOPIC_ALIAS:
        status = (property->integer < MQTT5_TOPIC_ALIAS_MIN) ?
                     MQTT_TOPIC_ALIAS_INVALID :
                     MQTT_OK;
        properties->topic_alias = two_value;
        break;
    case MQTT5_PROP_MAXIMUM_PACKET_SIZE:
        status = (property->integer < MQTT5_MAXIMUM_PACKET_SIZE_MIN) ?
                     MQTT_PROTOCOL_ERROR :
                     MQTT_OK;
        properties->maximum_packet_size = property->integer;
        break;
    default:
        status = MQTT_MALFORMED_PACKET;
        break;
    }
    return status;
}

/**
 * @brief Store a Property whose value is a UTF-8 Encoded String, a UTF-8
 * String Pair, or Binary Data.
 *
 * Only the number of User Properties is kept here. A cursor reads the pairs.
 *
 * @param[in] property The Property as read.
 * @param[in,out] properties The destination.
 * @return MQTT_OK, or MQTT_MALFORMED_PACKET.
 */
static mqtt_status v5_property_store_reference(const mqtt5_property *property,
                                               mqtt5_properties *properties)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    switch (property->identifier) {
    case MQTT5_PROP_CONTENT_TYPE:
        properties->content_type = property->text;
        break;
    case MQTT5_PROP_RESPONSE_TOPIC:
        properties->response_topic = property->text;
        break;
    case MQTT5_PROP_CORRELATION_DATA:
        properties->correlation_data = property->binary;
        break;
    case MQTT5_PROP_ASSIGNED_CLIENT_IDENTIFIER:
        properties->assigned_client_identifier = property->text;
        break;
    case MQTT5_PROP_AUTHENTICATION_METHOD:
        properties->authentication_method = property->text;
        break;
    case MQTT5_PROP_AUTHENTICATION_DATA:
        properties->authentication_data = property->binary;
        break;
    case MQTT5_PROP_RESPONSE_INFORMATION:
        properties->response_information = property->text;
        break;
    case MQTT5_PROP_SERVER_REFERENCE:
        properties->server_reference = property->text;
        break;
    case MQTT5_PROP_REASON_STRING:
        properties->reason_string = property->text;
        break;
    case MQTT5_PROP_USER_PROPERTY:
        properties->user_pair_count += 1U;
        break;
    default:
        status = MQTT_MALFORMED_PACKET;
        break;
    }
    return status;
}

/**
 * @brief Check a Property's value and store it.
 * @param[in] property The Property as read.
 * @param[in,out] properties The destination, which says what is there already.
 * @return MQTT_OK, MQTT_MALFORMED_PACKET, MQTT_PROTOCOL_ERROR, or
 * MQTT_TOPIC_ALIAS_INVALID.
 */
static mqtt_status v5_property_store(const mqtt5_property *property,
                                     mqtt5_properties *properties)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_MALFORMED_PACKET;

    if (property->identifier <= V5_PROPERTY_LAST) {
        switch (v5_property_rules[property->identifier].kind) {
        case V5_KIND_BYTE:
            status = v5_property_store_byte(property, properties);
            break;
        case V5_KIND_TWO:
        case V5_KIND_FOUR:
        case V5_KIND_VARIABLE:
            status = v5_property_store_integer(property, properties);
            break;
        case V5_KIND_UTF8:
        case V5_KIND_PAIR:
        case V5_KIND_BINARY:
            status = v5_property_store_reference(property, properties);
            break;
        default:
            /* No Property has this Identifier. */
            status = MQTT_MALFORMED_PACKET;
            break;
        }
    }
    return status;
}

/**
 * @brief Read, check and store one Property.
 * @param[in,out] reader Reader over the block.
 * @param[in] packet_type The packet, or MQTT5_PROPERTIES_WILL.
 * @param[in,out] properties The destination.
 * @return MQTT_OK, MQTT_MALFORMED_PACKET, or MQTT_PROTOCOL_ERROR.
 */
static mqtt_status v5_property_decode_one(mqtt_reader *reader,
                                          uint32_t packet_type,
                                          mqtt5_properties *properties)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief The Property as read. */
    mqtt5_property property;

    v5_property_clear(&property);
    property.identifier = mqtt_read_variable_byte_integer(reader);
    if ((reader->status != MQTT_OK) ||
        (property.identifier > V5_PROPERTY_LAST)) {
        status = MQTT_MALFORMED_PACKET;
    } else if ((v5_property_rules[property.identifier].kind == V5_KIND_NONE) ||
               ((v5_property_rules[property.identifier].scopes &
                 ((uint32_t)1U << packet_type)) == 0U)) {
        status = MQTT_MALFORMED_PACKET;
    } else {
        v5_property_read_value(
            reader, v5_property_rules[property.identifier].kind, &property);
        status = reader->status;
    }
    if (status == MQTT_OK) {
        /** @brief The data type. */
        const v5_property_kind kind =
            v5_property_rules[property.identifier].kind;
        /**
         * @brief true if the Property may be sent more than once in this
         * packet.
         */
        const bool repeatable =
            (property.identifier == MQTT5_PROP_USER_PROPERTY) ||
            ((property.identifier == MQTT5_PROP_SUBSCRIPTION_IDENTIFIER) &&
             (packet_type == MQTT_PUBLISH));

        if ((!repeatable) &&
            mqtt5_properties_has(properties, property.identifier)) {
            status = MQTT_PROTOCOL_ERROR;
        } else if ((kind == V5_KIND_UTF8) &&
                   (!mqtt_utf8_is_valid(property.text.data,
                                        (uint32_t)property.text.length))) {
            status = MQTT_MALFORMED_PACKET;
        } else if ((kind == V5_KIND_PAIR) &&
                   ((!mqtt_utf8_is_valid(property.text.data,
                                         (uint32_t)property.text.length)) ||
                    (!mqtt_utf8_is_valid(property.value.data,
                                         (uint32_t)property.value.length)))) {
            status = MQTT_MALFORMED_PACKET;
        } else {
            status = v5_property_store(&property, properties);
            mqtt5_properties_mark(properties, property.identifier);
        }
    }
    return status;
}

void mqtt5_properties_clear(mqtt5_properties *properties)
{
    if (properties != NULL) {
        mqtt_bool_mask_clear(&properties->present);
        properties->raw.data = NULL;
        properties->raw.length = 0U;
        properties->user_pairs = NULL;
        properties->user_pair_count = 0U;
    }
}

void mqtt5_properties_mark(mqtt5_properties *properties, uint32_t identifier)
{
    if (properties != NULL) {
        mqtt_bool_mask_mark(&properties->present, identifier);
    }
}

bool mqtt5_properties_has(const mqtt5_properties *properties,
                          uint32_t identifier)
{
    return (properties != NULL) &&
           mqtt_bool_mask_has(&properties->present, identifier);
}

bool mqtt5_properties_empty(const mqtt5_properties *properties)
{
    return (properties != NULL) &&
           mqtt_bool_mask_is_valid(&properties->present) &&
           (!mqtt_bool_mask_any(&properties->present, ~(uint64_t)0U));
}

/**
 * @brief Do what mqtt5_properties_decode does, for arguments known to be valid.
 * @param[in,out] reader Reader placed at the Property Length.
 * @param[in] packet_type The packet, or MQTT5_PROPERTIES_WILL.
 * @param[out] properties The result.
 * @return MQTT_OK, MQTT_MALFORMED_PACKET, or MQTT_PROTOCOL_ERROR.
 */
static mqtt_status properties_decode_body(mqtt_reader *reader,
                                          uint32_t packet_type,
                                          mqtt5_properties *properties)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief Property Length. */
    const uint32_t length = mqtt_read_variable_byte_integer(reader);

    mqtt5_properties_clear(properties);
    if ((reader->status != MQTT_OK) || (length > mqtt_reader_left(reader))) {
        status = MQTT_MALFORMED_PACKET;
        reader->status = MQTT_MALFORMED_PACKET;
    } else if (length > 0U) {
        /**
         * @brief Reader over the block alone, so a Property cannot run past it.
         */
        mqtt_reader block = {NULL, 0U, 0U, MQTT_OK};

        mqtt_reader_init(&block, &reader->data[reader->position], length);
        properties->raw.data = block.data;
        properties->raw.length = length;
        while ((status == MQTT_OK) && (mqtt_reader_left(&block) > 0U)) {
            status = v5_property_decode_one(&block, packet_type, properties);
        }
        reader->position += length;
        if (status != MQTT_OK) {
            reader->status = status;
        }
    } else {
        /* An empty block is all there is to read. */
    }
    return status;
}

mqtt_status mqtt5_properties_decode(mqtt_reader *reader, uint32_t packet_type,
                                    mqtt5_properties *properties)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if ((reader != NULL) && (properties != NULL)) {
        answer = properties_decode_body(reader, packet_type, properties);
    }
    return answer;
}

/**
 * @brief Do what mqtt5_properties_decode_block does, for arguments known to be
 * valid.
 * @param[in] data The block.
 * @param[in] length Number of bytes in it.
 * @param[in] packet_type The packet, or MQTT5_PROPERTIES_WILL.
 * @param[out] properties The result.
 * @return MQTT_OK, MQTT_MALFORMED_PACKET, or MQTT_PROTOCOL_ERROR.
 */
static mqtt_status properties_decode_block_body(const uint8_t *data,
                                                uint32_t length,
                                                uint32_t packet_type,
                                                mqtt5_properties *properties)
{
    /** @brief Reader over the block. */
    mqtt_reader block = {NULL, 0U, 0U, MQTT_OK};
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    mqtt5_properties_clear(properties);
    mqtt_reader_init(&block, data, length);
    properties->raw.data = data;
    properties->raw.length = length;
    while ((status == MQTT_OK) && (mqtt_reader_left(&block) > 0U)) {
        status = v5_property_decode_one(&block, packet_type, properties);
    }
    return status;
}

mqtt_status mqtt5_properties_decode_block(const uint8_t *data, uint32_t length,
                                          uint32_t packet_type,
                                          mqtt5_properties *properties)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if ((properties != NULL) && ((data != NULL) || (length == 0U))) {
        answer = properties_decode_block_body(data, length, packet_type,
                                              properties);
    }
    return answer;
}

bool mqtt5_property_next(mqtt_reader *cursor, mqtt5_property *property)
{
    /** @brief The answer. */
    bool found = false;

    if ((cursor != NULL) && (property != NULL) &&
        (cursor->status == MQTT_OK) && (mqtt_reader_left(cursor) > 0U)) {
        v5_property_clear(property);
        property->identifier = mqtt_read_variable_byte_integer(cursor);
        if ((cursor->status == MQTT_OK) &&
            (property->identifier <= V5_PROPERTY_LAST)) {
            v5_property_read_value(
                cursor, v5_property_rules[property->identifier].kind, property);
            found = (cursor->status == MQTT_OK);
        }
    }
    return found;
}

/**
 * @brief Write the value of a Property whose value is a Byte.
 * @param[in,out] writer The writer.
 * @param[in] properties The Properties.
 * @param[in] identifier The Property's Identifier.
 */
static void v5_property_encode_byte(mqtt_writer *writer,
                                    const mqtt5_properties *properties,
                                    uint32_t identifier)
{
    switch (identifier) {
    case MQTT5_PROP_PAYLOAD_FORMAT_INDICATOR:
        mqtt_write_byte(writer, properties->payload_format_indicator);
        break;
    case MQTT5_PROP_REQUEST_PROBLEM_INFORMATION:
        mqtt_write_byte(writer, properties->request_problem_information);
        break;
    case MQTT5_PROP_REQUEST_RESPONSE_INFORMATION:
        mqtt_write_byte(writer, properties->request_response_information);
        break;
    case MQTT5_PROP_MAXIMUM_QOS:
        mqtt_write_byte(writer, properties->maximum_qos);
        break;
    case MQTT5_PROP_RETAIN_AVAILABLE:
        mqtt_write_byte(writer, properties->retain_available);
        break;
    case MQTT5_PROP_WILDCARD_SUBSCRIPTION_AVAILABLE:
        mqtt_write_byte(writer, properties->wildcard_subscription_available);
        break;
    case MQTT5_PROP_SUBSCRIPTION_IDENTIFIER_AVAILABLE:
        mqtt_write_byte(writer, properties->subscription_identifier_available);
        break;
    case MQTT5_PROP_SHARED_SUBSCRIPTION_AVAILABLE:
        mqtt_write_byte(writer, properties->shared_subscription_available);
        break;
    default:
        writer->status = MQTT_INVALID_ARGUMENT;
        break;
    }
}

/**
 * @brief Write the value of a Property whose value is a Two Byte, Four Byte or
 * Variable Byte Integer.
 * @param[in,out] writer The writer.
 * @param[in] properties The Properties.
 * @param[in] identifier The Property's Identifier.
 */
static void v5_property_encode_integer(mqtt_writer *writer,
                                       const mqtt5_properties *properties,
                                       uint32_t identifier)
{
    switch (identifier) {
    case MQTT5_PROP_MESSAGE_EXPIRY_INTERVAL:
        mqtt_write_four_byte_integer(writer,
                                     properties->message_expiry_interval);
        break;
    case MQTT5_PROP_SUBSCRIPTION_IDENTIFIER:
        mqtt_write_variable_byte_integer(writer,
                                         properties->subscription_identifier);
        break;
    case MQTT5_PROP_SESSION_EXPIRY_INTERVAL:
        mqtt_write_four_byte_integer(writer,
                                     properties->session_expiry_interval);
        break;
    case MQTT5_PROP_SERVER_KEEP_ALIVE:
        mqtt_write_two_byte_integer(writer, properties->server_keep_alive);
        break;
    case MQTT5_PROP_WILL_DELAY_INTERVAL:
        mqtt_write_four_byte_integer(writer, properties->will_delay_interval);
        break;
    case MQTT5_PROP_RECEIVE_MAXIMUM:
        mqtt_write_two_byte_integer(writer, properties->receive_maximum);
        break;
    case MQTT5_PROP_TOPIC_ALIAS_MAXIMUM:
        mqtt_write_two_byte_integer(writer, properties->topic_alias_maximum);
        break;
    case MQTT5_PROP_TOPIC_ALIAS:
        mqtt_write_two_byte_integer(writer, properties->topic_alias);
        break;
    case MQTT5_PROP_MAXIMUM_PACKET_SIZE:
        mqtt_write_four_byte_integer(writer, properties->maximum_packet_size);
        break;
    default:
        writer->status = MQTT_INVALID_ARGUMENT;
        break;
    }
}

/**
 * @brief Write the value of a Property whose value is a UTF-8 Encoded String
 * or Binary Data.
 * @param[in,out] writer The writer.
 * @param[in] properties The Properties.
 * @param[in] identifier The Property's Identifier.
 */
static void v5_property_encode_reference(mqtt_writer *writer,
                                         const mqtt5_properties *properties,
                                         uint32_t identifier)
{
    switch (identifier) {
    case MQTT5_PROP_CONTENT_TYPE:
        mqtt_write_utf8(writer, &properties->content_type);
        break;
    case MQTT5_PROP_RESPONSE_TOPIC:
        mqtt_write_utf8(writer, &properties->response_topic);
        break;
    case MQTT5_PROP_CORRELATION_DATA:
        mqtt_write_binary(writer, &properties->correlation_data);
        break;
    case MQTT5_PROP_ASSIGNED_CLIENT_IDENTIFIER:
        mqtt_write_utf8(writer, &properties->assigned_client_identifier);
        break;
    case MQTT5_PROP_AUTHENTICATION_METHOD:
        mqtt_write_utf8(writer, &properties->authentication_method);
        break;
    case MQTT5_PROP_AUTHENTICATION_DATA:
        mqtt_write_binary(writer, &properties->authentication_data);
        break;
    case MQTT5_PROP_RESPONSE_INFORMATION:
        mqtt_write_utf8(writer, &properties->response_information);
        break;
    case MQTT5_PROP_SERVER_REFERENCE:
        mqtt_write_utf8(writer, &properties->server_reference);
        break;
    case MQTT5_PROP_REASON_STRING:
        mqtt_write_utf8(writer, &properties->reason_string);
        break;
    default:
        writer->status = MQTT_INVALID_ARGUMENT;
        break;
    }
}

/**
 * @brief Write the entries of a block, with no length before them.
 * @param[in,out] writer The writer.
 * @param[in] properties The Properties that are marked present, and any User
 * Properties.
 */
static void v5_properties_encode_entries(mqtt_writer *writer,
                                         const mqtt5_properties *properties)
{
    /** @brief The Identifier being considered. */
    uint32_t identifier = 1U;

    while (identifier <= V5_PROPERTY_LAST) {
        if ((identifier != MQTT5_PROP_USER_PROPERTY) &&
            mqtt5_properties_has(properties, identifier)) {
            mqtt_write_variable_byte_integer(writer, identifier);
            switch (v5_property_rules[identifier].kind) {
            case V5_KIND_BYTE:
                v5_property_encode_byte(writer, properties, identifier);
                break;
            case V5_KIND_TWO:
            case V5_KIND_FOUR:
            case V5_KIND_VARIABLE:
                v5_property_encode_integer(writer, properties, identifier);
                break;
            case V5_KIND_UTF8:
            case V5_KIND_BINARY:
                v5_property_encode_reference(writer, properties, identifier);
                break;
            default:
                /* No Property has this Identifier. */
                writer->status = MQTT_INVALID_ARGUMENT;
                break;
            }
        }
        identifier += 1U;
    }
    if (properties->user_pairs != NULL) {
        /** @brief Index of the pair being written. */
        uint32_t index = 0U;

        while (index < properties->user_pair_count) {
            mqtt_write_variable_byte_integer(writer, MQTT5_PROP_USER_PROPERTY);
            mqtt_write_utf8(writer, &properties->user_pairs[index].name);
            mqtt_write_utf8(writer, &properties->user_pairs[index].value);
            index += 1U;
        }
    }
}

void mqtt5_properties_encode(mqtt_writer *writer,
                             const mqtt5_properties *properties)
{
    /** @brief A writer that only counts. */
    mqtt_writer counter = {NULL, 0U, 0U, MQTT_OK};

    if ((properties == NULL) ||
        (!mqtt_bool_mask_is_valid(&properties->present))) {
        mqtt_writer_refuse(writer);
    } else {
        mqtt_writer_init(&counter, NULL, 0U);
        v5_properties_encode_entries(&counter, properties);
        mqtt_write_variable_byte_integer(writer, counter.position);
        v5_properties_encode_entries(writer, properties);
    }
}
