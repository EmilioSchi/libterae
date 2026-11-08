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
 * @file base_server_pool.c
 * @brief How many blocks of the pool each connection may take, and the
 * connections waiting for them.
 *
 * Every connection's rings take blocks from one pool, so memory follows the
 * connections that have bytes waiting, not all of them. Each purpose has a
 * rule. Replies may take every free block, up to the hard quota, so that a
 * packet received can always be answered. Bytes received and messages going
 * out leave pool_reserve blocks free while the connection holds fewer than its
 * soft quota, and pool_borrow_reserve once it holds more, so a few busy
 * connections can't take the blocks that quiet ones need when they wake.
 * Messages also stop at three quarters of the hard quota, which leaves the
 * last quarter for replies.
 *
 * A connection that finds too few blocks, and has nothing waiting to be sent
 * that would bring it back, waits at the back of a list. Each block given back
 * wakes one of them, by marking it as having work to do.
 */

#include <stddef.h>

#include "base_server_internal.h"

/** @brief The smallest hard quota, in bytes, a connection can work with. */
#define POOL_QUOTA_MIN 1024U
/** @brief The smallest scratch, in bytes, for packets that arrive in blocks. */
#define POOL_SCRATCH_MIN 16U

uint32_t mqtt_server_held(const mqtt_connection *entry)
{
    return mqtt_ring_held(&entry->ring) + mqtt_ring_held(&entry->receive);
}

/**
 * @brief Decide whether a connection with nothing received waiting may take a
 * first block, even the last one free. Its reply needs a block of its own, so a
 * quiet connection is read, and its PINGREQ answered, while two blocks are
 * free.
 * @param[in] server The server.
 * @param[in] entry The connection.
 * @return true if it may.
 */
static bool pool_first_block(const mqtt_server *server,
                             const mqtt_connection *entry)
{
    /** @brief The configuration. */
    const mqtt_server_config *config = &server->config;
    /** @brief Number of blocks it holds. */
    const uint32_t held = mqtt_server_held(entry);
    /** @brief Free blocks to leave. */
    const uint32_t floor =
        (held < config->pool_soft_quota) ? 0U : config->pool_borrow_reserve;

    return (entry->receive.used == 0U) && (held < config->pool_hard_quota) &&
           (server->pool.free_count > floor);
}

uint32_t mqtt_server_grant(const mqtt_server *server,
                           const mqtt_connection *entry,
                           mqtt_server_purpose purpose)
{
    /** @brief The configuration. */
    const mqtt_server_config *config = &server->config;
    /** @brief The rule for the purpose, as for a reply to begin with. */
    mqtt_pool_rule rule = {0U, 0U, 0U, 0U};
    /** @brief The answer. */
    uint32_t grant = 0U;

    rule.cap = config->pool_hard_quota;
    rule.soft = config->pool_soft_quota;
    if (purpose != MQTT_SERVER_FOR_REPLY) {
        rule.floor_low = config->pool_reserve;
        rule.floor_high = config->pool_borrow_reserve;
    }
    if (purpose == MQTT_SERVER_FOR_MESSAGE) {
        /* In 64 bits, as three times the quota may not fit in 32. */
        rule.cap =
            (uint32_t)(((uint64_t)config->pool_hard_quota * 3U) / 4U);
    }
    grant = mqtt_pool_grant(&server->pool, mqtt_server_held(entry), &rule);
    if ((grant == 0U) && (purpose == MQTT_SERVER_FOR_RECEIVE)) {
        if (pool_first_block(server, entry)) {
            grant = 1U;
        }
    }
    return grant;
}

uint32_t mqtt_server_room(const mqtt_server *server,
                          const mqtt_connection *entry,
                          mqtt_server_purpose purpose)
{
    return mqtt_ring_room(&entry->ring,
                          mqtt_server_grant(server, entry, purpose));
}

void mqtt_server_wait_blocks(mqtt_server *server, uint32_t connection)
{
    /** @brief The connection. */
    mqtt_connection *entry = &server->storage.connections[connection];

    if (!entry->waiting) {
        entry->waiting = true;
        entry->next_waiter = MQTT_SERVER_NONE;
        if (server->waiter_tail == MQTT_SERVER_NONE) {
            server->waiter_head = connection;
        } else {
            server->storage.connections[server->waiter_tail].next_waiter =
                connection;
        }
        server->waiter_tail = connection;
    }
}

void mqtt_server_blocks_returned(mqtt_server *server, uint32_t count)
{
    /** @brief Number of connections still to wake. */
    uint32_t left = count;

    while ((left > 0U) && (server->waiter_head != MQTT_SERVER_NONE)) {
        /** @brief The connection that has waited longest. */
        const uint32_t index = server->waiter_head;
        /** @brief Its entry. */
        mqtt_connection *entry = &server->storage.connections[index];

        server->waiter_head = entry->next_waiter;
        if (server->waiter_head == MQTT_SERVER_NONE) {
            server->waiter_tail = MQTT_SERVER_NONE;
        }
        entry->waiting = false;
        /* A connection freed meanwhile is just taken off. */
        if (entry->state != MQTT_CONNECTION_FREE) {
            mqtt_route_mark_pending(server, index);
            left -= 1U;
        }
    }
}

bool mqtt_server_pool_valid(const mqtt_server_config *config,
                            const mqtt_server_storage *storage)
{
    /** @brief The hard quota in bytes. */
    const uint64_t quota = (uint64_t)config->pool_hard_quota *
                           (uint64_t)storage->pool_block_size;

    /* The pool checks its own sizes when it is set up. */
    return (storage->receive_scratch != NULL) &&
           (storage->receive_scratch_size >= POOL_SCRATCH_MIN) &&
           (config->pool_soft_quota > 0U) &&
           (config->pool_soft_quota <= config->pool_hard_quota) &&
           (quota >= (uint64_t)POOL_QUOTA_MIN) &&
           (config->pool_reserve <= config->pool_borrow_reserve) &&
           ((uint64_t)config->pool_borrow_reserve +
                (uint64_t)config->pool_hard_quota <
            (uint64_t)storage->pool_block_count);
}

/**
 * @brief Do what mqtt_server_pool_audit does, for a server known to be ready.
 * @param[in] server The server.
 * @param[in,out] marks One byte for each block, all zero.
 * @return true if they do.
 */
static bool server_pool_audit_body(const mqtt_server *server, uint8_t *marks)
{
    /** @brief true while every block is accounted for. */
    bool sound = mqtt_pool_audit(&server->pool, marks);
    /** @brief Index of the connection or block being looked at. */
    uint32_t index = 0U;

    while (index < server->storage.connection_count) {
        /** @brief The connection. */
        const mqtt_connection *entry = &server->storage.connections[index];

        if (entry->state != MQTT_CONNECTION_FREE) {
            if (!mqtt_ring_audit(&entry->ring, marks)) {
                sound = false;
            }
            if (!mqtt_ring_audit(&entry->receive, marks)) {
                sound = false;
            }
        }
        index += 1U;
    }
    index = 0U;
    while (index < server->pool.block_count) {
        if (marks[index] != 1U) {
            sound = false;
        }
        index += 1U;
    }
    return sound;
}

bool mqtt_server_pool_audit(const mqtt_server *server, uint8_t *marks)
{
    /** @brief The answer. */
    bool answer = false;

    if (mqtt_server_ready(server) && (marks != NULL)) {
        answer = server_pool_audit_body(server, marks);
    }
    return answer;
}
