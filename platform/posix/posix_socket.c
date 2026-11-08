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
 * @file posix_socket.c
 * @brief Sockets, for POSIX systems.
 *
 * Name lookup goes through getaddrinfo, which gives each address in the form
 * the socket calls take, so no address has to be cast from one structure type
 * to another. Every send says MSG_NOSIGNAL, so a write to a closed socket
 * fails rather than raising SIGPIPE.
 */

#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "base_platform.h"
#include "posix_internal.h"

mqtt_status mqtt_platform_listen(const char *host, const char *port,
                                 int32_t *listener)
{
    /** @brief What to look for. */
    struct addrinfo hints;
    /** @brief The addresses found. */
    struct addrinfo *found = NULL;
    /** @brief The outcome. */
    mqtt_status status = MQTT_CONNECTION_CLOSED;

    posix_prepare();
    (void)memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    if (getaddrinfo(host, port, &hints, &found) == 0) {
        /** @brief The address being tried. */
        const struct addrinfo *entry = found;

        while ((entry != NULL) && (status != MQTT_OK)) {
            /** @brief The socket for this address. */
            const int32_t fd = socket(entry->ai_family, entry->ai_socktype,
                                      entry->ai_protocol);

            if (fd >= 0) {
                /** @brief The value that turns an option on. */
                const int on = 1;

                (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on,
                                 (socklen_t)sizeof(on));
                if ((bind(fd, entry->ai_addr, entry->ai_addrlen) == 0) &&
                    (listen(fd, 1024) == 0)) {
                    posix_no_block(fd);
                    *listener = fd;
                    status = MQTT_OK;
                } else {
                    (void)close(fd);
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
    /** @brief The size, as setsockopt takes it. */
    const int size = (bytes > (uint32_t)INT32_MAX) ? INT32_MAX : (int)bytes;

    (void)setsockopt(handle, SOL_SOCKET, SO_SNDBUF, &size,
                     (socklen_t)sizeof(size));
}

mqtt_status mqtt_platform_accept(int32_t listener, int32_t *handle)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief The new socket. */
    const int32_t fd = accept(listener, NULL, NULL);

    if (fd >= 0) {
        posix_socket_options(fd);
        *handle = fd;
    } else if ((errno == EAGAIN) || (errno == EWOULDBLOCK) ||
               (errno == EINTR) || (errno == ECONNABORTED)) {
        status = MQTT_INCOMPLETE;
    } else {
        status = MQTT_CONNECTION_CLOSED;
    }
    return status;
}

mqtt_status mqtt_platform_connect(const char *host, const char *port,
                                  int32_t *handle)
{
    /** @brief What to look for. */
    struct addrinfo hints;
    /** @brief The addresses found. */
    struct addrinfo *found = NULL;
    /** @brief The outcome. */
    mqtt_status status = MQTT_CONNECTION_CLOSED;

    posix_prepare();
    (void)memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &found) == 0) {
        /** @brief The address being tried. */
        const struct addrinfo *entry = found;

        while ((entry != NULL) && (status != MQTT_OK)) {
            /** @brief The socket for this address. */
            const int32_t fd = socket(entry->ai_family, entry->ai_socktype,
                                      entry->ai_protocol);

            if (fd >= 0) {
                if (connect(fd, entry->ai_addr, entry->ai_addrlen) == 0) {
                    posix_socket_options(fd);
                    *handle = fd;
                    status = MQTT_OK;
                } else {
                    (void)close(fd);
                }
            }
            entry = entry->ai_next;
        }
        freeaddrinfo(found);
    }
    return status;
}

mqtt_status mqtt_platform_read_spaces(int32_t handle, const mqtt_space *spaces,
                                      uint32_t count, uint32_t *count_read)
{
    /** @brief The places, as recvmsg takes them. */
    struct iovec vectors[MQTT_PLATFORM_READ_SPACES];
    /** @brief The message. */
    struct msghdr message;
    /** @brief Number of places used. */
    uint32_t used = (count < MQTT_PLATFORM_READ_SPACES) ?
                        count :
                        MQTT_PLATFORM_READ_SPACES;
    /** @brief Index of the place. */
    uint32_t index = 0U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief Number of bytes read. */
    ssize_t result = 0;

#if defined(IOV_MAX)
    if (used > (uint32_t)IOV_MAX) {
        used = (uint32_t)IOV_MAX;
    }
#endif
    (void)memset(&message, 0, sizeof(message));
    while (index < used) {
        vectors[index].iov_base = spaces[index].data;
        vectors[index].iov_len = (size_t)spaces[index].length;
        index += 1U;
    }
    message.msg_iov = vectors;
    message.msg_iovlen = (int)used;
    result = recvmsg(handle, &message, 0);
    if (result > 0) {
        *count_read = (uint32_t)result;
    } else if (result == 0) {
        status = MQTT_CONNECTION_CLOSED;
    } else if ((errno == EAGAIN) || (errno == EWOULDBLOCK) ||
               (errno == EINTR)) {
        status = MQTT_INCOMPLETE;
    } else {
        status = MQTT_CONNECTION_CLOSED;
    }
    return status;
}

mqtt_status mqtt_platform_read(int32_t handle, uint8_t *data, uint32_t capacity,
                               uint32_t *count)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief Number of bytes read. */
    const ssize_t result = read(handle, data, (size_t)capacity);

    if (result > 0) {
        *count = (uint32_t)result;
    } else if (result == 0) {
        status = MQTT_CONNECTION_CLOSED;
    } else if ((errno == EAGAIN) || (errno == EWOULDBLOCK) ||
               (errno == EINTR)) {
        status = MQTT_INCOMPLETE;
    } else {
        status = MQTT_CONNECTION_CLOSED;
    }
    return status;
}

