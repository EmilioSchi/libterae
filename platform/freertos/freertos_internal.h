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
 * @file freertos_internal.h
 * @brief What the files of freertos/ share: the table that gives each
 * FreeRTOS+TCP socket the int32_t the interface names it by, and what the
 * program's board gives them.
 *
 * A Socket_t is a pointer, so each one is kept in a table and named by its
 * index there. A server task and a client task may both use the table, so it
 * is changed only inside a critical section.
 */
#ifndef MQTT_FREERTOS_INTERNAL_H
#define MQTT_FREERTOS_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include <FreeRTOS.h>
#include <FreeRTOS_Sockets.h>

/**
 * @brief Write bytes to the program's console, which the board gives, as a
 * system with no files has no standard output of its own.
 * @param[in] error true for the diagnostic output, false for the normal one.
 * @param[in] data The bytes.
 * @param[in] length Number of bytes.
 */
void freertos_board_write(bool error, const uint8_t *data, uint32_t length);

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
bool freertos_board_store_region(const char *where, uint32_t *region,
                                 uint32_t *size, uint32_t *block);

/**
 * @brief Read bytes of a region of flash.
 * @param[in] region The region.
 * @param[in] offset Offset of the first byte.
 * @param[out] data Where they go.
 * @param[in] length Number of bytes.
 * @return true if they were read.
 */
bool freertos_board_store_read(uint32_t region, uint32_t offset, uint8_t *data,
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
bool freertos_board_store_program(uint32_t region, uint32_t offset,
                                  const uint8_t *data, uint32_t length);

/**
 * @brief Erase whole blocks of a region of flash.
 * @param[in] region The region.
 * @param[in] offset Offset of the first block.
 * @param[in] length Number of bytes: a whole number of blocks.
 * @return true if they were erased.
 */
bool freertos_board_store_erase(uint32_t region, uint32_t offset,
                                uint32_t length);

/**
 * @brief Make what was programmed in a region of flash outlast a loss of
 * power, if the flash or its controller holds writes back.
 * @param[in] region The region.
 * @return true once it does.
 */
bool freertos_board_store_sync(uint32_t region);

/**
 * @brief Keep what a region of flash holds somewhere else, under a number of
 * its own, before the store erases it.
 * @param[in] region The region.
 * @param[out] aside The number it is kept under.
 * @return true if it is kept, false if the board has nowhere to keep it.
 */
bool freertos_board_store_set_aside(uint32_t region, uint32_t *aside);

/**
 * @brief Remove the region of a store, and what was set aside from it, for the
 * tests.
 * @param[in] where The name.
 * @param[out] found true if there was such a region.
 * @return true if nothing of it is left.
 */
bool freertos_board_store_remove(const char *where, bool *found);

/**
 * @brief Find the socket of a handle.
 * @param[in] handle The handle.
 * @return The socket, or NULL if the handle has none.
 */
Socket_t freertos_socket(int32_t handle);

/**
 * @brief Give a socket a handle. A socket that finds the table full is
 * closed.
 * @param[in] socket The socket.
 * @return Its handle, or MQTT_SOCKET_NONE.
 */
int32_t freertos_adopt(Socket_t socket);

/**
 * @brief Close the sockets kept since they were closed whose connections have
 * ended, or that have been kept for FREERTOS_LINGER_MS.
 */
void freertos_reap(void);

/**
 * @brief Make a socket not block: its reads, writes and accepts give up at
 * once.
 * @param[in] socket The socket.
 */
void freertos_no_block(Socket_t socket);

/**
 * @brief Find the socket set kept for waiting on one socket alone, made the
 * first time it is asked for.
 * @param[in] handle The socket's handle.
 * @return The set, or NULL if none could be made.
 */
SocketSet_t freertos_own_set(int32_t handle);

/**
 * @brief Turn milliseconds into ticks of the kernel, rounding up so that a
 * wait is never shorter than asked, and capping at the longest wait.
 * @param[in] milliseconds The time.
 * @return The ticks.
 */
TickType_t freertos_ticks(uint32_t milliseconds);

#endif /* MQTT_FREERTOS_INTERNAL_H */
