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
 * @file base_tls.h
 * @brief TLS for the network layer, through Mbed TLS.
 *
 * The implementation is in platform/platform_tls.c. Built without
 * MQTT_WITH_TLS, each of these functions reports MQTT_TLS_ERROR.
 *
 * All memory Mbed TLS uses comes from a static pool in that file, so TLS adds
 * no calls to the system allocator either. The MQTT 3.1.1 and 5.0
 * specifications expect TLS to be the means of protecting data on the network,
 * and both recommend certificates: section 5.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 4.2.1, 5.4.5, 5.4.6, p.93, 113.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */
#ifndef MQTT_BASE_TLS_H
#define MQTT_BASE_TLS_H

#include <stdbool.h>
#include <stdint.h>

#include "base_status.h"

/** @brief The value of a TLS session that does not exist. */
#define MQTT_TLS_NONE 0xFFFFFFFFU

/**
 * @brief Find out whether the library was built with TLS.
 * @return true if it was.
 */
bool mqtt_tls_available(void);

/**
 * @brief Load what a server needs for TLS.
 * @param[in] certificate_file The Server's certificate chain, in PEM form.
 * @param[in] key_file The Server's private key, in PEM form.
 * @param[in] key_password The password of that key if it is encrypted, or NULL.
 * @param[in] ca_file The certificates to check client certificates against, or
 * NULL for none.
 * @return MQTT_OK or MQTT_TLS_ERROR.
 */
mqtt_status mqtt_tls_server_setup(const char *certificate_file,
                                  const char *key_file,
                                  const char *key_password,
                                  const char *ca_file);

/**
 * @brief Load what a client needs for TLS.
 *
 * This is loaded once in a program. A later call keeps what the first one
 * loaded.
 *
 * @param[in] ca_file The certificates to trust, or NULL if insecure is set.
 * @param[in] certificate_file The client's certificate chain, or NULL.
 * @param[in] key_file The client's private key, or NULL.
 * @param[in] key_password The password of that key if it is encrypted, or NULL.
 * @param[in] insecure true to not check the Server's certificate when ca_file
 * is NULL, which lets anyone between the two read and change everything.
 * @return MQTT_OK or MQTT_TLS_ERROR.
 */
mqtt_status mqtt_tls_client_setup(const char *ca_file,
                                  const char *certificate_file,
                                  const char *key_file,
                                  const char *key_password, bool insecure);

/**
 * @brief Start a TLS session on a connected socket.
 * @param[in] handle The socket.
 * @param[in] is_server true for the server's side of the session.
 * @param[in] server_name The name to expect in the Server's certificate, or
 * NULL.
 * @return The session, or MQTT_TLS_NONE if there is no room for another.
 */
uint32_t mqtt_tls_session_open(int32_t handle, bool is_server,
                               const char *server_name);

/**
 * @brief Read from a TLS session.
 * @param[in] session The session.
 * @param[out] data Where to put the bytes.
 * @param[in] capacity Number of bytes data can hold.
 * @param[out] count Number of bytes read, on MQTT_OK.
 * @return MQTT_OK, MQTT_INCOMPLETE if nothing is ready, or
 * MQTT_CONNECTION_CLOSED.
 */
mqtt_status mqtt_tls_read(uint32_t session, uint8_t *data, uint32_t capacity,
                          uint32_t *count);

/**
 * @brief Write to a TLS session.
 * @param[in] session The session.
 * @param[in] data The bytes.
 * @param[in] length Number of bytes in data.
 * @param[out] count Number of bytes taken, on MQTT_OK.
 * @return MQTT_OK, MQTT_INCOMPLETE if nothing could be taken, or
 * MQTT_CONNECTION_CLOSED.
 */
mqtt_status mqtt_tls_write(uint32_t session, const uint8_t *data,
                           uint32_t length, uint32_t *count);

/**
 * @brief Find out whether a TLS session needs the socket to be writable to go
 * on.
 * @param[in] session The session.
 * @return true if it does.
 */
bool mqtt_tls_wants_write(uint32_t session);

/**
 * @brief Find out whether the last call could not go on until the socket was
 * readable.
 * @param[in] session The session.
 * @return true if it does.
 */
bool mqtt_tls_wants_read(uint32_t session);

/**
 * @brief Find out whether a session holds bytes that have already been read
 * from the socket.
 * @param[in] session The session.
 * @return true if it does.
 */
bool mqtt_tls_has_pending(uint32_t session);

/**
 * @brief End a TLS session, and free it.
 * @param[in] session The session, or MQTT_TLS_NONE to do nothing.
 */
void mqtt_tls_session_close(uint32_t session);

#endif /* MQTT_BASE_TLS_H */
