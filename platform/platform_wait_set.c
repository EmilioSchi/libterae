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
 * @file platform_wait_set.c
 * @brief Waiting for sockets to be ready, on a system that keeps the set of
 * sockets to watch itself and reports only those that are ready, as epoll and
 * kqueue do.
 *
 * Such a system watches every socket all the time at no cost for the quiet
 * ones, so every wait covers the whole tier set, with mqtt_tier_plan_all. The
 * tier set is still kept, as the server tells it of reads, pins and Keep Alive,
 * and the sockets still move between tiers, but the tiers no longer decide
 * what is watched.
 *
 * The waits are level-triggered, as poll is: a socket that is ready and not
 * acted on is reported again by the next wait. A socket the system refused to
 * watch is reported as hung up by every wait until it is removed, as poll
 * reports a socket that isn't open, so that it gets closed. A socket removed
 * since the system found it is not reported.
 */

#include <stddef.h>

#include "base_platform.h"
#include "platform_wait.h"

/** @brief The tier set's records. */
static mqtt_tier_socket set_sockets[MQTT_PLATFORM_WAIT_SOCKETS];
/** @brief The tier set's slots. */
static uint32_t set_owners[MQTT_PLATFORM_WAIT_SOCKETS];
/** @brief For each handle, true if the system refused to watch its socket. */
static bool set_refused[MQTT_PLATFORM_WAIT_SOCKETS];
/** @brief Number of handles whose sockets the system refused. */
static uint32_t set_refused_count = 0U;
/** @brief The sockets the last call found ready. */
static platform_wait_fired set_fired[PLATFORM_WAIT_SET_BATCH];
/** @brief The tier set. */
static mqtt_tier set_tier;

/**
 * @brief Report the sockets the system refused to watch, as hung up.
 * @param[out] events Where to put them.
 * @param[in] capacity Number of entries events can hold.
 * @return Number of events.
 */
static uint32_t set_report_refused(mqtt_wait_event *events, uint32_t capacity)
{
    /** @brief Number of events. */
    uint32_t count = 0U;
    /** @brief The handle being looked at. */
    uint32_t handle = 0U;

    while ((set_refused_count > 0U) && (handle < MQTT_PLATFORM_WAIT_SOCKETS) &&
           (count < capacity)) {
        if (set_refused[handle]) {
            events[count].handle = handle;
            events[count].tag = set_sockets[handle].tag;
            events[count].readable = true;
            events[count].writable = false;
            events[count].hang_up = true;
            set_tier.counters.invalid += 1U;
            count += 1U;
        }
        handle += 1U;
    }
    return count;
}

/**
 * @brief Wait once, and again after each signal, until the deadline. Any
 * other failure is counted, and the time of the wait passes without waiting
 * for anything, so that it can't spin.
 * @param[out] plan The plan of the wait that ended.
 * @param[in] capacity Number of sockets to ask the system for.
 * @param[in] max_wait_ms The longest the wait may take.
 * @return Number of sockets found ready, or a PLATFORM_WAIT_ failure.
 */
static int32_t set_wait(mqtt_tier_plan *plan, uint32_t capacity,
                        uint32_t max_wait_ms)
{
    /** @brief When the wait began. */
    const uint64_t start_ms = mqtt_platform_time_ms();
    /** @brief When the wait has to end. */
    const uint64_t deadline = start_ms + max_wait_ms;
    /** @brief What the call returned. */
    int32_t result = PLATFORM_WAIT_FAILED;
    /** @brief true while the wait has to be tried again. */
    bool again = true;

    while (again) {
        /** @brief The time now. */
        const uint64_t now_ms = mqtt_platform_time_ms();
        /** @brief What is left of the wait. */
        const uint64_t left = (now_ms < deadline) ? (deadline - now_ms) : 0U;

        mqtt_tier_plan_all(&set_tier, now_ms, (uint32_t)left, plan);
        plan->now_ms = start_ms;
        result = (capacity > 0U) ?
                     platform_wait_set_fired(set_fired, capacity,
                                             plan->timeout_ms) :
                     0;
        set_tier.counters.calls += 1U;
        again = (result == PLATFORM_WAIT_INTERRUPTED);
        if (again) {
            set_tier.counters.interrupted += 1U;
        }
    }
    if (result < 0) {
        set_tier.counters.failures += 1U;
        mqtt_platform_sleep_ms(plan->timeout_ms);
    }
    return result;
}

/**
 * @brief Turn what the system found into events, leaving out the sockets
 * removed since it found them.
 * @param[out] events Where to put them.
 * @param[in] found Number of sockets the system found.
 * @return Number of events.
 */
