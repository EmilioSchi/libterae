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
 * @file base_ring.c
 * @brief A chain of pool blocks holding bytes that wait on a connection.
 *
 * The blocks of a ring are linked through the pool's links, from the oldest to
 * the newest, and the newest links to nothing. The blocks taken for bytes still
 * to come are a second list of their own, so a block is always in exactly one
 * place: the pool's free list, one ring's blocks, or one ring's spares.
 */

#include <stdbool.h>
#include <stddef.h>

#include "base_ring.h"
#include "base_string.h"

/**
 * @brief Divide, rounding up, one bit at a time.
 *
 * Some 32-bit targets have no instruction to divide, and the compiler would
 * call a helper of its own, which is not part of C99.
 *
 * @param[in] value The number divided.
 * @param[in] divisor The number to divide by, from 1 to MQTT_POOL_BLOCK_MAX.
 * @return The quotient, rounded up.
 */
static uint32_t ring_divide_up(uint32_t value, uint32_t divisor)
{
    /** @brief The quotient so far. */
    uint32_t quotient = 0U;
    /** @brief What is left to divide, always less than the divisor. */
    uint32_t remainder = 0U;
    /** @brief The bit of the value being brought down. */
    uint32_t bit = 32U;

    while (bit > 0U) {
        bit -= 1U;
        remainder = (remainder << 1U) | ((value >> bit) & 1U);
        if (remainder >= divisor) {
            remainder -= divisor;
            quotient |= (uint32_t)1U << bit;
        }
    }
    return quotient + ((remainder != 0U) ? 1U : 0U);
}

/**
 * @brief Find a block's bytes, here where the compiler can see it, as every
 * copy in and out of a ring needs it.
 * @param[in] ring The ring.
 * @param[in] block A block of the ring's pool.
 * @return Its first byte.
 */
static uint8_t *ring_block_bytes(const mqtt_ring *ring, uint32_t block)
{
    return &ring->pool->bytes[block * ring->pool->block_size];
}

/**
 * @brief Count the free bytes at the end of the newest block.
 * @param[in] ring The ring.
 * @return The number of bytes, zero if the ring holds no block.
 */
static uint32_t ring_slack(const mqtt_ring *ring)
{
    return (ring->blocks > 0U) ? (ring->pool->block_size - ring->tail_used) :
                                 0U;
}

/**
 * @brief Find where the waiting bytes of a block end.
 * @param[in] ring The ring.
 * @param[in] block A block of the ring.
 * @return The index after its last waiting byte.
 */
static uint32_t ring_block_end(const mqtt_ring *ring, uint32_t block)
{
    return (block == ring->tail) ? ring->tail_used : ring->pool->block_size;
}

/**
 * @brief Put a block at the new end of a ring, empty.
 * @param[in,out] ring The ring.
 * @param[in] block The block, which links to nothing.
 */
static void ring_link(mqtt_ring *ring, uint32_t block)
{
    if (ring->tail == MQTT_POOL_NONE) {
        ring->head = block;
        ring->head_offset = 0U;
    } else {
        ring->pool->next[ring->tail] = block;
    }
    ring->tail = block;
    ring->tail_used = 0U;
    ring->blocks += 1U;
}

/**
 * @brief Take a ring's next block: a spare if it has one, or else one from the
 * pool.
 * @param[in,out] ring The ring.
 * @return The block, which links to nothing, or MQTT_POOL_NONE.
 */
static uint32_t ring_take(mqtt_ring *ring)
{
    /** @brief The block. */
    uint32_t block = ring->spare;

    if (block != MQTT_POOL_NONE) {
        ring->spare = ring->pool->next[block];
        ring->spare_count -= 1U;
        ring->pool->next[block] = MQTT_POOL_NONE;
    } else {
        block = mqtt_pool_take(ring->pool);
    }
    return block;
}

void mqtt_ring_init(mqtt_ring *ring, mqtt_pool *pool)
{
    if ((ring != NULL) && (pool != NULL)) {
        ring->pool = pool;
        ring->head = MQTT_POOL_NONE;
        ring->tail = MQTT_POOL_NONE;
        ring->head_offset = 0U;
        ring->tail_used = 0U;
        ring->blocks = 0U;
        ring->used = 0U;
        ring->spare = MQTT_POOL_NONE;
        ring->spare_count = 0U;
    }
}

