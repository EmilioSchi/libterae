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
 * @file base_string.h
 * @brief Copying, comparing, wiping and measuring bytes, with no C library.
 *
 * The library calls nothing outside itself, so these take the place of memcpy,
 * memcmp and strlen. Each is told how many bytes it may touch and touches no
 * more: a copy that wouldn't fit copies nothing, and a text is measured only
 * up to a limit. A length of zero touches nothing, so a NULL pointer is fine
 * with it, which isn't so for the C library's functions.
 *
 * The loops index the arrays one byte at a time, which the compiler can
 * vectorise. Built without -ffreestanding, it may turn a loop back into a call
 * to the C library, so the library is built with it.
 *
 * Built with MQTT_PORT_MEMORY, mqtt_bytes_put, and so mqtt_bytes_copy once it
 * has checked the bounds, copies with the memcpy in assembly from the
 * target's folder of src/port, under its other name, mqtt_port_memcpy. The
 * comparison and the wipe stay in C: one takes the same time wherever the
 * bytes differ, and the other writes through volatile.
 */
#ifndef MQTT_BASE_STRING_H
#define MQTT_BASE_STRING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base_status.h"

/**
 * @brief The memcpy of src/port, in assembly, under a name of libterae's own.
 * Each folder's port_memcpy.S defines it at the same address as memcpy, in
 * libterae_port.a. Only a library built with MQTT_PORT_MEMORY calls it, so a
 * build without a folder of assembly needs no definition.
 * @param[out] destination Where to copy the bytes.
 * @param[in] source The bytes, not overlapping destination.
 * @param[in] length Number of bytes, more than zero.
 * @return destination.
 */
void *mqtt_port_memcpy(void *destination, const void *source, size_t length);

/**
 * @brief Copy bytes. The two ranges must not overlap.
 * @param[out] destination Where to copy them.
 * @param[in] capacity Number of bytes destination holds.
 * @param[in] source The bytes.
 * @param[in] length Number of bytes to copy.
 * @return MQTT_OK, or MQTT_INVALID_ARGUMENT, with nothing copied, if length is
 * more than capacity or a pointer is NULL while length isn't zero.
 */
mqtt_status mqtt_bytes_copy(uint8_t *destination, uint32_t capacity,
                            const uint8_t *source, uint32_t length);

/**
 * @brief Copy bytes that the caller has made sure fit, as when they fill a
 * block just taken for them, or copy one struct into another of its type. The
 * two ranges must not overlap. A length of zero touches nothing, and neither
 * does a NULL pointer.
 * @param[out] destination Where to copy them, which holds length bytes.
 * @param[in] source The bytes.
 * @param[in] length Number of bytes to copy.
 */
void mqtt_bytes_put(uint8_t *destination, const uint8_t *source,
                    uint32_t length);

/**
 * @brief Compare bytes. It reads every byte whatever it finds, so the time it
 * takes doesn't depend on where they differ.
 * @param[in] left One set of bytes.
 * @param[in] right The other.
 * @param[in] length Number of bytes in each.
 * @return true if they are the same, or length is zero. false if they differ,
 * or a pointer is NULL while length isn't zero.
 */
bool mqtt_bytes_equal(const uint8_t *left, const uint8_t *right,
                      uint32_t length);

/**
 * @brief Overwrite bytes with zeros, through a volatile pointer so that no
 * optimisation can leave them out. For secrets, before their memory is let go.
 * @param[out] destination The bytes, or NULL if length is zero.
 * @param[in] length Number of bytes.
 */
void mqtt_bytes_wipe(uint8_t *destination, uint32_t length);

/**
 * @brief Measure a NUL-terminated text, reading no more than a limit.
 * @param[in] text The text, or NULL.
 * @param[in] limit The most bytes to read.
 * @param[out] length Number of bytes before the NUL, or 0U if there is none.
 * @return true if a NUL is among the first limit bytes. false if not, if text
 * is NULL, or if length is NULL, when nothing is written.
 */
bool mqtt_text_length(const char *text, uint32_t limit, uint32_t *length);

#endif /* MQTT_BASE_STRING_H */
