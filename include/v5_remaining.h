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
 * @file v5_remaining.h
 * @brief Remaining Length of packets with a fixed size or a short form.
 *
 * PINGREQ and PINGRESP have only one size. PUBACK, PUBREC, PUBREL, PUBCOMP,
 * DISCONNECT and AUTH can leave out the Reason Code and Property Length when
 * the Reason Code is 0x00 and there are no Properties, so a decoder has to
 * accept these shorter lengths and use the values that were left out: a Reason
 * Code of 0x00 and a Property Length of 0.
 *
 * Each _PROPERTIES value is the lowest length that holds a Property Length.
 * AUTH has no such value: it describes only the length of 0 and the full form,
 * and gives no rule for a length of 1.
 *
 * The encoding of this field is in base_remaining.h.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 3.4.2.1, p.63-64; 3.4.2.2.1, p.64; 3.5.2.1, p.65-66; 3.5.2.2.1,
 * p.66; 3.6.2.1, 3.6.2.2.1, p.68; 3.7.2.1, 3.7.2.2.1, p.70; 3.12.1, 3.13.1,
 * p.84; 3.14.2.1, p.86-87; 3.14.2.2.1, p.88; 3.15.2.1, 3.15.2.2.1, p.90.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */
#ifndef MQTT_V5_REMAINING_H
#define MQTT_V5_REMAINING_H

/** @brief No Variable Header or Payload. */
#define MQTT5_PINGREQ_REMAINING_LENGTH 0U
/** @brief No Variable Header or Payload. */
#define MQTT5_PINGRESP_REMAINING_LENGTH 0U
/** @brief Short form: Packet Identifier only. */
#define MQTT5_PUBACK_REMAINING_LENGTH_MIN 2U
/** @brief Lowest length that holds a Property Length. */
#define MQTT5_PUBACK_REMAINING_LENGTH_PROPERTIES 4U
/** @brief Short form: Packet Identifier only. */
#define MQTT5_PUBREC_REMAINING_LENGTH_MIN 2U
/** @brief Lowest length that holds a Property Length. */
#define MQTT5_PUBREC_REMAINING_LENGTH_PROPERTIES 4U
/** @brief Short form: Packet Identifier only. */
#define MQTT5_PUBREL_REMAINING_LENGTH_MIN 2U
/** @brief Lowest length that holds a Property Length. */
#define MQTT5_PUBREL_REMAINING_LENGTH_PROPERTIES 4U
/** @brief Short form: Packet Identifier only. */
#define MQTT5_PUBCOMP_REMAINING_LENGTH_MIN 2U
/** @brief Lowest length that holds a Property Length. */
#define MQTT5_PUBCOMP_REMAINING_LENGTH_PROPERTIES 4U
/** @brief Short form: no Variable Header. */
#define MQTT5_DISCONNECT_REMAINING_LENGTH_MIN 0U
/** @brief Lowest length that holds a Reason Code. */
#define MQTT5_DISCONNECT_REMAINING_LENGTH_REASON_CODE 1U
/** @brief Lowest length that holds a Property Length. */
#define MQTT5_DISCONNECT_REMAINING_LENGTH_PROPERTIES 2U
/** @brief Short form: no Variable Header. */
#define MQTT5_AUTH_REMAINING_LENGTH_MIN 0U

#endif /* MQTT_V5_REMAINING_H */