uint32_t mqtt_ring_held(const mqtt_ring *ring)
{
    return (ring != NULL) ? (ring->blocks + ring->spare_count) : 0U;
}

/**
 * @brief Do what mqtt_ring_room does, for arguments known to be valid.
 * @param[in] ring The ring.
 * @param[in] blocks Number of blocks it may still take.
 * @return The number of bytes, or UINT32_MAX if that is fewer.
 */
static uint32_t ring_room_body(const mqtt_ring *ring, uint32_t blocks)
{
    /** @brief The answer, which may not fit 32 bits. */
    const uint64_t room =
        (uint64_t)ring_slack(ring) +
        (((uint64_t)blocks + (uint64_t)ring->spare_count) *
         (uint64_t)ring->pool->block_size);

    return (room > (uint64_t)UINT32_MAX) ? UINT32_MAX : (uint32_t)room;
}

uint32_t mqtt_ring_room(const mqtt_ring *ring, uint32_t blocks)
{
    /** @brief The answer. */
    uint32_t answer = 0U;

    if (ring != NULL) {
        answer = ring_room_body(ring, blocks);
    }
    return answer;
}

/**
 * @brief Do what mqtt_ring_blocks_for does, for arguments known to be valid.
 * @param[in] ring The ring.
 * @param[in] length Number of bytes.
 * @return The number of blocks.
 */
static uint32_t ring_blocks_for_body(const mqtt_ring *ring, uint32_t length)
{
    /** @brief The bytes that fit without a new block. */
    const uint32_t room = mqtt_ring_room(ring, 0U);
    /** @brief The answer. */
    uint32_t blocks = 0U;

    if (length > room) {
        blocks = ring_divide_up(length - room, ring->pool->block_size);
    }
    return blocks;
}

uint32_t mqtt_ring_blocks_for(const mqtt_ring *ring, uint32_t length)
{
    /** @brief The answer. */
    uint32_t answer = 0U;

    if (ring != NULL) {
        answer = ring_blocks_for_body(ring, length);
    }
    return answer;
}

/**
 * @brief Do what mqtt_ring_append does, for arguments known to be valid.
 * @param[in,out] ring The ring.
 * @param[in] data The bytes.
 * @param[in] length Number of bytes in data.
 * @return MQTT_OK, or MQTT_RING_FULL if the pool has too few free blocks, when
 * nothing is added.
 */
static mqtt_status ring_append_body(mqtt_ring *ring, const uint8_t *data,
                                    uint32_t length)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_RING_FULL;

    if ((length > 0U) && (length <= ring_slack(ring))) {
        /* It all fits in the newest block, the usual case for small parts. */
        mqtt_bytes_put(&ring_block_bytes(ring, ring->tail)[ring->tail_used],
                       data, length);
        ring->tail_used += length;
        ring->used += length;
        status = MQTT_OK;
    } else if (mqtt_ring_blocks_for(ring, length) <= ring->pool->free_count) {
        /** @brief Number of bytes in a block. */
        const uint32_t size = ring->pool->block_size;
        /** @brief Index of the next byte to add. */
        uint32_t position = 0U;

        while (position < length) {
            /** @brief Number of bytes that go in the newest block. */
            uint32_t take = 0U;

            if ((ring->blocks == 0U) || (ring->tail_used == size)) {
                ring_link(ring, ring_take(ring));
            }
            take = size - ring->tail_used;
            if (take > (length - position)) {
                take = length - position;
            }
            /* take is at most what is left of the block. */
            mqtt_bytes_put(&ring_block_bytes(ring, ring->tail)[ring->tail_used],
                           &data[position], take);
            ring->tail_used += take;
            ring->used += take;
            position += take;
        }
        status = MQTT_OK;
    } else {
        /* The pool has too few free blocks, so nothing is added. */
    }
    return status;
}

