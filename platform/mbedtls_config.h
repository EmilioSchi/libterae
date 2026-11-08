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
 * @file mbedtls_config.h
 * @brief What libterae adds to the default configuration of Mbed TLS.
 *
 * The default configuration is what the Mbed TLS project tests, and it has TLS
 * 1.2 and TLS 1.3 with the algorithms other implementations expect. All that
 * changes is where memory comes from: a static pool in platform_tls.c, not the
 * system allocator.
 *
 * [RFC5246] The Transport Layer Security (TLS) Protocol Version 1.2.
 * @li Source: www.rfc-editor.org/rfc/rfc5246
 *
 * [RFC9846] The Transport Layer Security (TLS) Protocol Version 1.3.
 * @li Source: www.rfc-editor.org/rfc/rfc9846
 */
#ifndef MQTT_MBEDTLS_CONFIG_H
#define MQTT_MBEDTLS_CONFIG_H

/** @brief Let the library's memory functions be replaced. */
#define MBEDTLS_PLATFORM_MEMORY
/** @brief Take memory from a static pool. */
#define MBEDTLS_MEMORY_BUFFER_ALLOC_C

#endif /* MQTT_MBEDTLS_CONFIG_H */
