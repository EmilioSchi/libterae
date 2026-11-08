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
 * @file platform_store_file.c
 * @brief A store kept in a folder of files.
 *
 * The folder holds snapshot.0 to snapshot.N-1 and journal.0 and journal.1,
 * each readable by its owner alone. A snapshot is written to snapshot.0, which
 * is emptied first, and once it is synced each other copy is written from it,
 * each synced before the next, so a crash damages one copy at most. A journal
 * segment takes the place its generation's parity names. Records are added at
 * the end of their file as they come. A record that can't be written whole is
 * taken back, so the file never holds part of one that this side wrote.
 *
 * Setting a store aside moves every file into a folder of its own, aside.1,
 * aside.2 and so on, with the first number not in use, and starts again with
 * empty files. Nothing is deleted.
 */

#include "base_platform.h"
#include "base_string.h"
#include "platform_store.h"

/** @brief The longest path of a file of a store, its end included. */
#define STORE_PATH_MAX 512U

/** @brief Number of bytes a copy is written from another in. */
#define STORE_COPY_CHUNK 4096U

/** @brief The most folders aside a store looks through for a free one. */
#define STORE_ASIDE_MAX 10000U

/** @brief Number of bytes of a number in a name, at most. */
#define STORE_NUMBER_MAX 10U

/** @brief One store that may be open. */
typedef struct {
    /** @brief true while it is open. */
    bool open;
    /** @brief Number of snapshot copies. */
    uint32_t copies;
    /** @brief The folder, with its end. */
    char where[STORE_PATH_MAX];
    /** @brief Number of bytes of the folder's name. */
    uint32_t where_length;
    /** @brief The snapshot copies. */
    int32_t copy[MQTT_PLATFORM_STORE_COPIES_MAX];
    /** @brief Number of bytes in each copy. */
    uint64_t copy_length[MQTT_PLATFORM_STORE_COPIES_MAX];
    /** @brief The journal places. */
    int32_t journal[2];
    /** @brief Number of bytes in each journal place. */
    uint64_t journal_length[2];
    /** @brief The journal place records go to. */
    uint32_t place;
    /** @brief true once a COMMIT asks for the journal to be synced. */
    bool sync_needed;
} store_entry;

/** @brief The stores. */
static store_entry store_table[MQTT_PLATFORM_STORES];

/**
 * @brief Find an open store.
 * @param[in] store The store.
 * @return Its entry, or NULL if it isn't open.
 */
static store_entry *store_find(int32_t store);

/**
 * @brief Add a name to a path.
 * @param[in,out] path The path.
 * @param[in,out] length Number of bytes in it.
 * @param[in] text The name, with its end.
 * @return true if it fits.
 */
static bool store_add_text(char *path, uint32_t *length, const char *text);

/**
 * @brief Add a number to a path, in decimal.
 * @param[in,out] path The path.
 * @param[in,out] length Number of bytes in it.
 * @param[in] number The number.
 * @return true if it fits.
 */
static bool store_add_number(char *path, uint32_t *length, uint32_t number);

/**
 * @brief Make the path of a file of a store: its folder, perhaps a folder
 * aside, then the file's name and number.
 * @param[in] where The store's folder, STORE_PATH_MAX / 2 bytes at most.
 * @param[in] where_length Number of bytes of its name.
 * @param[in] aside The folder aside, or 0 for none.
 * @param[in] name The file's name, before its number, or NULL for the folder
 * aside itself.
 * @param[in] number The file's number.
 * @param[out] path The path, STORE_PATH_MAX bytes at most.
 * @return true if it fits.
 */
static bool store_path(const char *where, uint32_t where_length,
                       uint32_t aside, const char *name, uint32_t number,
                       char *path);

/**
 * @brief Open every file of a store, and find how long each is.
 * @param[in,out] entry The store, with its folder and copies set.
 * @return MQTT_OK, or MQTT_INVALID_STATE.
 */
static mqtt_status store_open_files(store_entry *entry);

/**
 * @brief Close every file of a store.
 * @param[in,out] entry The store.
 */
static void store_close_files(store_entry *entry);

/**
 * @brief Write the runs of a record at the end of a file, or none of them.
 * @param[in] file The file.
 * @param[in,out] length Number of bytes in the file.
 * @param[in] record The record.
 * @return MQTT_OK, or MQTT_INVALID_STATE.
 */