mqtt_status mqtt_ring_append(mqtt_ring *ring, const uint8_t *data,
                             uint32_t length)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if ((ring != NULL) && ((data != NULL) || (length == 0U))) {
        answer = ring_append_body(ring, data, length);
    }
    return answer;
}

/**
 * @brief Add a run to a list, if it has bytes and the list has room.
 * @param[out] runs The list.
 * @param[in] max_runs Number of entries it can hold.
 * @param[in,out] count Number of entries in it.
 * @param[in] data The run's first byte.
 * @param[in] length Number of bytes in the run.
 */
static void ring_add(mqtt_run *runs, uint32_t max_runs, uint32_t *count,
                     const uint8_t *data, uint32_t length)
{
    if ((length > 0U) && (*count < max_runs)) {
        runs[*count].data = data;
        runs[*count].length = length;
        *count += 1U;
    }
}

/**
 * @brief Add the waiting bytes between two offsets to a list, one run for each
 * block.
 * @param[in] ring The ring.
 * @param[in] from The first offset.
 * @param[in] to The offset after the last, no more than the bytes waiting.
 * @param[out] runs The list.
 * @param[in] max_runs Number of entries it can hold.
 * @param[in,out] count Number of entries in it.
 */
static void ring_add_range(const mqtt_ring *ring, uint32_t from, uint32_t to,
                           mqtt_run *runs, uint32_t max_runs, uint32_t *count)
{
    /** @brief The block being looked at. */
    uint32_t block = ring->head;
    /** @brief Index in it of its first waiting byte. */
    uint32_t start = ring->head_offset;
    /** @brief Offset of that byte among the waiting bytes. */
    uint32_t position = 0U;

    while ((block != MQTT_POOL_NONE) && (position < to) &&
           (*count < max_runs)) {
        /** @brief Number of waiting bytes in the block. */
        const uint32_t here = ring_block_end(ring, block) - start;

        if ((position + here) > from) {
            /** @brief Number of its bytes before the range. */
            const uint32_t skip = (from > position) ? (from - position) : 0U;
            /** @brief Number of its bytes up to the end of the range. */
            const uint32_t stop =
                ((position + here) < to) ? here : (to - position);

            ring_add(runs, max_runs, count,
                     &ring_block_bytes(ring, block)[start + skip],
                     stop - skip);
        }
        position += here;
        start = 0U;
        block = ring->pool->next[block];
    }
}

/**
 * @brief Do what mqtt_ring_view does, for arguments known to be valid.
 * @param[in] ring The ring.
 * @param[out] runs The runs, in order.
 * @param[in] max_runs Number of entries runs can hold. With fewer than are
 * needed, the first ones are given.
 * @return Number of runs, which is zero only if no bytes are waiting.
 */
static uint32_t ring_view_body(const mqtt_ring *ring, mqtt_run *runs,
                               uint32_t max_runs)
{
    /** @brief Number of runs. */
    uint32_t count = 0U;

    ring_add_range(ring, 0U, ring->used, runs, max_runs, &count);
    return count;
}

uint32_t mqtt_ring_view(const mqtt_ring *ring, mqtt_run *runs,
                        uint32_t max_runs)
{
    /** @brief The answer. */
    uint32_t answer = 0U;

    if ((ring != NULL) && ((runs != NULL) || (max_runs == 0U))) {
        answer = ring_view_body(ring, runs, max_runs);
    }
    return answer;
}

/**
 * @brief Do what mqtt_ring_view_range does, for arguments known to be valid.
 * @param[in] ring The ring.
 * @param[in] offset Index of the first byte, counted from the oldest waiting.
 * @param[in] length Number of bytes. Those past the last byte waiting are left
 * out.
 * @param[out] runs The runs, in order.
 * @param[in] max_runs Number of entries runs can hold. With fewer than are
 * needed, the first ones are given.
 * @return Number of runs.
 */
static uint32_t ring_view_range_body(const mqtt_ring *ring, uint32_t offset,
                                     uint32_t length, mqtt_run *runs,
                                     uint32_t max_runs)
{
    /** @brief Number of runs. */
    uint32_t count = 0U;

    if (offset < ring->used) {
        /** @brief The offset after the last byte. */
        const uint32_t end = ((ring->used - offset) < length) ?
                                 ring->used :
                                 (offset + length);

        ring_add_range(ring, offset, end, runs, max_runs, &count);
    }
    return count;
}

