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
 * @file freertos_select.c
 * @brief The set of sockets platform_wait_set.c watches, as a FreeRTOS+TCP
 * socket set, and the wait for one socket.
 *
 * FreeRTOS_select says only whether something happened, so after each call
 * the sockets watched are asked one by one, which is cheap for the few sockets
 * of a small system. Every socket is watched for its exceptions, which is how
 * FreeRTOS+TCP says a connection has gone, so that one isn't missed when
 * reading is off. A socket belongs to one set at a time, so the wait for one
 * socket uses a set of that socket's own.
 */

#include <stddef.h>

#include <FreeRTOS.h>
#include <FreeRTOS_IP.h>
#include <FreeRTOS_Sockets.h>

#include "base_platform.h"
#include "freertos_internal.h"
#include "platform_wait.h"

/** @brief The set the server's sockets are watched in, or NULL. */
static SocketSet_t select_set = NULL;
/** @brief For each handle of the tier set, its socket, or NULL. */
static Socket_t select_sockets[MQTT_PLATFORM_WAIT_SOCKETS];

bool platform_wait_set_open(void)
{
    /** @brief Index of the handle. */
    uint32_t handle = 0U;

    if (select_set != NULL) {
        FreeRTOS_DeleteSocketSet(select_set);
    }
    while (handle < MQTT_PLATFORM_WAIT_SOCKETS) {
        select_sockets[handle] = NULL;
        handle += 1U;
    }
    select_set = FreeRTOS_CreateSocketSet();
    return select_set != NULL;
}

bool platform_wait_set_add(uint32_t handle, int32_t socket)
{
    /** @brief The socket. */
    const Socket_t found = freertos_socket(socket);

    if (found != NULL) {
        select_sockets[handle] = found;
        FreeRTOS_FD_SET(
            found, select_set,
            (EventBits_t)eSELECT_READ | (EventBits_t)eSELECT_EXCEPT);
    }
    return found != NULL;
}

void platform_wait_set_interest(uint32_t handle, int32_t socket, bool read,
                                bool write)
{
    /** @brief The socket. */
    const Socket_t found = select_sockets[handle];

    (void)socket;
    if (found != NULL) {
        FreeRTOS_FD_CLR(found, select_set,
                        (EventBits_t)eSELECT_READ | (EventBits_t)eSELECT_WRITE);
        FreeRTOS_FD_SET(found, select_set,
                        (read ? (EventBits_t)eSELECT_READ : 0U) |
                            (write ? (EventBits_t)eSELECT_WRITE : 0U) |
                            (EventBits_t)eSELECT_EXCEPT);
    }
}

void platform_wait_set_remove(uint32_t handle, int32_t socket)
{
    /** @brief The socket. */
    const Socket_t found = select_sockets[handle];

    (void)socket;
    if (found != NULL) {
        FreeRTOS_FD_CLR(found, select_set, (EventBits_t)eSELECT_ALL);
        select_sockets[handle] = NULL;
    }
}

/**
 * @brief Turn what FreeRTOS_FD_ISSET found into PLATFORM_WAIT_ bits.
 * @param[in] found What it found.
 * @return The bits.
 */
static uint32_t select_bits(EventBits_t found)
{
    /** @brief The answer. */
    uint32_t bits = 0U;

    if ((found & (EventBits_t)eSELECT_READ) != 0U) {
        bits |= PLATFORM_WAIT_READABLE;
    }
    if ((found & (EventBits_t)eSELECT_WRITE) != 0U) {
        bits |= PLATFORM_WAIT_WRITABLE;
    }
    if ((found & (EventBits_t)eSELECT_EXCEPT) != 0U) {
        bits |= PLATFORM_WAIT_READABLE | PLATFORM_WAIT_HANG_UP;
    }
    return bits;
}

int32_t platform_wait_set_fired(platform_wait_fired *fired, uint32_t capacity,
                                uint32_t timeout_ms)
{
    /** @brief Number of sockets found. */
    uint32_t count = 0U;
    /** @brief Index of the handle. */
    uint32_t handle = 0U;

    freertos_reap();
    if (FreeRTOS_select(select_set, freertos_ticks(timeout_ms)) != 0) {
        while ((handle < MQTT_PLATFORM_WAIT_SOCKETS) && (count < capacity)) {
            if (select_sockets[handle] != NULL) {
                /** @brief What was found on it. */
                const uint32_t bits = select_bits(
                    FreeRTOS_FD_ISSET(select_sockets[handle], select_set));

                if (bits != 0U) {
                    fired[count].handle = handle;
                    fired[count].bits = bits;
                    count += 1U;
                }
            }
            handle += 1U;
        }
    }
    return (int32_t)count;
}

bool mqtt_platform_wait_socket(int32_t socket, bool read, bool write,
                               uint32_t max_wait_ms, mqtt_wait_event *event)
{
    /** @brief The socket. */
    const Socket_t found = freertos_socket(socket);
    /** @brief The set it waits in alone. */
    const SocketSet_t set = freertos_own_set(socket);
    /** @brief What was found. */
    uint32_t bits = 0U;

    if ((found != NULL) && (set != NULL)) {
        FreeRTOS_FD_CLR(found, set, (EventBits_t)eSELECT_ALL);
        FreeRTOS_FD_SET(found, set,
                        (read ? (EventBits_t)eSELECT_READ : 0U) |
                            (write ? (EventBits_t)eSELECT_WRITE : 0U) |
                            (EventBits_t)eSELECT_EXCEPT);
        if (FreeRTOS_select(set, freertos_ticks(max_wait_ms)) != 0) {
            bits = select_bits(FreeRTOS_FD_ISSET(found, set));
        }
    }
    event->tag = 0U;
    event->handle = 0U;
    event->readable = (bits & PLATFORM_WAIT_READABLE) != 0U;
    event->writable = (bits & PLATFORM_WAIT_WRITABLE) != 0U;
    event->hang_up = (bits & PLATFORM_WAIT_HANG_UP) != 0U;
    return bits != 0U;
}
