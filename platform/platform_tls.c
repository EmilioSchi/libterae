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
 * @file platform_tls.c
 * @brief TLS for the network layer, through Mbed TLS.
 *
 * Bytes go to and from the socket through two functions this file gives Mbed
 * TLS, in place of the ones that would have it own the socket. They read and
 * write with base_platform.h, so this file is the same on every system. Memory
 * comes from a static pool: Mbed TLS keeps two 16 KiB buffers for each session
 * while it is open, and more during the handshake.
 *
 * In both versions, the client answers the server's first messages with its
 * own, ending with Finished ([RFC5246] section 7.3, [RFC9846] section 2). A
 * client that has stopped sending before the handshake is over can never
 * finish it, so a server's session ends as soon as it finds that out, before
 * Mbed TLS makes and signs an answer no one will read. Each read during the
 * handshake that gives Mbed TLS all it asked for reads one byte ahead to find
 * out, and keeps a byte that is there for the next read. A connection that
 * sends a ClientHello and goes then costs a server that has fallen behind no
 * signature, so it catches up with such connections instead of signing for
 * each of them.
 *
 * [RFC5246] The Transport Layer Security (TLS) Protocol Version 1.2.
 * @li Source: www.rfc-editor.org/rfc/rfc5246
 *
 * [RFC9846] The Transport Layer Security (TLS) Protocol Version 1.3.
 * @li Source: www.rfc-editor.org/rfc/rfc9846
 */

#include <stddef.h>

#include "base_net.h"
#include "base_platform.h"
#include "base_tls.h"

#if defined(MQTT_WITH_TLS)

#include <string.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/memory_buffer_alloc.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <psa/crypto.h>
#include <mbedtls/x509_crt.h>

/**
 * @brief The most bytes one read or write of the socket is asked for: more than
 * a TLS record, and few enough for the count to fit what Mbed TLS takes back.
 */
#define TLS_SOCKET_MOST 0x10000U

#if !defined(MQTT_TLS_POOL_BYTES)
/**
 * @brief Number of bytes in the pool Mbed TLS takes its memory from, unless the
 * build sets another: 64 MiB, which the default MQTT_NET_TLS_SESSIONS need.
 */
#define MQTT_TLS_POOL_BYTES (64UL * 1024UL * 1024UL)
#endif

/** @brief One TLS session. */
typedef struct {
    /** @brief The Mbed TLS context. */
    mbedtls_ssl_context context;
    /** @brief The socket. */
    int32_t handle;
    /**
     * @brief Number of bytes of a write Mbed TLS has to be asked for again, or
     * zero.
     */
    uint32_t unfinished;
    /** @brief true if the entry is in use. */
    bool in_use;
    /** @brief true if a server opened it. */
    bool is_server;
    /**
     * @brief true if ahead holds a byte read from the socket that Mbed TLS
     * hasn't been given yet.
     */
    bool has_ahead;
    /** @brief The byte read ahead. */
    uint8_t ahead;
    /**
     * @brief true if the last call could not go on until the socket was
     * writable.
     */
    bool wants_write;
    /**
     * @brief true if the last call could not go on until the socket was
     * readable.
     */
    bool wants_read;
} tls_session;

/** @brief What the server's sessions or the client's sessions all share. */
typedef struct {
    /** @brief The settings. */
    mbedtls_ssl_config config;
    /** @brief The certificate chain this side presents. */
    mbedtls_x509_crt certificate;
    /** @brief The certificates to trust. */
    mbedtls_x509_crt authorities;
    /** @brief The private key of this side. */
    mbedtls_pk_context key;
    /** @brief true once it is set up. */
    bool ready;
} tls_side;

/** @brief The sessions. */
static tls_session tls_sessions[MQTT_NET_TLS_SESSIONS];
/** @brief The random number generator. */
static mbedtls_ctr_drbg_context tls_random;
/** @brief What a server's sessions share. */
static tls_side tls_server_side;
/** @brief What a client's sessions share. */
static tls_side tls_client_side;

/**
 * @brief Set up what all sessions share.
 * @return true if that worked.
 */
