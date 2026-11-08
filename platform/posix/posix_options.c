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
 * @file posix_options.c
 * @brief The options sockets are given, with POSIX alone.
 *
 * Nothing here keeps a write to a closed socket from raising SIGPIPE: sends
 * say so with MSG_NOSIGNAL, and posix_prepare ignores the signal.
 */

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include "posix_internal.h"

void posix_no_block(int handle)
{
    (void)fcntl(handle, F_SETFL, fcntl(handle, F_GETFL, 0) | O_NONBLOCK);
}

void posix_socket_options(int32_t handle)
{
    /** @brief The value that turns an option on. */
    const int on = 1;

    posix_no_block(handle);
    (void)setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, &on,
                     (socklen_t)sizeof(on));
}

void posix_pair_options(int32_t handle)
{
    posix_no_block(handle);
}
