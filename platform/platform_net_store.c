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
 * @file platform_net_store.c
 * @brief The store of a server or a client that runs on real network
 * connections.
 *
 * Each round, once its events are handled, the records they wrote are closed
 * into a group, which is synced if one of them asked for it, and only then are
 * the answers that waited for the group let go. One sync covers everything
 * that came in the round.
 *
 * A snapshot is written about NET_STORE_ROUND bytes a round, so the
 * connections don't wait for it. Its journal segment starts first, as the
 * records written while it is taken belong to it, then the first copy is
 * written, then the others from it. Until the first copy is whole, the copies
 * before it and both journal places are as they were, so a crash loses
 * nothing; after, the first copy and its segment hold everything. A copy that
 * can't be written gives the snapshot up, to be taken again a second later. If
 * the others can't be written from the first, that is tried again, and the
 * first copy is kept.
 *
 * The other journal place may be needed until a snapshot is whole: after a
 * restore that read both, or after a gap in the journal, when the snapshot
 * that mends it is not whole yet. A snapshot that starts a new segment then is
 * written in one go, with the server stopped, and its segment starts only once
 * its first copy is whole and synced. A start writes one so before it accepts
 * any connection, and fails if it can't.
 *
 * A client's snapshot holds only what its memory holds, so it is always taken
 * whole, between two steps, in the same order: the first copy, its segment,
 * then the others. Its records are closed each step, once what arrived is
 * handled, and a step doesn't wait while some are open.
 *
 * Restoring a store that is damaged, or that doesn't fit the server or the
 * client, sets it aside untouched, says why on the console, and starts empty.
 * Nothing of it is restored by halves. A program runs one client with a store
 * at a time, as it runs one server.
 */

#include <stddef.h>

#include "base_net.h"
#include "base_platform.h"
#include "base_server.h"
#include "base_store.h"
#include "platform_net.h"

/**
 * @brief Number of bytes of snapshot written in a round, after which it stops
 * at the next message or Session.
 */
#define NET_STORE_ROUND (64U * 1024U)
/** @brief How long to wait, in milliseconds, after a snapshot failed. */
#define NET_STORE_RETRY_MS 1000U
/**
 * @brief Number of bytes a restore reads into, and a snapshot gathers each
 * record in: any record of a restore fits, and a Topic Filter of the longest.
 */
#define NET_STORE_SPACE MQTT_SERVER_RESTORE_SPACE
/** @brief Number of characters a 64-bit number takes in decimal, at most. */
#define NET_STORE_DIGITS 20U
/**
 * @brief Number of bytes of a client's journal segment after which a snapshot
 * is due: its snapshot is small, so it is taken often.
 */
#define NET_CLIENT_SNAPSHOT_BYTES (1024U * 1024U)
/** @brief Number of bytes a client's restore reads into. */
#define NET_CLIENT_RESTORE_BYTES 4096U

/** @brief Where the snapshot of a running server is. */
typedef enum {
    /** @brief No snapshot is under way. */
    NET_STORE_IDLE = 0,
    /** @brief The first copy is being written. */
    NET_STORE_COPYING,
    /** @brief The first copy is whole, and the others are written from it. */
    NET_STORE_DUPLICATING
} net_store_phase;

/** @brief What the loop keeps of a server's store. */
typedef struct {
    /** @brief The store, or MQTT_STORE_NONE. */
    int32_t store;
    /** @brief Where the snapshot is. */
    net_store_phase phase;
    /** @brief The time before which nothing is tried again after a failure. */
    uint64_t resume_ms;
} net_store_state;

/** @brief What the loop keeps of a client's store. */
typedef struct {
    /** @brief The store, or MQTT_STORE_NONE. */
    int32_t store;
    /** @brief The client it is the store of. */
    const mqtt_client *client;
    /**
     * @brief true while the other copies have still to be written from the
     * first.
     */
    bool duplicating;
    /** @brief The time before which nothing is tried again after a failure. */
    uint64_t resume_ms;
} net_client_store_state;

/** @brief The store of the server that runs. */
static net_store_state net_store = {MQTT_STORE_NONE, NET_STORE_IDLE, 0U};

/** @brief The store of the client that runs. */
static net_client_store_state net_client_store = {MQTT_STORE_NONE, NULL,
                                                  false, 0U};

/** @brief Where a restore reads, and a snapshot gathers each record. */
static uint8_t net_store_space[NET_STORE_SPACE];