static bool tls_start(void)
{
    /** @brief The entropy source. */
    static mbedtls_entropy_context tls_entropy;
    /** @brief true once the things shared by all sessions are set up. */
    static bool tls_started = false;
    /** @brief The answer. */
    bool ready = tls_started;

    if (!ready) {
        /** @brief The memory Mbed TLS uses. */
        static unsigned char tls_pool[MQTT_TLS_POOL_BYTES];

        mbedtls_memory_buffer_alloc_init(tls_pool, sizeof(tls_pool));
        mbedtls_entropy_init(&tls_entropy);
        mbedtls_ctr_drbg_init(&tls_random);
        ready = (psa_crypto_init() == PSA_SUCCESS) &&
                (mbedtls_ctr_drbg_seed(&tls_random, mbedtls_entropy_func,
                                       &tls_entropy, NULL, 0U) == 0);
        tls_started = ready;
    }
    return ready;
}

/**
 * @brief Find out whether the client of a server's session whose handshake
 * isn't over has stopped sending, by reading one byte ahead. A byte that is
 * there is kept for the next read.
 * @param[in,out] session The session, whose last read gave Mbed TLS all it
 * asked for.
 * @return true if the client has stopped sending, so that the handshake can
 * never be over.
 */
static bool tls_client_gone(tls_session *session)
{
    /** @brief The answer. */
    bool gone = false;
    /** @brief true if a byte read ahead can tell. */
    bool asking = session->is_server && !session->has_ahead;

    if (asking) {
        asking = (mbedtls_ssl_is_handshake_over(&session->context) == 0);
    }
    if (asking) {
        /** @brief Number of bytes read. */
        uint32_t count = 0U;
        /** @brief The outcome. */
        const mqtt_status status =
            mqtt_platform_read(session->handle, &session->ahead, 1U, &count);

        if (status == MQTT_OK) {
            session->has_ahead = (count == 1U);
        } else {
            gone = (status != MQTT_INCOMPLETE);
        }
    }
    return gone;
}

/**
 * @brief Give Mbed TLS the bytes it wants to read from the socket, starting
 * with the byte read ahead, if there is one.
 * @param[in] context The session.
 * @param[out] data Where to put the bytes.
 * @param[in] length Number of bytes wanted.
 * @return Number of bytes, or an Mbed TLS error code.
 */
static int tls_socket_receive(void *context, unsigned char *data, size_t length)
{
    /** @brief The session. */
    tls_session *session = context;
    /** @brief Number of bytes to ask for. */
    const uint32_t wanted = (length < (size_t)TLS_SOCKET_MOST) ?
                                (uint32_t)length :
                                TLS_SOCKET_MOST;
    /** @brief Number of bytes given from the one read ahead. */
    uint32_t given = 0U;
    /** @brief Number of bytes read. */
    uint32_t count = 0U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /**
     * @brief The value to hand back. A connection that has gone, by an end of
     * file or a failure, ends the session in the same way.
     */
    int answer = MBEDTLS_ERR_SSL_CONN_EOF;

    if (session->has_ahead && (wanted > 0U)) {
        data[0] = session->ahead;
        session->has_ahead = false;
        given = 1U;
    }
    if (given < wanted) {
        status = mqtt_platform_read(session->handle, &data[given],
                                    wanted - given, &count);
        if (status != MQTT_OK) {
            count = 0U;
        }
    }
    if (given > 0U) {
        /* The byte read ahead goes to Mbed TLS whatever the socket says now,
         * and the next read finds that out again. */
        status = MQTT_OK;
        count += given;
    }
    if (status == MQTT_OK) {
        /** @brief true if the client can never finish the handshake. */
        bool gone = false;

        if ((count > 0U) && (count == wanted)) {
            gone = tls_client_gone(session);
        }
        answer = gone ? MBEDTLS_ERR_SSL_CONN_EOF : (int)count;
    } else if (status == MQTT_INCOMPLETE) {
        answer = MBEDTLS_ERR_SSL_WANT_READ;
    } else {
        /* The connection has gone. */
    }
    return answer;
}

/**
 * @brief Take the bytes Mbed TLS wants to write to the socket.
 * @param[in] context The session.
 * @param[in] data The bytes.
 * @param[in] length Number of bytes.
 * @return Number of bytes taken, or an Mbed TLS error code.
 */
