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
 * @file base_tier.c
 * @brief Sockets sorted by how busy they are, so that each wait looks at few.
 *
 * The slots in use are, in order, the HOT sockets, the WARM ones and the COLD
 * ones. A socket changes tier by trading slots with the socket at the edge of
 * its tier, and the edge then moves past it. Leaving the slots fills the hole
 * with the last socket of the same tier, then carries the hole past the later
 * tiers to the end.
 */

#include <stddef.h>

#include "base_string.h"
#include "base_tier.h"

/** @brief The order of a socket that has no slot. */
#define TIER_RANK_NONE 0U
/** @brief The order of HOT, the warmest tier. */
#define TIER_RANK_HOT 1U
/** @brief The order of WARM. */
#define TIER_RANK_WARM 2U
/** @brief The order of COLD, the coldest tier. */
#define TIER_RANK_COLD 3U
/** @brief Milliseconds in a second. */
#define TIER_SECOND_MS 1000U
/**
 * @brief Keep Alive allows a tier whose period is at most its interval divided
 * by this.
 */
#define TIER_KEEP_ALIVE_SHARE 4U

/**
 * @brief Give a tier's place in the order from warmest to coldest.
 * @param[in] state The tier.
 * @return TIER_RANK_HOT, TIER_RANK_WARM, TIER_RANK_COLD, or TIER_RANK_NONE for
 * a record not in use.
 */
static uint32_t tier_rank(mqtt_tier_state state)
{
    /** @brief The place. */
    uint32_t rank = TIER_RANK_NONE;

    switch (state) {
    case MQTT_TIER_HOT:
        rank = TIER_RANK_HOT;
        break;
    case MQTT_TIER_WARM:
        rank = TIER_RANK_WARM;
        break;
    case MQTT_TIER_COLD:
        rank = TIER_RANK_COLD;
        break;
    default:
        rank = TIER_RANK_NONE;
        break;
    }
    return rank;
}

/**
 * @brief Decide whether a set can be used: mqtt_tier_init succeeded, and its
 * marker has not been overwritten since.
 * @param[in] tier The set.
 * @return true if it can.
 */
static bool tier_ready(const mqtt_tier *tier)
{
    return (tier != NULL) && mqtt_bool_is_set(tier->ready);
}

/**
 * @brief Find whether a handle is a record in use, which is a socket in one of
 * the tiers, with a slot.
 * @param[in] tier The set.
 * @param[in] handle The handle.
 * @return true if it is.
 */
static bool tier_placed(const mqtt_tier *tier, uint32_t handle)
{
    /** @brief The answer. */
    bool placed = false;

    if (tier_ready(tier) && (handle < tier->config.max_sockets)) {
        placed = tier_rank(tier->storage.sockets[handle].tier) !=
                 TIER_RANK_NONE;
    }
    return placed;
}

/**
 * @brief Find the coldest tier a Keep Alive allows.
 * @param[in] config How the tiers behave.
 * @param[in] keep_alive Seconds, or 0 for none.
 * @return The tier.
 */
static mqtt_tier_state tier_coldest(const mqtt_tier_config *config,
                                    uint16_t keep_alive)
{
    /** @brief The tier. */
    mqtt_tier_state coldest = MQTT_TIER_COLD;

    if (keep_alive != 0U) {
        /** @brief The longest period allowed, in milliseconds. */
        const uint32_t limit =
            ((uint32_t)keep_alive * TIER_SECOND_MS) / TIER_KEEP_ALIVE_SHARE;

        if (config->cold_period_ms > limit) {
            if (config->warm_period_ms > limit) {
                coldest = MQTT_TIER_HOT;
            } else {
                coldest = MQTT_TIER_WARM;
            }
        }
    }
    return coldest;
}

/**
 * @brief Find the coldest tier a socket may be in now.
 * @param[in] entry The socket.
 * @return The tier's place in the order.
 */
static uint32_t tier_limit(const mqtt_tier_socket *entry)
{
    /** @brief The place. */
    uint32_t limit = tier_rank(entry->coldest);

    if (entry->pins != 0U) {
        limit = TIER_RANK_HOT;
    } else if (entry->busy && (limit > TIER_RANK_WARM)) {
        limit = TIER_RANK_WARM;
    } else {
        /* Keep Alive alone decides. */
    }
    return limit;
}

/**
 * @brief Find whether a socket has been quiet long enough.
 * @param[in] since When it last had traffic.
 * @param[in] now_ms The time now.
 * @param[in] idle_ms How long it has to have been quiet.
 * @return true if it has.
 */
static bool tier_quiet(uint64_t since, uint64_t now_ms, uint32_t idle_ms)
{
    return (now_ms >= since) && ((now_ms - since) >= (uint64_t)idle_ms);
}

/**
 * @brief Trade two slots, and tell the caller.
 * @param[in,out] tier The set.
 * @param[in] first One slot in use.
 * @param[in] second Another.
 */