/**
 * @brief Write the records of the snapshot under way to its first copy.
 * @param[in,out] server The server.
 * @param[in] budget Number of bytes after which to stop, where a message or a
 * Session starts, or 0 to write them all.
 * @return MQTT_OK if more are to come, MQTT_NOT_FOUND once END is written, or
 * why a record couldn't be had or written.
 */
static mqtt_status net_store_records(mqtt_server *server, uint32_t budget)
{
    /** @brief Number of bytes written. */
    uint64_t written = 0U;
    /** @brief true if the next record starts a message or a Session. */
    bool between = false;
    /** @brief true once the budget is spent. */
    bool spent = false;
    /** @brief The record. */
    mqtt_store_record record;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    while ((status == MQTT_OK) && (!spent)) {
        status = mqtt_server_snapshot_next(server, net_store_space,
                                           NET_STORE_SPACE, &record, &between);
        if (status == MQTT_OK) {
            status = mqtt_platform_store_copy_record(net_store.store, &record);
            written += (uint64_t)record.length;
        }
        spent = between && (budget > 0U) && (written >= (uint64_t)budget);
    }
    return status;
}

/**
 * @brief Write the other copies from the first, and finish the snapshot once
 * they are whole, or try again a second later, which the phase then says.
 * @param[in,out] server The server.
 * @param[in] now_ms The time.
 */
static void net_store_duplicate(mqtt_server *server, uint64_t now_ms)
{
    if (mqtt_platform_store_copy_duplicate(net_store.store) == MQTT_OK) {
        mqtt_server_snapshot_done(server);
        net_store.phase = NET_STORE_IDLE;
    } else {
        net_store.phase = NET_STORE_DUPLICATING;
        net_store.resume_ms = now_ms + (uint64_t)NET_STORE_RETRY_MS;
    }
}

/**
 * @brief Give up the snapshot under way, to be taken again a second later.
 * @param[in,out] server The server.
 * @param[in] now_ms The time.
 * @param[in] gap true if records may have been lost, as when a journal
 * segment could not be started.
 */
static void net_store_give_up(mqtt_server *server, uint64_t now_ms, bool gap)
{
    mqtt_server_snapshot_abort(server);
    if (gap) {
        mqtt_server_store_failed(server);
    }
    net_store.phase = NET_STORE_IDLE;
    net_store.resume_ms = now_ms + (uint64_t)NET_STORE_RETRY_MS;
}

/**
 * @brief Write a whole snapshot, with the server stopped: its first copy, then
 * its journal segment if it starts one, then the other copies.
 * @param[in,out] server The server.
 * @param[in] now_ms The time.
 * @return MQTT_OK, or why it couldn't be written.
 */
static mqtt_status net_store_whole(mqtt_server *server, uint64_t now_ms)
{
    /** @brief The header of the copies. */
    uint8_t snapshot_header[MQTT_STORE_HEADER_SIZE];
    /** @brief The header of the journal segment. */
    uint8_t journal_header[MQTT_STORE_HEADER_SIZE];
    /** @brief true if a journal segment starts. */
    bool new_segment = false;
    /** @brief true once the journal segment is whole, if one starts. */
    bool segment_written = false;
    /** @brief The outcome. */
    mqtt_status status = mqtt_server_snapshot_begin(
        server, now_ms, snapshot_header, journal_header, &new_segment);
    /** @brief true once the snapshot has begun. */
    const bool begun = (status == MQTT_OK);

    if (status == MQTT_OK) {
        status = mqtt_platform_store_copy_begin(net_store.store,
                                                snapshot_header);
    }
    if (status == MQTT_OK) {
        status = net_store_records(server, 0U);
    }
    if (status == MQTT_NOT_FOUND) {
        status = mqtt_platform_store_copy_end(net_store.store);
    }
    if ((status == MQTT_OK) && new_segment) {
        status = mqtt_platform_store_segment(net_store.store, journal_header);
        segment_written = (status == MQTT_OK);
    }
    if (status == MQTT_OK) {
        net_store_duplicate(server, now_ms);
        if (net_store.phase != NET_STORE_IDLE) {
            /* The first copy is whole with its segment, and is kept. */
            status = MQTT_INVALID_STATE;
        }
    } else if (begun) {
        /* The journal places are as they were if no segment started, and the
         * server has written nothing since the snapshot began. */
        net_store_give_up(server, now_ms, new_segment && (!segment_written));
    } else {
        /* It didn't begin, so there is nothing to give up. */
    }
    return status;
}