static int tls_socket_send(void *context, const unsigned char *data,
                           size_t length)
{
    /** @brief The session. */
    const tls_session *session = context;
    /** @brief Number of bytes to offer. */
    const uint32_t offered = (length < (size_t)TLS_SOCKET_MOST) ?
                                 (uint32_t)length :
                                 TLS_SOCKET_MOST;
    /** @brief Number of bytes taken. */
    uint32_t count = 0U;
    /** @brief The outcome. */
    const mqtt_status status =
        mqtt_platform_write(session->handle, data, offered, &count);
    /** @brief The value to hand back. */
    int answer = MBEDTLS_ERR_SSL_INTERNAL_ERROR;

    if (status == MQTT_OK) {
        answer = (int)count;
    } else if (status == MQTT_INCOMPLETE) {
        answer = MBEDTLS_ERR_SSL_WANT_WRITE;
    } else {
        /* The connection has gone. */
    }
    return answer;
}

/**
 * @brief Give up what a side loaded, so that it can be set up again.
 * @param[in,out] side The side.
 */
static void tls_side_free(tls_side *side)
{
    mbedtls_ssl_config_free(&side->config);
    mbedtls_x509_crt_free(&side->certificate);
    mbedtls_x509_crt_free(&side->authorities);
    mbedtls_pk_free(&side->key);
    side->ready = false;
}

/**
 * @brief Load what one side needs, the first time it is asked for.
 *
 * Sessions keep pointing at what is loaded here, so a later call changes
 * nothing, and a client that connects again does not load its files again.
 *
 * @param[in,out] side The side.
 * @param[in] endpoint MBEDTLS_SSL_IS_SERVER or MBEDTLS_SSL_IS_CLIENT.
 * @param[in] ca_file The certificates to trust, or NULL.
 * @param[in] certificate_file The certificate chain, or NULL.
 * @param[in] key_file The private key, or NULL.
 * @param[in] key_password The password of that key if it is encrypted, or NULL.
 * @param[in] verify_mode How to treat the peer's certificate when there is no
 * ca_file.
 * @return MQTT_OK or MQTT_TLS_ERROR.
 */
static mqtt_status tls_side_setup(tls_side *side, int endpoint,
                                  const char *ca_file,
                                  const char *certificate_file,
                                  const char *key_file,
                                  const char *key_password, int verify_mode)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if ((!side->ready) && tls_start()) {
        /** @brief true if it has its own certificate to present. */
        const bool identity = (certificate_file != NULL) && (key_file != NULL);

        mbedtls_ssl_config_init(&side->config);
        mbedtls_x509_crt_init(&side->certificate);
        mbedtls_x509_crt_init(&side->authorities);
        mbedtls_pk_init(&side->key);
        if (mbedtls_ssl_config_defaults(&side->config, endpoint,
                                        MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
            status = MQTT_TLS_ERROR;
        }
        mbedtls_ssl_conf_rng(&side->config, mbedtls_ctr_drbg_random,
                             &tls_random);
        if ((status == MQTT_OK) && identity &&
            ((mbedtls_x509_crt_parse_file(&side->certificate,
                                          certificate_file) != 0) ||
             (mbedtls_pk_parse_keyfile(&side->key, key_file, key_password,
                                       mbedtls_ctr_drbg_random,
                                       &tls_random) != 0) ||
             (mbedtls_ssl_conf_own_cert(&side->config, &side->certificate,
                                        &side->key) != 0))) {
            status = MQTT_TLS_ERROR;
        }
        if ((status == MQTT_OK) && (ca_file != NULL)) {
            if (mbedtls_x509_crt_parse_file(&side->authorities, ca_file) == 0) {
                mbedtls_ssl_conf_ca_chain(&side->config, &side->authorities,
                                          NULL);
                mbedtls_ssl_conf_authmode(&side->config,
                                          MBEDTLS_SSL_VERIFY_REQUIRED);
            } else {
                status = MQTT_TLS_ERROR;
            }
        } else if (status == MQTT_OK) {
            mbedtls_ssl_conf_authmode(&side->config, verify_mode);
        } else {
            /* There is nothing more to set up. */
        }
        if (status == MQTT_OK) {
            side->ready = true;
        } else {
            tls_side_free(side);
        }
    } else if (!side->ready) {
        status = MQTT_TLS_ERROR;
    } else {
        /* It was set up by an earlier call. */
    }
    return status;
}