static void tier_swap(mqtt_tier *tier, uint32_t first, uint32_t second)
{
    if (first != second) {
        /** @brief The record in the first slot. */
        const uint32_t first_owner = tier->storage.slot_owner[first];
        /** @brief The record in the second slot. */
        const uint32_t second_owner = tier->storage.slot_owner[second];

        tier->storage.slot_owner[first] = second_owner;
        tier->storage.slot_owner[second] = first_owner;
        tier->storage.sockets[first_owner].slot = second;
        tier->storage.sockets[second_owner].slot = first;
        if (tier->swap != NULL) {
            tier->swap(first, second);
        }
    }
}

/**
 * @brief Move a socket to HOT, without counting it.
 * @param[in,out] tier The set.
 * @param[in] handle A socket that has a slot.
 */
static void tier_move_hot(mqtt_tier *tier, uint32_t handle)
{
    /** @brief The socket. */
    mqtt_tier_socket *entry = &tier->storage.sockets[handle];

    if (entry->tier == MQTT_TIER_COLD) {
        tier_swap(tier, entry->slot, tier->hot + tier->warm);
        tier->warm += 1U;
        entry->tier = MQTT_TIER_WARM;
    }
    if (entry->tier == MQTT_TIER_WARM) {
        tier_swap(tier, entry->slot, tier->hot);
        tier->hot += 1U;
        tier->warm -= 1U;
        entry->tier = MQTT_TIER_HOT;
    }
}

/**
 * @brief Move a socket to HOT, and count the move.
 * @param[in,out] tier The set.
 * @param[in] handle A socket that has a slot.
 */
static void tier_raise(mqtt_tier *tier, uint32_t handle)
{
    /** @brief Where the socket is. */
    const mqtt_tier_state from = tier->storage.sockets[handle].tier;

    if (from == MQTT_TIER_COLD) {
        tier->counters.cold_to_hot += 1U;
    } else if (from == MQTT_TIER_WARM) {
        tier->counters.warm_to_hot += 1U;
    } else {
        /* It is HOT already. */
    }
    tier_move_hot(tier, handle);
}

/**
 * @brief Move a socket one tier colder, and count the move.
 * @param[in,out] tier The set.
 * @param[in] handle A HOT or WARM socket.
 */
static void tier_lower(mqtt_tier *tier, uint32_t handle)
{
    /** @brief The socket. */
    mqtt_tier_socket *entry = &tier->storage.sockets[handle];

    if (entry->tier == MQTT_TIER_HOT) {
        tier_swap(tier, entry->slot, tier->hot - 1U);
        tier->hot -= 1U;
        tier->warm += 1U;
        entry->tier = MQTT_TIER_WARM;
        tier->counters.hot_to_warm += 1U;
    } else if (entry->tier == MQTT_TIER_WARM) {
        tier_swap(tier, entry->slot, (tier->hot + tier->warm) - 1U);
        tier->warm -= 1U;
        entry->tier = MQTT_TIER_COLD;
        tier->counters.warm_to_cold += 1U;
    } else {
        /* It is COLD already. */
    }
}

/**
 * @brief Take a socket out of the slots, leaving no hole.
 * @param[in,out] tier The set.
 * @param[in] handle A socket that has a slot.
 */
static void tier_leave(mqtt_tier *tier, uint32_t handle)
{
    /** @brief The socket. */
    mqtt_tier_socket *entry = &tier->storage.sockets[handle];
    /** @brief The slot that has to be filled. */
    uint32_t hole = entry->slot;

    if (entry->tier == MQTT_TIER_HOT) {
        tier_swap(tier, hole, tier->hot - 1U);
        tier->hot -= 1U;
        /* The hole is now just before the WARM sockets, so the last of them
         * takes it. */
        tier_swap(tier, tier->hot, tier->hot + tier->warm);
        hole = tier->hot + tier->warm;
    } else if (entry->tier == MQTT_TIER_WARM) {
        tier_swap(tier, hole, (tier->hot + tier->warm) - 1U);
        tier->warm -= 1U;
        hole = tier->hot + tier->warm;
    } else {
        /* A COLD hole only has to reach the end. */
    }
    tier_swap(tier, hole, tier->used - 1U);
    tier->used -= 1U;
    if ((entry->pins & MQTT_TIER_PIN_ALWAYS) != 0U) {
        tier->always -= 1U;
    }
    entry->slot = MQTT_TIER_NONE;
}

/**
 * @brief Move a socket to HOT if it is colder than it may be now.
 * @param[in,out] tier The set.
 * @param[in] handle A socket that has a slot.
 */
static void tier_enforce(mqtt_tier *tier, uint32_t handle)
{
    /** @brief The socket. */
    const mqtt_tier_socket *entry = &tier->storage.sockets[handle];

    if (tier_rank(entry->tier) > tier_limit(entry)) {
        tier_raise(tier, handle);
    }
}

