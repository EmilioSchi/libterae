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
 * @file v31_flags.h
 * @brief Fixed header flags other than PUBLISH.
 *
 * PUBREL, SUBSCRIBE and UNSUBSCRIBE carry QoS level 1, since each expects a
 * reply. These values are for the first send: a retry also sets MQTT_DUP, so a
 * receiver has to allow that bit as well. Other packets don't use the flags,
 * and unused bits should be 0. This version gives no rule for a receiver that
 * gets anything else. PUBLISH flags are in base_flags.h.
 *
 * [V31] MQTT V3.1 Protocol Specification, IBM and Eurotech, 42 pages.
 * @li Cited: "DUP", p.5; "bits marked as unused", p.14;
 * "PUBREL messages use QoS level 1", p.24;
 * "SUBSCRIBE messages use QoS level 1", p.27;
 * "UNSUBSCRIBE messages use QoS level 1", p.31.
 * @li Source: public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html
 */
#ifndef MQTT_V31_FLAGS_H
#define MQTT_V31_FLAGS_H

/** @brief All packets not listed below, other than PUBLISH. */
#define MQTT31_UNUSED_FLAGS      0x00U
/** @brief QoS level 1: 0, 0, 1, 0. */
#define MQTT31_PUBREL_FLAGS      0x02U
/** @brief QoS level 1: 0, 0, 1, 0. */
#define MQTT31_SUBSCRIBE_FLAGS   0x02U
/** @brief QoS level 1: 0, 0, 1, 0. */
#define MQTT31_UNSUBSCRIBE_FLAGS 0x02U

#endif /* MQTT_V31_FLAGS_H */