bool mqtt_tls_available(void)
{
    return true;
}

mqtt_status mqtt_tls_server_setup(const char *certificate_file,
                                  const char *key_file,
                                  const char *key_password, const char *ca_file)
{
    /**
     * @brief The outcome: a server has nothing to offer without its
     * certificate.
     */
    mqtt_status status = MQTT_TLS_ERROR;

    if ((certificate_file != NULL) && (key_file != NULL)) {
        status = tls_side_setup(&tls_server_side, MBEDTLS_SSL_IS_SERVER,
                                ca_file, certificate_file, key_file,
                                key_password, MBEDTLS_SSL_VERIFY_NONE);
    }
    return status;
}

mqtt_status mqtt_tls_client_setup(const char *ca_file,
                                  const char *certificate_file,
                                  const char *key_file,
                                  const char *key_password, bool insecure)
{
    /**
     * @brief The outcome: without certificates to trust, it takes being asked
     * to not check.
     */
    mqtt_status status = MQTT_TLS_ERROR;

    if ((ca_file != NULL) || insecure) {
        status = tls_side_setup(&tls_client_side, MBEDTLS_SSL_IS_CLIENT,
                                ca_file, certificate_file, key_file,
                                key_password, MBEDTLS_SSL_VERIFY_NONE);
    }
    return status;
}

uint32_t mqtt_tls_session_open(int32_t handle, bool is_server,
                               const char *server_name)
{
    /** @brief Index of the entry being looked at. */
    uint32_t index = 0U;
    /** @brief The session found. */
    uint32_t found = MQTT_TLS_NONE;

    while ((found == MQTT_TLS_NONE) && (index < MQTT_NET_TLS_SESSIONS)) {
        if (!tls_sessions[index].in_use) {
            found = index;
        }
        index += 1U;
    }
    if (found != MQTT_TLS_NONE) {
        /** @brief The session. */
        tls_session *session = &tls_sessions[found];

        mbedtls_ssl_init(&session->context);
        if (mbedtls_ssl_setup(&session->context,
                              is_server ? &tls_server_side.config :
                                          &tls_client_side.config) != 0) {
            mbedtls_ssl_free(&session->context);
            found = MQTT_TLS_NONE;
        } else {
            if ((server_name != NULL) &&
                (mbedtls_ssl_set_hostname(&session->context, server_name) !=
                 0)) {
                mbedtls_ssl_free(&session->context);
                found = MQTT_TLS_NONE;
            } else {
                session->handle = handle;
                session->in_use = true;
                session->is_server = is_server;
                session->has_ahead = false;
                session->ahead = 0U;
                session->wants_write = false;
                session->wants_read = false;
                session->unfinished = 0U;
                mbedtls_ssl_set_bio(&session->context, session, tls_socket_send,
                                    tls_socket_receive, NULL);
            }
        }
    }
    return found;
}

/**
 * @brief Find what a failure of Mbed TLS means here.
 * @param[in] session The session.
 * @param[in] result The Mbed TLS result, which is negative.
 * @return MQTT_INCOMPLETE if it can go on later, or MQTT_CONNECTION_CLOSED.
 */
static mqtt_status tls_failure(tls_session *session, int result)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_CONNECTION_CLOSED;

    if ((result == MBEDTLS_ERR_SSL_WANT_READ) ||
        (result == MBEDTLS_ERR_SSL_WANT_WRITE) ||
        (result == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)) {
        session->wants_write = (result == MBEDTLS_ERR_SSL_WANT_WRITE);
        session->wants_read = (result == MBEDTLS_ERR_SSL_WANT_READ);
        status = MQTT_INCOMPLETE;
    }
    return status;
}

