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
 * @file macos_sync.c
 * @brief Making what was written to a file outlast a loss of power, on macOS.
 *
 * On macOS, fsync gives the bytes to the drive, which may still hold them in
 * its own cache. The F_FULLFSYNC command of fcntl asks the drive to write its
 * cache out too, which is what outlasting a loss of power needs. A file system
 * that doesn't take F_FULLFSYNC gets fsync instead, the most it offers.
 */

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "base_platform.h"
#include "platform_store.h"

mqtt_status mqtt_platform_file_sync(int32_t file)
{
    /** @brief What the command gave. */
    int result = fcntl((int)file, F_FULLFSYNC);

    while ((result != 0) && (errno == EINTR)) {
        result = fcntl((int)file, F_FULLFSYNC);
    }
    if (result != 0) {
        result = fsync((int)file);
        while ((result != 0) && (errno == EINTR)) {
            result = fsync((int)file);
        }
    }
    return (result == 0) ? MQTT_OK : MQTT_INVALID_STATE;
}