static mqtt_status store_write_record(int32_t file, uint64_t *length,
                                      const mqtt_store_record *record);

/**
 * @brief Empty a file and write a header at its start.
 * @param[in] file The file.
 * @param[out] length Number of bytes in it after.
 * @param[in] header The MQTT_STORE_HEADER_SIZE bytes.
 * @return MQTT_OK, or MQTT_INVALID_STATE.
 */
static mqtt_status store_restart_file(int32_t file, uint64_t *length,
                                      const uint8_t *header);

/**
 * @brief Write one copy from the first, and sync it.
 * @param[in,out] entry The store.
 * @param[in] copy The copy to write.
 * @return MQTT_OK, or MQTT_INVALID_STATE.
 */
static mqtt_status store_duplicate(store_entry *entry, uint32_t copy);

/**
 * @brief Remove each file a store may have in its folder or a folder aside,
 * and then that folder.
 * @param[in] where The store's folder.
 * @param[in] where_length Number of bytes of its name.
 * @param[in] aside The folder aside, or 0 for the store's own.
 * @return MQTT_OK, MQTT_NOT_FOUND if there is no such folder, or
 * MQTT_INVALID_STATE.
 */
static mqtt_status store_remove_folder(const char *where,
                                       uint32_t where_length, uint32_t aside);

/**
 * @brief Move each file of a store into a folder aside.
 * @param[in] entry The store, whose files are closed.
 * @param[in] aside The folder aside.
 * @return MQTT_OK, or MQTT_INVALID_STATE.
 */
static mqtt_status store_move_files(const store_entry *entry, uint32_t aside);

static store_entry *store_find(int32_t store)
{
    /** @brief The entry. */
    store_entry *entry = NULL;

    if ((store >= 0) && ((uint32_t)store < MQTT_PLATFORM_STORES) &&
        store_table[store].open) {
        entry = &store_table[store];
    }
    return entry;
}

static bool store_add_text(char *path, uint32_t *length, const char *text)
{
    /** @brief Number of bytes of the name. */
    uint32_t size = 0U;
    /** @brief true if it fits. */
    bool fits = mqtt_text_length(text, STORE_PATH_MAX, &size);

    if (fits) {
        fits = ((*length + size) < STORE_PATH_MAX);
    }
    if (fits) {
        mqtt_bytes_put((uint8_t *)&path[*length], (const uint8_t *)text, size);
        *length += size;
        path[*length] = '\0';
    }
    return fits;
}

static bool store_add_number(char *path, uint32_t *length, uint32_t number)
{
    /** @brief Each digit, by its value. */
    static const char names[] = "0123456789";
    /** @brief The digits, from the last. */
    char digits[STORE_NUMBER_MAX + 1U];
    /** @brief Number of digits. */
    uint32_t count = 0U;
    /** @brief The part of the number not written yet. */
    uint32_t rest = number;
    /** @brief The digits, from the first. */
    char text[STORE_NUMBER_MAX + 1U];
    /** @brief Index of the digit being turned. */
    uint32_t index = 0U;

    do {
        digits[count] = names[rest % 10U];
        rest /= 10U;
        count += 1U;
    } while ((rest > 0U) && (count < STORE_NUMBER_MAX));
    while (index < count) {
        text[index] = digits[count - 1U - index];
        index += 1U;
    }
    text[count] = '\0';
    return store_add_text(path, length, text);
}

static bool store_path(const char *where, uint32_t where_length,
                       uint32_t aside, const char *name, uint32_t number,
                       char *path)
{
    /** @brief Number of bytes so far. */
    uint32_t length = where_length;
    /** @brief true while it fits. */
    bool fits = true;

    mqtt_bytes_put((uint8_t *)path, (const uint8_t *)where, where_length);
    path[length] = '\0';
    if (aside != 0U) {
        fits = store_add_text(path, &length, "/aside.");
        if (fits) {
            fits = store_add_number(path, &length, aside);
        }
    }
    if (fits && (name != NULL)) {
        fits = store_add_text(path, &length, "/");
        if (fits) {
            fits = store_add_text(path, &length, name);
        }
        if (fits) {
            fits = store_add_text(path, &length, ".");
        }
        if (fits) {
            fits = store_add_number(path, &length, number);
        }
    }
    return fits;
}

