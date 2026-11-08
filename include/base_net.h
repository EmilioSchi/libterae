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
 * @file base_net.h
 * @brief Running a server or a client on real network connections.
 *
 * This is the part of the library that joins the protocol code in src/ to the
 * sockets of the operating system and to TLS. Its implementation is in
 * platform/, so it can be left out of a system that has its own way of moving
 * bytes: nothing in src/ depends on it.
 *
 * A server that keeps a store closes the records of each round into a group,
 * syncs it if it asks for that, and only then sends the answers that waited
 * for it. A snapshot is written a little each round, and a whole one before
 * the first connection is accepted.
 *
 * A server waits for readiness on its sockets with poll, so the code is the
 * same on every POSIX system. It sorts them into the tiers of base_tier.h, so
 * each wait hands poll only the busy sockets and, now and then, the quiet ones.
 * A quiet Client loses nothing by this: its bytes wait in the system's buffer,
 * and are read at most a COLD period late. Bytes for a Client are written as
 * soon as they are ready, whatever its tier. It runs on one thread.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 4.2, p.93. 5, p.111-115. 6, p.116.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */
#ifndef MQTT_BASE_NET_H
#define MQTT_BASE_NET_H

#include <stdbool.h>
#include <stdint.h>

#include "base_client.h"
#include "base_server.h"
#include "base_status.h"
#include "base_store.h"
#include "base_tier.h"

#if !defined(MQTT_NET_TLS_SESSIONS)
/**
 * @brief The largest number of TLS connections at one time, unless the build
 * sets another number from 1 to 65536.
 */
#define MQTT_NET_TLS_SESSIONS 1024U
#endif

#if (MQTT_NET_TLS_SESSIONS < 1U) || (MQTT_NET_TLS_SESSIONS > 65536U)
#error "MQTT_NET_TLS_SESSIONS must be from 1 to 65536"
#endif

/**
 * @brief Number of bytes in the longest Pong frame: its header and the largest
 * control payload.
 */
#define MQTT_NET_PONG_BYTES 127U

/** @brief How a connection's bytes are carried. */
typedef enum {
    /** @brief Directly over TCP. */
    MQTT_NET_TCP = 0,
    /** @brief Over TLS. */
    MQTT_NET_TLS,
    /** @brief Over WebSocket, over TCP. */
    MQTT_NET_WEBSOCKET
} mqtt_net_kind;

/** @brief What the network layer holds for one connection of a server. */
typedef struct {
    /** @brief The socket, or MQTT_SOCKET_NONE if the entry is not in use. */
    int32_t handle;
    /** @brief Its handle in the tier set, or MQTT_TIER_NONE. */
    uint32_t wait;
    /** @brief The TLS session, if there is one. */
    uint32_t tls_session;
    /** @brief How the bytes are carried. */
    mqtt_net_kind kind;
    /** @brief The socket's write readiness is being waited for. */
    bool want_write;
    /** @brief The socket's read readiness is being waited for. */
    bool want_read;
    /** @brief The WebSocket opening handshake is over. */
    bool websocket_open;
    /**
     * @brief A WebSocket data frame without FIN has come, so the next data
     * frame must continue its message.
     */
    bool websocket_fragmented;
    /**
     * @brief Number of bytes in the partial data of the WebSocket opening or of
     * a frame.
     */
    uint32_t websocket_used;
    /**
     * @brief Number of bytes still to come in the WebSocket frame being read.
     */
    uint64_t websocket_left;
    /** @brief The mask of the WebSocket frame being read. */
    uint8_t websocket_mask[4];
    /** @brief The position in the mask. */
    uint32_t websocket_mask_at;
    /**
     * @brief Bytes of the WebSocket opening or of a frame's header, kept until
     * they are whole.
     */
    uint8_t websocket_buffer[1024];
    /**
     * @brief The rest of a frame header to send, when only a part of it could
     * go.
     */
    uint8_t websocket_header[10];
    /** @brief Number of bytes in that frame header. */
    uint32_t websocket_header_size;
    /** @brief Number of bytes of the frame header still to send. */
    uint32_t websocket_header_left;
    /** @brief Number of bytes of the frame's payload still to send. */
    uint32_t websocket_payload_left;
    /** @brief Number of bytes still to come in the control frame being read. */
    uint32_t websocket_control_left;
    /** @brief The opcode of the control frame being read. */
    uint8_t websocket_control_opcode;
    /** @brief A Pong frame waiting for the frame being sent to end. */
    uint8_t websocket_pong[MQTT_NET_PONG_BYTES];
    /** @brief Number of bytes in that Pong frame. */
    uint32_t websocket_pong_size;
    /** @brief Number of bytes of that Pong frame still to send. */
    uint32_t websocket_pong_left;
    /**
     * @brief TLS holds bytes it has read, and there was no room to be given
     * them.
     */
    bool tls_stranded;
    /**
     * @brief true while the entry is on the list of those to read again. It
     * stays set when the connection closes, until the list is next taken.
     */
    bool again_listed;
    /** @brief The next entry on that list. */
    uint32_t again_next;
} mqtt_net_connection;

