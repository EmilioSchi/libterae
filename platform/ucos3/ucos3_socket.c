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
 * @file ucos3_socket.c
 * @brief Sockets, through uC/TCP-IP's own socket interface.
 *
 * Every socket is made not to block once it is open, and only a connect
 * waits. A connected socket also has the Nagle algorithm off, as the other
 * systems' sockets have TCP_NODELAY, so that a small packet such as an
 * acknowledgement goes at once. A socket's number is its handle. Hosts are
 * numeric IPv4 addresses, and a listener given NULL takes every address.
 * uC/TCP-IP moves at most 65535 bytes in one call, and reads into one place
 * and writes one run at a time, so the reads into several places and the
 * writes of several runs are loops that stop at the first that falls short.
 */

#include <stddef.h>

#include <net_app.h>
#include <net_ascii.h>
#include <net_err.h>
#include <net_sock.h>

#include "base_platform.h"
#include "ucos3_internal.h"

/** @brief The most bytes one call moves. */
#define UCOS3_MOST 0xFFFFU
/** @brief Number of connections a listener holds before they are accepted. */
#define UCOS3_BACKLOG 8U
/** @brief The highest port number. */
#define UCOS3_PORT_MOST 65535U
/** @brief The first port socket pairs listen on, one after the other. */
#define UCOS3_PAIR_PORT 40000U

/**
 * @brief Read a host and port into an address.
 * @param[in] host A numeric IPv4 address, or NULL for every address.
 * @param[in] port The port, as text.
 * @param[out] address The address.
 * @return true if the port and host are valid.
 */
static bool ucos3_address(const char *host, const char *port,
                          NET_SOCK_ADDR *address)
{
    /** @brief The port's number. */
    uint32_t number = 0U;
    /**
     * @brief The host's address, in the host's byte order, which is how
     * NetApp_SetSockAddr takes it.
     */
    NET_IPv4_ADDR ip = NET_IPv4_ADDR_ANY;
    /** @brief What the conversions said. */
    NET_ERR error = NET_ERR_NONE;
    /** @brief The answer. */
    bool valid = mqtt_platform_parse_u32(port, &number) &&
                 (number <= UCOS3_PORT_MOST);

    if (valid && (host != NULL)) {
        ip = NetASCII_Str_to_IPv4((CPU_CHAR *)host, &error);
        valid = (error == NET_ASCII_ERR_NONE);
    }
    if (valid) {
        NetApp_SetSockAddr(address, NET_SOCK_ADDR_FAMILY_IP_V4,
                           (NET_PORT_NBR)number, (CPU_INT08U *)&ip,
                           NET_IPv4_ADDR_SIZE, &error);
        valid = (error == NET_APP_ERR_NONE);
    }
    return valid;
}

/**
 * @brief Open a TCP socket.
 * @return Its number, or MQTT_SOCKET_NONE.
 */
static int32_t ucos3_open(void)
{
    /** @brief What the call said. */
    NET_ERR error = NET_SOCK_ERR_NONE;
    /** @brief The socket. */
    const NET_SOCK_ID socket = NetSock_Open(NET_SOCK_PROTOCOL_FAMILY_IP_V4,
                                            NET_SOCK_TYPE_STREAM,
                                            NET_SOCK_PROTOCOL_TCP, &error);

    return ((error == NET_SOCK_ERR_NONE) && (socket >= 0)) ? (int32_t)socket :
                                                             MQTT_SOCKET_NONE;
}

/**
 * @brief Make a socket not block.
 * @param[in] handle The socket.
 */
static void ucos3_no_block(int32_t handle)
{
    /** @brief What the call said. */
    NET_ERR error = NET_SOCK_ERR_NONE;

    (void)NetSock_CfgBlock((NET_SOCK_ID)handle, NET_SOCK_BLOCK_SEL_NO_BLOCK,
                           &error);
}

/**
 * @brief Make a connected socket not block, and send what it is given at
 * once: with the Nagle algorithm, uC/TCP-IP holds a small segment back while
 * an earlier one is unacknowledged, which lasts as long as the peer delays
 * its acknowledgement.
 * @param[in] handle The socket.
 */
static void ucos3_connected(int32_t handle)
{
    /**
     * @brief The algorithm's state: uC/TCP-IP's no-delay option takes whether
     * the algorithm is on, not whether delay is off.
     */
    const CPU_BOOLEAN nagle = DEF_DISABLED;
    /** @brief What the call said. */
    NET_ERR error = NET_SOCK_ERR_NONE;

    ucos3_no_block(handle);
    (void)NetSock_OptSet((NET_SOCK_ID)handle, NET_SOCK_PROTOCOL_TCP,
                         NET_SOCK_OPT_TCP_NO_DELAY, &nagle,
                         (NET_SOCK_OPT_LEN)sizeof(nagle), &error);
}