mqtt_status mqtt_platform_write(int32_t handle, const uint8_t *data,
                                uint32_t length, uint32_t *count)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief Number of bytes taken. */
    const ssize_t result = send(handle, data, (size_t)length, MSG_NOSIGNAL);

    if (result >= 0) {
        *count = (uint32_t)result;
    } else if ((errno == EAGAIN) || (errno == EWOULDBLOCK) ||
               (errno == EINTR)) {
        status = MQTT_INCOMPLETE;
    } else {
        status = MQTT_CONNECTION_CLOSED;
    }
    return status;
}

mqtt_status mqtt_platform_write_runs(int32_t handle, const mqtt_run *runs,
                                     uint32_t count, uint32_t *count_taken)
{
    /** @brief The runs, as sendmsg takes them. */
    struct iovec vectors[MQTT_PLATFORM_WRITE_RUNS];
    /** @brief The message. */
    struct msghdr message;
    /** @brief Number of runs written. */
    uint32_t used = (count < MQTT_PLATFORM_WRITE_RUNS) ?
                        count :
                        MQTT_PLATFORM_WRITE_RUNS;
    /** @brief Index of the run. */
    uint32_t index = 0U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief Number of bytes taken. */
    ssize_t result = 0;

#if defined(IOV_MAX)
    if (used > (uint32_t)IOV_MAX) {
        used = (uint32_t)IOV_MAX;
    }
#endif
    (void)memset(&message, 0, sizeof(message));
    while (index < used) {
        /* sendmsg only reads the bytes, though the field isn't const. A
         * pointer to void is laid out as a pointer to bytes is, so the value is
         * copied rather than cast. */
        (void)memcpy(&vectors[index].iov_base, &runs[index].data,
                     sizeof(vectors[index].iov_base));
        vectors[index].iov_len = (size_t)runs[index].length;
        index += 1U;
    }
    message.msg_iov = vectors;
    message.msg_iovlen = (int)used;
    result = sendmsg(handle, &message, MSG_NOSIGNAL);
    if (result >= 0) {
        *count_taken = (uint32_t)result;
    } else if ((errno == EAGAIN) || (errno == EWOULDBLOCK) ||
               (errno == EINTR)) {
        status = MQTT_INCOMPLETE;
    } else {
        status = MQTT_CONNECTION_CLOSED;
    }
    return status;
}

void mqtt_platform_close(int32_t handle)
{
    (void)close(handle);
}

bool mqtt_platform_close_resets(void)
{
    /* A POSIX close of a socket with bytes it hasn't read resets the
     * connection. */
    return true;
}

mqtt_status mqtt_platform_socket_pair(int32_t *first, int32_t *second)
{
    /** @brief The two ends. */
    int ends[2] = {-1, -1};
    /** @brief The outcome. */
    mqtt_status status = MQTT_CONNECTION_CLOSED;

    posix_prepare();
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, ends) == 0) {
        posix_pair_options(ends[0]);
        posix_pair_options(ends[1]);
        *first = ends[0];
        *second = ends[1];
        status = MQTT_OK;
    }
    return status;
}
