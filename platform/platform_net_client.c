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
 * @file platform_net_client.c
 * @brief Running a client on a real network connection.
 *
 * A client waits on its one socket with mqtt_platform_wait_socket, which keeps
 * nothing between calls, and has its own place to put runs together for TLS,
 * so it can run on one thread while a server runs on another. A step reads,
 * closes and syncs what that wrote to the client's store, then writes.
 */

#include <stddef.h>

#include "base_net.h"
#include "base_platform.h"
#include "base_tls.h"
#include "platform_net.h"

/**
 * @brief Where a client's small runs are put together into one TLS record.
 * A program runs one client at a time.
 */
static uint8_t client_tls_scratch[PLATFORM_NET_TLS_RECORD];

mqtt_status mqtt_net_client_open(mqtt_net_client *network, mqtt_client *client,
                                 const char *host, const char *port,
                                 const mqtt_net_client_tls *tls)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    network->handle = MQTT_SOCKET_NONE;
    status = mqtt_platform_connect(host, port, &network->handle);
    network->client = client;
    network->tls = (tls != NULL) && tls->enabled;
    network->tls_session = MQTT_TLS_NONE;
    network->write_blocked = false;
    if ((status == MQTT_OK) && network->tls) {
        status = mqtt_tls_client_setup(tls->ca_file, tls->certificate_file,
                                       tls->key_file, tls->key_password,
                                       tls->insecure);
        if (status == MQTT_OK) {
            /** @brief The name to expect in the Server's certificate. */
            const char *name =
                (tls->server_name != NULL) ? tls->server_name : host;

            network->tls_session =
                mqtt_tls_session_open(network->handle, false, name);
            if (network->tls_session == MQTT_TLS_NONE) {
                status = MQTT_TLS_ERROR;
            }
        }
        if (status != MQTT_OK) {
            mqtt_platform_close(network->handle);
            network->handle = MQTT_SOCKET_NONE;
        }
    }
    return status;
}

/**
 * @brief Run a client's timers, then wait on its socket for as long as they
 * and the caller allow, and only for what can be acted on.
 * @param[in] network The client's network connection.
 * @param[in] wait_ms How long the caller allows a wait to be, in milliseconds.
 * @param[out] came What came on the socket.
 */
static void net_client_wait(const mqtt_net_client *network, uint32_t wait_ms,
                            mqtt_wait_event *came)
{
    /** @brief The client. */
    mqtt_client *client = network->client;
    /** @brief The time. */
    const uint64_t now_ms = mqtt_platform_time_ms();
    /** @brief How long the timers allow a wait to be. */
    const uint32_t timer_ms = mqtt_client_tick(client, now_ms);
    /** @brief How long to wait. */
    uint32_t wait = (wait_ms < timer_ms) ? wait_ms : timer_ms;
    /** @brief The first run of the bytes to send. */
    mqtt_run view[1];
    /** @brief Number of runs: none if there is nothing to send. */
    const uint32_t available = mqtt_client_transmit_view(client, view, 1U);
    /** @brief Where bytes read would go. */
    mqtt_space space = {NULL, 0U};
    /** @brief Number of bytes the client may be given. */
    const uint32_t capacity = mqtt_client_receive_space(client, &space);
    /** @brief true if TLS can't go on until the socket has more to read. */
    const bool awaiting_read =
        network->tls && mqtt_tls_wants_read(network->tls_session);
    /**
     * @brief Whether to wait for read readiness. Asking for what can't be acted
     * on would only make the wait end at once, and spin.
     */
    const bool want_read = capacity > 0U;

    if (mqtt_client_store_open(client)) {
        /* Records are open, which the step closes at once. */
        wait = 0U;
    } else if (network->write_blocked) {
        /* The wait is for the socket to take more. */
    } else if ((available > 0U) && (!awaiting_read)) {
        /* There is something to try writing, so there is no reason to wait. */
        wait = 0U;
    } else {
        /* There is nothing to write, or it depends on what is still to be
         * read. */
    }
    /* TLS may hold bytes the socket no longer shows, and must not wait for
     * them. */
    if (network->tls && want_read &&
        mqtt_tls_has_pending(network->tls_session)) {
        came->tag = 0U;
        came->handle = 0U;
        came->readable = true;
        came->writable = false;
        came->hang_up = false;
    } else {
        (void)mqtt_platform_wait_socket(network->handle, want_read,
                                        network->write_blocked, wait, came);
    }
}