uint32_t mqtt_ring_view_range(const mqtt_ring *ring, uint32_t offset,
                              uint32_t length, mqtt_run *runs,
                              uint32_t max_runs)
{
    /** @brief The answer. */
    uint32_t answer = 0U;

    if ((ring != NULL) && ((runs != NULL) || (max_runs == 0U))) {
        answer = ring_view_range_body(ring, offset, length, runs, max_runs);
    }
    return answer;
}

/**
 * @brief Do what mqtt_ring_peek does, for arguments known to be valid.
 * @param[in] ring The ring.
 * @param[in] offset Index of the first byte, counted from the oldest waiting.
 * @param[out] bytes Where they go.
 * @param[in] length Number of bytes wanted.
 * @return Number of bytes copied, fewer if fewer are waiting.
 */
static uint32_t ring_peek_body(const mqtt_ring *ring, uint32_t offset,
                               uint8_t *bytes, uint32_t length)
{
    /** @brief The block being looked at. */
    uint32_t block = ring->head;
    /** @brief Index in it of its first waiting byte. */
    uint32_t start = ring->head_offset;
    /** @brief Offset of that byte among the waiting bytes. */
    uint32_t position = 0U;
    /** @brief Number of bytes copied. */
    uint32_t copied = 0U;

    while ((block != MQTT_POOL_NONE) && (copied < length)) {
        /** @brief Number of waiting bytes in the block. */
        const uint32_t here = ring_block_end(ring, block) - start;

        if ((position + here) > offset) {
            /** @brief Number of its bytes before the first wanted. */
            const uint32_t skip =
                (offset > position) ? (offset - position) : 0U;
            /** @brief Number of its bytes to copy. */
            const uint32_t take = ((here - skip) < (length - copied)) ?
                                      (here - skip) :
                                      (length - copied);

            /* take is at most what is left of bytes. */
            mqtt_bytes_put(&bytes[copied],
                           &ring_block_bytes(ring, block)[start + skip], take);
            copied += take;
        }
        position += here;
        start = 0U;
        block = ring->pool->next[block];
    }
    return copied;
}

uint32_t mqtt_ring_peek(const mqtt_ring *ring, uint32_t offset, uint8_t *bytes,
                        uint32_t length)
{
    /** @brief The answer. */
    uint32_t answer = 0U;

    if ((ring != NULL) && ((bytes != NULL) || (length == 0U))) {
        answer = ring_peek_body(ring, offset, bytes, length);
    }
    return answer;
}

uint32_t mqtt_ring_transmit_view(const mqtt_ring *ring, uint32_t before,
                                 const uint8_t *payload,
                                 uint32_t payload_length, mqtt_run *runs,
                                 uint32_t max_runs)
{
    return (ring != NULL) ?
               mqtt_ring_transmit_view_until(ring, before, payload,
                                             payload_length, ring->used, runs,
                                             max_runs) :
               0U;
}

/**
 * @brief Do what mqtt_ring_transmit_view_until does, for arguments known to be
 * valid.
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
static uint32_t ring_transmit_view_until_body(const mqtt_ring *ring,
                                              uint32_t before,
                                              const uint8_t *payload,
                                              uint32_t payload_length,
                                              uint32_t limit, mqtt_run *runs,
                                              uint32_t max_runs)
{
    /** @brief Number of runs. */
    uint32_t count = 0U;
    /** @brief The bytes that may go, no more than are waiting. */
    const uint32_t end = (limit < ring->used) ? limit : ring->used;
    /** @brief The bytes before the payload, no more than may go. */
    const uint32_t split = (before < end) ? before : end;

    ring_add_range(ring, 0U, split, runs, max_runs, &count);
    /* Once one run has not fitted, none after it may go in, or bytes would
     * be sent out of order. The runs added below only go in while there is
     * still room, which keeps them a prefix. The payload goes only if every
     * byte before it may. */
    if ((payload != NULL) && (before <= end)) {
        ring_add(runs, max_runs, &count, payload, payload_length);
    }
    ring_add_range(ring, split, end, runs, max_runs, &count);
    return count;
}

