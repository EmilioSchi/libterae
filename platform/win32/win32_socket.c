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
 * @file win32_socket.c
 * @brief Sockets, through Winsock 2.
 *
 * A SOCKET is as wide as a pointer, so each one is kept in a table and named by
 * its index there. Name lookup goes through getaddrinfo, which gives each
 * address in the form the socket calls take, so no address is cast from one
 * structure type to another. Option values are copied into bytes, as setsockopt
 * takes them, rather than cast. Winsock raises no signal for a write to a
 * closed connection, so sends need no flag for it.
 */

#include <stddef.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include "base_platform.h"
#include "platform_slots.h"
#include "win32_internal.h"

/** @brief The longest read or write one call is asked for. */
#define WIN32_MOST 0x40000000U

/** @brief The free indexes of the table. */
static platform_slots win32_slots;
/** @brief The table: the SOCKET of each handle. */
static SOCKET win32_table[MQTT_PLATFORM_SOCKETS];

void win32_prepare(void)
{
    /** @brief true once Winsock has started. */
    static bool started = false;

    if (!started) {
        /** @brief What Winsock says of itself. */
        WSADATA data;

        started = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
}

SOCKET win32_socket(int32_t handle)
{
    return ((handle >= 0) && ((uint32_t)handle < MQTT_PLATFORM_SOCKETS)) ?
               win32_table[handle] :
               INVALID_SOCKET;
}

int32_t win32_adopt(SOCKET socket)
{
    /** @brief The handle. */
    const int32_t handle = platform_slots_take(&win32_slots);

    if (handle == MQTT_SOCKET_NONE) {
        (void)closesocket(socket);
    } else {
        win32_table[handle] = socket;
    }
    return handle;
}

bool win32_would_block(void)
{
    /** @brief Why the call failed. */
    const int error = WSAGetLastError();

    return (error == WSAEWOULDBLOCK) || (error == WSAEINTR) ||
           (error == WSAEINPROGRESS);
}

void win32_no_block(SOCKET socket)
{
    /** @brief The value that turns the mode on. */
    u_long on = 1UL;

    (void)ioctlsocket(socket, FIONBIO, &on);
}

/**
 * @brief Set an option that takes an int.
 * @param[in] socket The socket.
 * @param[in] level The option's level.
 * @param[in] name The option.
 * @param[in] value Its value.
 */
static void win32_option(SOCKET socket, int level, int name, int value)
{
    /** @brief The value, as setsockopt takes it. */
    char bytes[sizeof(int)];

    (void)memcpy(bytes, &value, sizeof(bytes));
    (void)setsockopt(socket, level, name, bytes, (int)sizeof(bytes));
}

/**
 * @brief Find the addresses of a host and port.
 * @param[in] host The host, or NULL for every local address.
 * @param[in] port The port, as text.
 * @param[in] passive true to find local addresses to listen on.
 * @param[out] found The addresses, to be given to freeaddrinfo.
 * @return true if any were found.
 */
static bool win32_lookup(const char *host, const char *port, bool passive,
                         struct addrinfo **found)
{
    /** @brief What to look for. */
    struct addrinfo hints;

    win32_prepare();
    (void)memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = passive ? AI_PASSIVE : 0;
    return getaddrinfo(host, port, &hints, found) == 0;
}

mqtt_status mqtt_platform_listen(const char *host, const char *port,
                                 int32_t *listener)
{
    /** @brief The addresses found. */
    struct addrinfo *found = NULL;
    /** @brief The outcome. */
    mqtt_status status = MQTT_CONNECTION_CLOSED;

    if (win32_lookup(host, port, true, &found)) {
        /** @brief The address being tried. */
        const struct addrinfo *entry = found;

        while ((entry != NULL) && (status != MQTT_OK)) {
            /** @brief The socket for this address. */
            const SOCKET socket_found = socket(
                entry->ai_family, entry->ai_socktype, entry->ai_protocol);

            if (socket_found != INVALID_SOCKET) {
                if ((bind(socket_found, entry->ai_addr,
                          (int)entry->ai_addrlen) == 0) &&
                    (listen(socket_found, SOMAXCONN) == 0)) {
                    win32_no_block(socket_found);
                    *listener = win32_adopt(socket_found);
                    status = (*listener != MQTT_SOCKET_NONE) ?
                                 MQTT_OK :
                                 MQTT_CONNECTION_CLOSED;
                } else {
                    (void)closesocket(socket_found);
                }
            }
            entry = entry->ai_next;
        }
        freeaddrinfo(found);
    }
    return status;
}

void mqtt_platform_send_buffer(int32_t handle, uint32_t bytes)
{
    win32_option(win32_socket(handle), SOL_SOCKET, SO_SNDBUF,
                 (bytes > (uint32_t)INT32_MAX) ? INT32_MAX : (int)bytes);
}

mqtt_status mqtt_platform_accept(int32_t listener, int32_t *handle)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief The new socket. */
    const SOCKET accepted = accept(win32_socket(listener), NULL, NULL);

    if (accepted != INVALID_SOCKET) {
        win32_no_block(accepted);
        win32_option(accepted, IPPROTO_TCP, TCP_NODELAY, 1);
        *handle = win32_adopt(accepted);
        if (*handle == MQTT_SOCKET_NONE) {
            status = MQTT_INCOMPLETE;
        }
    } else if (win32_would_block() ||
               (WSAGetLastError() == WSAECONNRESET)) {
        status = MQTT_INCOMPLETE;
    } else {
        status = MQTT_CONNECTION_CLOSED;
    }
    return status;
}