/** @brief Counts kept by a running server's network layer. */
typedef struct {
    /** @brief The tiers' counts. */
    mqtt_tier_counters tier;
    /** @brief Number of times bytes were found waiting to be sent. */
    uint64_t batches;
    /** @brief Number of calls that wrote to a socket or to TLS. */
    uint64_t write_calls;
} mqtt_net_counters;

/**
 * @brief Where a server listens, and with what. Each port may be NULL to leave
 * it out.
 */
typedef struct {
    /** @brief The local address to bind, or NULL for all of them. */
    const char *host;
    /** @brief The port for connections directly over TCP. */
    const char *port;
    /** @brief The port for connections over TLS. */
    const char *tls_port;
    /** @brief The port for connections over WebSocket. */
    const char *websocket_port;
    /** @brief The file with the Server's certificate chain, in PEM form. */
    const char *certificate_file;
    /** @brief The file with the Server's private key, in PEM form. */
    const char *key_file;
    /** @brief The password of that key if it is encrypted, or NULL. */
    const char *key_password;
    /**
     * @brief A file of certificates, to require a client certificate that
     * chains to them.
     */
    const char *ca_file;
    /** @brief How the tiers behave, or NULL for the defaults. */
    const mqtt_tier_config *tier;
    /**
     * @brief Bytes of the system's send buffer for each connection accepted,
     * or zero to leave it to the system. A size that is set also keeps the
     * system from growing it.
     */
    uint32_t send_buffer;
    /**
     * @brief The store the server keeps its Session state in, opened with
     * mqtt_platform_store_open, or MQTT_STORE_NONE to keep none.
     */
    int32_t store;
} mqtt_net_server_options;

/** @brief What TLS a client should use, or nothing at all. */
typedef struct {
    /** @brief true to use TLS. */
    bool enabled;
    /**
     * @brief true to not check the Server's certificate. With a ca_file, it is
     * checked anyway.
     */
    bool insecure;
    /**
     * @brief The file of certificates to trust, which is needed unless insecure
     * is set.
     */
    const char *ca_file;
    /**
     * @brief The name to expect in the Server's certificate, or NULL for the
     * host's name.
     */
    const char *server_name;
    /** @brief The client's certificate chain, or NULL. */
    const char *certificate_file;
    /** @brief The client's private key, or NULL. */
    const char *key_file;
    /** @brief The password of that key if it is encrypted, or NULL. */
    const char *key_password;
} mqtt_net_client_tls;

/** @brief A client with a network connection. */
typedef struct {
    /** @brief The client. */
    mqtt_client *client;
    /** @brief The socket, or MQTT_SOCKET_NONE. */
    int32_t handle;
    /** @brief The TLS session, if there is one. */
    uint32_t tls_session;
    /** @brief true if the connection uses TLS. */
    bool tls;
    /**
     * @brief true if the socket could not take what the last step had for it.
     */
    bool write_blocked;
} mqtt_net_client;

/**
 * @brief Run a server until the program is asked to stop.
 * @param[in,out] server A server that has been set up.
 * @param[in] connections One entry for each connection of the server.
 * @param[in] options Where to listen.
 * @return MQTT_OK after a stop, or why it could not start.
 */
mqtt_status mqtt_net_server_run(mqtt_server *server,
                                mqtt_net_connection *connections,
                                const mqtt_net_server_options *options);

/**
 * @brief Set up a server's configuration to keep its Session state in a store.
 *
 * Records go to the store with mqtt_platform_store_append, which is given the
 * store as its tag, so every callback of the server is then given the store
 * as its user_tag. Sessions and messages are kept to outlast a loss of power,
 * which a program may lower to MQTT_STORE_WRITTEN after this call. A snapshot
 * is due after half of a place of the store at most, so that a journal
 * segment never fills its place.
 *
 * @param[in,out] config The configuration, before mqtt_server_init.
 * @param[in] store The store, opened with mqtt_platform_store_open.
 */
