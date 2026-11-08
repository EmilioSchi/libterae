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
 * @file ucos3_select.c
 * @brief The entries of platform_wait.c, as uC/TCP-IP's NetSock_Sel takes
 * them, and the wait for one socket.
 *
 * NetSock_Sel takes sets of socket numbers, as select does, so each call makes
 * the sets from the entries of the slots it covers, and reads each entry's
 * outcome back from them. Every socket is watched for errors, which is how a
 * connection that has gone is found when reading is off.
 */

#include <stddef.h>

#include <net_err.h>
#include <net_sock.h>

#include "base_platform.h"
#include "platform_wait.h"
#include "ucos3_internal.h"

/** @brief Number of sockets each word of a set holds. */
#define SELECT_WORD_BITS ((uint32_t)sizeof(CPU_DATA) * 8U)

/** @brief The entry of one slot. */
typedef struct {
    /** @brief The socket, or MQTT_SOCKET_NONE. */
    int32_t socket;
    /** @brief Whether to wait for read readiness. */
    bool read;
    /** @brief Whether to wait for write readiness. */
    bool write;
    /** @brief What the last call found, PLATFORM_WAIT_ bits. */
    uint32_t returned;
} select_entry;

/** @brief The entries, in slot order. */
static select_entry select_entries[MQTT_PLATFORM_WAIT_SOCKETS];

/**
 * @brief Mark a socket in a set. The set's own macros compare the socket with
 * an unsigned bound, which a signed handle can't be compared with cleanly.
 * @param[in,out] set The set.
 * @param[in] socket The socket, which is not negative.
 */
static void select_mark(NET_SOCK_DESC *set, int32_t socket)
{
    /** @brief The socket's number. */
    const uint32_t number = (uint32_t)socket;

    set->SockID_DescNbrSet[number / SELECT_WORD_BITS] |=
        (CPU_DATA)((CPU_DATA)1U << (number % SELECT_WORD_BITS));
}

/**
 * @brief Find whether a socket is marked in a set.
 * @param[in] set The set.
 * @param[in] socket The socket, which is not negative.
 * @return true if it is.
 */
static bool select_marked(const NET_SOCK_DESC *set, int32_t socket)
{
    /** @brief The socket's number. */
    const uint32_t number = (uint32_t)socket;

    return (set->SockID_DescNbrSet[number / SELECT_WORD_BITS] &
            (CPU_DATA)((CPU_DATA)1U << (number % SELECT_WORD_BITS))) != 0U;
}

void platform_wait_clear(uint32_t count)
{
    /** @brief Index of the entry. */
    uint32_t index = 0U;

    while ((index < count) && (index < MQTT_PLATFORM_WAIT_SOCKETS)) {
        select_entries[index].socket = MQTT_SOCKET_NONE;
        select_entries[index].read = false;
        select_entries[index].write = false;
        select_entries[index].returned = 0U;
        index += 1U;
    }
}

void platform_wait_swap(uint32_t first, uint32_t second)
{
    /** @brief The entry in the first slot. */
    const select_entry kept = select_entries[first];

    select_entries[first] = select_entries[second];
    select_entries[second] = kept;
}

void platform_wait_place(uint32_t slot, int32_t socket)
{
    select_entries[slot].socket = socket;
    select_entries[slot].read = true;
    select_entries[slot].write = false;
    select_entries[slot].returned = 0U;
}

void platform_wait_events(uint32_t slot, bool read, bool write)
{
    select_entries[slot].read = read;
    select_entries[slot].write = write;
}

/**
 * @brief Wait with NetSock_Sel on a run of entries, and write what it found
 * into them.
 * @param[in,out] entries The entries.
 * @param[in] count Number of entries.
 * @param[in] timeout_ms How long to wait, in milliseconds.
 * @return Number of entries found ready, or PLATFORM_WAIT_FAILED.
 */
static int32_t select_entries_wait(select_entry *entries, uint32_t count,
                                   uint32_t timeout_ms)
{
    /** @brief The sockets to read, to write, and to check for errors. */
    NET_SOCK_DESC sets[3];
    /** @brief How long to wait. */
    NET_SOCK_TIMEOUT timeout;
    /** @brief What the call said. */
    NET_ERR error = NET_SOCK_ERR_NONE;
    /** @brief One more than the highest socket. */
    NET_SOCK_QTY highest = 0U;
    /** @brief Index of the entry. */
    uint32_t index = 0U;
    /** @brief Number of entries found ready. */
    int32_t ready = 0;

    NET_SOCK_DESC_INIT(&sets[0]);
    NET_SOCK_DESC_INIT(&sets[1]);
    NET_SOCK_DESC_INIT(&sets[2]);
    while (index < count) {
        /** @brief The socket. */
        const int32_t socket = entries[index].socket;

        entries[index].returned = 0U;
        if (socket >= 0) {
            if (entries[index].read) {
                select_mark(&sets[0], socket);
            }
            if (entries[index].write) {
                select_mark(&sets[1], socket);
            }
            select_mark(&sets[2], socket);
            if ((NET_SOCK_QTY)(socket + 1) > highest) {
                highest = (NET_SOCK_QTY)(socket + 1);
            }
        }
        index += 1U;
    }
    timeout.timeout_sec = (CPU_INT32S)(timeout_ms / 1000U);
    timeout.timeout_us = (CPU_INT32S)((timeout_ms % 1000U) * 1000U);
    if (highest == 0U) {
        mqtt_platform_sleep_ms(timeout_ms);
    } else if (NetSock_Sel(highest, &sets[0], &sets[1], &sets[2], &timeout,
                           &error) < 0) {
        ready = (error == NET_SOCK_ERR_TIMEOUT) ? 0 : PLATFORM_WAIT_FAILED;
    } else {
        index = 0U;
        while (index < count) {
            /** @brief The socket. */
            const int32_t socket = entries[index].socket;

            if (socket >= 0) {
                if (select_marked(&sets[0], socket)) {
                    entries[index].returned |= PLATFORM_WAIT_READABLE;
                }
                if (select_marked(&sets[1], socket)) {
                    entries[index].returned |= PLATFORM_WAIT_WRITABLE;
                }
                if (select_marked(&sets[2], socket)) {
                    entries[index].returned |=
                        PLATFORM_WAIT_READABLE | PLATFORM_WAIT_HANG_UP;
                }
                if (entries[index].returned != 0U) {
                    ready += 1;
                }
            }
            index += 1U;
        }
    }
    return ready;
}

int32_t platform_wait_call(uint32_t first, uint32_t count,
                           uint32_t timeout_ms)
{
    return select_entries_wait(&select_entries[first], count, timeout_ms);
}

uint32_t platform_wait_returned(uint32_t slot)
{
    return select_entries[slot].returned;
}

bool mqtt_platform_wait_socket(int32_t socket, bool read, bool write,
                               uint32_t max_wait_ms, mqtt_wait_event *event)
{
    /** @brief The socket's own entry. */
    select_entry entry;

    entry.socket = socket;
    entry.read = read;
    entry.write = write;
    entry.returned = 0U;
    (void)select_entries_wait(&entry, 1U, max_wait_ms);
    event->tag = 0U;
    event->handle = 0U;
    event->readable = (entry.returned & PLATFORM_WAIT_READABLE) != 0U;
    event->writable = (entry.returned & PLATFORM_WAIT_WRITABLE) != 0U;
    event->hang_up = (entry.returned & PLATFORM_WAIT_HANG_UP) != 0U;
    return entry.returned != 0U;
}