uint32_t mqtt_ring_transmit_view_until(const mqtt_ring *ring, uint32_t before,
                                       const uint8_t *payload,
                                       uint32_t payload_length, uint32_t limit,
                                       mqtt_run *runs, uint32_t max_runs)
{
    /** @brief The answer. */
    uint32_t answer = 0U;

    if ((ring != NULL) && ((runs != NULL) || (max_runs == 0U))) {
        answer = ring_transmit_view_until_body(ring, before, payload,
                                               payload_length, limit, runs,
                                               max_runs);
    }
    return answer;
}

/**
 * @brief Do what mqtt_ring_wipe_front does, for arguments known to be valid.
 * @param[in,out] ring The ring.
 * @param[in] count Number of bytes. No more than are waiting are wiped.
 */
static void ring_wipe_front_body(mqtt_ring *ring, uint32_t count)
{
    /** @brief The block being wiped. */
    uint32_t block = ring->head;
    /** @brief Index in it of its first waiting byte. */
    uint32_t start = ring->head_offset;
    /** @brief Number of bytes wiped. */
    uint32_t wiped = 0U;

    while ((block != MQTT_POOL_NONE) && (wiped < count)) {
        /** @brief Number of waiting bytes in the block. */
        const uint32_t here = ring_block_end(ring, block) - start;
        /** @brief Number of them to wipe. */
        const uint32_t take =
            (here < (count - wiped)) ? here : (count - wiped);

        mqtt_bytes_wipe(&ring_block_bytes(ring, block)[start], take);
        wiped += take;
        start = 0U;
        block =
            (block == ring->tail) ? MQTT_POOL_NONE : ring->pool->next[block];
    }
}

void mqtt_ring_wipe_front(mqtt_ring *ring, uint32_t count)
{
    if (ring != NULL) {
        ring_wipe_front_body(ring, count);
    }
}

/**
 * @brief Do what mqtt_ring_consume does, for arguments known to be valid.
 * @param[in,out] ring The ring.
 * @param[in] count Number of bytes. No more than are waiting are removed.
 * @return Number of blocks given back to the pool.
 */
static uint32_t ring_consume_body(mqtt_ring *ring, uint32_t count)
{
    /** @brief Number of bytes still to remove. */
    uint32_t left = (count < ring->used) ? count : ring->used;
    /** @brief Number of blocks given back. */
    uint32_t freed = 0U;

    ring->used -= left;
    while (left > 0U) {
        /** @brief Number of waiting bytes in the oldest block. */
        const uint32_t here = ring_block_end(ring, ring->head) -
                              ring->head_offset;

        if (left < here) {
            ring->head_offset += left;
            left = 0U;
        } else {
            /** @brief The block after it. */
            const uint32_t next = ring->pool->next[ring->head];

            left -= here;
            mqtt_pool_give(ring->pool, ring->head);
            freed += 1U;
            ring->blocks -= 1U;
            ring->head = next;
            ring->head_offset = 0U;
            if (next == MQTT_POOL_NONE) {
                ring->tail = MQTT_POOL_NONE;
                ring->tail_used = 0U;
            }
        }
    }
    return freed;
}

uint32_t mqtt_ring_consume(mqtt_ring *ring, uint32_t count)
{
    /** @brief The answer. */
    uint32_t answer = 0U;

    if (ring != NULL) {
        answer = ring_consume_body(ring, count);
    }
    return answer;
}

/**
 * @brief Do what mqtt_ring_reserve does, for arguments known to be valid.
 * @param[in,out] ring The ring.
 * @param[in] count Number of blocks wanted.
 * @return Number of blocks taken, fewer if fewer are free.
 */