void mqtt_net_server_store_config(mqtt_server_config *config, int32_t store);

/**
 * @brief Put back what a store holds into a server just set up, before
 * mqtt_net_server_run.
 *
 * A store that is damaged, from a later version, or more than this server can
 * take, is set aside untouched with mqtt_platform_store_set_aside, a line on
 * the console says why and where it went, and the server starts empty. Nothing
 * of it is restored by halves.
 *
 * @param[in,out] server The server, set up with mqtt_net_server_store_config's
 * configuration and holding nothing.
 * @param[in] store The store.
 * @param[out] map Room for the table of store ids: the server storage's
 * message_count and delivery_count entries always suffice.
 * @param[in] map_count Number of entries in map.
 * @param[out] fault What was wrong, or MQTT_STORE_FAULT_NONE.
 * @return MQTT_OK if the server can run, restored or empty;
 * MQTT_INVALID_ARGUMENT or MQTT_INVALID_STATE as mqtt_server_restore gives
 * them; or MQTT_INVALID_STATE if a damaged store could not be set aside, when
 * the server must not run on it.
 */
mqtt_status mqtt_net_server_restore(mqtt_server *server, int32_t store,
                                    mqtt_server_restore_entry *map,
                                    uint32_t map_count,
                                    mqtt_store_fault *fault);

/**
 * @brief Set up a client's store configuration to keep its Session State in a
 * store, for mqtt_client_store_init.
 *
 * Records go to the store with mqtt_platform_store_append. Sessions and
 * messages are kept to outlast a loss of power, which a program may lower to
 * MQTT_STORE_WRITTEN after this call.
 *
 * @param[out] config The configuration.
 * @param[in] store The store, opened with mqtt_platform_store_open.
 */
void mqtt_net_client_store_config(mqtt_client_store_config *config,
                                  int32_t store);

/**
 * @brief Put back what a store holds into a client just given it with
 * mqtt_client_store_init, and take the first snapshot, before the first
 * connection.
 *
 * A store that is damaged, from a later version, or more than this client
 * can take, is set aside untouched, a line on the console says why, and the
 * client starts empty. From then on, each mqtt_net_client_step of this client
 * closes and syncs its records, and lets go of the answers that waited for
 * them. A program runs one client with a store at a time.
 *
 * @param[in,out] client The client.
 * @param[in] store The store.
 * @param[out] fault What was wrong, or MQTT_STORE_FAULT_NONE.
 * @return MQTT_OK if the client can connect, restored or empty;
 * MQTT_INVALID_ARGUMENT or MQTT_INVALID_STATE as mqtt_client_restore gives
 * them; MQTT_INVALID_STATE if a damaged store could not be set aside; or why
 * the first snapshot couldn't be written.
 */
mqtt_status mqtt_net_client_restore(mqtt_client *client, int32_t store,
                                    mqtt_store_fault *fault);

/**
 * @brief Read the counts of the server that runs, or ran last.
 * @param[out] counters The counts.
 */
void mqtt_net_server_counters(mqtt_net_counters *counters);

/**
 * @brief Open a client's network connection, with TLS if it is asked for.
 * @param[out] network The state to set up.
 * @param[in] client The client, which stays offline until mqtt_client_connect
 * is called.
 * @param[in] host The host to connect to.
 * @param[in] port The port.
 * @param[in] tls What TLS to use.
 * @return MQTT_OK, MQTT_CONNECTION_CLOSED, or MQTT_TLS_ERROR.
 */
mqtt_status mqtt_net_client_open(mqtt_net_client *network, mqtt_client *client,
                                 const char *host, const char *port,
                                 const mqtt_net_client_tls *tls);

/**
 * @brief Move what can be moved, waiting for up to the time given if there is
 * nothing.
 *
 * Events arrive through the client's callback, during this call.
 *
 * @param[in,out] network The client's network connection.
 * @param[in] wait_ms How long to wait, in milliseconds.
 * @return MQTT_OK, or MQTT_CONNECTION_CLOSED once the connection has gone.
 */
mqtt_status mqtt_net_client_step(mqtt_net_client *network, uint32_t wait_ms);

/**
 * @brief Close a client's network connection.
 * @param[in,out] network The client's network connection.
 */
void mqtt_net_client_close(mqtt_net_client *network);

#endif /* MQTT_BASE_NET_H */
