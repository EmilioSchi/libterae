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
 * @file win32_file.c
 * @brief The files a store is kept in, for Windows.
 *
 * A HANDLE is kept in a table here, and a file is named by its index in it.
 * Reads and writes name their offset in an OVERLAPPED, so nothing depends on
 * where the file's pointer was left. A file is opened with
 * FILE_FLAG_OPEN_REPARSE_POINT, so a symbolic link or junction put where a
 * file of the store should be is opened as itself, seen to be one, and
 * refused, not followed. A file and a folder made here take the permissions
 * of the folder they are in, as Windows gives them.
 *
 * A move asks MOVEFILE_WRITE_THROUGH, so it is on the disk before it returns.
 * NTFS logs every change to a folder's names in its own journal, and
 * FlushFileBuffers of a file writes that journal out, so syncing a folder has
 * nothing more to do.
 */

#include <stddef.h>
#include <string.h>
#include <windows.h>

#include "base_platform.h"
#include "platform_store.h"

/** @brief Number of files that may be open at once. */
#define WIN32_FILES \
    (MQTT_PLATFORM_STORES * (MQTT_PLATFORM_STORE_COPIES_MAX + 2U))

/** @brief Number of 100-nanosecond units in a millisecond. */
#define WIN32_UNITS_PER_MS 10000U

/** @brief The open files. */
static HANDLE win32_files[WIN32_FILES];

/** @brief true for each entry of win32_files in use. */
static bool win32_file_used[WIN32_FILES];

/**
 * @brief Find the HANDLE of an open file.
 * @param[in] file The file.
 * @return Its HANDLE, or INVALID_HANDLE_VALUE.
 */
static HANDLE win32_file_handle(int32_t file)
{
    /** @brief The answer. */
    HANDLE handle = INVALID_HANDLE_VALUE;

    if ((file >= 0) && ((uint32_t)file < WIN32_FILES) &&
        win32_file_used[file]) {
        handle = win32_files[file];
    }
    return handle;
}

/**
 * @brief Make an OVERLAPPED that names an offset.
 * @param[out] place The OVERLAPPED.
 * @param[in] offset The offset.
 */
static void win32_file_offset(OVERLAPPED *place, uint64_t offset)
{
    (void)memset(place, 0, sizeof(*place));
    place->Offset = (DWORD)(offset & 0xFFFFFFFFU);
    place->OffsetHigh = (DWORD)(offset >> 32U);
}