mqtt_status mqtt_tls_read(uint32_t session, uint8_t *data, uint32_t capacity,
                          uint32_t *count)
{
    /** @brief The session. */
    tls_session *entry = &tls_sessions[session];
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief The result. */
    const int result =
        mbedtls_ssl_read(&entry->context, data, (size_t)capacity);

    entry->wants_write = false;
    entry->wants_read = false;
    if (result > 0) {
        *count = (uint32_t)result;
    } else if (result == 0) {
        status = MQTT_CONNECTION_CLOSED;
    } else {
        status = tls_failure(entry, result);
    }
    return status;
}

mqtt_status mqtt_tls_write(uint32_t session, const uint8_t *data,
                           uint32_t length, uint32_t *count)
{
    /** @brief The session. */
    tls_session *entry = &tls_sessions[session];
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /* After a write that could not finish, Mbed TLS has to be called with the
     * same length. It only sends what it already holds, and then says all of
     * that length has gone, so a longer view of the same bytes would have the
     * caller drop what was never sent. */
    /** @brief Number of bytes to ask it to write. */
    const uint32_t size =
        (entry->unfinished != 0U) ? entry->unfinished : length;
    /** @brief The result. */
    const int result = mbedtls_ssl_write(&entry->context, data, (size_t)size);

    entry->wants_write = false;
    entry->wants_read = false;
    entry->unfinished = 0U;
    if (result >= 0) {
        *count = (uint32_t)result;
    } else {
        status = tls_failure(entry, result);
        if (result == MBEDTLS_ERR_SSL_WANT_WRITE) {
            entry->unfinished = size;
        }
    }
    return status;
}

bool mqtt_tls_wants_write(uint32_t session)
{
    return tls_sessions[session].wants_write;
}

bool mqtt_tls_wants_read(uint32_t session)
{
    return tls_sessions[session].wants_read;
}

bool mqtt_tls_has_pending(uint32_t session)
{
    return mbedtls_ssl_get_bytes_avail(&tls_sessions[session].context) > 0U;
}

void mqtt_tls_session_close(uint32_t session)
{
    if (session != MQTT_TLS_NONE) {
        /** @brief The session. */
        tls_session *entry = &tls_sessions[session];

        (void)mbedtls_ssl_close_notify(&entry->context);
        mbedtls_ssl_free(&entry->context);
        entry->in_use = false;
    }
}

#else

bool mqtt_tls_available(void)
{
    return false;
}

mqtt_status mqtt_tls_server_setup(const char *certificate_file,
                                  const char *key_file,
                                  const char *key_password, const char *ca_file)
{
    (void)certificate_file;
    (void)key_file;
    (void)key_password;
    (void)ca_file;
    return MQTT_TLS_ERROR;
}

mqtt_status mqtt_tls_client_setup(const char *ca_file,
                                  const char *certificate_file,
                                  const char *key_file,
                                  const char *key_password, bool insecure)
{
    (void)ca_file;
    (void)certificate_file;
    (void)key_file;
    (void)key_password;
    (void)insecure;
    return MQTT_TLS_ERROR;
}

uint32_t mqtt_tls_session_open(int32_t handle, bool is_server,
                               const char *server_name)
{
    (void)handle;
    (void)is_server;
    (void)server_name;
    return MQTT_TLS_NONE;
}

mqtt_status mqtt_tls_read(uint32_t session, uint8_t *data, uint32_t capacity,
                          uint32_t *count)
{
    (void)session;
    (void)data;
    (void)capacity;
    (void)count;
    return MQTT_TLS_ERROR;
}

mqtt_status mqtt_tls_write(uint32_t session, const uint8_t *data,
                           uint32_t length, uint32_t *count)
{
    (void)session;
    (void)data;
    (void)length;
    (void)count;
    return MQTT_TLS_ERROR;
}

bool mqtt_tls_wants_write(uint32_t session)
{
    (void)session;
    return false;
}

bool mqtt_tls_wants_read(uint32_t session)
{
    (void)session;
    return false;
}

bool mqtt_tls_has_pending(uint32_t session)
{
    (void)session;
    return false;
}

void mqtt_tls_session_close(uint32_t session)
{
    (void)session;
}

#endif /* MQTT_WITH_TLS */
