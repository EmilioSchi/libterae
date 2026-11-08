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
 * @file win32_random.c
 * @brief Random bytes, from the system's generator through BCryptGenRandom.
 *
 * The generator is reached through a provider opened for each call and closed
 * after it, so no handle is shared between threads. Asking BCryptGenRandom for
 * the system's preferred generator with no provider would be shorter, but
 * Windows Vista, the oldest version this builds for, doesn't take that flag.
 * A program links bcrypt.
 */

#include <stddef.h>
#include <windows.h>
#include <bcrypt.h>

#include "base_platform.h"

mqtt_status mqtt_platform_random_bytes(uint8_t *data, uint32_t length)
{
    /** @brief The provider of random numbers. */
    BCRYPT_ALG_HANDLE provider = NULL;
    /** @brief The outcome of opening it. */
    const NTSTATUS opened = BCryptOpenAlgorithmProvider(
        &provider, BCRYPT_RNG_ALGORITHM, NULL, 0U);
    /** @brief The outcome. */
    mqtt_status status = MQTT_INVALID_STATE;

    if (BCRYPT_SUCCESS(opened)) {
        if ((length == 0U) || BCRYPT_SUCCESS(BCryptGenRandom(
                                  provider, data, (ULONG)length, 0U))) {
            status = MQTT_OK;
        }
        (void)BCryptCloseAlgorithmProvider(provider, 0U);
    }
    return status;
}
