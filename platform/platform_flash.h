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
 * @file platform_flash.h
 * @brief The few calls on flash that platform_store_flash.c keeps a store
 * with, which a real-time system's folder gives from its board.
 *
 * A store takes one region of flash that the board names, made of erase
 * blocks. Erasing sets every byte of a block to 0xFF, and programming can only
 * clear bits of bytes that are erased, as NOR flash does. The board programs
 * any run of erased bytes, whatever its flash's own unit of programming.
 */
#ifndef MQTT_PLATFORM_FLASH_H
#define MQTT_PLATFORM_FLASH_H

#include <stdint.h>

#include "base_status.h"

/** @brief The value of a byte of flash that is erased. */
#define MQTT_FLASH_ERASED 0xFFU

/**
 * @brief Find the region of flash a store of this name is kept in.
 * @param[in] where The name.
 * @param[out] region The region.
 * @param[out] size Number of bytes in it: a whole number of erase blocks.
 * @param[out] block Number of bytes of an erase block.
 * @return MQTT_OK, or MQTT_INVALID_STATE if the board has no such region.
 */
mqtt_status mqtt_platform_flash_region(const char *where, uint32_t *region,
                                       uint32_t *size, uint32_t *block);

/**
 * @brief Read bytes of a region.
 * @param[in] region The region.
 * @param[in] offset Offset of the first byte.
 * @param[out] data Where they go.
 * @param[in] length Number of bytes, all inside the region.
 * @return MQTT_OK, or MQTT_INVALID_STATE if the flash failed.
 */
mqtt_status mqtt_platform_flash_read(uint32_t region, uint32_t offset,
                                     uint8_t *data, uint32_t length);

/**
 * @brief Program bytes of a region that are erased.
 * @param[in] region The region.
 * @param[in] offset Offset of the first byte.
 * @param[in] data The bytes.
 * @param[in] length Number of bytes, all inside the region.
 * @return MQTT_OK, or MQTT_INVALID_STATE if the flash failed.
 */
mqtt_status mqtt_platform_flash_program(uint32_t region, uint32_t offset,
                                        const uint8_t *data, uint32_t length);

/**
 * @brief Erase whole blocks of a region.
 * @param[in] region The region.
 * @param[in] offset Offset of the first block.
 * @param[in] length Number of bytes: a whole number of blocks.
 * @return MQTT_OK, or MQTT_INVALID_STATE if the flash failed.
 */
mqtt_status mqtt_platform_flash_erase(uint32_t region, uint32_t offset,
                                      uint32_t length);

/**
 * @brief Make what was programmed in a region outlast a loss of power, for a
 * board whose flash, or the controller in front of it, holds writes back.
 * @param[in] region The region.
 * @return MQTT_OK, or MQTT_INVALID_STATE if it failed.
 */
mqtt_status mqtt_platform_flash_sync(uint32_t region);

/**
 * @brief Keep what a region holds somewhere else, under a number of its own,
 * before the store erases it. Nothing is lost: a board with nowhere to keep it
 * refuses.
 * @param[in] region The region.
 * @param[out] aside The number it is kept under.
 * @return MQTT_OK, or MQTT_INVALID_STATE if it could not be kept.
 */
mqtt_status mqtt_platform_flash_set_aside(uint32_t region, uint32_t *aside);

/**
 * @brief Remove the region of a store that isn't open, and what was set aside
 * from it, for the tests.
 * @param[in] where The name.
 * @return MQTT_OK, MQTT_NOT_FOUND if there is no such region, or
 * MQTT_INVALID_STATE if it could not be removed.
 */
mqtt_status mqtt_platform_flash_remove(const char *where);

#endif /* MQTT_PLATFORM_FLASH_H */