mqtt_status mqtt_platform_listen(const char *host, const char *port,
                                 int32_t *listener)
{
    /** @brief The address. */
    NET_SOCK_ADDR address;
    /** @brief What the calls said. */
    NET_ERR error = NET_SOCK_ERR_NONE;
    /** @brief The socket. */
    int32_t socket = MQTT_SOCKET_NONE;
    /** @brief The outcome. */
    mqtt_status status = MQTT_CONNECTION_CLOSED;

    if (ucos3_address(host, port, &address)) {
        socket = ucos3_open();
    }
    if (socket != MQTT_SOCKET_NONE) {
        (void)NetSock_Bind((NET_SOCK_ID)socket, &address,
                           (NET_SOCK_ADDR_LEN)sizeof(address), &error);
        if (error == NET_SOCK_ERR_NONE) {
            (void)NetSock_Listen((NET_SOCK_ID)socket, UCOS3_BACKLOG, &error);
        }
        if (error == NET_SOCK_ERR_NONE) {
            ucos3_no_block(socket);
            *listener = socket;
            status = MQTT_OK;
        } else {
            mqtt_platform_close(socket);
        }
    }
    return status;
}

void mqtt_platform_send_buffer(int32_t handle, uint32_t bytes)
{
    /* uC/TCP-IP sizes its windows when it is configured, not for each
     * socket. */
    (void)handle;
    (void)bytes;
}

mqtt_status mqtt_platform_accept(int32_t listener, int32_t *handle)
{
    /** @brief The peer's address, which isn't used. */
    NET_SOCK_ADDR address;
    /** @brief Its length. */
    NET_SOCK_ADDR_LEN length = (NET_SOCK_ADDR_LEN)sizeof(address);
    /** @brief What the call said. */
    NET_ERR error = NET_SOCK_ERR_NONE;
    /** @brief The new socket. */
    const NET_SOCK_ID accepted =
        NetSock_Accept((NET_SOCK_ID)listener, &address, &length, &error);
    /** @brief The outcome. */
    mqtt_status status = MQTT_INCOMPLETE;

    if ((error == NET_SOCK_ERR_NONE) && (accepted >= 0)) {
        ucos3_connected((int32_t)accepted);
        *handle = (int32_t)accepted;
        status = MQTT_OK;
    } else if ((error != NET_SOCK_ERR_CONN_ACCEPT_Q_NONE_AVAIL) &&
               (error != NET_SOCK_ERR_CONN_SIGNAL_TIMEOUT)) {
        status = MQTT_CONNECTION_CLOSED;
    } else {
        /* Nothing is waiting. */
    }
    return status;
}

mqtt_status mqtt_platform_connect(const char *host, const char *port,
                                  int32_t *handle)
{
    /** @brief The address. */
    NET_SOCK_ADDR address;
    /** @brief What the call said. */
    NET_ERR error = NET_SOCK_ERR_NONE;
    /** @brief The socket. */
    int32_t socket = MQTT_SOCKET_NONE;
    /** @brief The outcome. */
    mqtt_status status = MQTT_CONNECTION_CLOSED;

    if ((host != NULL) && ucos3_address(host, port, &address)) {
        socket = ucos3_open();
    }
    if (socket != MQTT_SOCKET_NONE) {
        (void)NetSock_Conn((NET_SOCK_ID)socket, &address,
                           (NET_SOCK_ADDR_LEN)sizeof(address), &error);
        if (error == NET_SOCK_ERR_NONE) {
            ucos3_connected(socket);
            *handle = socket;
            status = MQTT_OK;
        } else {
            mqtt_platform_close(socket);
        }
    }
    return status;
}

/**
 * @brief Find what a call that moved nothing means.
 * @param[in] result What it returned.
 * @param[in] error What it said.
 * @return MQTT_INCOMPLETE if it only had to wait, or MQTT_CONNECTION_CLOSED.
 */
static mqtt_status ucos3_failure(NET_SOCK_RTN_CODE result, NET_ERR error)
{
    return ((result != NET_SOCK_BSD_RTN_CODE_CONN_CLOSED) &&
            ((error == NET_SOCK_ERR_RX_Q_EMPTY) || (error == NET_ERR_TX))) ?
               MQTT_INCOMPLETE :
               MQTT_CONNECTION_CLOSED;
}

mqtt_status mqtt_platform_read(int32_t handle, uint8_t *data, uint32_t capacity,
                               uint32_t *count)
{
    /** @brief What the call said. */
    NET_ERR error = NET_SOCK_ERR_NONE;
    /** @brief Number of bytes read. */
    const NET_SOCK_RTN_CODE result = NetSock_RxData(
        (NET_SOCK_ID)handle, data,
        (CPU_INT16U)((capacity < UCOS3_MOST) ? capacity : UCOS3_MOST),
        NET_SOCK_FLAG_NONE, &error);
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if ((result > 0) && (error == NET_SOCK_ERR_NONE)) {
        *count = (uint32_t)result;
    } else {
        status = ucos3_failure(result, error);
    }
    return status;
}