/**
 * @brief Remember the longest time a tier went unwatched.
 * @param[in,out] longest The longest so far.
 * @param[in] start When the wait began.
 * @param[in] watched When a wait that included the tier last ended.
 */
static void tier_latency(uint64_t *longest, uint64_t start, uint64_t watched)
{
    if (start > watched) {
        /** @brief The time it went unwatched. */
        const uint64_t gap = start - watched;

        if (gap > *longest) {
            *longest = gap;
        }
    }
}

/**
 * @brief Count the sockets waited for in each second.
 * @param[in,out] tier The set.
 * @param[in] length Number of sockets in this wait.
 * @param[in] now_ms The time now.
 */
static void tier_count_second(mqtt_tier *tier, uint32_t length,
                              uint64_t now_ms)
{
    if (now_ms >= (tier->second_start_ms + TIER_SECOND_MS)) {
        /* With a whole second between waits, the last second had none. */
        if (now_ms < (tier->second_start_ms + (2U * TIER_SECOND_MS))) {
            tier->counters.polled_last_second = tier->second_polled;
        } else {
            tier->counters.polled_last_second = 0U;
        }
        tier->second_polled = 0U;
        tier->second_start_ms = now_ms;
    }
    tier->second_polled += (uint64_t)length;
}

/**
 * @brief Move each quiet WARM socket to COLD.
 *
 * The sockets are visited from the last slot back, so the one that trades
 * places with a socket moving out has already been visited.
 *
 * @param[in,out] tier The set.
 * @param[in] now_ms The time now.
 */
static void tier_age_warm(mqtt_tier *tier, uint64_t now_ms)
{
    /** @brief The slot after the one to visit. */
    uint32_t slot = tier->hot + tier->warm;

    while (slot > tier->hot) {
        /** @brief The socket in the slot. */
        uint32_t handle = MQTT_TIER_NONE;
        /** @brief Its record. */
        const mqtt_tier_socket *entry = NULL;

        slot -= 1U;
        handle = tier->storage.slot_owner[slot];
        entry = &tier->storage.sockets[handle];
        if ((tier_limit(entry) >= TIER_RANK_COLD) &&
            tier_quiet(entry->last_activity_ms, now_ms,
                       tier->config.warm_idle_ms)) {
            tier_lower(tier, handle);
        }
    }
}

/**
 * @brief Move each quiet HOT socket to WARM, in the same order as
 * tier_age_warm.
 * @param[in,out] tier The set.
 * @param[in] now_ms The time now.
 */
static void tier_age_hot(mqtt_tier *tier, uint64_t now_ms)
{
    /** @brief The slot after the one to visit. */
    uint32_t slot = tier->hot;

    while (slot > 0U) {
        /** @brief The socket in the slot. */
        uint32_t handle = MQTT_TIER_NONE;
        /** @brief Its record. */
        const mqtt_tier_socket *entry = NULL;

        slot -= 1U;
        handle = tier->storage.slot_owner[slot];
        entry = &tier->storage.sockets[handle];
        if ((tier_limit(entry) >= TIER_RANK_WARM) &&
            tier_quiet(entry->last_activity_ms, now_ms,
                       tier->config.hot_idle_ms)) {
            tier_lower(tier, handle);
        }
    }
}

/**
 * @brief Give the tier a slot belongs to.
 * @param[in] tier The set.
 * @param[in] slot A slot in use.
 * @return The tier.
 */
static mqtt_tier_state tier_of_slot(const mqtt_tier *tier, uint32_t slot)
{
    /** @brief The tier. */
    mqtt_tier_state state = MQTT_TIER_COLD;

    if (slot < tier->hot) {
        state = MQTT_TIER_HOT;
    } else if (slot < (tier->hot + tier->warm)) {
        state = MQTT_TIER_WARM;
    } else {
        /* The rest are COLD. */
    }
    return state;
}

/**
 * @brief Find whether a configuration can be used.
 * @param[in] config The configuration.
 * @param[in] storage The memory it would use.
 * @return true if it can.
 */
static bool tier_config_valid(const mqtt_tier_config *config,
                              const mqtt_tier_storage *storage)
{
    /** @brief The answer. */
    bool valid = false;

    if ((config != NULL) && (storage != NULL) && (storage->sockets != NULL) &&
        (storage->slot_owner != NULL) &&
        (config->max_sockets > 0U) &&
        (config->max_sockets <= storage->count) &&
        (config->max_sockets < MQTT_TIER_NONE) &&
        (config->warm_period_ms > 0U) &&
        (config->cold_period_ms >= config->warm_period_ms) &&
        (config->hot_idle_ms > 0U) &&
        (config->warm_idle_ms >= config->hot_idle_ms)) {
        valid = true;
    }
    return valid;
}

