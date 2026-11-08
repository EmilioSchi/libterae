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
 * @file posix_wait_one.c
 * @brief Waiting for one socket with poll, keeping nothing between calls.
 *
 * POSIX has poll report a socket that has failed or hung up whatever it is
 * asked to wait for, but macOS reports neither unless the entry asks for some
 * event, so the interface promises it only for a wait to read.
 */

#include <limits.h>
#include <poll.h>

#include "base_platform.h"

bool mqtt_platform_wait_socket(int32_t socket, bool read, bool write,
                               uint32_t max_wait_ms, mqtt_wait_event *event)
{
    /** @brief The entry given to poll. */
    struct pollfd polled;
    /** @brief How long poll may wait, as it takes it. */
    const int timeout = (max_wait_ms < (uint32_t)INT_MAX) ? (int)max_wait_ms :
                                                            INT_MAX;
    /** @brief What happened on the socket. */
    short returned = 0;

    polled.fd = socket;
    polled.events = (short)((read ? POLLIN : 0) | (write ? POLLOUT : 0));
    polled.revents = 0;
    if (poll(&polled, 1U, timeout) > 0) {
        returned = polled.revents;
    }
    event->tag = 0U;
    event->handle = 0U;
    event->readable = (returned & (POLLIN | POLLHUP | POLLERR)) != 0;
    event->writable = (returned & POLLOUT) != 0;
    event->hang_up = (returned & (POLLHUP | POLLERR | POLLNVAL)) != 0;
    return returned != 0;
}
