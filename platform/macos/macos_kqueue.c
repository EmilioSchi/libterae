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
 * @file macos_kqueue.c
 * @brief The set of sockets platform_wait_set.c watches, as a kqueue.
 *
 * Each socket has a read filter and a write filter, enabled as it is asked to
 * wait to read or to write, and neither clears itself, so a socket that stays
 * ready is reported again, as poll would report it. A filter that is disabled
 * reports nothing, not even an end of file, which is also what poll does on
 * macOS for an entry that asks for nothing.
 *
 * Each event carries a pointer to the socket's entry in a table indexed by its
 * handle, so the handle is found from the pointer with no conversion between
 * integers and pointers. One socket may give two events in one call, which are
 * put together.
 */

#include <errno.h>
#include <stddef.h>
#include <sys/event.h>
#include <time.h>
#include <unistd.h>

#include "base_platform.h"
#include "platform_wait.h"

/** @brief What the kqueue knows of one socket. */
typedef struct {
    /**
     * @brief The call that last reported the socket, to put its two events
     * together.
     */
    uint32_t call;
    /** @brief Where in the sockets found by that call it is. */
    uint32_t index;
} kqueue_entry;

/** @brief The kqueue, or -1 before it is made. */
static int kqueue_handle = -1;
/** @brief One entry for each handle of the tier set. */
static kqueue_entry kqueue_entries[MQTT_PLATFORM_WAIT_SOCKETS];
/** @brief The events of one call. */
static struct kevent kqueue_events[PLATFORM_WAIT_SET_BATCH];
/** @brief Number of calls made, to tell one call's entries from another's. */
static uint32_t kqueue_calls = 0U;

/**
 * @brief Change a socket's two filters.
 * @param[in] handle The socket's handle in the tier set.
 * @param[in] socket The socket.
 * @param[in] read_flags The flags for the read filter.
 * @param[in] write_flags The flags for the write filter.
 * @return true if the kqueue took both.
 */
static bool kqueue_change(uint32_t handle, int32_t socket, uint16_t read_flags,
                          uint16_t write_flags)
{
    /** @brief The changes. */
    struct kevent changes[2];

    EV_SET(&changes[0], (uintptr_t)socket, EVFILT_READ, read_flags, 0U, 0,
           &kqueue_entries[handle]);
    EV_SET(&changes[1], (uintptr_t)socket, EVFILT_WRITE, write_flags, 0U, 0,
           &kqueue_entries[handle]);
    return kevent(kqueue_handle, changes, 2, NULL, 0, NULL) == 0;
}

bool platform_wait_set_open(void)
{
    if (kqueue_handle >= 0) {
        (void)close(kqueue_handle);
    }
    kqueue_handle = kqueue();
    return kqueue_handle >= 0;
}

bool platform_wait_set_add(uint32_t handle, int32_t socket)
{
    kqueue_entries[handle].call = 0U;
    kqueue_entries[handle].index = 0U;
    return kqueue_change(handle, socket, EV_ADD | EV_ENABLE,
                         EV_ADD | EV_DISABLE);
}

void platform_wait_set_interest(uint32_t handle, int32_t socket, bool read,
                                bool write)
{
    (void)kqueue_change(handle, socket, read ? EV_ENABLE : EV_DISABLE,
                        write ? EV_ENABLE : EV_DISABLE);
}

void platform_wait_set_remove(uint32_t handle, int32_t socket)
{
    /* A filter that is already gone makes this fail, which changes nothing. */
    (void)kqueue_change(handle, socket, EV_DELETE, EV_DELETE);
}

/**
 * @brief Find what one event says.
 * @param[in] event The event.
 * @return PLATFORM_WAIT_ bits.
 */
static uint32_t kqueue_bits(const struct kevent *event)
{
    /** @brief The answer. */
    uint32_t bits = 0U;

    if ((event->flags & EV_ERROR) != 0U) {
        bits = PLATFORM_WAIT_READABLE | PLATFORM_WAIT_HANG_UP;
        if (event->data == (intptr_t)EBADF) {
            bits |= PLATFORM_WAIT_INVALID;
        }
    } else if (event->filter == EVFILT_READ) {
        bits = PLATFORM_WAIT_READABLE;
    } else {
        bits = PLATFORM_WAIT_WRITABLE;
    }
    if ((event->flags & EV_EOF) != 0U) {
        bits |= PLATFORM_WAIT_HANG_UP;
    }
    return bits;
}

int32_t platform_wait_set_fired(platform_wait_fired *fired, uint32_t capacity,
                                uint32_t timeout_ms)
{
    /** @brief How long kevent may wait. */
    const struct timespec timeout = {
        (time_t)(timeout_ms / 1000U), (long)(timeout_ms % 1000U) * 1000000L};
    /** @brief Number of events. */
    const int found = kevent(kqueue_handle, NULL, 0, kqueue_events,
                             (int)capacity, &timeout);
    /** @brief Number of sockets found. */
    uint32_t count = 0U;
    /** @brief Index of the event. */
    uint32_t index = 0U;

    kqueue_calls += 1U;
    while ((found > 0) && (index < (uint32_t)found)) {
        /** @brief The socket's entry. */
        kqueue_entry *entry = kqueue_events[index].udata;

        if ((entry->call == kqueue_calls) && (entry->index < count)) {
            fired[entry->index].bits |= kqueue_bits(&kqueue_events[index]);
        } else {
            entry->call = kqueue_calls;
            entry->index = count;
            fired[count].handle = (uint32_t)(entry - kqueue_entries);
            fired[count].bits = kqueue_bits(&kqueue_events[index]);
            count += 1U;
        }
        index += 1U;
    }
    return (found >= 0) ? (int32_t)count :
           (errno == EINTR) ? PLATFORM_WAIT_INTERRUPTED :
                              PLATFORM_WAIT_FAILED;
}