void mqtt_tier_defaults(mqtt_tier_config *config, uint32_t max_sockets)
{
    if (config != NULL) {
        config->warm_period_ms = MQTT_TIER_WARM_PERIOD_MS;
        config->cold_period_ms = MQTT_TIER_COLD_PERIOD_MS;
        config->hot_idle_ms = MQTT_TIER_HOT_IDLE_MS;
        config->warm_idle_ms = MQTT_TIER_WARM_IDLE_MS;
        config->idle_escalation = true;
        config->max_sockets = max_sockets;
    }
}

/**
 * @brief Set every count to zero.
 * @param[out] counters The counts.
 */
static void tier_counters_clear(mqtt_tier_counters *counters)
{
    counters->hot = 0U;
    counters->warm = 0U;
    counters->cold = 0U;
    counters->warm_to_hot = 0U;
    counters->cold_to_hot = 0U;
    counters->hot_to_warm = 0U;
    counters->warm_to_cold = 0U;
    counters->polls = 0U;
    counters->calls = 0U;
    counters->failures = 0U;
    counters->polled = 0U;
    counters->polled_last_second = 0U;
    counters->escalations = 0U;
    counters->interrupted = 0U;
    counters->invalid = 0U;
    counters->hot_latency_ms = 0U;
    counters->warm_latency_ms = 0U;
    counters->cold_latency_ms = 0U;
}

/**
 * @brief Set every field of a tier that the records don't hold. The memory may
 * hold anything before.
 * @param[out] tier The tier.
 * @param[in] config How the tier works.
 * @param[in] storage The records.
 * @param[in] swap What moves a socket between tiers.
 * @param[in] now_ms The time now.
 */
static void tier_reset(mqtt_tier *tier, const mqtt_tier_config *config,
                       const mqtt_tier_storage *storage, mqtt_tier_swap swap,
                       uint64_t now_ms)
{
    /* Copied as bytes, as assigning structs this size may be done with a
     * call to memcpy. */
    mqtt_bytes_put((uint8_t *)&tier->config, (const uint8_t *)config,
                   (uint32_t)sizeof(*config));
    mqtt_bytes_put((uint8_t *)&tier->storage, (const uint8_t *)storage,
                   (uint32_t)sizeof(*storage));
    tier->swap = swap;
    tier->hot = 0U;
    tier->warm = 0U;
    tier->used = 0U;
    tier->always = 0U;
    tier->poll_limit = MQTT_TIER_NONE;
    tier->free_head = MQTT_TIER_NONE;
    tier->next_warm_ms = now_ms + config->warm_period_ms;
    tier->next_cold_ms = now_ms + config->cold_period_ms;
    tier->watched_hot_ms = now_ms;
    tier->watched_warm_ms = now_ms;
    tier->watched_cold_ms = now_ms;
    tier->second_start_ms = now_ms;
    tier->second_polled = 0U;
    tier_counters_clear(&tier->counters);
}

/**
 * @brief Do what mqtt_tier_init does, for arguments known to be valid.
 * @param[out] tier The set.
 * @param[in] config How it behaves. The periods and idle times must not be 0,
 * the COLD period no shorter than the WARM one, and warm_idle_ms no shorter
 * than hot_idle_ms.
 * @param[in] storage Its memory, with at least max_sockets records and slots.
 * @param[in] swap Told of every swap, or NULL.
 * @param[in] now_ms The time now.
 * @return MQTT_OK or MQTT_INVALID_ARGUMENT.
 */
static mqtt_status tier_init_body(mqtt_tier *tier,
                                  const mqtt_tier_config *config,
                                  const mqtt_tier_storage *storage,
                                  mqtt_tier_swap swap, uint64_t now_ms)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_INVALID_ARGUMENT;

    tier->ready = MQTT_BOOL_UNSET;
    if (tier_config_valid(config, storage)) {
        /** @brief The record being put on the free list. */
        uint32_t index = config->max_sockets;

        tier_reset(tier, config, storage, swap, now_ms);
        while (index > 0U) {
            index -= 1U;
            tier->storage.sockets[index].tier = MQTT_TIER_FREE;
            tier->storage.sockets[index].slot = MQTT_TIER_NONE;
            tier->storage.sockets[index].pins = 0U;
            tier->storage.sockets[index].next_free = tier->free_head;
            tier->free_head = index;
        }
        tier->ready = MQTT_BOOL_SET;
        status = MQTT_OK;
    }
    return status;
}

mqtt_status mqtt_tier_init(mqtt_tier *tier, const mqtt_tier_config *config,
                           const mqtt_tier_storage *storage,
                           mqtt_tier_swap swap, uint64_t now_ms)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if (tier != NULL) {
        answer = tier_init_body(tier, config, storage, swap, now_ms);
    }
    return answer;
}