mqtt_status mqtt_platform_read_spaces(int32_t handle, const mqtt_space *spaces,
                                      uint32_t count, uint32_t *count_read)
{
    /** @brief Index of the place. */
    uint32_t place = 0U;
    /** @brief Number of bytes read. */
    uint32_t total = 0U;
    /** @brief The outcome of the last read. */
    mqtt_status status = MQTT_OK;
    /** @brief true while each place has been filled. */
    bool going = true;

    while (going && (place < count) && (place < MQTT_PLATFORM_READ_SPACES)) {
        /** @brief Number of bytes read into this place. */
        uint32_t got = 0U;

        status = mqtt_platform_read(handle, spaces[place].data,
                                    spaces[place].length, &got);
        if (status == MQTT_OK) {
            total += got;
            going = (got == spaces[place].length);
        } else {
            going = false;
        }
        place += 1U;
    }
    if (total > 0U) {
        status = MQTT_OK;
    }
    *count_read = total;
    return status;
}

mqtt_status mqtt_platform_write(int32_t handle, const uint8_t *data,
                                uint32_t length, uint32_t *count)
{
    /** @brief What the call said. */
    NET_ERR error = NET_SOCK_ERR_NONE;
    /** @brief Number of bytes taken. */
    const NET_SOCK_RTN_CODE result = NetSock_TxData(
        (NET_SOCK_ID)handle, (void *)data,
        (CPU_INT16U)((length < UCOS3_MOST) ? length : UCOS3_MOST),
        NET_SOCK_FLAG_NONE, &error);
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if ((result > 0) && (error == NET_SOCK_ERR_NONE)) {
        *count = (uint32_t)result;
    } else if (length == 0U) {
        *count = 0U;
    } else {
        status = ucos3_failure(result, error);
    }
    return status;
}

mqtt_status mqtt_platform_write_runs(int32_t handle, const mqtt_run *runs,
                                     uint32_t count, uint32_t *count_taken)
{
    /** @brief Index of the run. */
    uint32_t run = 0U;
    /** @brief Number of bytes taken. */
    uint32_t total = 0U;
    /** @brief The outcome of the last write. */
    mqtt_status status = MQTT_OK;
    /** @brief true while each run has been taken whole. */
    bool going = true;

    while (going && (run < count) && (run < MQTT_PLATFORM_WRITE_RUNS)) {
        /** @brief Number of bytes of this run taken. */
        uint32_t taken = 0U;

        status = mqtt_platform_write(handle, runs[run].data, runs[run].length,
                                     &taken);
        if (status == MQTT_OK) {
            total += taken;
            going = (taken == runs[run].length);
        } else {
            going = false;
        }
        run += 1U;
    }
    if (total > 0U) {
        status = MQTT_OK;
    }
    *count_taken = total;
    return status;
}

void mqtt_platform_close(int32_t handle)
{
    /** @brief What the call said. */
    NET_ERR error = NET_SOCK_ERR_NONE;

    if (handle >= 0) {
        (void)NetSock_Close((NET_SOCK_ID)handle, &error);
    }
}

/**
 * @brief Write a port number as text.
 * @param[in] number The port.
 * @param[out] text Room for six characters.
 */
static void ucos3_port_text(uint32_t number, char *text)
{
    /** @brief What is left of the number. */
    uint32_t left = number;
    /** @brief Index of the digit. */
    uint32_t digit = 5U;

    text[5] = '\0';
    while (digit > 0U) {
        digit -= 1U;
        text[digit] = (char)('0' + (char)(left % 10U));
        left /= 10U;
    }
}

bool mqtt_platform_close_resets(void)
{
    /* uC/TCP-IP ends every connection it closes with a FIN, never a reset. */
    return false;
}

mqtt_status mqtt_platform_socket_pair(int32_t *first, int32_t *second)
{
    /** @brief The next port to try, as no port is given out by the stack. */
    static uint32_t next = UCOS3_PAIR_PORT;
    /** @brief The port, as text. */
    char port[6];
    /** @brief The listener. */
    int32_t listener = MQTT_SOCKET_NONE;
    /** @brief The outcome. */
    mqtt_status status = MQTT_CONNECTION_CLOSED;

    ucos3_port_text(next, port);
    next = (next < UCOS3_PORT_MOST) ? (next + 1U) : UCOS3_PAIR_PORT;
    status = mqtt_platform_listen("127.0.0.1", port, &listener);
    if (status == MQTT_OK) {
        status = mqtt_platform_connect("127.0.0.1", port, first);
    }
    if (status == MQTT_OK) {
        /** @brief Tries left at accepting the end that connected. */
        uint32_t tries = 100U;

        status = MQTT_INCOMPLETE;
        while ((status == MQTT_INCOMPLETE) && (tries > 0U)) {
            status = mqtt_platform_accept(listener, second);
            if (status == MQTT_INCOMPLETE) {
                mqtt_platform_sleep_ms(1U);
            }
            tries -= 1U;
        }
        if (status != MQTT_OK) {
            mqtt_platform_close(*first);
            status = MQTT_CONNECTION_CLOSED;
        }
    }
    if (listener != MQTT_SOCKET_NONE) {
        mqtt_platform_close(listener);
    }
    return status;
}
