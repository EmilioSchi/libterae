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
 * @file posix_sync.c
 * @brief Making what was written to a file outlast a loss of power, with
 * fsync.
 *
 * fsync belongs to File Synchronization, an option of POSIX.1-2008 and not
 * part of its base, so this is the one thing in posix/ besides /dev/urandom
 * that POSIX doesn't promise. Every system this folder is built for has it.
 * macos/ has a file of its own in place of this one, as fsync there gives the
 * bytes to the drive but doesn't ask the drive to keep them.
 */

#include <errno.h>
#include <unistd.h>

#include "base_platform.h"
#include "platform_store.h"

mqtt_status mqtt_platform_file_sync(int32_t file)
{
    /** @brief What fsync gave. */
    int result = fsync((int)file);

    while ((result != 0) && (errno == EINTR)) {
        result = fsync((int)file);
    }
    return (result == 0) ? MQTT_OK : MQTT_INVALID_STATE;
}