/**
 * @brief Do what mqtt_tier_start does, for a set known to be ready.
 * @param[in,out] tier The set.
 * @param[in] socket The socket.
 * @param[in] tag What the caller reports it with.
 * @param[in] pins Reasons it has to stay HOT, MQTT_TIER_PIN_ bits.
 * @param[in] now_ms The time now.
 * @return Its handle, or MQTT_TIER_NONE if the set is full.
 */
static uint32_t tier_start_body(mqtt_tier *tier, int32_t socket, uint32_t tag,
                                uint32_t pins, uint64_t now_ms)
{
    /** @brief The new socket's handle. */
    const uint32_t handle = tier->free_head;

    if (handle != MQTT_TIER_NONE) {
        /** @brief Its record. */
        mqtt_tier_socket *entry = &tier->storage.sockets[handle];

        tier->free_head = entry->next_free;
        entry->socket = socket;
        entry->tag = tag;
        entry->pins = pins & MQTT_TIER_PINS;
        entry->busy = false;
        entry->keep_alive = 0U;
        entry->coldest = MQTT_TIER_COLD;
        entry->last_activity_ms = now_ms;
        entry->next_free = MQTT_TIER_NONE;
        /* It joins at the end, which is COLD, and moves up from there. */
        entry->slot = tier->used;
        entry->tier = MQTT_TIER_COLD;
        tier->storage.slot_owner[tier->used] = handle;
        tier->used += 1U;
        tier_move_hot(tier, handle);
        if ((entry->pins & MQTT_TIER_PIN_ALWAYS) != 0U) {
            tier->always += 1U;
        }
    }
    return handle;
}

uint32_t mqtt_tier_start(mqtt_tier *tier, int32_t socket, uint32_t tag,
                         uint32_t pins, uint64_t now_ms)
{
    /** @brief The answer. */
    uint32_t answer = MQTT_TIER_NONE;

    if (tier_ready(tier)) {
        answer = tier_start_body(tier, socket, tag, pins, now_ms);
    }
    return answer;
}

mqtt_status mqtt_tier_stop(mqtt_tier *tier, uint32_t handle)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_INVALID_ARGUMENT;

    if (tier_placed(tier, handle)) {
        /** @brief The socket. */
        mqtt_tier_socket *entry = &tier->storage.sockets[handle];

        tier_leave(tier, handle);
        entry->tier = MQTT_TIER_FREE;
        entry->pins = 0U;
        entry->next_free = tier->free_head;
        tier->free_head = handle;
        status = MQTT_OK;
    }
    return status;
}

void mqtt_tier_received(mqtt_tier *tier, uint32_t handle, uint64_t now_ms,
                        bool pingreq_only)
{
    if ((!pingreq_only) && tier_placed(tier, handle)) {
        tier->storage.sockets[handle].last_activity_ms = now_ms;
        tier_raise(tier, handle);
    }
}

void mqtt_tier_pin(mqtt_tier *tier, uint32_t handle, uint32_t reasons)
{
    if (tier_placed(tier, handle)) {
        /** @brief The socket. */
        mqtt_tier_socket *entry = &tier->storage.sockets[handle];

        if (((reasons & MQTT_TIER_PIN_ALWAYS) != 0U) &&
            ((entry->pins & MQTT_TIER_PIN_ALWAYS) == 0U)) {
            tier->always += 1U;
        }
        entry->pins |= reasons & MQTT_TIER_PINS;
        tier_enforce(tier, handle);
    }
}

void mqtt_tier_unpin(mqtt_tier *tier, uint32_t handle, uint32_t reasons)
{
    if (tier_placed(tier, handle)) {
        /** @brief The socket. */
        mqtt_tier_socket *entry = &tier->storage.sockets[handle];

        if (((reasons & MQTT_TIER_PIN_ALWAYS) != 0U) &&
            ((entry->pins & MQTT_TIER_PIN_ALWAYS) != 0U)) {
            tier->always -= 1U;
        }
        entry->pins &= ~reasons;
    }
}

void mqtt_tier_want_write(mqtt_tier *tier, uint32_t handle, bool on)
{
    if (on) {
        mqtt_tier_pin(tier, handle, MQTT_TIER_PIN_TX_PENDING);
    } else {
        mqtt_tier_unpin(tier, handle, MQTT_TIER_PIN_TX_PENDING);
    }
}

void mqtt_tier_keep_alive(mqtt_tier *tier, uint32_t handle,
                          uint16_t keep_alive)
{
    if (tier_placed(tier, handle)) {
        /** @brief The socket. */
        mqtt_tier_socket *entry = &tier->storage.sockets[handle];

        entry->keep_alive = keep_alive;
        entry->coldest = tier_coldest(&tier->config, keep_alive);
        tier_enforce(tier, handle);
    }
}

void mqtt_tier_busy(mqtt_tier *tier, uint32_t handle, bool busy)
{
    if (tier_placed(tier, handle)) {
        tier->storage.sockets[handle].busy = busy;
        tier_enforce(tier, handle);
    }
}