static mqtt_status store_open_files(store_entry *entry)
{
    /** @brief The path of a file. */
    char path[STORE_PATH_MAX];
    /** @brief Index of the file: the copies, then the journal places. */
    uint32_t index = 0U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    while ((status == MQTT_OK) && (index < (entry->copies + 2U))) {
        /** @brief true for a copy, false for a journal place. */
        const bool copy = (index < entry->copies);
        /** @brief The file's number. */
        const uint32_t number = copy ? index : (index - entry->copies);
        /** @brief Where its handle goes. */
        int32_t *file = copy ? &entry->copy[number] : &entry->journal[number];
        /** @brief Where its length goes. */
        uint64_t *length = copy ? &entry->copy_length[number] :
                                  &entry->journal_length[number];

        if (!store_path(entry->where, entry->where_length, 0U,
                        copy ? "snapshot" : "journal", number, path)) {
            status = MQTT_INVALID_STATE;
        } else {
            status = mqtt_platform_file_open(path, file);
        }
        if (status == MQTT_OK) {
            status = mqtt_platform_file_length(*file, length);
        }
        index += 1U;
    }
    if (status == MQTT_OK) {
        status = mqtt_platform_folder_sync(entry->where);
    }
    return status;
}

static void store_close_files(store_entry *entry)
{
    /** @brief Index of the copy. */
    uint32_t index = 0U;

    while (index < MQTT_PLATFORM_STORE_COPIES_MAX) {
        if (entry->copy[index] != MQTT_FILE_NONE) {
            mqtt_platform_file_close(entry->copy[index]);
            entry->copy[index] = MQTT_FILE_NONE;
        }
        index += 1U;
    }
    index = 0U;
    while (index < 2U) {
        if (entry->journal[index] != MQTT_FILE_NONE) {
            mqtt_platform_file_close(entry->journal[index]);
            entry->journal[index] = MQTT_FILE_NONE;
        }
        index += 1U;
    }
}

static mqtt_status store_write_record(int32_t file, uint64_t *length,
                                      const mqtt_store_record *record)
{
    /** @brief Where the next run goes. */
    uint64_t offset = *length;
    /** @brief Index of the run. */
    uint32_t run = 0U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    while ((status == MQTT_OK) && (run < record->run_count)) {
        status = mqtt_platform_file_write(file, offset, record->runs[run].data,
                                          record->runs[run].length);
        offset += (uint64_t)record->runs[run].length;
        run += 1U;
    }
    if (status == MQTT_OK) {
        *length = offset;
    } else if (mqtt_platform_file_truncate(file, *length) != MQTT_OK) {
        /* What was written of it stays, but the next record is written over
         * it, from where this one began. */
    } else {
        /* What was written of it is taken back. */
    }
    return status;
}

static mqtt_status store_restart_file(int32_t file, uint64_t *length,
                                      const uint8_t *header)
{
    /** @brief The outcome. */
    mqtt_status status = mqtt_platform_file_truncate(file, 0U);

    *length = 0U;
    if (status == MQTT_OK) {
        status = mqtt_platform_file_write(file, 0U, header,
                                          MQTT_STORE_HEADER_SIZE);
    }
    if (status == MQTT_OK) {
        *length = MQTT_STORE_HEADER_SIZE;
    }
    return status;
}

static mqtt_status store_duplicate(store_entry *entry, uint32_t copy)
{
    /** @brief The bytes being copied. */
    static uint8_t chunk[STORE_COPY_CHUNK];
    /** @brief Offset of the next chunk. */
    uint64_t offset = 0U;
    /** @brief The outcome. */
    mqtt_status status = mqtt_platform_file_truncate(entry->copy[copy], 0U);

    entry->copy_length[copy] = 0U;
    while ((status == MQTT_OK) && (offset < entry->copy_length[0])) {
        /** @brief Number of bytes read. */
        uint32_t count = 0U;

        status = mqtt_platform_file_read(entry->copy[0], offset, chunk,
                                         STORE_COPY_CHUNK, &count);
        if ((status == MQTT_OK) && (count == 0U)) {
            status = MQTT_INVALID_STATE;
        }
        if (status == MQTT_OK) {
            status = mqtt_platform_file_write(entry->copy[copy], offset, chunk,
                                              count);
        }
        offset += (uint64_t)count;
    }
    if (status == MQTT_OK) {
        status = mqtt_platform_file_sync(entry->copy[copy]);
    }
    if (status == MQTT_OK) {
        entry->copy_length[copy] = entry->copy_length[0];
    }
    return status;
}

