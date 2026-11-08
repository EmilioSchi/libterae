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
 * @file base_tier.h
 * @brief Sockets sorted by how busy they are, so that each wait looks at few.
 *
 * Every socket is in one of three tiers. HOT sockets are waited for on every
 * round, WARM ones every warm_period_ms and COLD ones every cold_period_ms. A
 * socket that has been quiet for hot_idle_ms moves from HOT to WARM, and after
 * warm_idle_ms to COLD, one tier at a time. Traffic other than PINGREQ brings
 * it straight back to HOT.
 *
 * The sockets sit in one array of slots in tier order: HOT first, then WARM,
 * then COLD. The caller keeps its own array of the same order, such as the
 * entries it gives poll, and a callback tells it each time two slots trade
 * places. A wait then covers a prefix of that array with no copying. Every
 * change of tier, start and stop moves at most three slots, so each takes a
 * fixed number of steps.
 *
 * Nothing is lost on a COLD socket. Its bytes wait in the system's receive
 * buffer, and are read at most cold_period_ms later than on a HOT socket.
 *
 * A system may limit how many entries one call to poll takes: macOS takes no
 * more than 10240. A wait over more slots than that is split. The slots past
 * the limit are checked once, without waiting, and the call that waits covers
 * the first ones, where the HOT sockets are. A tier with sockets past the limit
 * then counts as watched only at the start of the wait, so the wait ends in
 * time for its next check.
 *
 * Keep Alive limits how cold a socket may get. A Server closes a connection
 * that sends nothing for one and a half Keep Alive intervals, so a socket only
 * sits in a tier whose period is at most a quarter of its interval. A PINGREQ
 * sent at the end of an interval is then read well before the limit.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 3.1.2.10, p.34.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015,
 * 81 pages.
 * @li Cited: 3.1.2.10, p.27-28.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 */
#ifndef MQTT_BASE_TIER_H
#define MQTT_BASE_TIER_H

#include <stdbool.h>
#include <stdint.h>

#include "base_bool.h"
#include "base_status.h"

/** @brief The handle that stands for no socket. */
#define MQTT_TIER_NONE 0xFFFFFFFFU
/** @brief Pin: the connection has not had its CONNECT accepted yet. */
#define MQTT_TIER_PIN_CONNECTING 0x01U
/** @brief Pin: bytes are waiting for the socket to take them. */
#define MQTT_TIER_PIN_TX_PENDING 0x02U
/** @brief Pin: TLS holds bytes it has read that poll can't see. */
#define MQTT_TIER_PIN_TLS_PENDING 0x04U
/** @brief Pin: the socket is always waited for, such as a listener. */
#define MQTT_TIER_PIN_ALWAYS 0x08U
/** @brief Every pin. */
#define MQTT_TIER_PINS 0x0FU
/** @brief How often WARM sockets are waited for, unless set otherwise. */
#define MQTT_TIER_WARM_PERIOD_MS 100U
/** @brief How often COLD sockets are waited for, unless set otherwise. */
#define MQTT_TIER_COLD_PERIOD_MS 1000U
/** @brief Quiet time that moves a socket from HOT to WARM, by default. */
#define MQTT_TIER_HOT_IDLE_MS 2000U
/** @brief Quiet time that moves a socket from WARM to COLD, by default. */
#define MQTT_TIER_WARM_IDLE_MS 30000U

/** @brief Where a socket is. */
typedef enum {
    /** @brief The record is not in use. */
    MQTT_TIER_FREE = 0,
    /** @brief Waited for on every round. */
    MQTT_TIER_HOT,
    /** @brief Waited for every warm_period_ms. */
    MQTT_TIER_WARM,
    /** @brief Waited for every cold_period_ms. */
    MQTT_TIER_COLD
} mqtt_tier_state;

/** @brief How the tiers behave. */
typedef struct {
    /** @brief Milliseconds between waits that include WARM sockets. */
    uint32_t warm_period_ms;
    /** @brief Milliseconds between waits that include COLD sockets. */
    uint32_t cold_period_ms;
    /** @brief Quiet milliseconds before a HOT socket becomes WARM. */
    uint32_t hot_idle_ms;
    /** @brief Quiet milliseconds before a WARM socket becomes COLD. */
    uint32_t warm_idle_ms;
    /**
     * @brief true to wait for every socket at once while only sockets pinned
     * MQTT_TIER_PIN_ALWAYS are HOT and none is WARM.
     */
    bool idle_escalation;
    /** @brief The most sockets at one time. */
    uint32_t max_sockets;
} mqtt_tier_config;

