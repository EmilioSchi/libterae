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
 * @file base_wire.h
 * @brief Reading and writing the data representations.
 *
 * A reader or a writer remembers its first failure, so a caller can make a
 * series of calls and check the outcome once. After a failure, reads return
 * zero values and writes change nothing.
 *
 * A writer with no buffer counts bytes and stores nothing, which gives the size
 * of a packet from the same code that encodes it. A count that would pass 32
 * bits stops at UINT32_MAX, more than any packet may take, and fails with
 * MQTT_INVALID_ARGUMENT.
 *
 * A NULL pointer is refused unless its parameter says it may be NULL, or it
 * comes with a length of zero. A read from no reader returns a zero value, and
 * a write to no writer does nothing. A writer given no string, or bytes it
 * would have to store from NULL, fails with MQTT_INVALID_ARGUMENT, and a reader
 * set up with NULL bytes and a length fails the same way.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 1.5, p.17-20. 2.1.4, p.23.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015.
 * @li Cited: 1.5, 2.2.3.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 */
#ifndef MQTT_BASE_WIRE_H
#define MQTT_BASE_WIRE_H

#include <stdbool.h>
#include <stdint.h>

#include "base_binary_t.h"
#include "base_status.h"
#include "base_utf8_t.h"

/** @brief Position within received bytes. */
typedef struct {
    /** @brief The bytes being read. */
    const uint8_t *data;
    /** @brief Number of bytes in data. */
    uint32_t length;
    /** @brief Index of the next byte to read. */
    uint32_t position;
    /** @brief MQTT_OK, or the first failure. */
    mqtt_status status;
} mqtt_reader;

/** @brief Position within a buffer being written. */
typedef struct {
    /** @brief The destination, or NULL to count bytes only. */
    uint8_t *data;
    /** @brief Number of bytes data can hold. Ignored when data is NULL. */
    uint32_t capacity;
    /** @brief Number of bytes written or counted so far. */
    uint32_t position;
    /** @brief MQTT_OK, or the first failure. */
    mqtt_status status;
} mqtt_writer;

/**
 * @brief Start reading a range of bytes.
 * @param[out] reader The reader to set up.
 * @param[in] data The bytes to read.
 * @param[in] length Number of bytes in data.
 */
void mqtt_reader_init(mqtt_reader *reader, const uint8_t *data,
                      uint32_t length);

/**
 * @brief Count the bytes not read yet.
 * @param[in] reader The reader.
 * @return Number of bytes left.
 */
uint32_t mqtt_reader_left(const mqtt_reader *reader);

/**
 * @brief Read one byte.
 * @param[in,out] reader The reader.
 * @return The byte, or 0U after a failure.
 */
uint8_t mqtt_read_byte(mqtt_reader *reader);

/**
 * @brief Read a Two Byte Integer.
 * @param[in,out] reader The reader.
 * @return The value, or 0U after a failure.
 */
uint16_t mqtt_read_two_byte_integer(mqtt_reader *reader);

/**
 * @brief Read a Four Byte Integer.
 * @param[in,out] reader The reader.
 * @return The value, or 0U after a failure.
 */
uint32_t mqtt_read_four_byte_integer(mqtt_reader *reader);

/**
 * @brief Read a Variable Byte Integer, which must use the fewest bytes
 * possible.
 * @param[in,out] reader The reader.
 * @return The value, or 0U after a failure.
 */
uint32_t mqtt_read_variable_byte_integer(mqtt_reader *reader);

/**
 * @brief Read a length-prefixed string without checking its characters.
 * @param[in,out] reader The reader.
 * @return A view into the reader's bytes, empty after a failure.
 */
mqtt_utf8 mqtt_read_utf8(mqtt_reader *reader);

/**
 * @brief Read Binary Data.
 * @param[in,out] reader The reader.
 * @return A view into the reader's bytes, empty after a failure.
 */
mqtt_binary mqtt_read_binary(mqtt_reader *reader);

/**
 * @brief Start writing to a buffer.
 * @param[out] writer The writer to set up.
 * @param[out] data The destination, or NULL to count bytes only.
 * @param[in] capacity Number of bytes data can hold.
 */
void mqtt_writer_init(mqtt_writer *writer, uint8_t *data, uint32_t capacity);

/**
 * @brief Fail a writer with MQTT_INVALID_ARGUMENT, unless it has already
 * failed, for an encoder given an argument it refuses.
 * @param[in,out] writer The writer. Nothing happens if it is NULL.
 */
