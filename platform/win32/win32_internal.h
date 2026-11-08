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
 * @file win32_internal.h
 * @brief What the files of win32/ share: Winsock's start, and the table that
 * gives each SOCKET the int32_t the interface names it by.
 */
#ifndef MQTT_WIN32_INTERNAL_H
#define MQTT_WIN32_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <winsock2.h>

/** @brief Start Winsock, the first time it is called. */
void win32_prepare(void);

/**
 * @brief Find the SOCKET of a handle.
 * @param[in] handle The handle.
 * @return The SOCKET, or INVALID_SOCKET if the handle has none.
 */
SOCKET win32_socket(int32_t handle);

/**
 * @brief Give a SOCKET a handle. A SOCKET that finds the table full is closed.
 * @param[in] socket The SOCKET.
 * @return Its handle, or MQTT_SOCKET_NONE.
 */
int32_t win32_adopt(SOCKET socket);

/**
 * @brief Find whether the last Winsock call only had to wait.
 * @return true for a call that would have blocked or was interrupted.
 */
bool win32_would_block(void);

/**
 * @brief Make a socket not block.
 * @param[in] socket The socket.
 */
void win32_no_block(SOCKET socket);

#endif /* MQTT_WIN32_INTERNAL_H */
