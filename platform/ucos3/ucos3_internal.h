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
 * @file ucos3_internal.h
 * @brief What the files of ucos3/ share, and what the program's board gives
 * them.
 *
 * A socket of uC/TCP-IP is a small number, NET_SOCK_ID, which is used as the
 * interface's int32_t handle as it is.
 */
#ifndef MQTT_UCOS3_INTERNAL_H
#define MQTT_UCOS3_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include <os.h>

/**
 * @brief Write bytes to the program's console, which the board gives, as a
 * system with no files has no standard output of its own.
 * @param[in] error true for the diagnostic output, false for the normal one.
 * @param[in] data The bytes.
 * @param[in] length Number of bytes.
 */
void ucos3_board_write(bool error, const uint8_t *data, uint32_t length);

/**
 * @brief Fill bytes from the board's source of random numbers, which an
 * attacker can't predict, such as a hardware generator.
 * @param[out] data Where to put them.
 * @param[in] length Number of bytes.
 * @return true if they are filled, false if the board has no such source or it
 * failed.
 */
bool ucos3_board_random(uint8_t *data, uint32_t length);

/**
 * @brief Find the region of flash a store of a name is kept in, which the
 * board gives. Its flash reads 0xFF where it is erased, and only has bits
 * cleared where it is programmed, as NOR flash does.
 * @param[in] where The name.
 * @param[out] region The region.
 * @param[out] size Number of bytes in it: a whole number of erase blocks.
 * @param[out] block Number of bytes of an erase block.
 * @return true if the board has such a region.
 */
bool ucos3_board_store_region(const char *where, uint32_t *region,
                              uint32_t *size, uint32_t *block);

/**
 * @brief Read bytes of a region of flash.
 * @param[in] region The region.
 * @param[in] offset Offset of the first byte.
 * @param[out] data Where they go.
 * @param[in] length Number of bytes.
 * @return true if they were read.
 */
bool ucos3_board_store_read(uint32_t region, uint32_t offset, uint8_t *data,
                            uint32_t length);

/**
 * @brief Program bytes of a region of flash that are erased, whatever the
 * flash's own unit of programming.
 * @param[in] region The region.
 * @param[in] offset Offset of the first byte.
 * @param[in] data The bytes.
 * @param[in] length Number of bytes.
 * @return true if they were programmed.
 */
bool ucos3_board_store_program(uint32_t region, uint32_t offset,
                               const uint8_t *data, uint32_t length);

/**
 * @brief Erase whole blocks of a region of flash.
 * @param[in] region The region.
 * @param[in] offset Offset of the first block.
 * @param[in] length Number of bytes: a whole number of blocks.
 * @return true if they were erased.
 */
bool ucos3_board_store_erase(uint32_t region, uint32_t offset,
                             uint32_t length);

/**
 * @brief Make what was programmed in a region of flash outlast a loss of
 * power, if the flash or its controller holds writes back.
 * @param[in] region The region.
 * @return true once it does.
 */
bool ucos3_board_store_sync(uint32_t region);

/**
 * @brief Keep what a region of flash holds somewhere else, under a number of
 * its own, before the store erases it.
 * @param[in] region The region.
 * @param[out] aside The number it is kept under.
 * @return true if it is kept, false if the board has nowhere to keep it.
 */
bool ucos3_board_store_set_aside(uint32_t region, uint32_t *aside);

/**
 * @brief Remove the region of a store, and what was set aside from it, for the
 * tests.
 * @param[in] where The name.
 * @param[out] found true if there was such a region.
 * @return true if nothing of it is left.
 */
bool ucos3_board_store_remove(const char *where, bool *found);

/**
 * @brief Turn milliseconds into ticks of the kernel, rounding up so that a
 * wait is never shorter than asked.
 * @param[in] milliseconds The time.
 * @return The ticks.
 */
OS_TICK ucos3_ticks(uint32_t milliseconds);

#endif /* MQTT_UCOS3_INTERNAL_H */