static mqtt_status store_move_files(const store_entry *entry, uint32_t aside)
{
    /** @brief The path a file has. */
    char from[STORE_PATH_MAX];
    /** @brief The path it gets. */
    char to[STORE_PATH_MAX];
    /** @brief Index of the file: the copies, then the journal places. */
    uint32_t index = 0U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    while ((status == MQTT_OK) && (index < (entry->copies + 2U))) {
        /** @brief true for a copy, false for a journal place. */
        const bool copy = (index < entry->copies);
        /** @brief The file's name. */
        const char *name = copy ? "snapshot" : "journal";
        /** @brief The file's number. */
        const uint32_t number = copy ? index : (index - entry->copies);

        /** @brief true if both paths fit. */
        bool fits = store_path(entry->where, entry->where_length, 0U, name,
                               number, from);

        if (fits) {
            fits = store_path(entry->where, entry->where_length, aside, name,
                              number, to);
        }
        if (!fits) {
            status = MQTT_INVALID_STATE;
        } else {
            status = mqtt_platform_file_move(from, to);
            if (status == MQTT_NOT_FOUND) {
                status = MQTT_OK;
            }
        }
        index += 1U;
    }
    return status;
}

static mqtt_status store_remove_folder(const char *where,
                                       uint32_t where_length, uint32_t aside)
{
    /** @brief The path of a file, then of the folder. */
    char path[STORE_PATH_MAX];
    /** @brief Index of the file: each copy there may be, then the journal. */
    uint32_t index = 0U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    while ((status == MQTT_OK) &&
           (index < (MQTT_PLATFORM_STORE_COPIES_MAX + 2U))) {
        /** @brief true for a copy, false for a journal place. */
        const bool copy = (index < MQTT_PLATFORM_STORE_COPIES_MAX);
        /** @brief The file's number. */
        const uint32_t number =
            copy ? index : (index - MQTT_PLATFORM_STORE_COPIES_MAX);

        status = store_path(where, where_length, aside,
                            copy ? "snapshot" : "journal", number, path) ?
                     mqtt_platform_file_remove(path) :
                     MQTT_INVALID_STATE;
        if (status == MQTT_NOT_FOUND) {
            status = MQTT_OK;
        }
        index += 1U;
    }
    if (status == MQTT_OK) {
        status = store_path(where, where_length, aside, NULL, 0U, path) ?
                     mqtt_platform_folder_remove(path) :
                     MQTT_INVALID_STATE;
    }
    return status;
}

mqtt_status mqtt_platform_store_open(const char *where, uint32_t copies,
                                     int32_t *store)
{
    /** @brief Index of the entry being looked at. */
    uint32_t index = 0U;
    /** @brief Number of bytes of the folder's name. */
    uint32_t length = 0U;
    /** @brief true if the folder was made now. */
    bool made = false;
    /** @brief The outcome. */
    mqtt_status status = MQTT_INVALID_ARGUMENT;

    *store = MQTT_STORE_NONE;
    if (!mqtt_text_length(where, STORE_PATH_MAX / 2U, &length)) {
        /* No folder, or one whose name is too long. */
        length = 0U;
    }
    if ((length > 0U) && (copies >= MQTT_PLATFORM_STORE_COPIES_MIN) &&
        (copies <= MQTT_PLATFORM_STORE_COPIES_MAX)) {
        status = MQTT_QUOTA_EXCEEDED;
        while ((index < MQTT_PLATFORM_STORES) && store_table[index].open) {
            index += 1U;
        }
    }
    if ((status == MQTT_QUOTA_EXCEEDED) && (index < MQTT_PLATFORM_STORES)) {
        /** @brief The entry. */
        store_entry *entry = &store_table[index];
        /** @brief Index of a copy. */
        uint32_t copy = 0U;

        mqtt_bytes_put((uint8_t *)entry->where, (const uint8_t *)where,
                       length + 1U);
        entry->where_length = length;
        entry->copies = copies;
        while (copy < MQTT_PLATFORM_STORE_COPIES_MAX) {
            entry->copy[copy] = MQTT_FILE_NONE;
            entry->copy_length[copy] = 0U;
            copy += 1U;
        }
        entry->journal[0] = MQTT_FILE_NONE;
        entry->journal[1] = MQTT_FILE_NONE;
        entry->place = 0U;
        entry->sync_needed = false;
        status = mqtt_platform_folder_make(where, &made);
        if (status == MQTT_OK) {
            status = store_open_files(entry);
        }
        if (status == MQTT_OK) {
            entry->open = true;
            *store = (int32_t)index;
        } else {
            store_close_files(entry);
        }
    }
    return status;
}

