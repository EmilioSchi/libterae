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
 * @file platform_net_runs.c
 * @brief Writing runs of bytes, on a socket or through TLS, for the server
 * and the client of the network layer.
 */

#include <stddef.h>
#include <string.h>

#include "base_tls.h"
#include "platform_net.h"

uint32_t platform_net_runs_length(const mqtt_run *runs, uint32_t count)
{
    /** @brief Number of bytes. */
    uint32_t total = 0U;
    /** @brief Index of the run. */
    uint32_t index = 0U;

    while (index < count) {
        total += runs[index].length;
        index += 1U;
    }
    return total;
}

mqtt_status platform_net_write_runs(int32_t handle, uint32_t session,
                                    const mqtt_run *runs, uint32_t count,
                                    uint8_t *scratch, uint32_t *taken)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (session == MQTT_TLS_NONE) {
        status = mqtt_platform_write_runs(handle, runs, count, taken);
    } else if ((count == 1U) || (runs[0].length >= PLATFORM_NET_TLS_RECORD)) {
        status = mqtt_tls_write(session, runs[0].data, runs[0].length, taken);
    } else {
        /** @brief Number of bytes put together. */
        uint32_t filled = 0U;
        /** @brief Index of the run. */
        uint32_t index = 0U;

        while ((index < count) && (filled < PLATFORM_NET_TLS_RECORD)) {
            /** @brief Number of bytes of this run that fit. */
            const uint32_t part =
                ((PLATFORM_NET_TLS_RECORD - filled) < runs[index].length) ?
                    (PLATFORM_NET_TLS_RECORD - filled) :
                    runs[index].length;

            (void)memcpy(&scratch[filled], runs[index].data,
                         (size_t)part);
            filled += part;
            index += 1U;
        }
        status = mqtt_tls_write(session, scratch, filled, taken);
    }
    return status;
}
