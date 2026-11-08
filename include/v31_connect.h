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
 * @file v31_connect.h
 * @brief CONNECT specific to MQTT 3.1.
 *
 * With the Clean session flag at 1, the server must forget everything it held
 * about the client and start the connection with no earlier state. It must also
 * drop that client's state on disconnect.
 *
 * The Will Message has a length in front of it like a string, but only its
 * bytes are published, so it has to be 7-bit ASCII.
 *
 * The other Connect flags are in base_connect.h.
 *
 * [V31] MQTT V3.1 Protocol Specification, IBM and Eurotech, 42 pages.
 * @li Cited: "Clean session flag", p.8; "Variable header", p.15;
 * "Will Message", p.17.
 * @li Source: public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html
 */
#ifndef MQTT_V31_CONNECT_H
#define MQTT_V31_CONNECT_H

/** @brief Length of the Variable header in bytes. */
#define MQTT31_CONNECT_VARIABLE_HEADER 12U
/** @brief Clean session: Connect flags bit 1. */
#define MQTT31_CLEAN_SESSION           0x02U
/** @brief Highest byte value a Will Message can hold. */
#define MQTT31_WILL_MESSAGE_ASCII_MAX  0x7FU

#endif /* MQTT_V31_CONNECT_H */
