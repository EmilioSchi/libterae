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
 * @file win32_poll.c
 * @brief The entries of platform_wait.c, as Winsock's WSAPoll takes them, and
 * the wait for one socket.
 *
 * WSAPoll is poll's twin, with two differences that matter here. It refuses a
 * call in which no entry names a socket, so such a call only sleeps, as poll
 * would. And it has no limit on the number of entries below what memory
 * allows, so a refusal is a failure, not a limit to halve. Entries ask for
 * normal data only: WSAPoll refuses the flags for priority data.
 */

#include <limits.h>
#include <winsock2.h>

#include "base_platform.h"
#include "platform_wait.h"
#include "win32_internal.h"

/** @brief The entries given to WSAPoll, in slot order. */
static WSAPOLLFD poll_entries[MQTT_PLATFORM_WAIT_SOCKETS];

void platform_wait_clear(uint32_t count)
{
    /** @brief Index of the entry. */
    uint32_t index = 0U;

    while ((index < count) && (index < MQTT_PLATFORM_WAIT_SOCKETS)) {
        poll_entries[index].fd = INVALID_SOCKET;
        poll_entries[index].events = 0;
        poll_entries[index].revents = 0;
        index += 1U;
    }
}

void platform_wait_swap(uint32_t first, uint32_t second)
{
    /** @brief The entry in the first slot. */
    const WSAPOLLFD kept = poll_entries[first];

    poll_entries[first] = poll_entries[second];
    poll_entries[second] = kept;
}

void platform_wait_place(uint32_t slot, int32_t socket)
{
    poll_entries[slot].fd = win32_socket(socket);
    poll_entries[slot].events = POLLRDNORM;
    poll_entries[slot].revents = 0;
}

void platform_wait_events(uint32_t slot, bool read, bool write)
{
    poll_entries[slot].events =
        (SHORT)((read ? POLLRDNORM : 0) | (write ? POLLWRNORM : 0));
}

/**
 * @brief Find whether a run of entries names any socket.
 * @param[in] first The first slot.
 * @param[in] count Number of slots.
 * @return true if one does.
 */
static bool poll_any(uint32_t first, uint32_t count)
{
    /** @brief The answer. */
    bool any = false;
    /** @brief Index of the slot. */
    uint32_t slot = first;

    while ((!any) && (slot < (first + count))) {
        any = (poll_entries[slot].fd != INVALID_SOCKET);
        slot += 1U;
    }
    return any;
}

/**
 * @brief Find how long WSAPoll may wait, as it takes it.
 * @param[in] timeout_ms How long, in milliseconds.
 * @return The same, or the longest it takes.
 */
static INT poll_timeout(uint32_t timeout_ms)
{
    return (timeout_ms < (uint32_t)INT_MAX) ? (INT)timeout_ms : INT_MAX;
}

int32_t platform_wait_call(uint32_t first, uint32_t count,
                           uint32_t timeout_ms)
{
    /** @brief The answer. */
    int32_t result = 0;

    if (poll_any(first, count)) {
        /** @brief What WSAPoll returned. */
        const int found = WSAPoll(&poll_entries[first], (ULONG)count,
                                  poll_timeout(timeout_ms));

        result = (found >= 0) ? (int32_t)found :
                 (WSAGetLastError() == WSAEINTR) ? PLATFORM_WAIT_INTERRUPTED :
                                                   PLATFORM_WAIT_FAILED;
    } else {
        Sleep((DWORD)poll_timeout(timeout_ms));
    }
    return result;
}

/**
 * @brief Turn what WSAPoll found into PLATFORM_WAIT_ bits.
 * @param[in] returned What it found.
 * @return The bits.
 */
static uint32_t poll_bits(SHORT returned)
{
    /** @brief The answer. */
    uint32_t bits = 0U;

    if ((returned & (POLLRDNORM | POLLHUP | POLLERR)) != 0) {
        bits |= PLATFORM_WAIT_READABLE;
    }
    if ((returned & POLLWRNORM) != 0) {
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

uint32_t platform_wait_returned(uint32_t slot)
{
    return poll_bits(poll_entries[slot].revents);
}

bool mqtt_platform_wait_socket(int32_t socket, bool read, bool write,
                               uint32_t max_wait_ms, mqtt_wait_event *event)
{
    /** @brief The entry given to WSAPoll. */
    WSAPOLLFD polled;
    /** @brief What was found. */
    uint32_t bits = 0U;

    polled.fd = win32_socket(socket);
    polled.events = (SHORT)((read ? POLLRDNORM : 0) | (write ? POLLWRNORM : 0));
    polled.revents = 0;
    if (WSAPoll(&polled, 1U, poll_timeout(max_wait_ms)) > 0) {
        bits = poll_bits(polled.revents);
    }
    event->tag = 0U;
    event->handle = 0U;
    event->readable = (bits & PLATFORM_WAIT_READABLE) != 0U;
    event->writable = (bits & PLATFORM_WAIT_WRITABLE) != 0U;
    event->hang_up = (bits & PLATFORM_WAIT_HANG_UP) != 0U;
    return bits != 0U;
}