/**
 * @brief Begin a snapshot that is written a little each round: start its
 * journal segment if it starts one, then its first copy.
 * @param[in,out] server The server.
 * @param[in] now_ms The time.
 */
static void net_store_begin(mqtt_server *server, uint64_t now_ms)
{
    /** @brief The header of the copies. */
    uint8_t snapshot_header[MQTT_STORE_HEADER_SIZE];
    /** @brief The header of the journal segment. */
    uint8_t journal_header[MQTT_STORE_HEADER_SIZE];
    /** @brief true if a journal segment starts. */
    bool new_segment = false;
    /**
     * @brief true if the segment could not be started, so the records written
     * from now on would have nowhere to go.
     */
    bool gap = false;
    /** @brief The outcome. */
    mqtt_status status = mqtt_server_snapshot_begin(
        server, now_ms, snapshot_header, journal_header, &new_segment);
    /** @brief true once the snapshot has begun. */
    const bool begun = (status == MQTT_OK);

    if ((status == MQTT_OK) && new_segment) {
        status = mqtt_platform_store_segment(net_store.store, journal_header);
        gap = (status != MQTT_OK);
    }
    if (status == MQTT_OK) {
        status = mqtt_platform_store_copy_begin(net_store.store,
                                                snapshot_header);
    }
    if (status == MQTT_OK) {
        net_store.phase = NET_STORE_COPYING;
    } else if (begun) {
        net_store_give_up(server, now_ms, gap);
    } else {
        /* A group is still open, which the next round closes. */
    }
}

/**
 * @brief Write the next part of the snapshot under way, and finish its first
 * copy once END is written.
 * @param[in,out] server The server.
 * @param[in] now_ms The time.
 */
static void net_store_copy(mqtt_server *server, uint64_t now_ms)
{
    /** @brief The outcome. */
    mqtt_status status = net_store_records(server, NET_STORE_ROUND);

    if (status == MQTT_NOT_FOUND) {
        status = mqtt_platform_store_copy_end(net_store.store);
        if (status == MQTT_OK) {
            net_store_duplicate(server, now_ms);
        }
    }
    if ((status != MQTT_OK) && (status != MQTT_NOT_FOUND)) {
        /* The other copies are whole, so the first is begun again. */
        net_store_give_up(server, now_ms, false);
    }
}

/**
 * @brief Close the round's records into a group, sync it if it asks for that,
 * and let go of the answers that waited for it.
 * @param[in,out] server The server.
 * @param[in] now_ms The time.
 * @return true if the journal is broken.
 */
static bool net_store_commit(mqtt_server *server, uint64_t now_ms)
{
    /** @brief The last group closed. */
    uint64_t sequence = 0U;
    /** @brief true if the journal is broken. */
    bool broken = false;

    if (mqtt_server_store_commit(server, now_ms, &sequence) != MQTT_OK) {
        broken = true;
    } else if (mqtt_platform_store_commit(net_store.store) != MQTT_OK) {
        /* What the round wrote may be lost: every answer waits for a
         * snapshot. */
        mqtt_server_store_failed(server);
        broken = true;
    } else {
        mqtt_server_stored(server, sequence);
    }
    return broken;
}

mqtt_status platform_net_store_start(mqtt_server *server, int32_t store)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    net_store.store = store;
    net_store.phase = NET_STORE_IDLE;
    net_store.resume_ms = 0U;
    if (store != MQTT_STORE_NONE) {
        status = net_store_whole(server, mqtt_platform_time_ms());
    } else if (mqtt_server_snapshot_due(server)) {
        /* The server keeps a store, and has none to write to. */
        status = MQTT_INVALID_ARGUMENT;
    } else {
        /* Nothing is kept. */
    }
    return status;
}

