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
 * @file linux_epoll.c
 * @brief The set of sockets platform_wait_set.c watches, as an epoll instance.
 *
 * Each socket is registered with its handle in the tier set as its data, so an
 * event names the socket's handle directly. Nothing is edge-triggered, so a
 * socket that stays ready is reported again, as poll would report it. epoll
 * reports a hang-up or an error whatever a socket is watched for, as poll does
 * on Linux.
 */

#include <errno.h>
#include <limits.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "base_platform.h"
#include "platform_wait.h"

/** @brief The epoll instance, or -1 before it is made. */
static int epoll_handle = -1;
/** @brief The events of one call. */
static struct epoll_event epoll_events[PLATFORM_WAIT_SET_BATCH];

/**
 * @brief Register a socket, or change or end its registration.
 * @param[in] operation EPOLL_CTL_ADD, EPOLL_CTL_MOD or EPOLL_CTL_DEL.
 * @param[in] handle The socket's handle in the tier set.
 * @param[in] socket The socket.
 * @param[in] read Whether to watch for read readiness.
 * @param[in] write Whether to watch for write readiness.
 * @return true if epoll took it.
 */
static bool epoll_change(int operation, uint32_t handle, int32_t socket,
                         bool read, bool write)
{
    /** @brief What to watch for, and the handle to report it with. */
    struct epoll_event event;

    event.events = (read ? (uint32_t)EPOLLIN : 0U) |
                   (write ? (uint32_t)EPOLLOUT : 0U);
    event.data.u32 = handle;
    return epoll_ctl(epoll_handle, operation, socket, &event) == 0;
}

bool platform_wait_set_open(void)
{
    if (epoll_handle >= 0) {
        (void)close(epoll_handle);
    }
    epoll_handle = epoll_create1(0);
    return epoll_handle >= 0;
}

bool platform_wait_set_add(uint32_t handle, int32_t socket)
{
    return epoll_change(EPOLL_CTL_ADD, handle, socket, true, false);
}

void platform_wait_set_interest(uint32_t handle, int32_t socket, bool read,
                                bool write)
{
    (void)epoll_change(EPOLL_CTL_MOD, handle, socket, read, write);
}

void platform_wait_set_remove(uint32_t handle, int32_t socket)
{
    /* A socket that is already gone makes this fail, which changes nothing. */
    (void)epoll_change(EPOLL_CTL_DEL, handle, socket, false, false);
}

/**
 * @brief Find what one event says.
 * @param[in] returned The event's bits.
 * @return PLATFORM_WAIT_ bits.
 */
static uint32_t epoll_bits(uint32_t returned)
{
    /** @brief The answer. */
    uint32_t bits = 0U;

    if ((returned & (uint32_t)(EPOLLIN | EPOLLHUP | EPOLLERR)) != 0U) {
        bits |= PLATFORM_WAIT_READABLE;
    }
    if ((returned & (uint32_t)EPOLLOUT) != 0U) {
        bits |= PLATFORM_WAIT_WRITABLE;
    }
    if ((returned & (uint32_t)(EPOLLHUP | EPOLLERR)) != 0U) {
        bits |= PLATFORM_WAIT_HANG_UP;
    }
    return bits;
}

int32_t platform_wait_set_fired(platform_wait_fired *fired, uint32_t capacity,
                                uint32_t timeout_ms)
{
    /** @brief How long epoll_wait may wait, as it takes it. */
    const int timeout =
        (timeout_ms < (uint32_t)INT_MAX) ? (int)timeout_ms : INT_MAX;
    /** @brief Number of events. */
    const int found =
        epoll_wait(epoll_handle, epoll_events, (int)capacity, timeout);
    /** @brief Index of the event. */
    uint32_t index = 0U;

    while ((found > 0) && (index < (uint32_t)found)) {
        fired[index].handle = epoll_events[index].data.u32;
        fired[index].bits = epoll_bits(epoll_events[index].events);
        index += 1U;
    }
    return (found >= 0) ? (int32_t)found :
           (errno == EINTR) ? PLATFORM_WAIT_INTERRUPTED :
                              PLATFORM_WAIT_FAILED;
}