static uint32_t ring_reserve_body(mqtt_ring *ring, uint32_t count)
{
    /** @brief Number of blocks taken. */
    uint32_t taken = 0U;
    /** @brief The block taken last. */
    uint32_t block = 0U;

    while ((taken < count) && (block != MQTT_POOL_NONE)) {
        block = mqtt_pool_take(ring->pool);
        if (block != MQTT_POOL_NONE) {
            ring->pool->next[block] = ring->spare;
            ring->spare = block;
            ring->spare_count += 1U;
            taken += 1U;
        }
    }
    return taken;
}

uint32_t mqtt_ring_reserve(mqtt_ring *ring, uint32_t count)
{
    /** @brief The answer. */
    uint32_t answer = 0U;

    if (ring != NULL) {
        answer = ring_reserve_body(ring, count);
    }
    return answer;
}

/**
 * @brief Do what mqtt_ring_space does, for arguments known to be valid.
 * @param[in] ring The ring.
 * @param[out] spaces The places, in the order bytes must fill them.
 * @param[in] max_spaces Number of entries spaces can hold.
 * @return Number of places.
 */
static uint32_t ring_space_body(const mqtt_ring *ring, mqtt_space *spaces,
                                uint32_t max_spaces)
{
    /** @brief Number of places. */
    uint32_t count = 0U;
    /** @brief The spare block being listed. */
    uint32_t block = ring->spare;
    /** @brief The free bytes at the end of the newest block. */
    const uint32_t slack = ring_slack(ring);

    if ((slack > 0U) && (count < max_spaces)) {
        spaces[count].data =
            &ring_block_bytes(ring, ring->tail)[ring->tail_used];
        spaces[count].length = slack;
        count += 1U;
    }
    while ((block != MQTT_POOL_NONE) && (count < max_spaces)) {
        spaces[count].data = ring_block_bytes(ring, block);
        spaces[count].length = ring->pool->block_size;
        count += 1U;
        block = ring->pool->next[block];
    }
    return count;
}

uint32_t mqtt_ring_space(const mqtt_ring *ring, mqtt_space *spaces,
                         uint32_t max_spaces)
{
    /** @brief The answer. */
    uint32_t answer = 0U;

    if ((ring != NULL) && ((spaces != NULL) || (max_spaces == 0U))) {
        answer = ring_space_body(ring, spaces, max_spaces);
    }
    return answer;
}

/**
 * @brief Do what mqtt_ring_commit does, for arguments known to be valid.
 * @param[in,out] ring The ring.
 * @param[in] count Number of bytes written. No more than fit are counted.
 * @return Number of blocks given back to the pool.
 */
static uint32_t ring_commit_body(mqtt_ring *ring, uint32_t count)
{
    /** @brief Number of bytes still to count. */
    uint32_t left = count;
    /** @brief The free bytes at the end of the newest block. */
    const uint32_t slack = ring_slack(ring);
    /** @brief Number of bytes in a block. */
    const uint32_t size = ring->pool->block_size;
    /** @brief Number of blocks given back. */
    uint32_t freed = 0U;

    if (slack > 0U) {
        /** @brief Number of bytes in the newest block. */
        const uint32_t take = (left < slack) ? left : slack;

        ring->tail_used += take;
        ring->used += take;
        left -= take;
    }
    while ((left > 0U) && (ring->spare != MQTT_POOL_NONE)) {
        /** @brief Number of bytes in this block. */
        const uint32_t take = (left < size) ? left : size;

        ring_link(ring, ring_take(ring));
        ring->tail_used = take;
        ring->used += take;
        left -= take;
    }
    while (ring->spare != MQTT_POOL_NONE) {
        mqtt_pool_give(ring->pool, ring_take(ring));
        freed += 1U;
    }
    return freed;
}

uint32_t mqtt_ring_commit(mqtt_ring *ring, uint32_t count)
{
    /** @brief The answer. */
    uint32_t answer = 0U;

    if (ring != NULL) {
        answer = ring_commit_body(ring, count);
    }
    return answer;
}

/**
 * @brief Do what mqtt_ring_clear does, for arguments known to be valid.
 * @param[in,out] ring The ring.
 * @return Number of blocks given back to the pool.
 */
