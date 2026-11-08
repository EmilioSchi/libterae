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
 * @file platform_wait.h
 * @brief What the waiting code of platform/ needs from a system, in one of two
 * ways.
 *
 * A system that waits on an array of entries, one for each socket, as poll
 * does, is used by platform_wait.c. That keeps the tier set and decides which
 * slots each wait covers, and the system's folder keeps one entry for each
 * slot, in slot order, and makes the calls. A slot's entry is written only
 * through these functions, so the entries can be of any type the system's call
 * takes.
 *
 * A system that keeps the set of sockets to watch itself and reports only
 * those that are ready, as epoll and kqueue do, is used by platform_wait_set.c.
 * Its sockets are known by their handles in the tier set, which every wait
 * covers whole.
 */
#ifndef MQTT_PLATFORM_WAIT_H
#define MQTT_PLATFORM_WAIT_H

#include <stdbool.h>
#include <stdint.h>

#include "base_platform.h"

/** @brief The call was interrupted by a signal, and can be made again. */
#define PLATFORM_WAIT_INTERRUPTED (-1)
/** @brief The system refuses that many entries in one call. */
#define PLATFORM_WAIT_REFUSED (-2)
/** @brief The call failed in some other way. */
#define PLATFORM_WAIT_FAILED (-3)

/** @brief The socket can be read, or it has failed or hung up. */
#define PLATFORM_WAIT_READABLE 0x01U
/** @brief The socket can be written. */
#define PLATFORM_WAIT_WRITABLE 0x02U
/** @brief The other end has gone, or the socket has failed. */
#define PLATFORM_WAIT_HANG_UP 0x04U
/** @brief The entry's socket is not open. */
#define PLATFORM_WAIT_INVALID 0x08U

/**
 * @brief The most sockets one wait of platform_wait_set.c reports, which is
 * what the system is asked for at a time.
 */
#define PLATFORM_WAIT_SET_BATCH \
    ((MQTT_PLATFORM_WAIT_SOCKETS < 1024U) ? MQTT_PLATFORM_WAIT_SOCKETS : 1024U)

/** @brief A socket a system that keeps the set found ready. */
typedef struct {
    /** @brief Its handle in the tier set. */
    uint32_t handle;
    /** @brief What was found, PLATFORM_WAIT_ bits. */
    uint32_t bits;
} platform_wait_fired;

/**
 * @brief Make the first entries wait for nothing, with no socket in them.
 * @param[in] count Number of entries, at most MQTT_PLATFORM_WAIT_SOCKETS.
 */
void platform_wait_clear(uint32_t count);

/**
 * @brief Follow a swap of two slots in the entries.
 * @param[in] first One slot.
 * @param[in] second The other.
 */
void platform_wait_swap(uint32_t first, uint32_t second);

/**
 * @brief Put a socket in a slot's entry, waiting to read.
 * @param[in] slot The slot.
 * @param[in] socket The socket.
 */
void platform_wait_place(uint32_t slot, int32_t socket);

/**
 * @brief Say what a slot's entry waits for.
 * @param[in] slot The slot.
 * @param[in] read Whether to wait for read readiness.
 * @param[in] write Whether to wait for write readiness.
 */
void platform_wait_events(uint32_t slot, bool read, bool write);

/**
 * @brief Wait for the entries of a run of slots, with one call.
 * @param[in] first The first slot.
 * @param[in] count Number of slots. With none, the call only lets the time
 * pass.
 * @param[in] timeout_ms How long the call may wait, in milliseconds, with
 * UINT32_MAX or anything beyond what the system takes meaning the longest it
 * takes.
 * @return Number of entries found ready, or PLATFORM_WAIT_INTERRUPTED,
 * PLATFORM_WAIT_REFUSED or PLATFORM_WAIT_FAILED.
 */
int32_t platform_wait_call(uint32_t first, uint32_t count,
                           uint32_t timeout_ms);

/**
 * @brief Find what the last call found on a slot's entry.
 * @param[in] slot The slot.
 * @return PLATFORM_WAIT_ bits, or zero if nothing.
 */
uint32_t platform_wait_returned(uint32_t slot);

/**
 * @brief Make the system's set of sockets to watch, empty. A set made before is
 * given up.
 * @return true if it could be made.
 */
bool platform_wait_set_open(void);

/**
 * @brief Start watching a socket, to read.
 * @param[in] handle Its handle in the tier set, below
 * MQTT_PLATFORM_WAIT_SOCKETS.
 * @param[in] socket The socket.
 * @return true, or false if the system refused it, as for a socket that isn't
 * open.
 */
bool platform_wait_set_add(uint32_t handle, int32_t socket);

/**
 * @brief Say what to watch a socket for.
 * @param[in] handle Its handle in the tier set.
 * @param[in] socket The socket.
 * @param[in] read Whether to watch for read readiness.
 * @param[in] write Whether to watch for write readiness.
 */
void platform_wait_set_interest(uint32_t handle, int32_t socket, bool read,
                                bool write);

/**
 * @brief Stop watching a socket. It may have been stopped already.
 * @param[in] handle Its handle in the tier set.
 * @param[in] socket The socket.
 */
void platform_wait_set_remove(uint32_t handle, int32_t socket);

/**
 * @brief Wait for some of the sockets watched to be ready, with one call.
 * @param[out] fired The sockets found ready, each at most once.
 * @param[in] capacity Number of entries fired can hold, at most
 * PLATFORM_WAIT_SET_BATCH.
 * @param[in] timeout_ms How long the call may wait, in milliseconds.
 * @return Number of sockets found ready, or PLATFORM_WAIT_INTERRUPTED or
 * PLATFORM_WAIT_FAILED.
 */
int32_t platform_wait_set_fired(platform_wait_fired *fired, uint32_t capacity,
                                uint32_t timeout_ms);

#endif /* MQTT_PLATFORM_WAIT_H */