static uint32_t set_collect(mqtt_wait_event *events, uint32_t found)
{
    /** @brief Number of events. */
    uint32_t count = 0U;
    /** @brief Index of the socket found. */
    uint32_t index = 0U;

    while (index < found) {
        /** @brief The socket's handle. */
        const uint32_t handle = set_fired[index].handle;
        /** @brief What was found on it. */
        const uint32_t bits = set_fired[index].bits;

        if (mqtt_tier_slot(&set_tier, handle) != MQTT_TIER_NONE) {
            events[count].handle = handle;
            events[count].tag = set_sockets[handle].tag;
            events[count].readable = (bits & PLATFORM_WAIT_READABLE) != 0U;
            events[count].writable = (bits & PLATFORM_WAIT_WRITABLE) != 0U;
            events[count].hang_up = (bits & PLATFORM_WAIT_HANG_UP) != 0U;
            if ((bits & PLATFORM_WAIT_INVALID) != 0U) {
                set_tier.counters.invalid += 1U;
            }
            count += 1U;
        } else {
            /* It was removed since the system found it: every socket in use
             * has a slot. */
        }
        index += 1U;
    }
    return count;
}

mqtt_status mqtt_platform_wait_start(const mqtt_tier_config *config)
{
    /** @brief The configuration used. */
    mqtt_tier_config used;
    /** @brief The tier set's memory. */
    mqtt_tier_storage storage;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief Index of the handle. */
    uint32_t handle = 0U;

    if (config == NULL) {
        mqtt_tier_defaults(&used, MQTT_PLATFORM_WAIT_SOCKETS);
    } else {
        used = *config;
    }
    while (handle < MQTT_PLATFORM_WAIT_SOCKETS) {
        set_refused[handle] = false;
        handle += 1U;
    }
    set_refused_count = 0U;
    storage.sockets = set_sockets;
    storage.slot_owner = set_owners;
    storage.count = MQTT_PLATFORM_WAIT_SOCKETS;
    status = mqtt_tier_init(&set_tier, &used, &storage, NULL,
                            mqtt_platform_time_ms());
    if ((status == MQTT_OK) && (!platform_wait_set_open())) {
        status = MQTT_INVALID_STATE;
    }
    return status;
}

uint32_t mqtt_platform_wait_add(int32_t socket, uint32_t tag, uint32_t pins)
{
    /** @brief The socket's handle. */
    const uint32_t handle = mqtt_tier_start(&set_tier, socket, tag, pins,
                                            mqtt_platform_time_ms());

    if ((handle != MQTT_TIER_NONE) &&
        (!platform_wait_set_add(handle, socket))) {
        set_refused[handle] = true;
        set_refused_count += 1U;
    }
    return handle;
}

void mqtt_platform_wait_interest(uint32_t handle, bool read, bool write)
{
    /** @brief The socket's record. */
    const mqtt_tier_socket *entry = mqtt_tier_get(&set_tier, handle);

    mqtt_tier_want_write(&set_tier, handle, write);
    if ((entry != NULL) && (!set_refused[handle])) {
        platform_wait_set_interest(handle, entry->socket, read, write);
    }
}

void mqtt_platform_wait_remove(uint32_t handle)
{
    /** @brief The socket's record. */
    const mqtt_tier_socket *entry = mqtt_tier_get(&set_tier, handle);

    if (entry != NULL) {
        if (set_refused[handle]) {
            set_refused[handle] = false;
            set_refused_count -= 1U;
        } else {
            platform_wait_set_remove(handle, entry->socket);
        }
        (void)mqtt_tier_stop(&set_tier, handle);
    }
}

mqtt_tier *mqtt_platform_wait_tier(void)
{
    return &set_tier;
}

uint32_t mqtt_platform_wait(mqtt_wait_event *events, uint32_t capacity,
                            uint32_t max_wait_ms)
{
    /** @brief The plan of the wait. */
    mqtt_tier_plan plan;
    /** @brief Number of events of sockets the system refused. */
    uint32_t count = set_report_refused(events, capacity);
    /** @brief Number of sockets to ask the system for. */
    const uint32_t wanted = ((capacity - count) < PLATFORM_WAIT_SET_BATCH) ?
                                (capacity - count) :
                                PLATFORM_WAIT_SET_BATCH;
    /** @brief What the call returned. */
    const int32_t result =
        set_wait(&plan, wanted, (count > 0U) ? 0U : max_wait_ms);
    /** @brief When the wait ended. */
    const uint64_t now_ms = mqtt_platform_time_ms();
    /** @brief Index of the event. */
    uint32_t event = 0U;

    if (result > 0) {
        count += set_collect(&events[count], (uint32_t)result);
    }
    while (event < count) {
        mqtt_tier_woke(&set_tier, events[event].handle, &plan, now_ms);
        event += 1U;
    }
    mqtt_tier_polled(&set_tier, &plan, now_ms,
                     (result > 0) && ((uint32_t)result == wanted));
    return count;
}