void mqtt_platform_store_close(int32_t store)
{
    /** @brief The store. */
    store_entry *entry = store_find(store);

    if (entry != NULL) {
        store_close_files(entry);
        entry->open = false;
    }
}

uint32_t mqtt_platform_store_copies(int32_t store)
{
    /** @brief The store. */
    const store_entry *entry = store_find(store);

    return (entry == NULL) ? 0U : entry->copies;
}

mqtt_status mqtt_platform_store_append(uint32_t user_tag,
                                       const mqtt_store_record *record)
{
    /** @brief The store. */
    store_entry *entry = store_find((int32_t)user_tag);
    /** @brief The outcome. */
    mqtt_status status = MQTT_INVALID_STATE;

    if (entry != NULL) {
        status = store_write_record(entry->journal[entry->place],
                                    &entry->journal_length[entry->place],
                                    record);
    }
    if ((status == MQTT_OK) &&
        (record->kind == (uint8_t)MQTT_STORE_RECORD_COMMIT) &&
        (record->durability == MQTT_STORE_SYNCED)) {
        entry->sync_needed = true;
    }
    return status;
}

mqtt_status mqtt_platform_store_commit(int32_t store)
{
    /** @brief The store. */
    store_entry *entry = store_find(store);
    /** @brief The outcome. */
    mqtt_status status = (entry == NULL) ? MQTT_INVALID_STATE : MQTT_OK;

    if ((status == MQTT_OK) && entry->sync_needed) {
        status = mqtt_platform_file_sync(entry->journal[entry->place]);
        entry->sync_needed = false;
    }
    return status;
}

mqtt_status mqtt_platform_store_read(uint32_t user_tag, mqtt_store_part part,
                                     uint32_t place, uint64_t offset,
                                     uint8_t *data, uint32_t capacity,
                                     uint32_t *count)
{
    /** @brief The store. */
    const store_entry *entry = store_find((int32_t)user_tag);
    /** @brief The file. */
    int32_t file = MQTT_FILE_NONE;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    *count = 0U;
    if (entry == NULL) {
        status = MQTT_INVALID_STATE;
    } else if ((part == MQTT_STORE_SNAPSHOT) && (place < entry->copies)) {
        file = entry->copy[place];
    } else if ((part == MQTT_STORE_JOURNAL) && (place < 2U)) {
        file = entry->journal[place];
    } else {
        /* No such place: nothing in it. */
    }
    if (file != MQTT_FILE_NONE) {
        status = mqtt_platform_file_read(file, offset, data, capacity, count);
    }
    return status;
}

mqtt_status mqtt_platform_store_segment(int32_t store, const uint8_t *header)
{
    /** @brief The store. */
    store_entry *entry = store_find(store);
    /** @brief The outcome. */
    mqtt_status status = MQTT_INVALID_STATE;

    if (entry != NULL) {
        /** @brief The place its generation takes. */
        const uint32_t place = (uint32_t)(mqtt_store_get_u64(&header[8]) % 2U);

        status = store_restart_file(entry->journal[place],
                                    &entry->journal_length[place], header);
        if (status == MQTT_OK) {
            status = mqtt_platform_file_sync(entry->journal[place]);
        }
        if (status == MQTT_OK) {
            entry->place = place;
            entry->sync_needed = false;
        }
    }
    return status;
}

mqtt_status mqtt_platform_store_copy_begin(int32_t store,
                                           const uint8_t *header)
{
    /** @brief The store. */
    store_entry *entry = store_find(store);

    return (entry == NULL) ?
               MQTT_INVALID_STATE :
               store_restart_file(entry->copy[0], &entry->copy_length[0],
                                  header);
}

