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
 * @file platform_wait.c
 * @brief Waiting for sockets to be ready, in tiers, on a system that waits on
 * an array of entries, as poll does.
 *
 * The entries are kept by the system's folder, through platform_wait.h, in the
 * order of the tier set's slots, HOT first, so each wait hands the system only
 * the prefix that is due. The tier set reports every swap of slots, and the
 * entries follow.
 *
 * A system may refuse a call with more entries than it allows, which for poll
 * on macOS is 10240, even where a process may open many more sockets. The
 * limit is found when waiting starts, and a wait over more slots is split into
 * calls of that size: see base_tier.h.
 */

#include <stddef.h>

#include "base_platform.h"
#include "platform_wait.h"

/** @brief The tier set's records. */
static mqtt_tier_socket wait_sockets[MQTT_PLATFORM_WAIT_SOCKETS];
/** @brief The tier set's slots. */
static uint32_t wait_owners[MQTT_PLATFORM_WAIT_SOCKETS];
/** @brief The tier set. */
static mqtt_tier wait_tier;
/**
 * @brief The slot to start looking at, so that the sockets at the front can't
 * keep the others from being reported.
 */
static uint32_t wait_start_slot = 0U;
/** @brief The most entries one call takes. */
static uint32_t wait_limit = MQTT_PLATFORM_WAIT_SOCKETS;

/**
 * @brief Write what a socket waits for into its entry.
 * @param[in] handle The socket's handle.
 * @param[in] read Whether to wait for read readiness.
 * @param[in] write Whether to wait for write readiness.
 */
static void wait_events(uint32_t handle, bool read, bool write)
{
    /** @brief The socket's slot. */
    const uint32_t slot = mqtt_tier_slot(&wait_tier, handle);

    if (slot != MQTT_TIER_NONE) {
        platform_wait_events(slot, read, write);
    }
}

/**
 * @brief Find the most entries the system takes in one call.
 *
 * Only the number is looked at: every entry is cleared, which the call passes
 * over, as no socket has been added yet.
 *
 * @return The number, at most MQTT_PLATFORM_WAIT_SOCKETS.
 */
static uint32_t wait_find_limit(void)
{
    /** @brief A number known to be taken. */
    uint32_t low = 1U;
    /** @brief A number not known to be refused. */
    uint32_t high = MQTT_PLATFORM_WAIT_SOCKETS;

    platform_wait_clear(MQTT_PLATFORM_WAIT_SOCKETS);
    while (low < high) {
        /** @brief The number tried. */
        const uint32_t middle = low + (((high - low) + 1U) / 2U);

        if (platform_wait_call(0U, middle, 0U) >= 0) {
            low = middle;
        } else {
            high = middle - 1U;
        }
    }
    return low;
}

/**
 * @brief Check, without waiting, the slots of a split wait that the call that
 * waits leaves out.
 * @param[in] plan The plan.
 * @return Number of ready entries, or a PLATFORM_WAIT_ failure.
 */
static int32_t wait_pieces(const mqtt_tier_plan *plan)
{
    /** @brief The first slot of the next call. */
    uint32_t first = plan->blocking;
    /** @brief Number of ready entries. */
    int32_t total = 0;
    /** @brief What the last call returned. */
    int32_t result = 0;

    while ((result >= 0) && (first < plan->length)) {
        /** @brief Number of slots in this call. */
        const uint32_t count = ((plan->length - first) < plan->blocking) ?
                                   (plan->length - first) :
                                   plan->blocking;

        result = platform_wait_call(first, count, 0U);
        wait_tier.counters.calls += 1U;
        if (result > 0) {
            total += result;
        }
        first += count;
    }
    return (result < 0) ? result : total;
}

/**
 * @brief Make the calls of one wait: the slots past the limit first, without
 * waiting, then the call that waits, which doesn't wait if those found any.
 * @param[in] plan The plan.
 * @return Number of ready entries, or a PLATFORM_WAIT_ failure.
 */
static int32_t wait_calls(const mqtt_tier_plan *plan)
{
    /** @brief What the calls returned. */
    int32_t result = wait_pieces(plan);

    if (result >= 0) {
        /** @brief What the call that waits returned. */
        const int32_t found = platform_wait_call(
            0U, plan->blocking, (result > 0) ? 0U : plan->timeout_ms);

        wait_tier.counters.calls += 1U;
        result = (found < 0) ? found : (result + found);
    }
    return result;
}

/**
 * @brief Wait once, and again after each signal, until the deadline.
 *
 * A system that refuses the number of entries after all has its limit halved,
 * and the wait is tried again. Any other failure is counted, and the time of
 * the wait passes without waiting for anything, so that it can't spin.
 *
 * @param[out] plan The plan of the wait that ended.
 * @param[in] max_wait_ms The longest the wait may take.
 * @return Number of ready entries, or a PLATFORM_WAIT_ failure.
 */
