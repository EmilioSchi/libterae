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
 * @file v5_subscribe.h
 * @brief Subscription Options byte in the SUBSCRIBE Payload.
 *
 * One Subscription Options byte comes after every Topic Filter. A value of 3 in
 * either Maximum QoS or Retain Handling is a Protocol Error. If a reserved bit
 * in the Payload is set, the Server MUST treat the SUBSCRIBE packet as
 * malformed.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 3.8.3.1, p.73.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */
#ifndef MQTT_V5_SUBSCRIBE_H
#define MQTT_V5_SUBSCRIBE_H

/** @brief Maximum QoS: bits 1-0. */
#define MQTT5_MAXIMUM_QOS_MASK 0x03U
/** @brief No Local: bit 2. */
#define MQTT5_NO_LOCAL 0x04U
/** @brief Retain As Published: bit 3. */
#define MQTT5_RETAIN_AS_PUBLISHED 0x08U
/** @brief Retain Handling: bits 5-4. */
#define MQTT5_RETAIN_HANDLING_MASK 0x30U
/** @brief Position of the lowest Retain Handling bit. */
#define MQTT5_RETAIN_HANDLING_POS 4U
/** @brief Reserved: bits 7-6. */
#define MQTT5_SUBSCRIPTION_OPTIONS_RESERVED 0xC0U

/** @brief Deliver retained messages when the subscription is made. */
#define MQTT5_RETAIN_HANDLING_0 0U
/** @brief Deliver them at that point only if the subscription is new. */
#define MQTT5_RETAIN_HANDLING_1 1U
/** @brief Deliver no retained messages when the subscription is made. */
#define MQTT5_RETAIN_HANDLING_2 2U
/** @brief Protocol Error. */
#define MQTT5_RETAIN_HANDLING_RESERVED 3U

#endif /* MQTT_V5_SUBSCRIBE_H */