/** @brief One socket. */
typedef struct {
    /** @brief The socket, as the system numbers it. */
    int32_t socket;
    /** @brief Its tier. */
    mqtt_tier_state tier;
    /** @brief Its slot, or MQTT_TIER_NONE when it has none. */
    uint32_t slot;
    /** @brief When it last had traffic other than PINGREQ. */
    uint64_t last_activity_ms;
    /** @brief Its Keep Alive in seconds, or 0 for none. */
    uint16_t keep_alive;
    /** @brief The reasons it has to stay HOT, MQTT_TIER_PIN_ bits. */
    uint32_t pins;
    /** @brief true while it holds part of a packet or bytes to send. */
    bool busy;
    /** @brief The coldest tier its Keep Alive allows. */
    mqtt_tier_state coldest;
    /** @brief What the caller reports it with. */
    uint32_t tag;
    /** @brief The next free record, while this one is free. */
    uint32_t next_free;
} mqtt_tier_socket;

/** @brief Counts kept for observing the tiers. */
typedef struct {
    /** @brief Number of HOT sockets. */
    uint32_t hot;
    /** @brief Number of WARM sockets. */
    uint32_t warm;
    /** @brief Number of COLD sockets. */
    uint32_t cold;
    /** @brief Moves from WARM to HOT. */
    uint64_t warm_to_hot;
    /** @brief Moves from COLD to HOT. */
    uint64_t cold_to_hot;
    /** @brief Moves from HOT to WARM. */
    uint64_t hot_to_warm;
    /** @brief Moves from WARM to COLD. */
    uint64_t warm_to_cold;
    /** @brief Number of waits. */
    uint64_t polls;
    /** @brief Number of calls to poll, which is more when waits are split. */
    uint64_t calls;
    /** @brief Number of calls to poll that failed, not counting signals. */
    uint64_t failures;
    /** @brief Number of sockets waited for, over all waits. */
    uint64_t polled;
    /** @brief Number of sockets waited for in the last whole second. */
    uint64_t polled_last_second;
    /** @brief Number of waits that covered every socket by escalation. */
    uint64_t escalations;
    /** @brief Number of waits a signal interrupted, which were then resumed. */
    uint64_t interrupted;
    /** @brief Number of sockets the system said were not open. */
    uint64_t invalid;
    /**
     * @brief The longest time a HOT socket went unwatched before it was found
     * ready, in milliseconds.
     */
    uint64_t hot_latency_ms;
    /** @brief The same for a WARM socket. */
    uint64_t warm_latency_ms;
    /** @brief The same for a COLD socket. */
    uint64_t cold_latency_ms;
} mqtt_tier_counters;

/**
 * @brief Tells the caller that two slots have traded places, so its own array
 * can follow.
 * @param[in] first One slot.
 * @param[in] second The other.
 */
typedef void (*mqtt_tier_swap)(uint32_t first, uint32_t second);

/** @brief The memory a tier set uses. */
typedef struct {
    /** @brief One record for each socket. */
    mqtt_tier_socket *sockets;
    /** @brief For each slot, the record of the socket in it. */
    uint32_t *slot_owner;
    /** @brief Number of entries in each of the two arrays. */
    uint32_t count;
} mqtt_tier_storage;

/** @brief What one wait covers. */
typedef struct {
    /** @brief Number of slots, from the first, to wait for. */
    uint32_t length;
    /**
     * @brief Number of them in the call that waits, at most the limit of one
     * call. The rest are checked without waiting.
     */
    uint32_t blocking;
    /** @brief true if the wait is split, as length is more than blocking. */
    bool split;
    /** @brief The longest the wait may take, in milliseconds. */
    uint32_t timeout_ms;
    /** @brief true if the WARM sockets are in it. */
    bool warm;
    /** @brief true if the COLD sockets are in it. */
    bool cold;
    /** @brief true if it covers every socket because all are quiet. */
    bool escalated;
    /** @brief When it was planned. */
    uint64_t now_ms;
} mqtt_tier_plan;