void platform_net_store_round(mqtt_server *server)
{
    if (net_store.store != MQTT_STORE_NONE) {
        /** @brief The time. */
        const uint64_t now_ms = mqtt_platform_time_ms();
        /** @brief true if the journal is broken. */
        const bool broken = net_store_commit(server, now_ms);
        /** @brief true if nothing waits for a second to pass. */
        const bool ready = now_ms >= net_store.resume_ms;

        if ((net_store.phase == NET_STORE_COPYING) && broken) {
            /* The snapshot that mends the journal is begun instead, now. */
            net_store_give_up(server, now_ms, false);
            net_store.resume_ms = now_ms;
        } else if (net_store.phase == NET_STORE_COPYING) {
            net_store_copy(server, now_ms);
        } else if ((net_store.phase == NET_STORE_DUPLICATING) && ready) {
            net_store_duplicate(server, now_ms);
        } else {
            /* Nothing under way, or a duplicate that waits. */
        }
        if ((net_store.phase != NET_STORE_IDLE) ||
            (now_ms < net_store.resume_ms)) {
            /* A snapshot is under way, or one failed a moment ago. */
        } else if (!mqtt_server_snapshot_due(server)) {
            /* The journal hasn't grown enough for one. */
        } else if (!broken) {
            net_store_begin(server, now_ms);
        } else if (net_store_whole(server, now_ms) != MQTT_OK) {
            /* Taken again a second later, and every answer waits. */
        } else {
            /* Mended: the answers go once the next group is stored. */
        }
    }
}

/**
 * @brief Find how long a wait may be, when the store has something to do once
 * a failure is a second old.
 * @param[in] wait_ms How long the server allows.
 * @return Milliseconds.
 */
static uint32_t net_store_until_resume(uint32_t wait_ms)
{
    /** @brief The time. */
    const uint64_t now_ms = mqtt_platform_time_ms();
    /** @brief Milliseconds until it is tried again. */
    const uint64_t left = (net_store.resume_ms > now_ms) ?
                              (net_store.resume_ms - now_ms) :
                              0U;

    return (left < (uint64_t)wait_ms) ? (uint32_t)left : wait_ms;
}

uint32_t platform_net_store_wait(const mqtt_server *server, uint32_t wait_ms)
{
    /** @brief The answer. */
    uint32_t wait = wait_ms;

    if (net_store.store == MQTT_STORE_NONE) {
        /* Nothing is kept. */
    } else if (mqtt_server_store_open(server)) {
        /* The records written since the round began are closed at once. */
        wait = 0U;
    } else if (net_store.phase == NET_STORE_COPYING) {
        wait = 0U;
    } else if (net_store.phase == NET_STORE_DUPLICATING) {
        wait = net_store_until_resume(wait_ms);
    } else if (mqtt_server_snapshot_due(server)) {
        wait = net_store_until_resume(wait_ms);
    } else {
        /* Nothing waits to be written. */
    }
    return wait;
}

void platform_net_store_stop(mqtt_server *server)
{
    if (net_store.store != MQTT_STORE_NONE) {
        /* What closing the connections wrote is closed and synced too. */
        (void)net_store_commit(server, mqtt_platform_time_ms());
        net_store.store = MQTT_STORE_NONE;
        net_store.phase = NET_STORE_IDLE;
    }
}

/**
 * @brief Write a number in decimal to the console.
 * @param[in] value The number.
 */
static void net_store_print_number(uint64_t value)
{
    /** @brief The digits. */
    static const char digits[] = "0123456789";
    /** @brief The text, filled from the end. */
    char text[NET_STORE_DIGITS + 1U];
    /** @brief Index of the first character. */
    uint32_t index = NET_STORE_DIGITS;
    /** @brief The digits not written yet. */
    uint64_t rest = value;

    text[NET_STORE_DIGITS] = '\0';
    do {
        index -= 1U;
        text[index] = digits[rest % 10U];
        rest /= 10U;
    } while (rest > 0U);
    mqtt_platform_console_write(&text[index]);
}

/**
 * @brief Say on the console why a store was set aside, and where it went.
 * @param[in] fault What was wrong.
 * @param[in] aside The number of the place it went to.
 * @param[in] who What starts empty: the server or the client.
 */
static void net_store_report(const mqtt_store_fault *fault, uint32_t aside,
                             const char *who)
{
    mqtt_platform_console_write("the store was set aside as number ");
    net_store_print_number((uint64_t)aside);
    mqtt_platform_console_write(", and ");
    mqtt_platform_console_write(who);
    mqtt_platform_console_write(" starts empty: ");
    mqtt_platform_console_write(mqtt_store_fault_name(fault->kind));
    /* When no copy passes, the copies' own faults are not kept. */
    if (fault->kind != MQTT_STORE_FAULT_NO_COPY) {
        mqtt_platform_console_write((fault->part == MQTT_STORE_SNAPSHOT) ?
                                        ", in snapshot copy " :
                                        ", in journal place ");
        net_store_print_number((uint64_t)fault->place);
        mqtt_platform_console_write(" of generation ");
        net_store_print_number(fault->generation);
        mqtt_platform_console_write(", at byte ");
        net_store_print_number(fault->offset);
    }
    if (fault->record_kind != 0U) {
        mqtt_platform_console_write(", in a record of kind ");
        net_store_print_number((uint64_t)fault->record_kind);
    }
    mqtt_platform_console_write("\n");
}

