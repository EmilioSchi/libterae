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
 * @file v31_subscribe.h
 * @brief Requested QoS in the SUBSCRIBE payload.
 *
 * Each topic name is followed by one byte holding its Requested QoS. This
 * version of the protocol uses only the low 2 bits of that byte. If both of
 * those bits are set, the packet should be treated as invalid and the
 * connection closed.
 *
 * [V31] MQTT V3.1 Protocol Specification, IBM and Eurotech, 42 pages.
 * @li Cited: "Requested QoS", p.29.
 * @li Source: public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html
 */
#ifndef MQTT_V31_SUBSCRIBE_H
#define MQTT_V31_SUBSCRIBE_H

/** @brief QoS level: bits 1-0. */
#define MQTT31_REQUESTED_QOS_MASK 0x03U

#endif /* MQTT_V31_SUBSCRIBE_H */
