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
 * @file posix_signal.c
 * @brief Stopping on a signal, and the pipe that wakes a wait when one comes.
 *
 * A signal handler may only do what is safe in one, so it sets a flag and
 * writes a byte to a pipe that the server waits on beside its sockets.
 */

#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#include "base_platform.h"
#include "posix_internal.h"

/**
 * @brief What a signal can do.
 * @param[in] number The signal.
 */
typedef void (*platform_signal_handler)(int number);

/** @brief Number of signals received that ask the program to stop. */
static volatile sig_atomic_t platform_stop_signals = 0;

/**
 * @brief The pipe a stop signal writes to, so that a wait ends at once: the end
 * to read, then the end to write. Each is -1 until the pipe is open.
 */
static int platform_wake[2] = {-1, -1};

/**
 * @brief Note that the program is to stop, and wake the wait. This is safe in
 * a signal handler.
 */
static void platform_note_stop(void)
{
    /** @brief The byte written. */
    const uint8_t byte = 1U;

    platform_stop_signals = 1;
    if (platform_wake[1] >= 0) {
        /**
         * @brief Number of bytes written, or -1 if the pipe is full, when a
         * byte is already there to wake the wait.
         */
        const ssize_t written = write(platform_wake[1], &byte, 1U);

        (void)written;
    }
}

/**
 * @brief Note that a signal arrived, and wake the wait.
 * @param[in] number The signal.
 */
static void platform_on_signal(int number)
{
    /** @brief The error number, which a signal must leave as it found it. */
    const int saved = errno;

    (void)number;
    platform_note_stop();
    errno = saved;
}

/**
 * @brief Set what a signal does.
 * @param[in] number The signal.
 * @param[in] handler What to do.
 */
static void platform_on(int number, platform_signal_handler handler)
{
    /** @brief The action. */
    struct sigaction action;

    (void)memset(&action, 0, sizeof(action));
    action.sa_handler = handler;
    (void)sigemptyset(&action.sa_mask);
    (void)sigaction(number, &action, NULL);
}

void posix_prepare(void)
{
    /** @brief true once done. */
    static bool prepared = false;

    if (!prepared) {
        prepared = true;
        platform_on(SIGPIPE, SIG_IGN);
        if (pipe(platform_wake) == 0) {
            posix_no_block(platform_wake[0]);
            posix_no_block(platform_wake[1]);
        } else {
            platform_wake[0] = -1;
            platform_wake[1] = -1;
        }
    }
}

bool mqtt_platform_stop_requested(void)
{
    /**
     * @brief true once the signals are being watched, which only happens if
     * asked for here.
     */
    static bool watching = false;

    posix_prepare();
    if (!watching) {
        watching = true;
        platform_on(SIGINT, platform_on_signal);
        platform_on(SIGTERM, platform_on_signal);
    }
    return platform_stop_signals != 0;
}

void mqtt_platform_request_stop(void)
{
    posix_prepare();
    platform_note_stop();
}

int32_t mqtt_platform_wake_handle(void)
{
    posix_prepare();
    return (platform_wake[0] >= 0) ? platform_wake[0] : MQTT_SOCKET_NONE;
}

void mqtt_platform_wake_drain(void)
{
    /** @brief Where the bytes go. */
    uint8_t bytes[16];

    if (platform_wake[0] >= 0) {
        while (read(platform_wake[0], bytes, sizeof(bytes)) > 0) {
            /* Each byte only says a signal came. */
        }
    }
}
