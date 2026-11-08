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
 * @file posix_file.c
 * @brief The files a store is kept in, with POSIX's descriptors.
 *
 * Reads and writes name their offset, with pread and pwrite, so nothing
 * depends on where a descriptor was left. A file is opened with O_CLOEXEC, so
 * a program the process starts doesn't get it, and with O_NOFOLLOW, so a
 * symbolic link put where a file of the store should be is refused, not
 * followed. Some systems' headers give O_NOFOLLOW only with their own
 * extensions, though POSIX.1-2008 has it, so it is used where the headers give
 * it, and after each open lstat checks that the path names the file opened and
 * is no link, whichever way it was opened.
 *
 * A file is moved by giving it the new name with link and then taking the old
 * one away with unlink, as rename is in C's stdio.h, which nothing here
 * includes. Syncing is in posix_sync.c, the one place that calls fsync.
 */

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "base_platform.h"
#include "platform_store.h"

/** @brief Milliseconds in a second. */
#define FILE_SECOND_MS 1000U

/** @brief The most bytes one call asks for, which a ssize_t always holds. */
#define FILE_CALL_MAX 32768U

/** @brief The largest offset an off_t of 32 bits holds. */
#define FILE_OFFSET_MAX_32 0x7FFFFFFFU

/** @brief The largest offset an off_t of 64 bits holds. */
#define FILE_OFFSET_MAX_64 0x7FFFFFFFFFFFFFFFU

#if defined(O_NOFOLLOW)
/** @brief How a file is opened so a link isn't followed, where it can be. */
#define FILE_NO_FOLLOW O_NOFOLLOW
#else
/** @brief How a file is opened so a link isn't followed, where it can be. */
#define FILE_NO_FOLLOW 0
#endif

/**
 * @brief Find whether an offset fits in an off_t.
 * @param[in] offset The offset.
 * @return true if it does.
 */
static bool file_offset_fits(uint64_t offset)
{
    /** @brief The largest offset an off_t holds. */
    const uint64_t largest = (sizeof(off_t) >= sizeof(uint64_t)) ?
                                 (uint64_t)FILE_OFFSET_MAX_64 :
                                 (uint64_t)FILE_OFFSET_MAX_32;

    return offset <= largest;
}

mqtt_status mqtt_platform_file_open(const char *path, int32_t *file)
{
    /** @brief The descriptor, or -1. */
    const int handle = open(path, O_RDWR | O_CREAT | O_CLOEXEC | FILE_NO_FOLLOW,
                            (mode_t)(S_IRUSR | S_IWUSR));
    /** @brief What the system says of the file opened. */
    struct stat opened;
    /** @brief What it says of the path. */
    struct stat named;
    /** @brief The outcome. */
    mqtt_status status = MQTT_INVALID_STATE;

    *file = MQTT_FILE_NONE;
    if ((handle >= 0) && (fstat(handle, &opened) == 0) &&
        (lstat(path, &named) == 0) && S_ISREG(named.st_mode) &&
        (named.st_dev == opened.st_dev) && (named.st_ino == opened.st_ino)) {
        *file = (int32_t)handle;
        status = MQTT_OK;
    } else if (handle >= 0) {
        (void)close(handle);
    } else {
        /* It could not be opened. */
    }
    return status;
}

void mqtt_platform_file_close(int32_t file)
{
    if (file >= 0) {
        (void)close((int)file);
    }
}

mqtt_status mqtt_platform_file_read(int32_t file, uint64_t offset,
                                    uint8_t *data, uint32_t capacity,
                                    uint32_t *count)
{
    /** @brief Number of bytes read so far. */
    uint32_t done = 0U;
    /** @brief true once the end of the file is reached. */
    bool end = false;
    /** @brief The outcome. */
    mqtt_status status = file_offset_fits(offset + (uint64_t)capacity) ?
                             MQTT_OK :
                             MQTT_INVALID_STATE;

    while ((status == MQTT_OK) && (!end) && (done < capacity)) {
        /** @brief Number of bytes to ask for. */
        const uint32_t want = ((capacity - done) < FILE_CALL_MAX) ?
                                  (capacity - done) :
                                  FILE_CALL_MAX;
        /** @brief Number of bytes read, 0 at the end, or -1. */
        const ssize_t got = pread((int)file, &data[done], (size_t)want,
                                  (off_t)(offset + (uint64_t)done));

        if (got > 0) {
            done += (uint32_t)got;
        } else if (got == 0) {
            end = true;
        } else if (errno == EINTR) {
            /* A signal came before any byte did, so it is asked again. */
        } else {
            status = MQTT_INVALID_STATE;
        }
    }
    *count = done;
    return status;
}

