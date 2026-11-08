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
 * @file platform_store.h
 * @brief The files a store is kept in, as each system with files gives them.
 *
 * platform_store_file.c keeps a store in a folder through these, and each
 * system's folder of platform/ gives them its own way. A file is named by a
 * path, and handled by a number.
 */
#ifndef MQTT_PLATFORM_STORE_H
#define MQTT_PLATFORM_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "base_status.h"

/** @brief The value of a file that is not open. */
#define MQTT_FILE_NONE (-1)

/**
 * @brief Open a file to read and write, making it if it isn't there, readable
 * by its owner alone. A path that is a symbolic link is refused.
 * @param[in] path The path.
 * @param[out] file The file.
 * @return MQTT_OK, or MQTT_INVALID_STATE.
 */
mqtt_status mqtt_platform_file_open(const char *path, int32_t *file);

/**
 * @brief Close a file.
 * @param[in] file The file.
 */
void mqtt_platform_file_close(int32_t file);

/**
 * @brief Read bytes at an offset.
 * @param[in] file The file.
 * @param[in] offset Offset of the first byte.
 * @param[out] data Where they go.
 * @param[in] capacity Number of bytes wanted.
 * @param[out] count Number of bytes read, fewer only at the end of the file.
 * @return MQTT_OK, or MQTT_INVALID_STATE.
 */
mqtt_status mqtt_platform_file_read(int32_t file, uint64_t offset,
                                    uint8_t *data, uint32_t capacity,
                                    uint32_t *count);

/**
 * @brief Write bytes at an offset, all of them.
 * @param[in] file The file.
 * @param[in] offset Offset of the first byte.
 * @param[in] data The bytes.
 * @param[in] length Number of bytes.
 * @return MQTT_OK, or MQTT_INVALID_STATE if not all were written.
 */
mqtt_status mqtt_platform_file_write(int32_t file, uint64_t offset,
                                     const uint8_t *data, uint32_t length);

/**
 * @brief Cut a file to a length.
 * @param[in] file The file.
 * @param[in] length The length.
 * @return MQTT_OK, or MQTT_INVALID_STATE.
 */
mqtt_status mqtt_platform_file_truncate(int32_t file, uint64_t length);

/**
 * @brief Find a file's length.
 * @param[in] file The file.
 * @param[out] length The length.
 * @return MQTT_OK, or MQTT_INVALID_STATE.
 */
mqtt_status mqtt_platform_file_length(int32_t file, uint64_t *length);

/**
 * @brief Make what was written to a file outlast a loss of power.
 * @param[in] file The file.
 * @return MQTT_OK, or MQTT_INVALID_STATE.
 */
mqtt_status mqtt_platform_file_sync(int32_t file);

/**
 * @brief Find how long ago a file was last written.
 * @param[in] file The file.
 * @return Milliseconds, or 0 if the system can't tell.
 */
uint64_t mqtt_platform_file_age_ms(int32_t file);

/**
 * @brief Make a folder, readable by its owner alone.
 * @param[in] path The path.
 * @param[out] made true if it was made, false if it was there already.
 * @return MQTT_OK, or MQTT_INVALID_STATE.
 */
mqtt_status mqtt_platform_folder_make(const char *path, bool *made);

/**
 * @brief Make the names in a folder, as made or moved, outlast a loss of
 * power.
 * @param[in] path The folder.
 * @return MQTT_OK, or MQTT_INVALID_STATE.
 */
mqtt_status mqtt_platform_folder_sync(const char *path);

/**
 * @brief Give a file another name, which nothing has yet.
 * @param[in] from The name it has.
 * @param[in] to The name it gets.
 * @return MQTT_OK, MQTT_NOT_FOUND if there is no such file, or
 * MQTT_INVALID_STATE.
 */
mqtt_status mqtt_platform_file_move(const char *from, const char *to);

/**
 * @brief Remove a file that isn't open, for mqtt_platform_store_remove.
 * @param[in] path The file.
 * @return MQTT_OK, MQTT_NOT_FOUND if there is no such file, or
 * MQTT_INVALID_STATE.
 */
mqtt_status mqtt_platform_file_remove(const char *path);

/**
 * @brief Remove an empty folder, for mqtt_platform_store_remove.
 * @param[in] path The folder.
 * @return MQTT_OK, MQTT_NOT_FOUND if there is no such folder, or
 * MQTT_INVALID_STATE.
 */
mqtt_status mqtt_platform_folder_remove(const char *path);

#endif /* MQTT_PLATFORM_STORE_H */
