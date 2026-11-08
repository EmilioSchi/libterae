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
 * @file freertos_socket.c
 * @brief Sockets, through FreeRTOS+TCP.
 *
 * Every socket is made not to block once it is open: its reads, writes and
 * accepts give up at once, and its waits are FreeRTOS_select's. Only a connect
 * waits, for up to FREERTOS_CONNECT_MS. Hosts are numeric IPv4 addresses, as
 * there is no name lookup without DNS, and a listener given NULL or a name
 * that isn't one takes every address.
 *
 * FreeRTOS+TCP reads into one place and writes one run at a time, so the reads
 * into several places and the writes of several runs are loops that stop at the
 * first that falls short.
 *
 * FreeRTOS_closesocket frees a socket at once, and sends nothing: a connection
 * it ends is never ended for the peer. So a close shuts the connection down,
 * which sends a FIN, gives the handle back, and keeps the socket until its
 * connection has ended, or for FREERTOS_LINGER_MS at most, then closes it. A
 * write that moves nothing on a connection the peer has ended reports it
 * closed, as nothing more will drain it.
 *
 * FreeRTOS+TCP has no way to reset a connection, as a POSIX close of a socket
 * holding bytes it hasn't read does, and it takes no FIN while it has bytes of
 * its own still to send: so mqtt_platform_close_resets is false.
 */

#include <stddef.h>
#include <string.h>

#include <FreeRTOS.h>
#include <FreeRTOS_IP.h>
#include <FreeRTOS_Sockets.h>
#include <task.h>

#include "base_platform.h"
#include "freertos_internal.h"
#include "platform_slots.h"

/** @brief How long a connect may wait, in milliseconds. */
#define FREERTOS_CONNECT_MS 5000U
/** @brief Number of connections a listener holds before they are accepted. */
#define FREERTOS_BACKLOG 8
/** @brief The highest port number. */
#define FREERTOS_PORT_MOST 65535U
/**
 * @brief The longest a closed socket is kept for its connection to end, in
 * milliseconds.
 */
#define FREERTOS_LINGER_MS 2000U

/** @brief The free indexes of the table. */
static platform_slots freertos_slots;
/** @brief The table: the socket of each handle. */
static Socket_t freertos_table[MQTT_PLATFORM_SOCKETS];
/** @brief For each handle, the set it waits in alone, or NULL. */
static SocketSet_t freertos_sets[MQTT_PLATFORM_SOCKETS];
/** @brief The sockets closed whose connections are still ending, or NULL. */
static Socket_t freertos_closing[MQTT_PLATFORM_SOCKETS];
/** @brief When each of them was closed, in ticks. */
static TickType_t freertos_closed_at[MQTT_PLATFORM_SOCKETS];

void freertos_reap(void)
{
    /** @brief The time now. */
    const TickType_t now = xTaskGetTickCount();
    /** @brief How long a socket may be kept. */
    const TickType_t linger = freertos_ticks(FREERTOS_LINGER_MS);
    /** @brief Index of the entry. */
    uint32_t index = 0U;

    while (index < MQTT_PLATFORM_SOCKETS) {
        /** @brief The socket kept there. */
        Socket_t socket = NULL;
        /** @brief When it was closed. */
        TickType_t closed = 0U;

        taskENTER_CRITICAL();
        socket = freertos_closing[index];
        closed = freertos_closed_at[index];
        taskEXIT_CRITICAL();
        if ((socket != NULL) &&
            ((FreeRTOS_issocketconnected(socket) == pdFALSE) ||
             ((TickType_t)(now - closed) >= linger))) {
            /** @brief true if this call took it off the list. */
            bool taken = false;

            taskENTER_CRITICAL();
            taken = (freertos_closing[index] == socket);
            if (taken) {
                freertos_closing[index] = NULL;
            }
            taskEXIT_CRITICAL();
            if (taken) {
                (void)FreeRTOS_closesocket(socket);
            }
        }
        index += 1U;
    }
}

Socket_t freertos_socket(int32_t handle)
{
    /** @brief The answer. */
    Socket_t found = NULL;

    if ((handle >= 0) && ((uint32_t)handle < MQTT_PLATFORM_SOCKETS)) {
        taskENTER_CRITICAL();
        found = freertos_table[handle];
        taskEXIT_CRITICAL();
    }
    return found;
}

int32_t freertos_adopt(Socket_t socket)
{
    /** @brief The handle. */
    int32_t handle = MQTT_SOCKET_NONE;

    taskENTER_CRITICAL();
    handle = platform_slots_take(&freertos_slots);
    if (handle != MQTT_SOCKET_NONE) {
        freertos_table[handle] = socket;
        freertos_sets[handle] = NULL;
    }
    taskEXIT_CRITICAL();
    if (handle == MQTT_SOCKET_NONE) {
        (void)FreeRTOS_closesocket(socket);
    }
    return handle;
}