/**
 * @brief Do what mqtt_tier_poll_limit does, for a set known to be ready.
 * @param[in,out] tier The set.
 * @param[in] limit The number, at least 1, or MQTT_TIER_NONE for no limit.
 */
static void tier_poll_limit_body(mqtt_tier *tier, uint32_t limit)
{
    if (limit > 0U) {
        tier->poll_limit = limit;
    }
}

void mqtt_tier_poll_limit(mqtt_tier *tier, uint32_t limit)
{
    if (tier_ready(tier)) {
        tier_poll_limit_body(tier, limit);
    }
}

/**
 * @brief Make a split wait end in time for the next check of the sockets that
 * only its start covers: those past the call that waits.
 * @param[in] tier The set.
 * @param[in,out] plan The plan.
 */
static void tier_plan_split(const mqtt_tier *tier, mqtt_tier_plan *plan)
{
    /** @brief The tier of the first slot the call that waits leaves out. */
    const mqtt_tier_state beyond = tier_of_slot(tier, plan->blocking);
    /** @brief The period of that tier's checks. */
    const uint32_t period = (beyond == MQTT_TIER_COLD) ?
                                tier->config.cold_period_ms :
                                tier->config.warm_period_ms;

    plan->split = true;
    if (plan->timeout_ms > period) {
        plan->timeout_ms = period;
    }
}

/**
 * @brief Do what mqtt_tier_plan_wait does, for a set known to be ready.
 * @param[in] tier The set.
 * @param[in] now_ms The time now.
 * @param[in] max_wait_ms The longest the caller can wait.
 * @param[out] plan The plan.
 */
static void tier_plan_wait_body(const mqtt_tier *tier, uint64_t now_ms,
                                uint32_t max_wait_ms, mqtt_tier_plan *plan)
{
    /** @brief Number of COLD sockets. */
    const uint32_t cold = tier->used - tier->hot - tier->warm;
    /** @brief The longest the wait may take. */
    uint64_t timeout = (uint64_t)max_wait_ms;

    plan->now_ms = now_ms;
    plan->escalated = tier->config.idle_escalation &&
                      (tier->hot == tier->always) && (tier->warm == 0U) &&
                      (cold > 0U);
    if (plan->escalated) {
        plan->warm = true;
        plan->cold = true;
    } else {
        plan->cold = (cold > 0U) && (now_ms >= tier->next_cold_ms);
        plan->warm = plan->cold ||
                     ((tier->warm > 0U) && (now_ms >= tier->next_warm_ms));
        /* A tier in this wait is watched for all of it, so only the others
         * can end it early. */
        if ((!plan->warm) && (tier->warm > 0U) &&
            ((tier->next_warm_ms - now_ms) < timeout)) {
            timeout = tier->next_warm_ms - now_ms;
        }
        if ((!plan->cold) && (cold > 0U) &&
            ((tier->next_cold_ms - now_ms) < timeout)) {
            timeout = tier->next_cold_ms - now_ms;
        }
    }
    if (plan->cold) {
        plan->length = tier->used;
    } else if (plan->warm) {
        plan->length = tier->hot + tier->warm;
    } else {
        plan->length = tier->hot;
    }
    plan->timeout_ms = (uint32_t)timeout;
    plan->blocking = plan->length;
    plan->split = false;
    if (plan->length > tier->poll_limit) {
        plan->blocking = tier->poll_limit;
        tier_plan_split(tier, plan);
    }
}

void mqtt_tier_plan_wait(const mqtt_tier *tier, uint64_t now_ms,
                         uint32_t max_wait_ms, mqtt_tier_plan *plan)
{
    if (plan == NULL) {
        /* There is nowhere to put a plan. */
    } else if (tier_ready(tier)) {
        tier_plan_wait_body(tier, now_ms, max_wait_ms, plan);
    } else {
        /* A plan that covers no slot, so the caller only waits. */
        plan->length = 0U;
        plan->blocking = 0U;
        plan->split = false;
        plan->timeout_ms = max_wait_ms;
        plan->warm = false;
        plan->cold = false;
        plan->escalated = false;
        plan->now_ms = now_ms;
    }
}

/**
 * @brief Do what mqtt_tier_plan_all does, for arguments known to be valid.
 * @param[in] tier The set.
 * @param[in] now_ms The time now.
 * @param[in] max_wait_ms The longest the caller can wait.
 * @param[out] plan The plan.
 */
static void tier_plan_all_body(const mqtt_tier *tier, uint64_t now_ms,
                               uint32_t max_wait_ms, mqtt_tier_plan *plan)
{
    /** @brief true if the set can be waited on. */
    const bool ready = tier_ready(tier);
    /** @brief Number of slots in use, or none for a set that isn't ready. */
    const uint32_t used = ready ? tier->used : 0U;

    plan->length = used;
    plan->blocking = used;
    plan->split = false;
    plan->timeout_ms = max_wait_ms;
    plan->warm = ready;
    plan->cold = ready;
    /* As in a wait that covers every socket because all are quiet, the one
     * that ends it moves up. */
    plan->escalated = ready && tier->config.idle_escalation &&
                      (tier->hot == tier->always) && (tier->warm == 0U) &&
                      (tier->used > tier->hot);
    plan->now_ms = now_ms;
}

