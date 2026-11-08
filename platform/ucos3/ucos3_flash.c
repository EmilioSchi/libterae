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
 * @file ucos3_flash.c
 * @brief The flash a store is kept in, from the board.
 *
 * uC/OS-III has no driver for flash, which every board wires its own way, so
 * the application gives the region a store takes with ucos3_board_store_region
 * and the calls on it, as it gives ucos3_board_write. A board with no flash
 * for a store says it has no region, and a store can't be opened.
 */

#include "platform_flash.h"
#include "ucos3_internal.h"

mqtt_status mqtt_platform_flash_region(const char *where, uint32_t *region,
                                       uint32_t *size, uint32_t *block)
{
    return ucos3_board_store_region(where, region, size, block) ?
               MQTT_OK :
               MQTT_INVALID_STATE;
}

mqtt_status mqtt_platform_flash_read(uint32_t region, uint32_t offset,
                                     uint8_t *data, uint32_t length)
{
    return ucos3_board_store_read(region, offset, data, length) ?
               MQTT_OK :
               MQTT_INVALID_STATE;
}

mqtt_status mqtt_platform_flash_program(uint32_t region, uint32_t offset,
                                        const uint8_t *data, uint32_t length)
{
    return ucos3_board_store_program(region, offset, data, length) ?
               MQTT_OK :
               MQTT_INVALID_STATE;
}

mqtt_status mqtt_platform_flash_erase(uint32_t region, uint32_t offset,
                                      uint32_t length)
{
    return ucos3_board_store_erase(region, offset, length) ?
               MQTT_OK :
               MQTT_INVALID_STATE;
}

mqtt_status mqtt_platform_flash_sync(uint32_t region)
{
    return ucos3_board_store_sync(region) ? MQTT_OK : MQTT_INVALID_STATE;
}

mqtt_status mqtt_platform_flash_set_aside(uint32_t region, uint32_t *aside)
{
    return ucos3_board_store_set_aside(region, aside) ? MQTT_OK :
                                                           MQTT_INVALID_STATE;
}

mqtt_status mqtt_platform_flash_remove(const char *where)
{
    /** @brief true if there was such a region. */
    bool found = false;
    /** @brief The outcome. */
    mqtt_status status = MQTT_INVALID_STATE;

    if (ucos3_board_store_remove(where, &found)) {
        status = found ? MQTT_OK : MQTT_NOT_FOUND;
    }
    return status;
}