mqtt_status mqtt_platform_store_copy_record(int32_t store,
                                            const mqtt_store_record *record)
{
    /** @brief The store. */
    store_entry *entry = store_find(store);

    return (entry == NULL) ?
               MQTT_INVALID_STATE :
               store_write_record(entry->copy[0], &entry->copy_length[0],
                                  record);
}

mqtt_status mqtt_platform_store_copy_end(int32_t store)
{
    /** @brief The store. */
    const store_entry *entry = store_find(store);

    return (entry == NULL) ? MQTT_INVALID_STATE :
                             mqtt_platform_file_sync(entry->copy[0]);
}

mqtt_status mqtt_platform_store_copy_duplicate(int32_t store)
{
    /** @brief The store. */
    store_entry *entry = store_find(store);
    /** @brief Index of the copy being written. */
    uint32_t copy = 1U;
    /** @brief The outcome. */
    mqtt_status status = (entry == NULL) ? MQTT_INVALID_STATE : MQTT_OK;

    while ((status == MQTT_OK) && (copy < entry->copies)) {
        status = store_duplicate(entry, copy);
        copy += 1U;
    }
    return status;
}

mqtt_status mqtt_platform_store_set_aside(int32_t store, uint32_t *aside)
{
    /** @brief The store. */
    store_entry *entry = store_find(store);
    /** @brief The path of the folder aside. */
    char path[STORE_PATH_MAX];
    /** @brief The number of the folder aside. */
    uint32_t number = 0U;
    /** @brief true once a new folder aside is made. */
    bool made = false;
    /** @brief The outcome. */
    mqtt_status status = (entry == NULL) ? MQTT_INVALID_STATE : MQTT_OK;

    *aside = 0U;
    while ((status == MQTT_OK) && (!made) && (number < STORE_ASIDE_MAX)) {
        number += 1U;
        status = store_path(entry->where, entry->where_length, number, NULL,
                            0U, path) ?
                     mqtt_platform_folder_make(path, &made) :
                     MQTT_INVALID_STATE;
    }
    if ((status == MQTT_OK) && (!made)) {
        status = MQTT_INVALID_STATE;
    }
    if (status == MQTT_OK) {
        store_close_files(entry);
        status = store_move_files(entry, number);
    }
    if (status == MQTT_OK) {
        status = mqtt_platform_folder_sync(path);
    }
    if (status == MQTT_OK) {
        status = store_open_files(entry);
    }
    if (status == MQTT_OK) {
        *aside = number;
    }
    return status;
}

uint8_t mqtt_platform_store_erased(int32_t store)
{
    /* A file reads as nothing past its end, never as erased bytes. */
    (void)store;
    return 0x00U;
}

uint64_t mqtt_platform_store_capacity(int32_t store)
{
    return (store_find(store) == NULL) ? 0U : UINT64_MAX;
}

uint64_t mqtt_platform_store_downtime(int32_t store)
{
    /** @brief The store. */
    const store_entry *entry = store_find(store);
    /** @brief The answer. */
    uint64_t downtime = 0U;

    if (entry != NULL) {
        /** @brief Index of the journal place. */
        uint32_t index = 0U;
        /** @brief true once a place has been looked at. */
        bool seen = false;

        while (index < 2U) {
            if (entry->journal_length[index] > 0U) {
                /** @brief How long ago it was written. */
                const uint64_t age =
                    mqtt_platform_file_age_ms(entry->journal[index]);

                if ((!seen) || (age < downtime)) {
                    downtime = age;
                    seen = true;
                }
            }
            index += 1U;
        }
    }
    return downtime;
}

mqtt_status mqtt_platform_store_remove(const char *where)
{
    /** @brief Number of bytes of the folder's name. */
    uint32_t length = 0U;
    /** @brief The number of the folder aside being removed. */
    uint32_t aside = 1U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (!mqtt_text_length(where, STORE_PATH_MAX / 2U, &length)) {
        length = 0U;
    }
    if (length == 0U) {
        status = MQTT_INVALID_STATE;
    }
    /* The folders aside are numbered from 1 with no gap, so the first one
     * missing is the end of them. */
    while ((status == MQTT_OK) && (aside < STORE_ASIDE_MAX)) {
        status = store_remove_folder(where, length, aside);
        aside += 1U;
    }
    if (status == MQTT_NOT_FOUND) {
        status = store_remove_folder(where, length, 0U);
    }
    return status;
}
