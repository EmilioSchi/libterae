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
 * @file base_ring.h
 * @brief A chain of pool blocks holding bytes that wait on a connection.
 *
 * A ring holds the bytes waiting to be sent to a peer, or those received and
 * not yet taken. They sit in blocks of a pool shared by every ring, linked from
 * the oldest to the newest. Bytes go in at the newest block and come out of the
 * oldest, and a block goes back to the pool as soon as its last byte comes
 * out, so a ring with nothing waiting holds no block.
 *
 * Whoever adds bytes decides first whether its ring may take the blocks they
 * need: mqtt_ring_blocks_for says how many, and mqtt_pool_grant how many a
 * rule allows. Only whole packets go into a transmit ring, so its bytes can be
 * taken out in any size without splitting a packet from another's point of
 * view.
 *
 * Reading from a socket into a ring goes in three steps: mqtt_ring_reserve
 * takes the blocks, mqtt_ring_space lists where bytes can go, and
 * mqtt_ring_commit counts what arrived and gives back the blocks nothing was
 * written to.
 */
#ifndef MQTT_BASE_RING_H
#define MQTT_BASE_RING_H

#include <stdbool.h>
#include <stdint.h>

#include "base_pool.h"
#include "base_run_t.h"
#include "base_status.h"

/** @brief The state of a ring. */
typedef struct {
    /** @brief The pool its blocks come from. */
    mqtt_pool *pool;
    /** @brief The oldest block, or MQTT_POOL_NONE when nothing is waiting. */
    uint32_t head;
    /** @brief The newest block, or MQTT_POOL_NONE when nothing is waiting. */
    uint32_t tail;
    /** @brief Index in the oldest block of the first byte waiting. */
    uint32_t head_offset;
    /** @brief Number of bytes written in the newest block. */
    uint32_t tail_used;
    /** @brief Number of blocks holding waiting bytes. */
    uint32_t blocks;
    /** @brief Number of bytes waiting. */
    uint32_t used;
    /** @brief The first block taken for bytes still to come, or none. */
    uint32_t spare;
    /** @brief Number of blocks taken for bytes still to come. */
    uint32_t spare_count;
} mqtt_ring;

/**
 * @brief Set up an empty ring.
 * @param[out] ring The ring.
 * @param[in] pool The pool its blocks come from.
 */
void mqtt_ring_init(mqtt_ring *ring, mqtt_pool *pool);

/**
 * @brief Count the blocks a ring holds, with those taken for bytes still to
 * come.
 * @param[in] ring The ring.
 * @return The number of blocks.
 */
uint32_t mqtt_ring_held(const mqtt_ring *ring);

/**
 * @brief Count the bytes that fit with what the ring holds and some more
 * blocks.
 * @param[in] ring The ring.
 * @param[in] blocks Number of blocks it may still take.
 * @return The number of bytes, or UINT32_MAX if that is fewer.
 */
uint32_t mqtt_ring_room(const mqtt_ring *ring, uint32_t blocks);

/**
 * @brief Count the blocks a ring must take for more bytes.
 * @param[in] ring The ring.
 * @param[in] length Number of bytes.
 * @return The number of blocks.
 */
uint32_t mqtt_ring_blocks_for(const mqtt_ring *ring, uint32_t length);

/**
 * @brief Add bytes, all of them or none.
 * @param[in,out] ring The ring.
 * @param[in] data The bytes.
 * @param[in] length Number of bytes in data.
 * @return MQTT_OK, MQTT_RING_FULL if the pool has too few free blocks, or
 * MQTT_INVALID_ARGUMENT for a NULL pointer, when nothing is added.
 */
mqtt_status mqtt_ring_append(mqtt_ring *ring, const uint8_t *data,
                             uint32_t length);

/**
 * @brief Find the waiting bytes, as one run for each block.
 * @param[in] ring The ring.
 * @param[out] runs The runs, in order.
 * @param[in] max_runs Number of entries runs can hold. With fewer than are
 * needed, the first ones are given.
 * @return Number of runs, which is zero only if no bytes are waiting.
 */
uint32_t mqtt_ring_view(const mqtt_ring *ring, mqtt_run *runs,
                        uint32_t max_runs);

/**
 * @brief Find some of the waiting bytes, as one run for each block they are
 * in.
 * @param[in] ring The ring.
 * @param[in] offset Index of the first byte, counted from the oldest waiting.
 * @param[in] length Number of bytes. Those past the last byte waiting are left
 * out.
 * @param[out] runs The runs, in order.
 * @param[in] max_runs Number of entries runs can hold. With fewer than are
 * needed, the first ones are given.
 * @return Number of runs.
 */
uint32_t mqtt_ring_view_range(const mqtt_ring *ring, uint32_t offset,
                              uint32_t length, mqtt_run *runs,
                              uint32_t max_runs);

