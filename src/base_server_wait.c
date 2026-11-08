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
 * @file base_server_wait.c
 * @brief What a network layer needs to know about a connection to decide how
 * often to wait for it.
 *
 * These calls change nothing but the activity they report, so a network layer
 * may make them after every read and every write.
 */

#include <stddef.h>

#include "base_server_internal.h"

/**
 * @brief Find a connection that is in use.
 * @param[in] server The server.
 * @param[in] connection The index.
 * @return The connection, or NULL if the index is not in the table or the
 * entry is free.
 */
static const mqtt_connection *wait_connection(const mqtt_server *server,
                                              uint32_t connection)
{
    /** @brief The connection. */
    const mqtt_connection *entry = NULL;

    if (mqtt_server_connection_ready(server, connection)) {
        entry = &server->storage.connections[connection];
    }
    return entry;
}

mqtt_server_activity mqtt_server_take_activity(mqtt_server *server,
                                               uint32_t connection)
{
    /** @brief What it has sent. */
    mqtt_server_activity activity = MQTT_SERVER_ACTIVITY_NONE;

    if (wait_connection(server, connection) != NULL) {
        /** @brief The connection. */
        mqtt_connection *entry = &server->storage.connections[connection];

        activity = entry->activity;
        entry->activity = MQTT_SERVER_ACTIVITY_NONE;
    }
    return activity;
}

bool mqtt_server_accepted(const mqtt_server *server, uint32_t connection)
{
    /** @brief The connection. */
    const mqtt_connection *entry = wait_connection(server, connection);

    return (entry != NULL) && (entry->state == MQTT_CONNECTION_ACTIVE);
}

uint16_t mqtt_server_keep_alive(const mqtt_server *server,
                                uint32_t connection)
{
    /** @brief The Keep Alive. */
    uint16_t keep_alive = 0U;

    if (mqtt_server_accepted(server, connection)) {
        keep_alive = server->storage.connections[connection].keep_alive;
    }
    return keep_alive;
}

bool mqtt_server_idle(const mqtt_server *server, uint32_t connection)
{
    /** @brief The connection. */
    const mqtt_connection *entry = wait_connection(server, connection);
    /** @brief The answer. */
    bool idle = false;

    if (entry != NULL) {
        idle = (entry->receive.used == 0U) &&
               (entry->assembly_block == MQTT_PAGE_ARENA_NONE) &&
               (entry->ring.used == 0U) && (entry->direct_left == 0U) &&
               (!entry->waiting) && (!entry->throttled);
    }
    return idle;
}
