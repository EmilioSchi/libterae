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
 * @file freertos_random.c
 * @brief Random bytes, from the generator the application gives FreeRTOS+TCP.
 *
 * FreeRTOS+TCP asks every application for xApplicationGetRandomNumber, for its
 * ports and sequence numbers, and the kernel has no generator of its own. So
 * the bytes are only as hard to predict as what the application gives, which
 * should be the board's hardware generator. Each call gives four bytes.
 */

#include <FreeRTOS.h>
#include <FreeRTOS_IP.h>

#include "base_platform.h"

/** @brief Number of bits in one number from the generator. */
#define RANDOM_NUMBER_BITS 32U

mqtt_status mqtt_platform_random_bytes(uint8_t *data, uint32_t length)
{
    /** @brief Number of bytes given so far. */
    uint32_t done = 0U;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    while ((status == MQTT_OK) && (done < length)) {
        /** @brief The number. */
        uint32_t number = 0U;

        if (xApplicationGetRandomNumber(&number) != pdFALSE) {
            /** @brief Number of the number's bits already used. */
            uint32_t shift = 0U;

            while ((shift < RANDOM_NUMBER_BITS) && (done < length)) {
                data[done] = (uint8_t)(number >> shift);
                done += 1U;
                shift += 8U;
            }
        } else {
            status = MQTT_INVALID_STATE;
        }
    }
    return status;
}
