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
 * @file base_server_transmit.c
 * @brief What a server's connection sends next, and what has gone.
 *
 * A connection's bytes are in its ring, except for a large payload, which is
 * sent from the stored message instead of being copied. The ring bytes that
 * come before such a payload, the payload and the ring bytes after it go out in
 * that order.
 *
 * An answer that waits for the journal stops the view at its first byte, with
 * everything after it, until the records before it are stored.
 */

#include <stddef.h>

#include "base_server_internal.h"

/**
 * @brief Find a connection that is in use.
 * @param[in,out] server The server.
 * @param[in] connection The index.
 * @return The connection, or NULL if the index is not in the table or the
 * entry is free.
 */
static mqtt_connection *transmit_connection(mqtt_server *server,
                                            uint32_t connection)
{
    /** @brief The connection. */
    mqtt_connection *entry = NULL;

    if (mqtt_server_connection_ready(server, connection)) {
        entry = &server->storage.connections[connection];
    }
    return entry;
}

uint32_t mqtt_server_transmit_view(mqtt_server *server, uint32_t connection,
                                   mqtt_run *runs, uint32_t max_runs)
{
    /** @brief Number of runs. */
    uint32_t count = 0U;
    /** @brief The connection. */
    mqtt_connection *entry = transmit_connection(server, connection);

    if ((entry != NULL) && ((runs != NULL) || (max_runs == 0U))) {
        mqtt_route_pump(server, connection);
        count = mqtt_ring_transmit_view_until(
            &entry->ring, entry->direct_ring_bytes, entry->direct_data,
            entry->direct_left,
            (entry->hold_bytes == MQTT_SERVER_NONE) ? entry->ring.used :
                                                      entry->hold_bytes,
            runs, max_runs);
    }
    return count;
}

void mqtt_server_transmit_consume(mqtt_server *server, uint32_t connection,
                                  uint32_t count)
{
    /** @brief The connection. */
    mqtt_connection *entry = transmit_connection(server, connection);

    if (entry != NULL) {
        /** @brief Number of bytes still to take. */
        uint32_t left = count;
        /** @brief Number taken from the ring before the payload. */
        const uint32_t before = (left < entry->direct_ring_bytes) ?
                                    left :
                                    entry->direct_ring_bytes;
        /** @brief Number of blocks given back. */
        uint32_t freed = mqtt_ring_consume(&entry->ring, before);

        entry->direct_ring_bytes -= before;
        left -= before;
        if ((left > 0U) && (entry->direct_left > 0U)) {
            /** @brief Number taken from the payload. */
            const uint32_t taken =
                (left < entry->direct_left) ? left : entry->direct_left;

            entry->direct_left -= taken;
            entry->direct_data = &entry->direct_data[taken];
            left -= taken;
            if (entry->direct_left == 0U) {
                mqtt_route_release(server, entry->direct_message);
                entry->direct_message = MQTT_SERVER_NONE;
                entry->direct_data = NULL;
            }
        }
        /* A count larger than what is waiting can only be a mistake, so no
         * more than that is taken. */
        if ((left > 0U) && (entry->direct_left == 0U)) {
            freed += mqtt_ring_consume(&entry->ring, left);
        }
        if (entry->hold_bytes != MQTT_SERVER_NONE) {
            /** @brief Number of bytes taken from the ring. */
            const uint32_t from_ring =
                before + ((entry->direct_left == 0U) ? left : 0U);

            entry->hold_bytes -= (from_ring < entry->hold_bytes) ?
                                     from_ring :
                                     entry->hold_bytes;
        }
        mqtt_server_blocks_returned(server, freed);
    }
}
