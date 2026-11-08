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
 * @file v5_payload.h
 * @brief Payload Format Indicator values.
 *
 * Possible settings of the Payload Format Indicator property, used in PUBLISH
 * packets and in Will Properties.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 3.1.3.2.3, p.41; 3.3.2.3.2, p.56.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */
#ifndef MQTT_V5_PAYLOAD_H
#define MQTT_V5_PAYLOAD_H

/** @brief Payload is unspecified bytes. */
#define MQTT5_PAYLOAD_FORMAT_BYTES 0U
/** @brief Payload is UTF-8 text. */
#define MQTT5_PAYLOAD_FORMAT_UTF8  1U

#endif /* MQTT_V5_PAYLOAD_H */