TickType_t freertos_ticks(uint32_t milliseconds)
{
    /** @brief The ticks, rounded up. */
    const uint64_t ticks = (((uint64_t)milliseconds * configTICK_RATE_HZ) +
                            999U) /
                           1000U;

    return (ticks < (uint64_t)portMAX_DELAY) ? (TickType_t)ticks :
                                               portMAX_DELAY;
}

void freertos_no_block(Socket_t socket)
{
    /** @brief A wait that gives up at once. */
    const TickType_t none = 0U;

    (void)FreeRTOS_setsockopt(socket, 0, FREERTOS_SO_RCVTIMEO, &none,
                              sizeof(none));
    (void)FreeRTOS_setsockopt(socket, 0, FREERTOS_SO_SNDTIMEO, &none,
                              sizeof(none));
}

SocketSet_t freertos_own_set(int32_t handle)
{
    /** @brief The set. */
    SocketSet_t set = NULL;

    if ((handle >= 0) && ((uint32_t)handle < MQTT_PLATFORM_SOCKETS)) {
        set = freertos_sets[handle];
        if (set == NULL) {
            set = FreeRTOS_CreateSocketSet();
            freertos_sets[handle] = set;
        }
    }
    return set;
}

/**
 * @brief Read a host and port into an address.
 * @param[in] host A numeric IPv4 address, or NULL for every address.
 * @param[in] port The port, as text.
 * @param[out] address The address.
 * @return true if the port is valid.
 */
static bool freertos_address(const char *host, const char *port,
                             struct freertos_sockaddr *address)
{
    /** @brief The port's number. */
    uint32_t number = 0U;
    /** @brief The answer. */
    const bool valid = mqtt_platform_parse_u32(port, &number) &&
                       (number <= FREERTOS_PORT_MOST);

    (void)memset(address, 0, sizeof(*address));
    address->sin_len = (uint8_t)sizeof(*address);
    address->sin_family = (uint8_t)FREERTOS_AF_INET;
    address->sin_port = FreeRTOS_htons((uint16_t)number);
    address->sin_address.ulIP_IPv4 =
        (host != NULL) ? FreeRTOS_inet_addr(host) : 0U;
    return valid;
}

/**
 * @brief Open a TCP socket.
 * @return The socket, or NULL.
 */
static Socket_t freertos_open(void)
{
    /** @brief The socket. */
    Socket_t socket = NULL;

    freertos_reap();
    socket = FreeRTOS_socket(FREERTOS_AF_INET, FREERTOS_SOCK_STREAM,
                             FREERTOS_IPPROTO_TCP);
    if (socket == FREERTOS_INVALID_SOCKET) {
        socket = NULL;
    }
    return socket;
}

mqtt_status mqtt_platform_listen(const char *host, const char *port,
                                 int32_t *listener)
{
    /** @brief The address. */
    struct freertos_sockaddr address;
    /** @brief The socket. */
    Socket_t socket = NULL;
    /** @brief The outcome. */
    mqtt_status status = MQTT_CONNECTION_CLOSED;

    if (freertos_address(host, port, &address)) {
        socket = freertos_open();
    }
    if (socket != NULL) {
        freertos_no_block(socket);
        if ((FreeRTOS_bind(socket, &address, sizeof(address)) == 0) &&
            (FreeRTOS_listen(socket, FREERTOS_BACKLOG) == 0)) {
            *listener = freertos_adopt(socket);
            status = (*listener != MQTT_SOCKET_NONE) ? MQTT_OK :
                                                       MQTT_CONNECTION_CLOSED;
        } else {
            (void)FreeRTOS_closesocket(socket);
        }
    }
    return status;
}

void mqtt_platform_send_buffer(int32_t handle, uint32_t bytes)
{
    /** @brief The size, as FreeRTOS+TCP takes it. */
    const uint32_t size = bytes;

    (void)FreeRTOS_setsockopt(freertos_socket(handle), 0, FREERTOS_SO_SNDBUF,
                              &size, sizeof(size));
}