void mqtt_tier_plan_all(const mqtt_tier *tier, uint64_t now_ms,
                        uint32_t max_wait_ms, mqtt_tier_plan *plan)
{
    if (plan != NULL) {
        tier_plan_all_body(tier, now_ms, max_wait_ms, plan);
    }
}

void mqtt_tier_woke(mqtt_tier *tier, uint32_t handle,
                    const mqtt_tier_plan *plan, uint64_t now_ms)
{
    if ((plan != NULL) && tier_placed(tier, handle)) {
        /** @brief The socket. */
        mqtt_tier_socket *entry = &tier->storage.sockets[handle];

        if (entry->tier == MQTT_TIER_HOT) {
            tier_latency(&tier->counters.hot_latency_ms, plan->now_ms,
                         tier->watched_hot_ms);
        } else if (entry->tier == MQTT_TIER_WARM) {
            tier_latency(&tier->counters.warm_latency_ms, plan->now_ms,
                         tier->watched_warm_ms);
        } else {
            tier_latency(&tier->counters.cold_latency_ms, plan->now_ms,
                         tier->watched_cold_ms);
        }
        /* After a wait that covered every socket, the one that ended it moves
         * up, so that the next waits are short ones again. */
        if (plan->escalated && (entry->tier != MQTT_TIER_HOT)) {
            entry->last_activity_ms = now_ms;
            tier_raise(tier, handle);
        }
    }
}

/**
 * @brief Find until when a wait watched a tier: to its end, or only at its
 * start when some of the tier's sockets were past the call that waited.
 * @param[in] plan The plan of the wait.
 * @param[in] end The slot after the tier's last.
 * @param[in] now_ms The time the wait ended.
 * @return The time.
 */
static uint64_t tier_watched(const mqtt_tier_plan *plan, uint32_t end,
                             uint64_t now_ms)
{
    return (plan->split && (end > plan->blocking)) ? plan->now_ms : now_ms;
}

/**
 * @brief Do what mqtt_tier_polled does, for a set known to be ready.
 * @param[in,out] tier The set.
 * @param[in] plan The plan of the wait.
 * @param[in] now_ms The time the wait ended.
 * @param[in] truncated true if more sockets were ready than the caller could
 * take. The tiers in the wait then stay due.
 */
static void tier_polled_body(mqtt_tier *tier, const mqtt_tier_plan *plan,
                             uint64_t now_ms, bool truncated)
{
    /** @brief Number of COLD sockets before any moves. */
    const uint32_t cold = tier->used - tier->hot - tier->warm;

    tier->counters.polls += 1U;
    tier->counters.polled += (uint64_t)plan->length;
    if (plan->escalated) {
        tier->counters.escalations += 1U;
    }
    tier_count_second(tier, plan->length, now_ms);
    /* Sockets left unreported were not watched in effect, so their tiers stay
     * due and keep their times. */
    if (!truncated) {
        tier->watched_hot_ms = tier_watched(plan, tier->hot, now_ms);
        /* An empty tier has nobody waiting unwatched, so its time starts
         * again, as its deadline does. */
        if (plan->warm || (tier->warm == 0U)) {
            tier->watched_warm_ms =
                tier_watched(plan, tier->hot + tier->warm, now_ms);
            tier->next_warm_ms =
                tier->watched_warm_ms + tier->config.warm_period_ms;
        }
        if (plan->cold || (cold == 0U)) {
            tier->watched_cold_ms = tier_watched(plan, tier->used, now_ms);
            tier->next_cold_ms =
                tier->watched_cold_ms + tier->config.cold_period_ms;
        }
    }
    if (plan->warm) {
        tier_age_warm(tier, now_ms);
    }
    tier_age_hot(tier, now_ms);
}

void mqtt_tier_polled(mqtt_tier *tier, const mqtt_tier_plan *plan,
                      uint64_t now_ms, bool truncated)
{
    if ((plan != NULL) && tier_ready(tier)) {
        tier_polled_body(tier, plan, now_ms, truncated);
    }
}

uint32_t mqtt_tier_slot(const mqtt_tier *tier, uint32_t handle)
{
    /** @brief The slot. */
    uint32_t slot = MQTT_TIER_NONE;

    if (tier_placed(tier, handle)) {
        slot = tier->storage.sockets[handle].slot;
    }
    return slot;
}

