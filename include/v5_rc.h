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
 * @file v5_rc.h
 * @brief Reason Codes.
 *
 * An unsigned byte reporting how an operation ended. Values below 0x80 mean it
 * succeeded, and values from 0x80 up mean it failed. Each Reason Code's comment
 * lists the packets, as in Table 2-6.
 *
 * Table 2-6 lists Bad authentication method for DISCONNECT, but Table 3-10 does
 * not, and a sender of DISCONNECT has to use a value from Table 3-10.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 2.4, Table 2-6, p.27-29; 3.14.2.1, Table 3-10, p.86-87.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */
#ifndef MQTT_V5_RC_H
#define MQTT_V5_RC_H

/** @brief Lowest Reason Code that indicates failure. */
#define MQTT5_RC_FAILURE_MIN 0x80U
/** @brief CONNACK, PUBACK, PUBREC, PUBREL, PUBCOMP, UNSUBACK, AUTH. */
#define MQTT5_RC_SUCCESS 0x00U
/** @brief DISCONNECT. */
#define MQTT5_RC_NORMAL_DISCONNECTION 0x00U
/** @brief SUBACK. */
#define MQTT5_RC_GRANTED_QOS_0 0x00U
/** @brief SUBACK. */
#define MQTT5_RC_GRANTED_QOS_1 0x01U
/** @brief SUBACK. */
#define MQTT5_RC_GRANTED_QOS_2 0x02U
/** @brief DISCONNECT. */
#define MQTT5_RC_DISCONNECT_WITH_WILL_MESSAGE 0x04U
/** @brief PUBACK, PUBREC. */
#define MQTT5_RC_NO_MATCHING_SUBSCRIBERS 0x10U
/** @brief UNSUBACK. */
#define MQTT5_RC_NO_SUBSCRIPTION_EXISTED 0x11U
/** @brief AUTH. */
#define MQTT5_RC_CONTINUE_AUTHENTICATION 0x18U
/** @brief AUTH. */
#define MQTT5_RC_RE_AUTHENTICATE 0x19U
/** @brief CONNACK, PUBACK, PUBREC, SUBACK, UNSUBACK, DISCONNECT. */
#define MQTT5_RC_UNSPECIFIED_ERROR 0x80U
/** @brief CONNACK, DISCONNECT. */
#define MQTT5_RC_MALFORMED_PACKET 0x81U
/** @brief CONNACK, DISCONNECT. */
#define MQTT5_RC_PROTOCOL_ERROR 0x82U
/** @brief CONNACK, PUBACK, PUBREC, SUBACK, UNSUBACK, DISCONNECT. */
#define MQTT5_RC_IMPLEMENTATION_SPECIFIC_ERROR 0x83U
/** @brief CONNACK. */
#define MQTT5_RC_UNSUPPORTED_PROTOCOL_VERSION 0x84U
/** @brief CONNACK. */
#define MQTT5_RC_CLIENT_IDENTIFIER_NOT_VALID 0x85U
/** @brief CONNACK. */
#define MQTT5_RC_BAD_USER_NAME_OR_PASSWORD 0x86U
/** @brief CONNACK, PUBACK, PUBREC, SUBACK, UNSUBACK, DISCONNECT. */
#define MQTT5_RC_NOT_AUTHORIZED 0x87U
/** @brief CONNACK. */
#define MQTT5_RC_SERVER_UNAVAILABLE 0x88U
/** @brief CONNACK, DISCONNECT. */
#define MQTT5_RC_SERVER_BUSY 0x89U
/** @brief CONNACK. */
#define MQTT5_RC_BANNED 0x8AU
/** @brief DISCONNECT. */
#define MQTT5_RC_SERVER_SHUTTING_DOWN 0x8BU
/** @brief CONNACK, DISCONNECT. */
#define MQTT5_RC_BAD_AUTHENTICATION_METHOD 0x8CU
/** @brief DISCONNECT. */
#define MQTT5_RC_KEEP_ALIVE_TIMEOUT 0x8DU
/** @brief DISCONNECT. */
#define MQTT5_RC_SESSION_TAKEN_OVER 0x8EU
/** @brief SUBACK, UNSUBACK, DISCONNECT. */
#define MQTT5_RC_TOPIC_FILTER_INVALID 0x8FU
/** @brief CONNACK, PUBACK, PUBREC, DISCONNECT. */
#define MQTT5_RC_TOPIC_NAME_INVALID 0x90U
/** @brief PUBACK, PUBREC, SUBACK, UNSUBACK. */
#define MQTT5_RC_PACKET_IDENTIFIER_IN_USE 0x91U
/** @brief PUBREL, PUBCOMP. */
#define MQTT5_RC_PACKET_IDENTIFIER_NOT_FOUND 0x92U
/** @brief DISCONNECT. */
#define MQTT5_RC_RECEIVE_MAXIMUM_EXCEEDED 0x93U
/** @brief DISCONNECT. */
#define MQTT5_RC_TOPIC_ALIAS_INVALID 0x94U
/** @brief CONNACK, DISCONNECT. */
#define MQTT5_RC_PACKET_TOO_LARGE 0x95U
/** @brief DISCONNECT. */
#define MQTT5_RC_MESSAGE_RATE_TOO_HIGH 0x96U
/** @brief CONNACK, PUBACK, PUBREC, SUBACK, DISCONNECT. */
#define MQTT5_RC_QUOTA_EXCEEDED 0x97U
/** @brief DISCONNECT. */
#define MQTT5_RC_ADMINISTRATIVE_ACTION 0x98U
/** @brief CONNACK, PUBACK, PUBREC, DISCONNECT. */
#define MQTT5_RC_PAYLOAD_FORMAT_INVALID 0x99U
/** @brief CONNACK, DISCONNECT. */
#define MQTT5_RC_RETAIN_NOT_SUPPORTED 0x9AU
/** @brief CONNACK, DISCONNECT. */
#define MQTT5_RC_QOS_NOT_SUPPORTED 0x9BU
/** @brief CONNACK, DISCONNECT. */
#define MQTT5_RC_USE_ANOTHER_SERVER 0x9CU
/** @brief CONNACK, DISCONNECT. */
#define MQTT5_RC_SERVER_MOVED 0x9DU
/** @brief SUBACK, DISCONNECT. */
#define MQTT5_RC_SHARED_SUBSCRIPTIONS_NOT_SUPPORTED 0x9EU
/** @brief CONNACK, DISCONNECT. */
#define MQTT5_RC_CONNECTION_RATE_EXCEEDED 0x9FU
/** @brief DISCONNECT. */
#define MQTT5_RC_MAXIMUM_CONNECT_TIME 0xA0U
/** @brief SUBACK, DISCONNECT. */
#define MQTT5_RC_SUBSCRIPTION_IDENTIFIERS_NOT_SUPPORTED 0xA1U
/** @brief SUBACK, DISCONNECT. */
#define MQTT5_RC_WILDCARD_SUBSCRIPTIONS_NOT_SUPPORTED 0xA2U

#endif /* MQTT_V5_RC_H */