mqtt_status mqtt_platform_file_open(const char *path, int32_t *file)
{
    /** @brief Index of the entry being looked at. */
    uint32_t index = 0U;
    /** @brief The file, or INVALID_HANDLE_VALUE. */
    HANDLE handle = INVALID_HANDLE_VALUE;
    /** @brief What the system says of it. */
    BY_HANDLE_FILE_INFORMATION facts;
    /** @brief The outcome. */
    mqtt_status status = MQTT_INVALID_STATE;

    *file = MQTT_FILE_NONE;
    while ((index < WIN32_FILES) && win32_file_used[index]) {
        index += 1U;
    }
    if (index < WIN32_FILES) {
        handle = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL |
                                 FILE_FLAG_OPEN_REPARSE_POINT,
                             NULL);
    }
    if ((handle != INVALID_HANDLE_VALUE) &&
        (GetFileInformationByHandle(handle, &facts) != FALSE) &&
        ((facts.dwFileAttributes &
          (DWORD)(FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ==
         0U)) {
        win32_files[index] = handle;
        win32_file_used[index] = true;
        *file = (int32_t)index;
        status = MQTT_OK;
    } else if (handle != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(handle);
    } else {
        /* It could not be opened, or there was no room for it. */
    }
    return status;
}

void mqtt_platform_file_close(int32_t file)
{
    /** @brief The file. */
    const HANDLE handle = win32_file_handle(file);

    if (handle != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(handle);
        win32_file_used[file] = false;
    }
}

mqtt_status mqtt_platform_file_read(int32_t file, uint64_t offset,
                                    uint8_t *data, uint32_t capacity,
                                    uint32_t *count)
{
    /** @brief The file. */
    const HANDLE handle = win32_file_handle(file);
    /** @brief Number of bytes read so far. */
    uint32_t done = 0U;
    /** @brief true once the end of the file is reached. */
    bool end = false;
    /** @brief The outcome. */
    mqtt_status status = (handle != INVALID_HANDLE_VALUE) ?
                             MQTT_OK :
                             MQTT_INVALID_STATE;

    while ((status == MQTT_OK) && (!end) && (done < capacity)) {
        /** @brief Where the read starts. */
        OVERLAPPED place;
        /** @brief Number of bytes read. */
        DWORD got = 0U;

        win32_file_offset(&place, offset + (uint64_t)done);
        if (ReadFile(handle, &data[done], (DWORD)(capacity - done), &got,
                     &place) == FALSE) {
            end = (GetLastError() == (DWORD)ERROR_HANDLE_EOF);
            status = end ? MQTT_OK : MQTT_INVALID_STATE;
        } else if (got == 0U) {
            end = true;
        } else {
            done += (uint32_t)got;
        }
    }
    *count = done;
    return status;
}

mqtt_status mqtt_platform_file_write(int32_t file, uint64_t offset,
                                     const uint8_t *data, uint32_t length)
{
    /** @brief The file. */
    const HANDLE handle = win32_file_handle(file);
    /** @brief Number of bytes written so far. */
    uint32_t done = 0U;
    /** @brief The outcome. */
    mqtt_status status = (handle != INVALID_HANDLE_VALUE) ?
                             MQTT_OK :
                             MQTT_INVALID_STATE;

    while ((status == MQTT_OK) && (done < length)) {
        /** @brief Where the write starts. */
        OVERLAPPED place;
        /** @brief Number of bytes written. */
        DWORD put = 0U;

        win32_file_offset(&place, offset + (uint64_t)done);
        if ((WriteFile(handle, &data[done], (DWORD)(length - done), &put,
                       &place) == FALSE) ||
            (put == 0U)) {
            status = MQTT_INVALID_STATE;
        } else {
            done += (uint32_t)put;
        }
    }
    return status;
}

mqtt_status mqtt_platform_file_truncate(int32_t file, uint64_t length)
{
    /** @brief The file. */
    const HANDLE handle = win32_file_handle(file);
    /** @brief The length, as Windows takes it. */
    LARGE_INTEGER place;
    /** @brief The outcome. */
    mqtt_status status = MQTT_INVALID_STATE;

    place.QuadPart = (LONGLONG)length;
    if ((handle != INVALID_HANDLE_VALUE) &&
        (length <= (uint64_t)0x7FFFFFFFFFFFFFFFU) &&
        (SetFilePointerEx(handle, place, NULL, FILE_BEGIN) != FALSE) &&
        (SetEndOfFile(handle) != FALSE)) {
        status = MQTT_OK;
    }
    return status;
}

mqtt_status mqtt_platform_file_length(int32_t file, uint64_t *length)
{
    /** @brief The file. */
    const HANDLE handle = win32_file_handle(file);
    /** @brief The length, as Windows gives it. */
    LARGE_INTEGER size;
    /** @brief The outcome. */
    mqtt_status status = MQTT_INVALID_STATE;

    *length = 0U;
    size.QuadPart = 0;
    if ((handle != INVALID_HANDLE_VALUE) &&
        (GetFileSizeEx(handle, &size) != FALSE) && (size.QuadPart >= 0)) {
        *length = (uint64_t)size.QuadPart;
        status = MQTT_OK;
    }
    return status;
}

mqtt_status mqtt_platform_file_sync(int32_t file)
{
    /** @brief The file. */
    const HANDLE handle = win32_file_handle(file);

    return ((handle != INVALID_HANDLE_VALUE) &&
            (FlushFileBuffers(handle) != FALSE)) ?
               MQTT_OK :
               MQTT_INVALID_STATE;
}

uint64_t mqtt_platform_file_age_ms(int32_t file)
{
    /** @brief The file. */
    const HANDLE handle = win32_file_handle(file);
    /** @brief When it was last written. */
    FILETIME written;
    /** @brief The time now. */
    FILETIME now;
    /** @brief The answer. */
    uint64_t age = 0U;

    GetSystemTimeAsFileTime(&now);
    if ((handle != INVALID_HANDLE_VALUE) &&
        (GetFileTime(handle, NULL, NULL, &written) != FALSE)) {
        /** @brief When it was written, in 100-nanosecond units. */
        const uint64_t then = ((uint64_t)written.dwHighDateTime << 32U) |
                              (uint64_t)written.dwLowDateTime;
        /** @brief The time now, in the same units. */
        const uint64_t time = ((uint64_t)now.dwHighDateTime << 32U) |
                              (uint64_t)now.dwLowDateTime;

        if (time > then) {
            age = (time - then) / (uint64_t)WIN32_UNITS_PER_MS;
        }
    }
    return age;
}

mqtt_status mqtt_platform_folder_make(const char *path, bool *made)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    *made = false;
    if (CreateDirectoryA(path, NULL) != FALSE) {
        *made = true;
    } else if (GetLastError() != (DWORD)ERROR_ALREADY_EXISTS) {
        status = MQTT_INVALID_STATE;
    } else {
        /* It is there already. */
    }
    return status;
}

mqtt_status mqtt_platform_folder_sync(const char *path)
{
    /* NTFS logs the names, as this file's comment says. */
    (void)path;
    return MQTT_OK;
}

mqtt_status mqtt_platform_file_move(const char *from, const char *to)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (MoveFileExA(from, to, MOVEFILE_WRITE_THROUGH) == FALSE) {
        /** @brief Why it failed. */
        const DWORD error = GetLastError();

        status = ((error == (DWORD)ERROR_FILE_NOT_FOUND) ||
                  (error == (DWORD)ERROR_PATH_NOT_FOUND)) ?
                     MQTT_NOT_FOUND :
                     MQTT_INVALID_STATE;
    }
    return status;
}

mqtt_status mqtt_platform_file_remove(const char *path)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (DeleteFileA(path) == FALSE) {
        /** @brief Why it failed. */
        const DWORD error = GetLastError();

        status = ((error == (DWORD)ERROR_FILE_NOT_FOUND) ||
                  (error == (DWORD)ERROR_PATH_NOT_FOUND)) ?
                     MQTT_NOT_FOUND :
                     MQTT_INVALID_STATE;
    }
    return status;
}

mqtt_status mqtt_platform_folder_remove(const char *path)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (RemoveDirectoryA(path) == FALSE) {
        /** @brief Why it failed. */
        const DWORD error = GetLastError();

        status = ((error == (DWORD)ERROR_FILE_NOT_FOUND) ||
                  (error == (DWORD)ERROR_PATH_NOT_FOUND)) ?
                     MQTT_NOT_FOUND :
                     MQTT_INVALID_STATE;
    }
    return status;
}