mqtt_status mqtt_platform_connect(const char *host, const char *port,
                                  int32_t *handle)
{
    /** @brief The addresses found. */
    struct addrinfo *found = NULL;
    /** @brief The outcome. */
    mqtt_status status = MQTT_CONNECTION_CLOSED;

    if (win32_lookup(host, port, false, &found)) {
        /** @brief The address being tried. */
        const struct addrinfo *entry = found;

        while ((entry != NULL) && (status != MQTT_OK)) {
            /** @brief The socket for this address. */
            const SOCKET socket_found = socket(
                entry->ai_family, entry->ai_socktype, entry->ai_protocol);

            if (socket_found != INVALID_SOCKET) {
                if (connect(socket_found, entry->ai_addr,
                            (int)entry->ai_addrlen) == 0) {
                    win32_no_block(socket_found);
                    win32_option(socket_found, IPPROTO_TCP, TCP_NODELAY, 1);
                    *handle = win32_adopt(socket_found);
                    status = (*handle != MQTT_SOCKET_NONE) ?
                                 MQTT_OK :
                                 MQTT_CONNECTION_CLOSED;
                } else {
                    (void)closesocket(socket_found);
                }
            }
            entry = entry->ai_next;
        }
        freeaddrinfo(found);
    }
    return status;
}

/**
 * @brief Find what a failed call means.
 * @return MQTT_INCOMPLETE if it only had to wait, or MQTT_CONNECTION_CLOSED.
 */
static mqtt_status win32_failure(void)
{
    return win32_would_block() ? MQTT_INCOMPLETE : MQTT_CONNECTION_CLOSED;
}

mqtt_status mqtt_platform_read_spaces(int32_t handle, const mqtt_space *spaces,
                                      uint32_t count, uint32_t *count_read)
{
    /** @brief The places, as WSARecv takes them. */
    WSABUF buffers[MQTT_PLATFORM_READ_SPACES];
    /** @brief Number of places used. */
    const uint32_t used = (count < MQTT_PLATFORM_READ_SPACES) ?
                              count :
                              MQTT_PLATFORM_READ_SPACES;
    /** @brief Index of the place. */
    uint32_t index = 0U;
    /** @brief Number of bytes read. */
    DWORD received = 0U;
    /** @brief The flags of the read, which it may change. */
    DWORD flags = 0U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    while (index < used) {
        buffers[index].buf = (CHAR *)spaces[index].data;
        buffers[index].len = (ULONG)spaces[index].length;
        index += 1U;
    }
    if (WSARecv(win32_socket(handle), buffers, (DWORD)used, &received, &flags,
                NULL, NULL) == 0) {
        if (received > 0U) {
            *count_read = (uint32_t)received;
        } else {
            status = MQTT_CONNECTION_CLOSED;
        }
    } else {
        status = win32_failure();
    }
    return status;
}

mqtt_status mqtt_platform_read(int32_t handle, uint8_t *data, uint32_t capacity,
                               uint32_t *count)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief Number of bytes read. */
    const int result =
        recv(win32_socket(handle), (char *)data,
             (int)((capacity < WIN32_MOST) ? capacity : WIN32_MOST), 0);

    if (result > 0) {
        *count = (uint32_t)result;
    } else if (result == 0) {
        status = MQTT_CONNECTION_CLOSED;
    } else {
        status = win32_failure();
    }
    return status;
}