/**
 * @brief Copy some of the waiting bytes out, leaving them in the ring.
 * @param[in] ring The ring.
 * @param[in] offset Index of the first byte, counted from the oldest waiting.
 * @param[out] bytes Where they go.
 * @param[in] length Number of bytes wanted.
 * @return Number of bytes copied, fewer if fewer are waiting.
 */
uint32_t mqtt_ring_peek(const mqtt_ring *ring, uint32_t offset, uint8_t *bytes,
                        uint32_t length);

/**
 * @brief Find what a connection sends next: the bytes in the ring that go
 * before a payload sent from elsewhere, that payload, then the rest of the
 * ring.
 * @param[in] ring The ring.
 * @param[in] before Number of bytes in the ring that go before the payload.
 * @param[in] payload The payload, or NULL.
 * @param[in] payload_length Number of bytes in it.
 * @param[out] runs The runs, in order, with no empty one.
 * @param[in] max_runs Number of entries runs can hold: with fewer than are
 * needed, the first runs are given.
 * @return Number of runs.
 */
uint32_t mqtt_ring_transmit_view(const mqtt_ring *ring, uint32_t before,
                                 const uint8_t *payload,
                                 uint32_t payload_length, mqtt_run *runs,
                                 uint32_t max_runs);

/**
 * @brief Find what a connection may send next when no more than some of the
 * ring's bytes may go: those before a payload sent from elsewhere, that payload
 * if every byte before it may go, then the rest of what may go.
 * @param[in] ring The ring.
 * @param[in] before Number of bytes in the ring that go before the payload.
 * @param[in] payload The payload, or NULL.
 * @param[in] payload_length Number of bytes in it.
 * @param[in] limit Number of bytes of the ring that may go, counted from the
 * oldest waiting.
 * @param[out] runs The runs, in order, with no empty one.
 * @param[in] max_runs Number of entries runs can hold: with fewer than are
 * needed, the first runs are given.
 * @return Number of runs.
 */
uint32_t mqtt_ring_transmit_view_until(const mqtt_ring *ring, uint32_t before,
                                       const uint8_t *payload,
                                       uint32_t payload_length, uint32_t limit,
                                       mqtt_run *runs, uint32_t max_runs);

/**
 * @brief Overwrite bytes at the front with zeros, as mqtt_bytes_wipe does, and
 * leave them waiting. For bytes that held a secret, before they are removed.
 * @param[in,out] ring The ring.
 * @param[in] count Number of bytes. No more than are waiting are wiped.
 */
void mqtt_ring_wipe_front(mqtt_ring *ring, uint32_t count);

/**
 * @brief Remove bytes from the front, and give back each block emptied.
 * @param[in,out] ring The ring.
 * @param[in] count Number of bytes. No more than are waiting are removed.
 * @return Number of blocks given back to the pool.
 */
uint32_t mqtt_ring_consume(mqtt_ring *ring, uint32_t count);

/**
 * @brief Take blocks from the pool for bytes still to come.
 * @param[in,out] ring The ring.
 * @param[in] count Number of blocks wanted.
 * @return Number of blocks taken, fewer if fewer are free.
 */
uint32_t mqtt_ring_reserve(mqtt_ring *ring, uint32_t count);

/**
 * @brief List where bytes can be written next: the rest of the newest block,
 * then each block taken with mqtt_ring_reserve.
 * @param[in] ring The ring.
 * @param[out] spaces The places, in the order bytes must fill them.
 * @param[in] max_spaces Number of entries spaces can hold.
 * @return Number of places.
 */
uint32_t mqtt_ring_space(const mqtt_ring *ring, mqtt_space *spaces,
                         uint32_t max_spaces);

/**
 * @brief Count bytes written to the places mqtt_ring_space listed, in order,
 * and give back the blocks taken for bytes that didn't come.
 * @param[in,out] ring The ring.
 * @param[in] count Number of bytes written. No more than fit are counted.
 * @return Number of blocks given back to the pool.
 */
uint32_t mqtt_ring_commit(mqtt_ring *ring, uint32_t count);

/**
 * @brief Drop every waiting byte, and give back every block.
 * @param[in,out] ring The ring.
 * @return Number of blocks given back to the pool.
 */
uint32_t mqtt_ring_clear(mqtt_ring *ring);

/**
 * @brief Mark a ring's blocks and spares, and check its counts and offsets.
 * @param[in] ring The ring.
 * @param[in,out] marks One byte for each block of the pool: zero for a block
 * not marked yet. Each of the ring's becomes one.
 * @return true if every block is in range and was not marked before, and the
 * counts and offsets agree with the blocks.
 */
bool mqtt_ring_audit(const mqtt_ring *ring, uint8_t *marks);

#endif /* MQTT_BASE_RING_H */
