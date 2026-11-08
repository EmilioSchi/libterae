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
 * @file v5_vbi.h
 * @brief Variable Byte Integer.
 *
 * Held in a uint32_t. The encoding is the one in base_remaining.h. The encoding
 * MUST be as short as the value allows.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 1.5.5, Table 1-1, p.18.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */
#ifndef MQTT_V5_VBI_H
#define MQTT_V5_VBI_H

/** @brief Longest encoding of this field, in bytes. */
#define MQTT5_VBI_BYTES 4U
/** @brief 0xFF, 0xFF, 0xFF, 0x7F on the wire. */
#define MQTT5_VBI_MAX   268435455U

#endif /* MQTT_V5_VBI_H */
