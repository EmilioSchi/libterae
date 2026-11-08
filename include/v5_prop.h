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
 * @file v5_prop.h
 * @brief Property Identifiers.
 *
 * Each Property is an Identifier, which sets what the Property means and its
 * data type, and then a value. Identifiers use the Variable Byte Integer
 * encoding, and every one defined in this version fits in one byte. Each
 * comment gives the type and the packets, as in Table 2-4.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 2.2.2.2, Table 2-4, p.25-26.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */
#ifndef MQTT_V5_PROP_H
#define MQTT_V5_PROP_H

/** @brief Byte. PUBLISH, Will Properties. */
#define MQTT5_PROP_PAYLOAD_FORMAT_INDICATOR 0x01U
/** @brief Four Byte Integer. PUBLISH, Will Properties. */
#define MQTT5_PROP_MESSAGE_EXPIRY_INTERVAL 0x02U
/** @brief UTF-8 Encoded String. PUBLISH, Will Properties. */
#define MQTT5_PROP_CONTENT_TYPE 0x03U
/** @brief UTF-8 Encoded String. PUBLISH, Will Properties. */
#define MQTT5_PROP_RESPONSE_TOPIC 0x08U
/** @brief Binary Data. PUBLISH, Will Properties. */
#define MQTT5_PROP_CORRELATION_DATA 0x09U
/** @brief Variable Byte Integer. PUBLISH, SUBSCRIBE. */
#define MQTT5_PROP_SUBSCRIPTION_IDENTIFIER 0x0BU
/** @brief Four Byte Integer. CONNECT, CONNACK, DISCONNECT. */
#define MQTT5_PROP_SESSION_EXPIRY_INTERVAL 0x11U
/** @brief UTF-8 Encoded String. CONNACK. */
#define MQTT5_PROP_ASSIGNED_CLIENT_IDENTIFIER 0x12U
/** @brief Two Byte Integer. CONNACK. */
#define MQTT5_PROP_SERVER_KEEP_ALIVE 0x13U
/** @brief UTF-8 Encoded String. CONNECT, CONNACK, AUTH. */
#define MQTT5_PROP_AUTHENTICATION_METHOD 0x15U
/** @brief Binary Data. CONNECT, CONNACK, AUTH. */
#define MQTT5_PROP_AUTHENTICATION_DATA 0x16U
/** @brief Byte. CONNECT. */
#define MQTT5_PROP_REQUEST_PROBLEM_INFORMATION 0x17U
/** @brief Four Byte Integer. Will Properties. */
#define MQTT5_PROP_WILL_DELAY_INTERVAL 0x18U
/** @brief Byte. CONNECT. */
#define MQTT5_PROP_REQUEST_RESPONSE_INFORMATION 0x19U
/** @brief UTF-8 Encoded String. CONNACK. */
#define MQTT5_PROP_RESPONSE_INFORMATION 0x1AU
/** @brief UTF-8 Encoded String. CONNACK, DISCONNECT. */
#define MQTT5_PROP_SERVER_REFERENCE 0x1CU
/**
 * @brief UTF-8 Encoded String. CONNACK, PUBACK, PUBREC, PUBREL, PUBCOMP,
 * SUBACK, UNSUBACK, DISCONNECT, AUTH.
 */
#define MQTT5_PROP_REASON_STRING 0x1FU
/** @brief Two Byte Integer. CONNECT, CONNACK. */
#define MQTT5_PROP_RECEIVE_MAXIMUM 0x21U
/** @brief Two Byte Integer. CONNECT, CONNACK. */
#define MQTT5_PROP_TOPIC_ALIAS_MAXIMUM 0x22U
/** @brief Two Byte Integer. PUBLISH. */
#define MQTT5_PROP_TOPIC_ALIAS 0x23U
/** @brief Byte. CONNACK. */
#define MQTT5_PROP_MAXIMUM_QOS 0x24U
/** @brief Byte. CONNACK. */
#define MQTT5_PROP_RETAIN_AVAILABLE 0x25U
/**
 * @brief UTF-8 String Pair. CONNECT, CONNACK, PUBLISH, Will Properties, PUBACK,
 * PUBREC, PUBREL, PUBCOMP, SUBSCRIBE, SUBACK, UNSUBSCRIBE, UNSUBACK,
 * DISCONNECT, AUTH.
 */
#define MQTT5_PROP_USER_PROPERTY 0x26U
/** @brief Four Byte Integer. CONNECT, CONNACK. */
#define MQTT5_PROP_MAXIMUM_PACKET_SIZE 0x27U
/** @brief Byte. CONNACK. */
#define MQTT5_PROP_WILDCARD_SUBSCRIPTION_AVAILABLE 0x28U
/** @brief Byte. CONNACK. */
#define MQTT5_PROP_SUBSCRIPTION_IDENTIFIER_AVAILABLE 0x29U
/** @brief Byte. CONNACK. */
#define MQTT5_PROP_SHARED_SUBSCRIPTION_AVAILABLE 0x2AU

#endif /* MQTT_V5_PROP_H */