mqtt_status mqtt_platform_write(int32_t handle, const uint8_t *data,
                                uint32_t length, uint32_t *count)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief Number of bytes taken. */
    const int result =
        send(win32_socket(handle), (const char *)data,
             (int)((length < WIN32_MOST) ? length : WIN32_MOST), 0);

    if (result >= 0) {
        *count = (uint32_t)result;
    } else {
        status = win32_failure();
    }
    return status;
}

mqtt_status mqtt_platform_write_runs(int32_t handle, const mqtt_run *runs,
                                     uint32_t count, uint32_t *count_taken)
{
    /** @brief The runs, as WSASend takes them. */
    WSABUF buffers[MQTT_PLATFORM_WRITE_RUNS];
    /** @brief Number of runs written. */
    const uint32_t used = (count < MQTT_PLATFORM_WRITE_RUNS) ?
                              count :
                              MQTT_PLATFORM_WRITE_RUNS;
    /** @brief Index of the run. */
    uint32_t index = 0U;
    /** @brief Number of bytes taken. */
    DWORD sent = 0U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    while (index < used) {
        /* WSASend only reads the bytes, though the field isn't const. A
         * pointer to CHAR is laid out as a pointer to bytes is, so the value is
         * copied rather than cast. */
        (void)memcpy(&buffers[index].buf, &runs[index].data,
                     sizeof(buffers[index].buf));
        buffers[index].len = (ULONG)runs[index].length;
        index += 1U;
    }
    if (WSASend(win32_socket(handle), buffers, (DWORD)used, &sent, 0U, NULL,
                NULL) == 0) {
        *count_taken = (uint32_t)sent;
    } else {
        status = win32_failure();
    }
    return status;
}

void mqtt_platform_close(int32_t handle)
{
    /** @brief The socket. */
    const SOCKET socket_closed = win32_socket(handle);

    if (socket_closed != INVALID_SOCKET) {
        (void)closesocket(socket_closed);
        win32_table[handle] = INVALID_SOCKET;
        platform_slots_give(&win32_slots, handle);
    }
}

/**
 * @brief Connect a socket to a listener on a loopback address, and accept it.
 * @param[in] listener The listener.
 * @param[in] address The listener's address, as getsockname wrote it.
 * @param[out] ends The end that connected, then the end accepted.
 * @return true if both are open.
 */
static bool win32_join(SOCKET listener, const struct addrinfo *address,
                       SOCKET *ends)
{
    ends[0] = socket(address->ai_family, address->ai_socktype,
                     address->ai_protocol);
    ends[1] = INVALID_SOCKET;
    if ((ends[0] != INVALID_SOCKET) &&
        (connect(ends[0], address->ai_addr, (int)address->ai_addrlen) == 0)) {
        ends[1] = accept(listener, NULL, NULL);
    }
    return (ends[0] != INVALID_SOCKET) && (ends[1] != INVALID_SOCKET);
}

bool mqtt_platform_close_resets(void)
{
    /* Winsock resets a connection whose socket is closed with bytes it hasn't
     * read, as POSIX does. */
    return true;
}

mqtt_status mqtt_platform_socket_pair(int32_t *first, int32_t *second)
{
    /** @brief The loopback address, which getsockname gives a port. */
    struct addrinfo *found = NULL;
    /** @brief The two ends. */
    SOCKET ends[2] = {INVALID_SOCKET, INVALID_SOCKET};
    /** @brief The outcome. */
    mqtt_status status = MQTT_CONNECTION_CLOSED;

    if (win32_lookup("127.0.0.1", "0", true, &found)) {
        /** @brief The listener, which is closed once both ends are open. */
        const SOCKET listener =
            socket(found->ai_family, found->ai_socktype, found->ai_protocol);
        /** @brief Number of bytes of the address. */
        int length = (int)found->ai_addrlen;

        if ((listener != INVALID_SOCKET) &&
            (bind(listener, found->ai_addr, length) == 0) &&
            (listen(listener, 1) == 0) &&
            (getsockname(listener, found->ai_addr, &length) == 0) &&
            win32_join(listener, found, ends)) {
            win32_no_block(ends[0]);
            win32_no_block(ends[1]);
            *first = win32_adopt(ends[0]);
            *second = win32_adopt(ends[1]);
            status = MQTT_OK;
        } else {
            (void)closesocket(ends[0]);
            (void)closesocket(ends[1]);
        }
        (void)closesocket(listener);
        freeaddrinfo(found);
    }
    return status;
}