void mqtt_writer_refuse(mqtt_writer *writer);

/**
 * @brief Write one byte.
 * @param[in,out] writer The writer.
 * @param[in] value The byte.
 */
void mqtt_write_byte(mqtt_writer *writer, uint8_t value);

/**
 * @brief Write a Two Byte Integer.
 * @param[in,out] writer The writer.
 * @param[in] value The value.
 */
void mqtt_write_two_byte_integer(mqtt_writer *writer, uint16_t value);

/**
 * @brief Write a Four Byte Integer.
 * @param[in,out] writer The writer.
 * @param[in] value The value.
 */
void mqtt_write_four_byte_integer(mqtt_writer *writer, uint32_t value);

/**
 * @brief Write a Variable Byte Integer.
 * @param[in,out] writer The writer.
 * @param[in] value The value, which must not exceed MQTT_REMAINING_LENGTH_MAX.
 */
void mqtt_write_variable_byte_integer(mqtt_writer *writer, uint32_t value);

/**
 * @brief Write bytes with no length prefix.
 * @param[in,out] writer The writer.
 * @param[in] data The bytes.
 * @param[in] length Number of bytes in data.
 */
void mqtt_write_bytes(mqtt_writer *writer, const uint8_t *data,
                      uint32_t length);

/**
 * @brief Write a length-prefixed string.
 * @param[in,out] writer The writer.
 * @param[in] value The string.
 */
void mqtt_write_utf8(mqtt_writer *writer, const mqtt_utf8 *value);

/**
 * @brief Write Binary Data.
 * @param[in,out] writer The writer.
 * @param[in] value The data.
 */
void mqtt_write_binary(mqtt_writer *writer, const mqtt_binary *value);

/**
 * @brief Count the bytes a Variable Byte Integer takes.
 * @param[in] value The value, which must not exceed MQTT_REMAINING_LENGTH_MAX.
 * @return The number of bytes, from 1U to 4U.
 */
uint32_t mqtt_variable_byte_integer_size(uint32_t value);

/**
 * @brief Decode a Remaining Length, also accepting a form longer than the value
 * needs.
 *
 * Only MQTT 5.0 rules out the longer forms, and the version of a CONNECT packet
 * is not known yet when its Fixed Header is read: mqtt_fixed_header_check
 * applies that rule.
 *
 * @param[in] data The bytes.
 * @param[in] length Number of bytes in data.
 * @param[out] value The value.
 * @param[out] used Number of bytes it took.
 * @param[out] shortest Whether it took the fewest bytes it can.
 * @return MQTT_OK, MQTT_INCOMPLETE if more bytes are needed,
 * MQTT_MALFORMED_PACKET, or MQTT_INVALID_ARGUMENT for a NULL pointer.
 */
mqtt_status mqtt_remaining_length_decode(const uint8_t *data, uint32_t length,
                                         uint32_t *value, uint32_t *used,
                                         bool *shortest);

/**
 * @brief Decode a Variable Byte Integer from bytes that may be incomplete.
 * @param[in] data The bytes.
 * @param[in] length Number of bytes in data.
 * @param[out] value The value, on MQTT_OK.
 * @param[out] used Number of bytes it took, on MQTT_OK.
 * @return MQTT_OK, MQTT_INCOMPLETE if more bytes are needed,
 * MQTT_MALFORMED_PACKET, or MQTT_INVALID_ARGUMENT for a NULL pointer.
 */
mqtt_status mqtt_variable_byte_integer_decode(const uint8_t *data,
                                              uint32_t length, uint32_t *value,
                                              uint32_t *used);

/**
 * @brief Build a string view of a NUL-terminated literal.
 * @param[in] text The text, or NULL for an empty view.
 * @return The view. The text is not copied.
 */
mqtt_utf8 mqtt_utf8_from_text(const char *text);

/**
 * @brief Compare bytes with NUL-terminated text.
 * @param[in] data The bytes.
 * @param[in] length Number of bytes in data.
 * @param[in] text The text.
 * @return true if the text has the same bytes and no others.
 */
bool mqtt_utf8_equal_text(const uint8_t *data, uint32_t length,
                          const char *text);

/**
 * @brief Compare two strings byte by byte.
 * @param[in] left A string.
 * @param[in] right A string.
 * @return true if they have the same length and the same bytes.
 */
bool mqtt_utf8_equal(const mqtt_utf8 *left, const mqtt_utf8 *right);

#endif /* MQTT_BASE_WIRE_H */