/** @brief A set of sockets in tiers. */
typedef struct {
    /**
     * @brief MQTT_BOOL_SET once mqtt_tier_init has succeeded. Every function
     * of the interface refuses a set that holds anything else.
     */
    mqtt_bool ready;
    /** @brief How it behaves. */
    mqtt_tier_config config;
    /** @brief Its memory. */
    mqtt_tier_storage storage;
    /** @brief Told of every swap, or NULL. */
    mqtt_tier_swap swap;
    /** @brief Number of HOT sockets, which fill the first slots. */
    uint32_t hot;
    /** @brief Number of WARM sockets, which follow them. */
    uint32_t warm;
    /** @brief Number of slots in use. COLD sockets fill the rest of them. */
    uint32_t used;
    /** @brief Number of sockets pinned MQTT_TIER_PIN_ALWAYS. */
    uint32_t always;
    /** @brief The most slots one call to poll takes, or MQTT_TIER_NONE. */
    uint32_t poll_limit;
    /** @brief The first free record, or MQTT_TIER_NONE. */
    uint32_t free_head;
    /** @brief When the WARM sockets are next due. */
    uint64_t next_warm_ms;
    /** @brief When the COLD sockets are next due. */
    uint64_t next_cold_ms;
    /** @brief When a wait that included the HOT sockets last ended. */
    uint64_t watched_hot_ms;
    /** @brief When a wait that included the WARM sockets last ended. */
    uint64_t watched_warm_ms;
    /** @brief When a wait that included the COLD sockets last ended. */
    uint64_t watched_cold_ms;
    /** @brief When the second being counted began. */
    uint64_t second_start_ms;
    /** @brief Sockets waited for so far in that second. */
    uint64_t second_polled;
    /** @brief The counts. */
    mqtt_tier_counters counters;
} mqtt_tier;

/**
 * @brief Fill in the defaults.
 * @param[out] config The configuration.
 * @param[in] max_sockets The most sockets at one time.
 */
void mqtt_tier_defaults(mqtt_tier_config *config, uint32_t max_sockets);

/**
 * @brief Set up a tier set.
 * @param[out] tier The set.
 * @param[in] config How it behaves. The periods and idle times must not be 0,
 * the COLD period no shorter than the WARM one, and warm_idle_ms no shorter
 * than hot_idle_ms.
 * @param[in] storage Its memory, with at least max_sockets records and slots.
 * @param[in] swap Told of every swap, or NULL.
 * @param[in] now_ms The time now.
 * @return MQTT_OK or MQTT_INVALID_ARGUMENT.
 */
mqtt_status mqtt_tier_init(mqtt_tier *tier, const mqtt_tier_config *config,
                           const mqtt_tier_storage *storage,
                           mqtt_tier_swap swap, uint64_t now_ms);

/**
 * @brief Add a socket. It starts HOT, with no Keep Alive limit until
 * mqtt_tier_keep_alive sets one.
 *
 * The socket's slot holds whatever the caller's array had there, so the caller
 * fills it in afterwards, at mqtt_tier_slot.
 *
 * @param[in,out] tier The set.
 * @param[in] socket The socket.
 * @param[in] tag What the caller reports it with.
 * @param[in] pins Reasons it has to stay HOT, MQTT_TIER_PIN_ bits.
 * @param[in] now_ms The time now.
 * @return Its handle, or MQTT_TIER_NONE if the set is full.
 */
uint32_t mqtt_tier_start(mqtt_tier *tier, int32_t socket, uint32_t tag,
                         uint32_t pins, uint64_t now_ms);

/**
 * @brief Remove a socket. The socket itself stays open: closing it is up to
 * the caller.
 * @param[in,out] tier The set.
 * @param[in] handle The socket's handle.
 * @return MQTT_OK, or MQTT_INVALID_ARGUMENT if the handle is not in use.
 */
mqtt_status mqtt_tier_stop(mqtt_tier *tier, uint32_t handle);

/**
 * @brief Note that bytes have been read from a socket.
 * @param[in,out] tier The set.
 * @param[in] handle The socket's handle.
 * @param[in] now_ms The time now.
 * @param[in] pingreq_only true if they held no packet other than PINGREQ. The
 * socket then stays in its tier.
 */
void mqtt_tier_received(mqtt_tier *tier, uint32_t handle, uint64_t now_ms,
                        bool pingreq_only);

/**
 * @brief Add reasons for a socket to stay HOT, and move it there.
 * @param[in,out] tier The set.
 * @param[in] handle The socket's handle.
 * @param[in] reasons MQTT_TIER_PIN_ bits.
 */
void mqtt_tier_pin(mqtt_tier *tier, uint32_t handle, uint32_t reasons);

/**
 * @brief Remove reasons for a socket to stay HOT.
 * @param[in,out] tier The set.
 * @param[in] handle The socket's handle.
 * @param[in] reasons MQTT_TIER_PIN_ bits.
 */
void mqtt_tier_unpin(mqtt_tier *tier, uint32_t handle, uint32_t reasons);

