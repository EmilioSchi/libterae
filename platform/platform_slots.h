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
 * @file platform_slots.h
 * @brief A table of handles, for a system whose sockets are not small numbers.
 *
 * The interface names a socket by an int32_t. Winsock's SOCKET is as wide as a
 * pointer, and FreeRTOS+TCP's Socket_t is a pointer, so their folders keep
 * their sockets in a table and give out its indexes. This takes and gives back
 * the indexes, with a free list, in a constant number of steps. It is not safe
 * for two threads at once: a folder whose system has threads takes its own
 * lock.
 */
#ifndef MQTT_PLATFORM_SLOTS_H
#define MQTT_PLATFORM_SLOTS_H

#include <stdbool.h>
#include <stdint.h>

#include "base_platform.h"

#if !defined(MQTT_PLATFORM_SOCKETS)
/**
 * @brief Number of sockets a table holds, unless the build sets another: what
 * can be waited for, and room for the listeners, the wake socket and the pairs
 * the tests open.
 */
#define MQTT_PLATFORM_SOCKETS (MQTT_PLATFORM_WAIT_SOCKETS + 64U)
#endif

/** @brief The free list of a table's indexes. */
typedef struct {
    /** @brief For each free index, the next one, or MQTT_SOCKET_NONE. */
    int32_t next[MQTT_PLATFORM_SOCKETS];
    /** @brief The first free index, or MQTT_SOCKET_NONE. */
    int32_t head;
    /** @brief true once the list has been made. */
    bool ready;
} platform_slots;

/**
 * @brief Take a free index, making the list the first time.
 * @param[in,out] slots The list.
 * @return The index, or MQTT_SOCKET_NONE if the table is full.
 */
int32_t platform_slots_take(platform_slots *slots);

/**
 * @brief Give an index back.
 * @param[in,out] slots The list.
 * @param[in] index An index taken from it.
 */
void platform_slots_give(platform_slots *slots, int32_t index);

#endif /* MQTT_PLATFORM_SLOTS_H */