mqtt_status mqtt_platform_accept(int32_t listener, int32_t *handle)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_INCOMPLETE;
    /** @brief The listener. */
    const Socket_t listening = freertos_socket(listener);
    /** @brief The new socket, NULL if none is waiting. */
    const Socket_t accepted =
        (listening != NULL) ? FreeRTOS_accept(listening, NULL, NULL) : NULL;

    if (listening == NULL) {
        status = MQTT_CONNECTION_CLOSED;
    } else if ((accepted != NULL) && (accepted != FREERTOS_INVALID_SOCKET)) {
        freertos_no_block(accepted);
        *handle = freertos_adopt(accepted);
        status = (*handle != MQTT_SOCKET_NONE) ? MQTT_OK : MQTT_INCOMPLETE;
    } else {
        /* Nothing is waiting, or what was waiting has gone. */
    }
    return status;
}

mqtt_status mqtt_platform_connect(const char *host, const char *port,
                                  int32_t *handle)
{
    /** @brief The address. */
    struct freertos_sockaddr address;
    /** @brief How long the connect may wait. */
    const TickType_t wait = freertos_ticks(FREERTOS_CONNECT_MS);
    /** @brief The socket. */
    Socket_t socket = NULL;
    /** @brief The outcome. */
    mqtt_status status = MQTT_CONNECTION_CLOSED;

    if ((host != NULL) && freertos_address(host, port, &address)) {
        socket = freertos_open();
    }
    if (socket != NULL) {
        (void)FreeRTOS_setsockopt(socket, 0, FREERTOS_SO_RCVTIMEO, &wait,
                                  sizeof(wait));
        (void)FreeRTOS_setsockopt(socket, 0, FREERTOS_SO_SNDTIMEO, &wait,
                                  sizeof(wait));
        if (FreeRTOS_connect(socket, &address, sizeof(address)) == 0) {
            freertos_no_block(socket);
            *handle = freertos_adopt(socket);
            status = (*handle != MQTT_SOCKET_NONE) ? MQTT_OK :
                                                     MQTT_CONNECTION_CLOSED;
        } else {
            (void)FreeRTOS_closesocket(socket);
        }
    }
    return status;
}

/**
 * @brief Find what a read or write that took nothing means.
 * @param[in] result What FreeRTOS_recv or FreeRTOS_send returned: zero or a
 * negative error.
 * @return MQTT_INCOMPLETE if it only had to wait, or MQTT_CONNECTION_CLOSED.
 */
static mqtt_status freertos_failure(BaseType_t result)
{
    return ((result == 0) || (result == -pdFREERTOS_ERRNO_ENOSPC) ||
            (result == -pdFREERTOS_ERRNO_EWOULDBLOCK) ||
            (result == -pdFREERTOS_ERRNO_EINTR)) ?
               MQTT_INCOMPLETE :
               MQTT_CONNECTION_CLOSED;
}

mqtt_status mqtt_platform_read(int32_t handle, uint8_t *data, uint32_t capacity,
                               uint32_t *count)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief The socket. */
    const Socket_t socket = freertos_socket(handle);
    /** @brief Number of bytes read, or an error. */
    const BaseType_t result =
        (socket != NULL) ? FreeRTOS_recv(socket, data, (size_t)capacity, 0) :
                           -pdFREERTOS_ERRNO_ENOTCONN;

    if (result > 0) {
        *count = (uint32_t)result;
    } else {
        status = freertos_failure(result);
    }
    return status;
}

mqtt_status mqtt_platform_read_spaces(int32_t handle, const mqtt_space *spaces,
                                      uint32_t count, uint32_t *count_read)
{
    /** @brief Index of the place. */
    uint32_t place = 0U;
    /** @brief Number of bytes read. */
    uint32_t total = 0U;
    /** @brief The outcome of the last read. */
    mqtt_status status = MQTT_OK;
    /** @brief true while each place has been filled. */
    bool going = true;

    while (going && (place < count) && (place < MQTT_PLATFORM_READ_SPACES)) {
        /** @brief Number of bytes read into this place. */
        uint32_t got = 0U;

        status = mqtt_platform_read(handle, spaces[place].data,
                                    spaces[place].length, &got);
        if (status == MQTT_OK) {
            total += got;
            going = (got == spaces[place].length);
        } else {
            going = false;
        }
        place += 1U;
    }
    /* Bytes that came before a failure are handed on, and the next read
     * finds it again. */
    if (total > 0U) {
        status = MQTT_OK;
    }
    *count_read = total;
    return status;
}

mqtt_status mqtt_platform_write(int32_t handle, const uint8_t *data,
                                uint32_t length, uint32_t *count)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief The socket. */
    const Socket_t socket = freertos_socket(handle);
    /** @brief Number of bytes taken, or an error. */
    const BaseType_t result =
        (socket != NULL) ? FreeRTOS_send(socket, data, (size_t)length, 0) :
                           -pdFREERTOS_ERRNO_ENOTCONN;

    if (result > 0) {
        *count = (uint32_t)result;
    } else if ((result == 0) && (length == 0U)) {
        *count = 0U;
    } else {
        status = freertos_failure(result);
        if ((status == MQTT_INCOMPLETE) &&
            (FreeRTOS_issocketconnected(socket) == pdFALSE)) {
            status = MQTT_CONNECTION_CLOSED;
        }
    }
    return status;
}

