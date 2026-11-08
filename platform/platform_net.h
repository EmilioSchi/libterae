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
 * @file platform_net.h
 * @brief What the server and the client of the network layer share.
 */
#ifndef MQTT_PLATFORM_NET_H
#define MQTT_PLATFORM_NET_H

#include <stdint.h>

#include "base_platform.h"
#include "base_client.h"
#include "base_run_t.h"
#include "base_server.h"
#include "base_status.h"

/** @brief Number of runs written in one go. */
#define PLATFORM_NET_RUNS MQTT_PLATFORM_WRITE_RUNS
/** @brief Number of bytes in the largest TLS record. */
#define PLATFORM_NET_TLS_RECORD 16384U

/**
 * @brief Count the bytes in runs.
 * @param[in] runs The runs.
 * @param[in] count Number of runs.
 * @return Number of bytes.
 */
uint32_t platform_net_runs_length(const mqtt_run *runs, uint32_t count);

/**
 * @brief Write runs of bytes, in one go: as one system call on a socket, or as
 * one record through TLS, which takes a single buffer.
 *
 * For TLS, runs smaller than a record are copied together first, so that small
 * packets don't each take a record of their own.
 *
 * @param[in] handle The socket.
 * @param[in] session The TLS session, or MQTT_TLS_NONE for none.
 * @param[in] runs The runs.
 * @param[in] count Number of runs.
 * @param[out] scratch Where runs are put together for TLS, with room for
 * PLATFORM_NET_TLS_RECORD bytes, which no other thread uses at the same time.
 * @param[out] taken Number of bytes taken. Fewer than were given means the
 * socket is full, or for TLS, that one record has gone.
 * @return MQTT_OK, MQTT_INCOMPLETE, or MQTT_CONNECTION_CLOSED.
 */
mqtt_status platform_net_write_runs(int32_t handle, uint32_t session,
                                    const mqtt_run *runs, uint32_t count,
                                    uint8_t *scratch, uint32_t *taken);

/**
 * @brief Make ready a server's store, before it accepts any connection: write
 * a whole snapshot of what the server holds, as a restore left it.
 * @param[in,out] server The server.
 * @param[in] store The store, or MQTT_STORE_NONE.
 * @return MQTT_OK; MQTT_INVALID_ARGUMENT if the server keeps a store and none
 * is given, or keeps none and one is; or why the snapshot couldn't be written.
 */
mqtt_status platform_net_store_start(mqtt_server *server, int32_t store);

/**
 * @brief Do a round's work on the store, after its events: close and sync the
 * records they wrote, let go of the answers that waited for them, and write a
 * part of the snapshot under way, or begin one that is due.
 * @param[in,out] server The server.
 */
void platform_net_store_round(mqtt_server *server);

/**
 * @brief Find how long a round may wait for the sockets, with what the store
 * has to do.
 * @param[in] server The server.
 * @param[in] wait_ms How long the server's timers allow.
 * @return Milliseconds: 0 while records wait to be closed or a snapshot is
 * being written.
 */
uint32_t platform_net_store_wait(const mqtt_server *server, uint32_t wait_ms);

/**
 * @brief Close and sync the last records, once the connections are closed.
 * @param[in,out] server The server.
 */
void platform_net_store_stop(mqtt_server *server);

/**
 * @brief Do a client's step's work on its store, after what arrived: close
 * and sync the records written since the last, let go of the answers that
 * waited for them, and take a snapshot that is due. A client other than the
 * one given to mqtt_net_client_restore is left alone.
 * @param[in,out] client The client.
 */
void platform_net_client_store_round(mqtt_client *client);

#endif /* MQTT_PLATFORM_NET_H */
