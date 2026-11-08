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
 * @file posix_poll.c
 * @brief The entries of platform_wait.c, as poll takes them.
 *
 * POSIX defines poll, so this is the same on every POSIX system. An entry with
 * a negative descriptor is passed over, which is how a cleared one waits for
 * nothing.
 */

#include <errno.h>
#include <limits.h>
#include <poll.h>

#include "base_platform.h"
#include "platform_wait.h"

/** @brief The entries given to poll, in slot order. */
static struct pollfd poll_entries[MQTT_PLATFORM_WAIT_SOCKETS];

void platform_wait_clear(uint32_t count)
{
    /** @brief Index of the entry. */
    uint32_t index = 0U;

    while ((index < count) && (index < MQTT_PLATFORM_WAIT_SOCKETS)) {
        poll_entries[index].fd = -1;
        poll_entries[index].events = 0;
        poll_entries[index].revents = 0;
        index += 1U;
    }
}

void platform_wait_swap(uint32_t first, uint32_t second)
{
    /** @brief The entry in the first slot. */
    const struct pollfd kept = poll_entries[first];

    poll_entries[first] = poll_entries[second];
    poll_entries[second] = kept;
}

void platform_wait_place(uint32_t slot, int32_t socket)
{
    poll_entries[slot].fd = socket;
    poll_entries[slot].events = POLLIN;
    poll_entries[slot].revents = 0;
}

void platform_wait_events(uint32_t slot, bool read, bool write)
{
    poll_entries[slot].events =
        (short)((read ? POLLIN : 0) | (write ? POLLOUT : 0));
}

int32_t platform_wait_call(uint32_t first, uint32_t count,
                           uint32_t timeout_ms)
{
    /** @brief How long poll may wait, as it takes it. */
    const int timeout =
        (timeout_ms < (uint32_t)INT_MAX) ? (int)timeout_ms : INT_MAX;
    /** @brief What poll returned. */
    const int found = poll(&poll_entries[first], (nfds_t)count, timeout);
    /** @brief The answer. */
    int32_t result = (int32_t)found;

    if (found < 0) {
        if (errno == EINTR) {
            result = PLATFORM_WAIT_INTERRUPTED;
        } else if (errno == EINVAL) {
            result = PLATFORM_WAIT_REFUSED;
        } else {
            result = PLATFORM_WAIT_FAILED;
        }
    }
    return result;
}

uint32_t platform_wait_returned(uint32_t slot)
{
    /** @brief What poll found. */
    const short returned = poll_entries[slot].revents;
    /** @brief The answer. */
    uint32_t bits = 0U;

    if ((returned & (POLLIN | POLLHUP | POLLERR)) != 0) {
        bits |= PLATFORM_WAIT_READABLE;
    }
    if ((returned & POLLOUT) != 0) {
        bits |= PLATFORM_WAIT_WRITABLE;
    }
    if ((returned & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
        bits |= PLATFORM_WAIT_HANG_UP;
    }
    if ((returned & POLLNVAL) != 0) {
        bits |= PLATFORM_WAIT_INVALID;
    }
    return bits;
}