mqtt_status mqtt_platform_write_runs(int32_t handle, const mqtt_run *runs,
                                     uint32_t count, uint32_t *count_taken)
{
    /** @brief Index of the run. */
    uint32_t run = 0U;
    /** @brief Number of bytes taken. */
    uint32_t total = 0U;
    /** @brief The outcome of the last write. */
    mqtt_status status = MQTT_OK;
    /** @brief true while each run has been taken whole. */
    bool going = true;

    while (going && (run < count) && (run < MQTT_PLATFORM_WRITE_RUNS)) {
        /** @brief Number of bytes of this run taken. */
        uint32_t taken = 0U;

        status = mqtt_platform_write(handle, runs[run].data, runs[run].length,
                                     &taken);
        if (status == MQTT_OK) {
            total += taken;
            going = (taken == runs[run].length);
        } else {
            going = false;
        }
        run += 1U;
    }
    if (total > 0U) {
        status = MQTT_OK;
    }
    *count_taken = total;
    return status;
}

void mqtt_platform_close(int32_t handle)
{
    /** @brief The socket. */
    const Socket_t socket = freertos_socket(handle);

    if (socket != NULL) {
        /** @brief The set it waited in alone, if it had one. */
        const SocketSet_t set = freertos_sets[handle];
        /** @brief Index of the entry it is kept in. */
        uint32_t index = 0U;
        /** @brief true once it is kept. */
        bool kept = false;

        if (set != NULL) {
            FreeRTOS_FD_CLR(socket, set, (EventBits_t)eSELECT_ALL);
            FreeRTOS_DeleteSocketSet(set);
        }
        (void)FreeRTOS_shutdown(socket, FREERTOS_SHUT_RDWR);
        taskENTER_CRITICAL();
        freertos_table[handle] = NULL;
        freertos_sets[handle] = NULL;
        platform_slots_give(&freertos_slots, handle);
        while ((!kept) && (index < MQTT_PLATFORM_SOCKETS)) {
            kept = (freertos_closing[index] == NULL);
            if (kept) {
                freertos_closing[index] = socket;
                freertos_closed_at[index] = xTaskGetTickCount();
            }
            index += 1U;
        }
        taskEXIT_CRITICAL();
        if (!kept) {
            (void)FreeRTOS_closesocket(socket);
        }
        freertos_reap();
    }
}

bool mqtt_platform_close_resets(void)
{
    return false;
}

mqtt_status mqtt_platform_socket_pair(int32_t *first, int32_t *second)
{
    /** @brief The listener, on a port of the loopback address. */
    int32_t listener = MQTT_SOCKET_NONE;
    /** @brief Its address. */
    struct freertos_sockaddr address;
    /** @brief The text of the port it was given. */
    char port[6] = {'0', '\0', '\0', '\0', '\0', '\0'};
    /** @brief The outcome. */
    mqtt_status status =
        mqtt_platform_listen("127.0.0.1", port, &listener);

    if (status == MQTT_OK) {
        /** @brief The port, which was chosen when the listener was bound. */
        uint32_t number = 0U;
        /** @brief Index of the digit. */
        uint32_t digit = 5U;

        (void)FreeRTOS_GetLocalAddress(freertos_socket(listener), &address);
        number = (uint32_t)FreeRTOS_ntohs(address.sin_port);
        while (digit > 0U) {
            digit -= 1U;
            port[digit] = (char)('0' + (char)(number % 10U));
            number /= 10U;
        }
        status = mqtt_platform_connect("127.0.0.1", port, first);
    }
    if (status == MQTT_OK) {
        /** @brief Tries left at accepting the end that connected. */
        uint32_t tries = 100U;

        status = MQTT_INCOMPLETE;
        while ((status == MQTT_INCOMPLETE) && (tries > 0U)) {
            status = mqtt_platform_accept(listener, second);
            if (status == MQTT_INCOMPLETE) {
                vTaskDelay(1U);
            }
            tries -= 1U;
        }
        if (status != MQTT_OK) {
            mqtt_platform_close(*first);
            status = MQTT_CONNECTION_CLOSED;
        }
    }
    if (listener != MQTT_SOCKET_NONE) {
        mqtt_platform_close(listener);
    }
    return status;
}
