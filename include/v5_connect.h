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
 * @file v5_connect.h
 * @brief CONNECT specific to MQTT 5.0.
 *
 * When a CONNECT packet has Clean Start 1, Client and Server MUST each throw
 * away any existing Session and begin a new one.
 *
 * Unlike 3.x Clean Session, Clean Start controls only the start of a Session,
 * and the Session Expiry Interval controls its end. To get the MQTT 3.1.1
 * behavior of CleanSession 1, combine Clean Start 1 with a zero Session Expiry
 * Interval. For CleanSession 0, set Clean Start 0 and leave out the Session
 * Expiry Interval.
 *
 * The other Connect Flags are in base_connect.h.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 3.1.2.4, p.32; 3.1.2.11.2, p.35.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */
#ifndef MQTT_V5_CONNECT_H
#define MQTT_V5_CONNECT_H

/** @brief Clean Start: Connect Flags bit 1. */
#define MQTT5_CLEAN_START 0x02U

#endif /* MQTT_V5_CONNECT_H */