mqtt_status mqtt_platform_file_write(int32_t file, uint64_t offset,
                                     const uint8_t *data, uint32_t length)
{
    /** @brief Number of bytes written so far. */
    uint32_t done = 0U;
    /** @brief The outcome. */
    mqtt_status status = file_offset_fits(offset + (uint64_t)length) ?
                             MQTT_OK :
                             MQTT_INVALID_STATE;

    while ((status == MQTT_OK) && (done < length)) {
        /** @brief Number of bytes to give. */
        const uint32_t want = ((length - done) < FILE_CALL_MAX) ?
                                  (length - done) :
                                  FILE_CALL_MAX;
        /** @brief Number of bytes written, or -1. */
        const ssize_t put = pwrite((int)file, &data[done], (size_t)want,
                                   (off_t)(offset + (uint64_t)done));

        if (put > 0) {
            done += (uint32_t)put;
        } else if ((put < 0) && (errno == EINTR)) {
            /* A signal came before any byte went, so it is tried again. */
        } else {
            /* No room left, or the medium failed. */
            status = MQTT_INVALID_STATE;
        }
    }
    return status;
}

mqtt_status mqtt_platform_file_truncate(int32_t file, uint64_t length)
{
    /** @brief The outcome. */
    mqtt_status status = file_offset_fits(length) ? MQTT_OK :
                                                    MQTT_INVALID_STATE;

    if ((status == MQTT_OK) && (ftruncate((int)file, (off_t)length) != 0)) {
        status = MQTT_INVALID_STATE;
    }
    return status;
}

mqtt_status mqtt_platform_file_length(int32_t file, uint64_t *length)
{
    /** @brief What the system says of the file. */
    struct stat facts;
    /** @brief The outcome. */
    mqtt_status status = MQTT_INVALID_STATE;

    *length = 0U;
    if ((fstat((int)file, &facts) == 0) && S_ISREG(facts.st_mode) &&
        (facts.st_size >= 0)) {
        *length = (uint64_t)facts.st_size;
        status = MQTT_OK;
    }
    return status;
}

uint64_t mqtt_platform_file_age_ms(int32_t file)
{
    /** @brief What the system says of the file. */
    struct stat facts;
    /** @brief The time now, in seconds. */
    const time_t now = time(NULL);
    /** @brief The answer. */
    uint64_t age = 0U;

    if ((now != (time_t)-1) && (fstat((int)file, &facts) == 0) &&
        (now > facts.st_mtime)) {
        age = (uint64_t)(now - facts.st_mtime) * (uint64_t)FILE_SECOND_MS;
    }
    return age;
}

mqtt_status mqtt_platform_folder_make(const char *path, bool *made)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    *made = false;
    if (mkdir(path, (mode_t)S_IRWXU) == 0) {
        *made = true;
    } else if (errno != EEXIST) {
        status = MQTT_INVALID_STATE;
    } else {
        /* It is there already. */
    }
    return status;
}

mqtt_status mqtt_platform_folder_sync(const char *path)
{
    /** @brief The folder, opened only to sync it, or -1. */
    const int handle = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    /** @brief The outcome. */
    mqtt_status status = MQTT_INVALID_STATE;

    if (handle >= 0) {
        status = mqtt_platform_file_sync((int32_t)handle);
        (void)close(handle);
    }
    return status;
}

mqtt_status mqtt_platform_file_move(const char *from, const char *to)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (link(from, to) != 0) {
        status = (errno == ENOENT) ? MQTT_NOT_FOUND : MQTT_INVALID_STATE;
    } else if (unlink(from) != 0) {
        status = MQTT_INVALID_STATE;
    } else {
        /* It has its new name, and only that. */
    }
    return status;
}

mqtt_status mqtt_platform_file_remove(const char *path)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (unlink(path) != 0) {
        status = (errno == ENOENT) ? MQTT_NOT_FOUND : MQTT_INVALID_STATE;
    }
    return status;
}

mqtt_status mqtt_platform_folder_remove(const char *path)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (rmdir(path) != 0) {
        status = (errno == ENOENT) ? MQTT_NOT_FOUND : MQTT_INVALID_STATE;
    }
    return status;
}