/**
 * @brief Find how much journal a snapshot is due after, as a store's places
 * allow: half a place at most, so that a segment never fills its place.
 * @param[in] store The store.
 * @param[in] wanted What the configuration would take otherwise.
 * @return Number of bytes.
 */
static uint64_t net_store_due_after(int32_t store, uint64_t wanted)
{
    /** @brief Number of bytes of a place. */
    const uint64_t capacity = mqtt_platform_store_capacity(store);

    return ((capacity / 2U) < wanted) ? (capacity / 2U) : wanted;
}

void mqtt_net_server_store_config(mqtt_server_config *config, int32_t store)
{
    config->store_append = &mqtt_platform_store_append;
    config->user_tag = (uint32_t)store;
    config->store_sessions = MQTT_STORE_SYNCED;
    config->store_messages = MQTT_STORE_SYNCED;
    config->store_snapshot_bytes =
        net_store_due_after(store, config->store_snapshot_bytes);
}

mqtt_status mqtt_net_server_restore(mqtt_server *server, int32_t store,
                                    mqtt_server_restore_entry *map,
                                    uint32_t map_count,
                                    mqtt_store_fault *fault)
{
    /** @brief The store, to read from. */
    mqtt_store_source source;
    /** @brief The number of the place a damaged store went to. */
    uint32_t aside = 0U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    source.read = &mqtt_platform_store_read;
    source.user_tag = (uint32_t)store;
    source.copies = mqtt_platform_store_copies(store);
    source.erased = mqtt_platform_store_erased(store);
    source.downtime_ms = mqtt_platform_store_downtime(store);
    fault->kind = MQTT_STORE_FAULT_NONE;
    status = mqtt_server_restore(server, &source, map, map_count,
                                 net_store_space, NET_STORE_SPACE,
                                 mqtt_platform_time_ms(), fault);
    if (status == MQTT_STORE_DAMAGED) {
        /* Never written over: a store that can't be set aside stops the
         * start. */
        status = mqtt_platform_store_set_aside(store, &aside);
        if (status == MQTT_OK) {
            net_store_report(fault, aside, "the server");
        }
    }
    return status;
}

/**
 * @brief Take a client's snapshot whole: its first copy, then its journal
 * segment if it starts one, then the other copies, which are tried again
 * later if they can't be written now.
 * @param[in,out] client The client.
 * @param[in] now_ms The time.
 * @return MQTT_OK once the first copy and its segment are whole, or why they
 * couldn't be written.
 */
static mqtt_status net_client_whole(mqtt_client *client, uint64_t now_ms)
{
    /** @brief Where each record's frame and fixed fields are gathered. */
    static uint8_t net_client_space[MQTT_CLIENT_SNAPSHOT_SPACE];
    /** @brief The header of the copies. */
    uint8_t snapshot_header[MQTT_STORE_HEADER_SIZE];
    /** @brief The header of the journal segment. */
    uint8_t journal_header[MQTT_STORE_HEADER_SIZE];
    /** @brief The record. */
    mqtt_store_record record;
    /** @brief true if a journal segment starts. */
    bool new_segment = false;
    /** @brief true once the journal segment is whole, if one starts. */
    bool segment_written = false;
    /** @brief The store. */
    const int32_t store = net_client_store.store;
    /** @brief The outcome. */
    mqtt_status status = mqtt_client_snapshot_begin(
        client, now_ms, snapshot_header, journal_header, &new_segment);
    /** @brief true once the snapshot has begun. */
    const bool begun = (status == MQTT_OK);

    if (status == MQTT_OK) {
        status = mqtt_platform_store_copy_begin(store, snapshot_header);
    }
    while (status == MQTT_OK) {
        status = mqtt_client_snapshot_next(client, net_client_space,
                                           MQTT_CLIENT_SNAPSHOT_SPACE, &record);
        if (status == MQTT_OK) {
            status = mqtt_platform_store_copy_record(store, &record);
        }
    }
    if (status == MQTT_NOT_FOUND) {
        status = mqtt_platform_store_copy_end(store);
    }
    if ((status == MQTT_OK) && new_segment) {
        status = mqtt_platform_store_segment(store, journal_header);
        segment_written = (status == MQTT_OK);
    }
    if (status == MQTT_OK) {
        mqtt_client_snapshot_done(client);
        net_client_store.duplicating =
            (mqtt_platform_store_copy_duplicate(store) != MQTT_OK);
        net_client_store.resume_ms = now_ms + (uint64_t)NET_STORE_RETRY_MS;
    } else if (begun) {
        /* The journal places are as they were if no segment started, and the
         * client has written nothing since the snapshot began. */
        mqtt_client_snapshot_abort(client);
        if (new_segment && (!segment_written)) {
            mqtt_client_store_failed(client);
        }
        net_client_store.resume_ms = now_ms + (uint64_t)NET_STORE_RETRY_MS;
    } else {
        /* It didn't begin, so there is nothing to give up. */
    }
    return status;
}