/**
 * @brief Read from a client's socket if it has something and the client has
 * room for it, and hand what arrived to the client.
 * @param[in] network The client's network connection.
 * @param[in] came What came on the socket.
 * @return MQTT_OK, or the outcome of finding that the connection has gone.
 */
static mqtt_status net_client_read(const mqtt_net_client *network,
                                   const mqtt_wait_event *came)
{
    /** @brief The client. */
    mqtt_client *client = network->client;
    /** @brief Where the bytes go: straight into the client's buffer. */
    mqtt_space space = {NULL, 0U};
    /** @brief Number of bytes the client may be given. */
    const uint32_t capacity = mqtt_client_receive_space(client, &space);
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (came->readable) {
        if (capacity > 0U) {
            /** @brief Number of bytes read. */
            uint32_t count = 0U;

            status = network->tls ?
                         mqtt_tls_read(network->tls_session, space.data,
                                       capacity, &count) :
                         mqtt_platform_read(network->handle, space.data,
                                            capacity, &count);
            if (status == MQTT_OK) {
                (void)mqtt_client_receive_commit(client, count,
                                                 mqtt_platform_time_ms());
            } else if (status == MQTT_INCOMPLETE) {
                status = MQTT_OK;
            } else {
                /* The connection has gone. */
            }
        } else if (came->hang_up) {
            /* It has gone, and the system goes on saying so for as long as
             * anyone asks. */
            status = MQTT_CONNECTION_CLOSED;
        } else {
            /* It stays where it is until there is room for it. */
        }
    }
    return status;
}

/**
 * @brief Write out what a client has to send, for as long as the socket takes
 * it.
 * @param[in,out] network The client's network connection.
 * @return MQTT_OK, or the outcome of finding that the connection has gone.
 */
static mqtt_status net_client_write(mqtt_net_client *network)
{
    /** @brief The client. */
    mqtt_client *client = network->client;
    /** @brief The runs of one write. */
    mqtt_run runs[PLATFORM_NET_RUNS];
    /** @brief Number of runs. */
    uint32_t count = 0U;
    /** @brief The TLS session, or MQTT_TLS_NONE for a plain socket. */
    const uint32_t session =
        network->tls ? network->tls_session : MQTT_TLS_NONE;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    network->write_blocked = false;
    count = mqtt_client_transmit_view(client, runs, PLATFORM_NET_RUNS);
    while ((status == MQTT_OK) && (count > 0U)) {
        /** @brief Number of bytes taken. */
        uint32_t taken = 0U;

        status = platform_net_write_runs(network->handle, session, runs, count,
                                         client_tls_scratch, &taken);
        if (status == MQTT_OK) {
            mqtt_client_transmit_consume(client, taken);
            if ((session == MQTT_TLS_NONE) &&
                (taken < platform_net_runs_length(runs, count))) {
                status = MQTT_INCOMPLETE;
            } else {
                count =
                    mqtt_client_transmit_view(client, runs, PLATFORM_NET_RUNS);
            }
        }
    }
    if (status == MQTT_INCOMPLETE) {
        network->write_blocked =
            (!network->tls) || mqtt_tls_wants_write(network->tls_session);
        status = MQTT_OK;
    }
    return status;
}

mqtt_status mqtt_net_client_step(mqtt_net_client *network, uint32_t wait_ms)
{
    /** @brief What came on the socket. */
    mqtt_wait_event came;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    net_client_wait(network, wait_ms, &came);
    status = net_client_read(network, &came);
    /* The answers that waited for what arrived can go once it is stored. */
    platform_net_client_store_round(network->client);
    if (status == MQTT_OK) {
        status = net_client_write(network);
    }
    if ((status == MQTT_OK) && mqtt_client_should_close(network->client)) {
        status = MQTT_CONNECTION_CLOSED;
    }
    return status;
}

void mqtt_net_client_close(mqtt_net_client *network)
{
    /* What the last write wrote to the store is closed and synced too. */
    platform_net_client_store_round(network->client);
    if (network->handle != MQTT_SOCKET_NONE) {
        if (network->tls) {
            mqtt_tls_session_close(network->tls_session);
        }
        mqtt_platform_close(network->handle);
        network->handle = MQTT_SOCKET_NONE;
    }
    mqtt_client_connection_lost(network->client, mqtt_platform_time_ms());
}