static int32_t wait_poll(mqtt_tier_plan *plan, uint32_t max_wait_ms)
{
    /** @brief When the wait began. */
    const uint64_t start_ms = mqtt_platform_time_ms();
    /** @brief When the wait has to end. */
    const uint64_t deadline = start_ms + max_wait_ms;
    /** @brief What the calls returned. */
    int32_t result = PLATFORM_WAIT_FAILED;
    /** @brief true while the wait has to be tried again. */
    bool again = true;

    while (again) {
        /** @brief The time now. */
        const uint64_t now_ms = mqtt_platform_time_ms();
        /** @brief What is left of the wait. */
        const uint64_t left = (now_ms < deadline) ? (deadline - now_ms) : 0U;

        mqtt_tier_plan_wait(&wait_tier, now_ms, (uint32_t)left, plan);
        /* The sockets were watched until the signal came, so the wait still
         * counts from its start. */
        plan->now_ms = start_ms;
        result = wait_calls(plan);
        again = (result == PLATFORM_WAIT_INTERRUPTED);
        if (again) {
            wait_tier.counters.interrupted += 1U;
        } else if ((result == PLATFORM_WAIT_REFUSED) &&
                   (plan->blocking > 1U)) {
            wait_limit = plan->blocking / 2U;
            mqtt_tier_poll_limit(&wait_tier, wait_limit);
            again = true;
        } else {
            /* It is over. */
        }
    }
    if (result < 0) {
        wait_tier.counters.failures += 1U;
        mqtt_platform_sleep_ms(plan->timeout_ms);
    }
    return result;
}

/**
 * @brief Report the ready entries of the prefix a wait covered.
 * @param[out] events Where to put them.
 * @param[in] capacity Number of entries events can hold.
 * @param[in] length Number of entries the wait covered.
 * @param[in] ready Number of entries the calls said were ready.
 * @param[out] truncated true if some ready entries did not fit.
 * @return Number of events.
 */
static uint32_t wait_collect(mqtt_wait_event *events, uint32_t capacity,
                             uint32_t length, uint32_t ready, bool *truncated)
{
    /** @brief Number of events. */
    uint32_t count = 0U;
    /** @brief Number of entries looked at. */
    uint32_t visited = 0U;
    /** @brief Number of ready entries found. */
    uint32_t found = 0U;
    /** @brief The slot to start at. */
    const uint32_t start = (wait_start_slot < length) ? wait_start_slot : 0U;

    while ((visited < length) && (found < ready) && (count < capacity)) {
        /** @brief The slot. */
        uint32_t slot = start + visited;
        /** @brief What happened on its socket. */
        uint32_t returned = 0U;

        if (slot >= length) {
            slot -= length;
        }
        returned = platform_wait_returned(slot);
        if (returned != 0U) {
            /** @brief The socket's handle. */
            const uint32_t handle = wait_owners[slot];

            found += 1U;
            events[count].handle = handle;
            events[count].tag = wait_sockets[handle].tag;
            events[count].readable = (returned & PLATFORM_WAIT_READABLE) != 0U;
            events[count].writable = (returned & PLATFORM_WAIT_WRITABLE) != 0U;
            events[count].hang_up = (returned & PLATFORM_WAIT_HANG_UP) != 0U;
            if ((returned & PLATFORM_WAIT_INVALID) != 0U) {
                /* A socket that isn't open is a mistake of the caller's. It
                 * is reported as hung up, so that it gets closed. */
                wait_tier.counters.invalid += 1U;
            }
            count += 1U;
        }
        visited += 1U;
    }
    *truncated = found < ready;
    wait_start_slot = (length > 0U) ? ((start + visited) % length) : 0U;
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

    if (config == NULL) {
        mqtt_tier_defaults(&used, MQTT_PLATFORM_WAIT_SOCKETS);
    } else {
        used = *config;
    }
    storage.sockets = wait_sockets;
    storage.slot_owner = wait_owners;
    storage.count = MQTT_PLATFORM_WAIT_SOCKETS;
    wait_start_slot = 0U;
    wait_limit = wait_find_limit();
    status = mqtt_tier_init(&wait_tier, &used, &storage, &platform_wait_swap,
                            mqtt_platform_time_ms());
    mqtt_tier_poll_limit(&wait_tier, wait_limit);
    return status;
}

uint32_t mqtt_platform_wait_add(int32_t socket, uint32_t tag, uint32_t pins)
{
    /** @brief The socket's handle. */
    const uint32_t handle = mqtt_tier_start(&wait_tier, socket, tag, pins,
                                            mqtt_platform_time_ms());
    /** @brief Its slot. */
    const uint32_t slot = mqtt_tier_slot(&wait_tier, handle);

    if (slot != MQTT_TIER_NONE) {
        platform_wait_place(slot, socket);
    }
    return handle;
}

void mqtt_platform_wait_interest(uint32_t handle, bool read, bool write)
{
    /* The socket may move to HOT first, so its slot is found after. */
    mqtt_tier_want_write(&wait_tier, handle, write);
    wait_events(handle, read, write);
}

void mqtt_platform_wait_remove(uint32_t handle)
{
    (void)mqtt_tier_stop(&wait_tier, handle);
}

mqtt_tier *mqtt_platform_wait_tier(void)
{
    return &wait_tier;
}

uint32_t mqtt_platform_wait(mqtt_wait_event *events, uint32_t capacity,
                            uint32_t max_wait_ms)
{
    /** @brief The plan of the wait. */
    mqtt_tier_plan plan;
    /** @brief What the calls returned. */
    const int32_t result = wait_poll(&plan, max_wait_ms);
    /** @brief When the wait ended. */
    const uint64_t now_ms = mqtt_platform_time_ms();
    /** @brief Number of events. */
    uint32_t count = 0U;
    /** @brief true if some ready entries did not fit. */
    bool truncated = false;
    /** @brief Index of the event. */
    uint32_t event = 0U;

    if (result > 0) {
        count = wait_collect(events, capacity, plan.length, (uint32_t)result,
                             &truncated);
    }
    while (event < count) {
        mqtt_tier_woke(&wait_tier, events[event].handle, &plan, now_ms);
        event += 1U;
    }
    mqtt_tier_polled(&wait_tier, &plan, now_ms, truncated);
    return count;
}