static uint32_t ring_clear_body(mqtt_ring *ring)
{
    /** @brief Number of blocks given back. */
    uint32_t freed = mqtt_ring_consume(ring, ring->used);

    while (ring->spare != MQTT_POOL_NONE) {
        mqtt_pool_give(ring->pool, ring_take(ring));
        freed += 1U;
    }
    return freed;
}

uint32_t mqtt_ring_clear(mqtt_ring *ring)
{
    /** @brief The answer. */
    uint32_t answer = 0U;

    if (ring != NULL) {
        answer = ring_clear_body(ring);
    }
    return answer;
}

/**
 * @brief Mark the blocks of a list, and count them.
 * @param[in] ring The ring whose pool the blocks are in.
 * @param[in,out] marks One byte for each block.
 * @param[in] first The first block of the list.
 * @param[out] last The last block, or MQTT_POOL_NONE for an empty list.
 * @return Number of blocks, or MQTT_POOL_NONE if one was out of range or
 * marked already.
 */
static uint32_t ring_mark(const mqtt_ring *ring, uint8_t *marks, uint32_t first,
                          uint32_t *last)
{
    /** @brief The block being looked at. */
    uint32_t block = first;
    /** @brief Number of blocks found. */
    uint32_t found = 0U;

    *last = MQTT_POOL_NONE;
    while ((found != MQTT_POOL_NONE) && (block != MQTT_POOL_NONE)) {
        if ((block >= ring->pool->block_count) || (marks[block] != 0U)) {
            found = MQTT_POOL_NONE;
        } else {
            marks[block] = 1U;
            found += 1U;
            *last = block;
            block = ring->pool->next[block];
        }
    }
    return found;
}

/**
 * @brief Check that the bytes waiting agree with a ring's blocks and offsets.
 * @param[in] ring The ring, whose blocks have been counted.
 * @return true if they agree.
 */
static bool ring_counts_agree(const mqtt_ring *ring)
{
    /** @brief Number of bytes in a block. */
    const uint64_t size = (uint64_t)ring->pool->block_size;
    /** @brief The answer for an empty ring. */
    bool agree = (ring->used == 0U) && (ring->head_offset == 0U) &&
                 (ring->tail_used == 0U);

    if (ring->blocks > 0U) {
        /** @brief The bytes the offsets say are waiting. */
        const uint64_t bytes = (((uint64_t)ring->blocks - 1U) * size) +
                               (uint64_t)ring->tail_used -
                               (uint64_t)ring->head_offset;
        /** @brief true if one block holds bytes from head to tail. */
        const bool ordered =
            (ring->blocks > 1U) || (ring->head_offset < ring->tail_used);

        agree = ((uint64_t)ring->head_offset < size) &&
                (ring->tail_used > 0U) &&
                ((uint64_t)ring->tail_used <= size) && ordered &&
                ((uint64_t)ring->used == bytes);
    }
    return agree;
}

/**
 * @brief Do what mqtt_ring_audit does, for arguments known to be valid.
 * @param[in] ring The ring.
 * @param[in,out] marks One byte for each block of the pool: zero for a block
 * not marked yet. Each of the ring's becomes one.
 * @return true if every block is in range and was not marked before, and the
 * counts and offsets agree with the blocks.
 */
static bool ring_audit_body(const mqtt_ring *ring, uint8_t *marks)
{
    /** @brief The last block of the ring. */
    uint32_t last = MQTT_POOL_NONE;
    /** @brief The last spare. */
    uint32_t last_spare = MQTT_POOL_NONE;
    /** @brief Number of blocks in the ring. */
    const uint32_t blocks = ring_mark(ring, marks, ring->head, &last);
    /** @brief Number of spares. */
    const uint32_t spares = ring_mark(ring, marks, ring->spare, &last_spare);
    /** @brief true if the bytes waiting agree with the offsets. */
    const bool agree = ring_counts_agree(ring);

    return (blocks == ring->blocks) && (spares == ring->spare_count) &&
           (last == ring->tail) && agree;
}

bool mqtt_ring_audit(const mqtt_ring *ring, uint8_t *marks)
{
    /** @brief The answer. */
    bool answer = false;

    if ((ring != NULL) && (marks != NULL)) {
        answer = ring_audit_body(ring, marks);
    }
    return answer;
}
