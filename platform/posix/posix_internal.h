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
 * @file posix_internal.h
 * @brief What the files of posix/ share, and what linux/ and macos/ may give
 * in their own way when they take the place of a file of posix/.
 */
#ifndef MQTT_POSIX_INTERNAL_H
#define MQTT_POSIX_INTERNAL_H

#include <stdint.h>

/**
 * @brief Keep writing to a closed socket from ending the program, and open
 * what wakes a wait when a stop is asked for, the first time it is called.
 */
void posix_prepare(void);

/**
 * @brief Make a descriptor not block.
 * @param[in] handle The descriptor.
 */
void posix_no_block(int handle);

/**
 * @brief Set the options every connected socket uses.
 * @param[in] handle The socket.
 */
void posix_socket_options(int32_t handle);

/**
 * @brief Set the options of each end of a pair of sockets.
 * @param[in] handle The end.
 */
void posix_pair_options(int32_t handle);

#endif /* MQTT_POSIX_INTERNAL_H */