void platform_net_client_store_round(mqtt_client *client)
{
    if ((net_client_store.store != MQTT_STORE_NONE) &&
        (net_client_store.client == client)) {
        /** @brief The time. */
        const uint64_t now_ms = mqtt_platform_time_ms();
        /** @brief The last group closed. */
        uint64_t sequence = 0U;

        if (mqtt_client_store_commit(client, now_ms, &sequence) != MQTT_OK) {
            /* The journal is broken, and a snapshot mends it. */
        } else if (mqtt_platform_store_commit(net_client_store.store) !=
                   MQTT_OK) {
            mqtt_client_store_failed(client);
        } else {
            mqtt_client_stored(client, sequence);
        }
        if (now_ms < net_client_store.resume_ms) {
            /* Something failed a moment ago. */
        } else if (net_client_store.duplicating) {
            net_client_store.duplicating =
                (mqtt_platform_store_copy_duplicate(net_client_store.store) !=
                 MQTT_OK);
            net_client_store.resume_ms = now_ms + (uint64_t)NET_STORE_RETRY_MS;
        } else if (!mqtt_client_snapshot_due(client)) {
            /* The journal hasn't grown enough for one. */
        } else if (net_client_whole(client, now_ms) != MQTT_OK) {
            /* Taken again a second later, and every answer waits. */
        } else {
            /* Whole: the journal starts again. */
        }
    }
}

void mqtt_net_client_store_config(mqtt_client_store_config *config,
                                  int32_t store)
{
    config->append = &mqtt_platform_store_append;
    config->user_tag = (uint32_t)store;
    config->sessions = MQTT_STORE_SYNCED;
    config->messages = MQTT_STORE_SYNCED;
    config->snapshot_bytes =
        net_store_due_after(store, NET_CLIENT_SNAPSHOT_BYTES);
}

mqtt_status mqtt_net_client_restore(mqtt_client *client, int32_t store,
                                    mqtt_store_fault *fault)
{
    /** @brief Room for the reads. */
    static uint8_t buffer[NET_CLIENT_RESTORE_BYTES];
    /** @brief The store, to read from. */
    mqtt_store_source source;
    /** @brief The number of the place a damaged store went to. */
    uint32_t aside = 0U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    source.read = &mqtt_platform_store_read;
    source.user_tag = (uint32_t)store;
    source.copies = mqtt_platform_store_copies(store);
    source.erased = mqtt_platform_store_erased(store);
    source.downtime_ms = 0U;
    fault->kind = MQTT_STORE_FAULT_NONE;
    status = mqtt_client_restore(client, &source, buffer,
                                 NET_CLIENT_RESTORE_BYTES, fault);
    if (status == MQTT_STORE_DAMAGED) {
        /* Never written over: a store that can't be set aside stops the
         * start. */
        status = mqtt_platform_store_set_aside(store, &aside);
        if (status == MQTT_OK) {
            net_store_report(fault, aside, "the client");
        }
    }
    if (status == MQTT_OK) {
        net_client_store.store = store;
        net_client_store.client = client;
        net_client_store.duplicating = false;
        net_client_store.resume_ms = 0U;
        status = net_client_whole(client, mqtt_platform_time_ms());
    }
    if (status != MQTT_OK) {
        net_client_store.store = MQTT_STORE_NONE;
        net_client_store.client = NULL;
    }
    return status;
}
