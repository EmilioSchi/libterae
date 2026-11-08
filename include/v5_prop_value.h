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
 * @file v5_prop_value.h
 * @brief Property values when absent, and the range of values allowed.
 *
 * A receiver uses the default value when a Property is not in the packet.
 * Session Expiry Interval differs by packet: its default applies to CONNECT
 * only, and when it is absent from CONNACK or DISCONNECT the value from CONNECT
 * stays in force. Maximum QoS can only be sent as 0 or 1, so its default of 2
 * never appears in a packet.
 *
 * Absence has a meaning but no number for three Properties. With no Maximum
 * Packet Size, only the limits of the protocol apply. With no Message Expiry
 * Interval, the Application Message does not expire. With no Server Keep Alive,
 * the Keep Alive the Client sent applies.
 *
 * Other special values are in v5_session.h and v5_payload.h.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 3.1.2.11.2, p.35; 3.1.2.11.3, 3.1.2.11.4, p.36; 3.1.2.11.5,
 * 3.1.2.11.6, 3.1.2.11.7, p.37; 3.1.3.2.2, 3.1.3.2.3, p.41; 3.2.2.3.2,
 * 3.2.2.3.3, 3.2.2.3.4, p.48; 3.2.2.3.5, 3.2.2.3.6, p.49; 3.2.2.3.8,
 * 3.2.2.3.11, p.50; 3.2.2.3.12, 3.2.2.3.13, 3.2.2.3.14, p.51; 3.3.2.3.2,
 * 3.3.2.3.3, p.56; 3.3.2.3.4, p.57; 3.3.2.3.8, p.59; 3.8.2.1.2, p.72;
 * 3.14.2.2.2, p.88.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */
#ifndef MQTT_V5_PROP_VALUE_H
#define MQTT_V5_PROP_VALUE_H

/** @brief Session Expiry Interval absent from CONNECT, in seconds. */
#define MQTT5_SESSION_EXPIRY_INTERVAL_DEFAULT 0U
/** @brief Receive Maximum absent from CONNECT or CONNACK. */
#define MQTT5_RECEIVE_MAXIMUM_DEFAULT 65535U
/**
 * @brief Topic Alias Maximum absent from CONNECT or CONNACK: no Topic Aliases
 * are accepted.
 */
#define MQTT5_TOPIC_ALIAS_MAXIMUM_DEFAULT 0U
/** @brief Request Response Information absent from CONNECT. */
#define MQTT5_REQUEST_RESPONSE_INFORMATION_DEFAULT 0U
/** @brief Request Problem Information absent from CONNECT. */
#define MQTT5_REQUEST_PROBLEM_INFORMATION_DEFAULT 1U
/** @brief Will Delay Interval absent from the Will Properties, in seconds. */
#define MQTT5_WILL_DELAY_INTERVAL_DEFAULT 0U
/**
 * @brief Payload Format Indicator absent: the same as
 * MQTT5_PAYLOAD_FORMAT_BYTES.
 */
#define MQTT5_PAYLOAD_FORMAT_INDICATOR_DEFAULT 0U
/** @brief Maximum QoS absent from CONNACK. */
#define MQTT5_MAXIMUM_QOS_DEFAULT 2U
/**
 * @brief Retain Available absent from CONNACK: retained messages are supported.
 */
#define MQTT5_RETAIN_AVAILABLE_DEFAULT 1U
/**
 * @brief Wildcard Subscription Available absent from CONNACK: they are
 * supported.
 */
#define MQTT5_WILDCARD_SUBSCRIPTION_AVAILABLE_DEFAULT 1U
/**
 * @brief Subscription Identifier Available absent from CONNACK: they are
 * supported.
 */
#define MQTT5_SUBSCRIPTION_IDENTIFIER_AVAILABLE_DEFAULT 1U
/**
 * @brief Shared Subscription Available absent from CONNACK: they are supported.
 */
#define MQTT5_SHARED_SUBSCRIPTION_AVAILABLE_DEFAULT 1U
/** @brief Lowest Receive Maximum: 0 is a Protocol Error. */
#define MQTT5_RECEIVE_MAXIMUM_MIN 1U
/** @brief Lowest Maximum Packet Size: 0 is a Protocol Error. */
#define MQTT5_MAXIMUM_PACKET_SIZE_MIN 1U
/** @brief Lowest Topic Alias: 0 is not permitted. */
#define MQTT5_TOPIC_ALIAS_MIN 1U
/** @brief Lowest Subscription Identifier: 0 is a Protocol Error. */
#define MQTT5_SUBSCRIPTION_IDENTIFIER_MIN 1U
/** @brief Highest Subscription Identifier. */
#define MQTT5_SUBSCRIPTION_IDENTIFIER_MAX 268435455U

#endif /* MQTT_V5_PROP_VALUE_H */