const mqtt_tier_socket *mqtt_tier_get(const mqtt_tier *tier, uint32_t handle)
{
    /** @brief The record. */
    const mqtt_tier_socket *entry = NULL;

    if (tier_placed(tier, handle)) {
        entry = &tier->storage.sockets[handle];
    }
    return entry;
}

/**
 * @brief Do what mqtt_tier_counters_get does, for a set known to be ready.
 * @param[in] tier The set.
 * @param[out] counters The counts, with the number of sockets in each tier.
 */
static void tier_counters_get_body(const mqtt_tier *tier,
                                   mqtt_tier_counters *counters)
{
    /* Copied as bytes, as assigning a struct this size would be done with a
     * call to memcpy. */
    mqtt_bytes_put((uint8_t *)counters, (const uint8_t *)&tier->counters,
                   (uint32_t)sizeof(tier->counters));
    counters->hot = tier->hot;
    counters->warm = tier->warm;
    counters->cold = tier->used - tier->hot - tier->warm;
}

void mqtt_tier_counters_get(const mqtt_tier *tier, mqtt_tier_counters *counters)
{
    if (counters == NULL) {
        /* There is nowhere to put the counts. */
    } else if (tier_ready(tier)) {
        tier_counters_get_body(tier, counters);
    } else {
        tier_counters_clear(counters);
    }
}

/**
 * @brief Check that each slot in use and its record agree.
 * @param[in] tier The set.
 * @return true if they do.
 */
static bool tier_check_slots(const mqtt_tier *tier)
{
    /** @brief The answer. */
    bool good = true;
    /** @brief The slot being checked. */
    uint32_t slot = 0U;

    while (good && (slot < tier->used)) {
        /** @brief The record in it. */
        const uint32_t owner = tier->storage.slot_owner[slot];

        if (owner >= tier->config.max_sockets) {
            good = false;
        } else {
            /** @brief The record. */
            const mqtt_tier_socket *entry = &tier->storage.sockets[owner];

            good = (entry->slot == slot) &&
                   (entry->tier == tier_of_slot(tier, slot)) &&
                   (tier_rank(entry->tier) <= tier_limit(entry)) &&
                   (entry->coldest ==
                    tier_coldest(&tier->config, entry->keep_alive));
        }
        slot += 1U;
    }
    return good;
}

/**
 * @brief Count the records on the free list, stopping at a loop.
 * @param[in] tier The set.
 * @return The number, or MQTT_TIER_NONE if the list is broken.
 */
static uint32_t tier_free_length(const mqtt_tier *tier)
{
    /** @brief The record being counted. */
    uint32_t index = tier->free_head;
    /** @brief Number counted. */
    uint32_t length = 0U;

    while ((index != MQTT_TIER_NONE) && (length != MQTT_TIER_NONE)) {
        if ((index >= tier->config.max_sockets) ||
            (length >= tier->config.max_sockets) ||
            (tier->storage.sockets[index].tier != MQTT_TIER_FREE)) {
            length = MQTT_TIER_NONE;
        } else {
            length += 1U;
            index = tier->storage.sockets[index].next_free;
        }
    }
    return length;
}

/**
 * @brief Check that the records agree with the counts.
 * @param[in] tier The set.
 * @return true if they do.
 */
static bool tier_check_records(const mqtt_tier *tier)
{
    /** @brief Number of records with a slot. */
    uint32_t placed = 0U;
    /** @brief Number of free records. */
    uint32_t free_count = 0U;
    /** @brief Number of sockets pinned MQTT_TIER_PIN_ALWAYS. */
    uint32_t always = 0U;
    /** @brief The record being counted. */
    uint32_t index = 0U;

    while (index < tier->config.max_sockets) {
        /** @brief The record. */
        const mqtt_tier_socket *entry = &tier->storage.sockets[index];

        if (entry->tier == MQTT_TIER_FREE) {
            free_count += 1U;
        } else {
            placed += 1U;
            if ((entry->pins & MQTT_TIER_PIN_ALWAYS) != 0U) {
                always += 1U;
            }
        }
        index += 1U;
    }
    return (placed == tier->used) && (always == tier->always) &&
           (free_count == tier_free_length(tier));
}

/**
 * @brief Do what mqtt_tier_check does, for a set known to be ready.
 * @param[in] tier The set.
 * @return true if it is consistent.
 */
static bool tier_check_body(const mqtt_tier *tier)
{
    /** @brief The answer. */
    bool good = false;

    if (((tier->hot + tier->warm) <= tier->used) &&
        (tier->used <= tier->config.max_sockets) &&
        (tier->config.max_sockets <= tier->storage.count) &&
        (tier->always <= tier->hot)) {
        if (tier_check_slots(tier)) {
            good = tier_check_records(tier);
        }
    }
    return good;
}

bool mqtt_tier_check(const mqtt_tier *tier)
{
    /** @brief The answer. */
    bool answer = false;

    if (tier_ready(tier)) {
        answer = tier_check_body(tier);
    }
    return answer;
}