/**
 * @brief Say whether bytes are waiting for a socket to take them. While they
 * are, it is HOT and pinned MQTT_TIER_PIN_TX_PENDING.
 * @param[in,out] tier The set.
 * @param[in] handle The socket's handle.
 * @param[in] on true while bytes are waiting.
 */
void mqtt_tier_want_write(mqtt_tier *tier, uint32_t handle, bool on);

/**
 * @brief Set a socket's Keep Alive. A socket in a tier colder than it allows
 * moves to HOT.
 * @param[in,out] tier The set.
 * @param[in] handle The socket's handle.
 * @param[in] keep_alive Seconds, or 0 for none.
 */
void mqtt_tier_keep_alive(mqtt_tier *tier, uint32_t handle,
                          uint16_t keep_alive);

/**
 * @brief Say whether a socket holds part of a packet or bytes to send. A busy
 * socket never becomes COLD, and one that is COLD moves to HOT.
 * @param[in,out] tier The set.
 * @param[in] handle The socket's handle.
 * @param[in] busy true while it is busy.
 */
void mqtt_tier_busy(mqtt_tier *tier, uint32_t handle, bool busy);

/**
 * @brief Say how many slots one call to poll takes, so that waits over more
 * are split.
 * @param[in,out] tier The set.
 * @param[in] limit The number, at least 1, or MQTT_TIER_NONE for no limit.
 */
void mqtt_tier_poll_limit(mqtt_tier *tier, uint32_t limit);

/**
 * @brief Plan the next wait: which slots it covers and how long it may take.
 * @param[in] tier The set.
 * @param[in] now_ms The time now.
 * @param[in] max_wait_ms The longest the caller can wait.
 * @param[out] plan The plan.
 */
void mqtt_tier_plan_wait(const mqtt_tier *tier, uint64_t now_ms,
                         uint32_t max_wait_ms, mqtt_tier_plan *plan);

/**
 * @brief Plan a wait that covers every slot for all the time allowed, for a
 * system that keeps the sockets to watch itself and reports only those that are
 * ready, as epoll and kqueue do. The wait is never split, it is escalated
 * when mqtt_tier_plan_wait would escalate it, and the sockets still move
 * between tiers as mqtt_tier_polled moves them.
 * @param[in] tier The set.
 * @param[in] now_ms The time now.
 * @param[in] max_wait_ms The longest the caller can wait.
 * @param[out] plan The plan.
 */
void mqtt_tier_plan_all(const mqtt_tier *tier, uint64_t now_ms,
                        uint32_t max_wait_ms, mqtt_tier_plan *plan);

/**
 * @brief Note that a wait found a socket ready. Call it for each one, before
 * mqtt_tier_polled.
 * @param[in,out] tier The set.
 * @param[in] handle The socket's handle.
 * @param[in] plan The plan of the wait.
 * @param[in] now_ms The time the wait ended.
 */
void mqtt_tier_woke(mqtt_tier *tier, uint32_t handle,
                    const mqtt_tier_plan *plan, uint64_t now_ms);

/**
 * @brief Note that a wait has ended, and move the quiet sockets it covered one
 * tier colder.
 * @param[in,out] tier The set.
 * @param[in] plan The plan of the wait.
 * @param[in] now_ms The time the wait ended.
 * @param[in] truncated true if more sockets were ready than the caller could
 * take. The tiers in the wait then stay due.
 */
void mqtt_tier_polled(mqtt_tier *tier, const mqtt_tier_plan *plan,
                      uint64_t now_ms, bool truncated);

/**
 * @brief Find a socket's slot.
 * @param[in] tier The set.
 * @param[in] handle The socket's handle.
 * @return The slot, or MQTT_TIER_NONE if it has none.
 */
uint32_t mqtt_tier_slot(const mqtt_tier *tier, uint32_t handle);

/**
 * @brief Find a socket's record.
 * @param[in] tier The set.
 * @param[in] handle The socket's handle.
 * @return The record, or NULL if the handle is not in use.
 */
const mqtt_tier_socket *mqtt_tier_get(const mqtt_tier *tier, uint32_t handle);

/**
 * @brief Read the counts.
 * @param[in] tier The set.
 * @param[out] counters The counts, with the number of sockets in each tier.
 */
void mqtt_tier_counters_get(const mqtt_tier *tier,
                            mqtt_tier_counters *counters);

/**
 * @brief Check that the set is consistent: every slot and record agree, the
 * tiers are in order, and every rule on pins and limits holds. This takes a
 * step for each record, so it is for tests.
 * @param[in] tier The set.
 * @return true if it is consistent.
 */
bool mqtt_tier_check(const mqtt_tier *tier);

#endif /* MQTT_BASE_TIER_H */
