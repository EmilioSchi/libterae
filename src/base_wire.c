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
 * @file base_wire.c
 * @brief Reading and writing the data representations.
 *
 * [V5] MQTT Version 5.0, OASIS Standard, 7 March 2019, 137 pages.
 * @li Cited: 1.5, p.17-20. 2.1.4, p.23.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v5.0/os/
 */

#include <stddef.h>
#include <stdint.h>
#include "base_remaining.h"
#include "base_string.h"
#include "base_wire.h"

/**
 * @brief Claim bytes from a reader.
 * @param[in,out] reader The reader, or NULL.
 * @param[in] count Number of bytes wanted.
 * @param[out] start The index of the first byte, when they are all there.
 * @return true if they are all there. If not, the reader has failed.
 */
static bool wire_take(mqtt_reader *reader, uint32_t count, uint32_t *start);

/**
 * @brief Claim space in a writer.
 * @param[in,out] writer The writer, or NULL.
 * @param[in] count Number of bytes wanted.
 * @return true if the bytes may be stored at the position, false when counting,
 * after a failure, or for no writer.
 */
static bool wire_room(mqtt_writer *writer, uint32_t count);

static bool wire_take(mqtt_reader *reader, uint32_t count, uint32_t *start)
{
    /** @brief The answer. */
    bool taken = false;

    if ((reader != NULL) && (reader->status == MQTT_OK)) {
        if (count > (reader->length - reader->position)) {
            reader->status = MQTT_MALFORMED_PACKET;
        } else {
            *start = reader->position;
            reader->position += count;
            taken = true;
        }
    }
    return taken;
}

void mqtt_writer_refuse(mqtt_writer *writer)
{
    if ((writer != NULL) && (writer->status == MQTT_OK)) {
        writer->status = MQTT_INVALID_ARGUMENT;
    }
}

static bool wire_room(mqtt_writer *writer, uint32_t count)
{
    /** @brief true if the caller should store the bytes. */
    bool store = false;

    if ((writer != NULL) && (writer->status == MQTT_OK)) {
        if ((writer->data == NULL) &&
            (count > (UINT32_MAX - writer->position))) {
            /* A count past 32 bits stays at the largest, which is more than
             * any packet may take. */
            writer->position = UINT32_MAX;
            writer->status = MQTT_INVALID_ARGUMENT;
        } else if (writer->data == NULL) {
            writer->position += count;
        } else if (count > (writer->capacity - writer->position)) {
            writer->status = MQTT_BUFFER_TOO_SMALL;
        } else {
            store = true;
        }
    }
    return store;
}

void mqtt_reader_init(mqtt_reader *reader, const uint8_t *data, uint32_t length)
{
    if (reader != NULL) {
        /** @brief true if there are bytes to read, or none are said to be. */
        const bool valid = (data != NULL) || (length == 0U);

        reader->data = valid ? data : NULL;
        reader->length = valid ? length : 0U;
        reader->position = 0U;
        reader->status = valid ? MQTT_OK : MQTT_INVALID_ARGUMENT;
    }
}

uint32_t mqtt_reader_left(const mqtt_reader *reader)
{
    return (reader != NULL) ? (reader->length - reader->position) : 0U;
}

uint8_t mqtt_read_byte(mqtt_reader *reader)
{
    /** @brief The value read. */
    uint8_t value = 0U;
    /** @brief Index of the byte. */
    uint32_t start = 0U;

    if (wire_take(reader, 1U, &start)) {
        value = reader->data[start];
    }
    return value;
}

uint16_t mqtt_read_two_byte_integer(mqtt_reader *reader)
{
    /** @brief The value read. */
    uint16_t value = 0U;
    /** @brief Index of the first byte. */
    uint32_t start = 0U;

    if (wire_take(reader, 2U, &start)) {
        value = (uint16_t)(((uint32_t)reader->data[start] << 8U) |
                           (uint32_t)reader->data[start + 1U]);
    }
    return value;
}

uint32_t mqtt_read_four_byte_integer(mqtt_reader *reader)
{
    /** @brief The value read. */
    uint32_t value = 0U;
    /** @brief Index of the first byte. */
    uint32_t start = 0U;

    if (wire_take(reader, 4U, &start)) {
        value = ((uint32_t)reader->data[start] << 24U) |
                ((uint32_t)reader->data[start + 1U] << 16U) |
                ((uint32_t)reader->data[start + 2U] << 8U) |
                (uint32_t)reader->data[start + 3U];
    }
    return value;
}

uint32_t mqtt_read_variable_byte_integer(mqtt_reader *reader)
{
    /** @brief The value read. */
    uint32_t value = 0U;

    if ((reader != NULL) && (reader->status == MQTT_OK)) {
        /** @brief Number of bytes it took. */
        uint32_t used = 0U;
        /** @brief Outcome of the decode. */
        const mqtt_status status = mqtt_variable_byte_integer_decode(
            &reader->data[reader->position], reader->length - reader->position,
            &value, &used);

        if (status == MQTT_OK) {
            reader->position += used;
        } else {
            reader->status = MQTT_MALFORMED_PACKET;
            value = 0U;
        }
    }
    return value;
}

mqtt_utf8 mqtt_read_utf8(mqtt_reader *reader)
{
    /** @brief The view handed back. */
    mqtt_utf8 value = {0U, NULL};
    /** @brief Number of bytes in the string. */
    const uint16_t length = mqtt_read_two_byte_integer(reader);
    /** @brief Index of the string's first byte. */
    uint32_t start = 0U;

    if (wire_take(reader, (uint32_t)length, &start)) {
        value.length = length;
        value.data = &reader->data[start];
    }
    return value;
}

mqtt_binary mqtt_read_binary(mqtt_reader *reader)
{
    /** @brief The view handed back. */
    mqtt_binary value = {0U, NULL};
    /** @brief Number of bytes in the data. */
    const uint16_t length = mqtt_read_two_byte_integer(reader);
    /** @brief Index of the data's first byte. */
    uint32_t start = 0U;

    if (wire_take(reader, (uint32_t)length, &start)) {
        value.length = length;
        value.data = &reader->data[start];
    }
    return value;
}

void mqtt_writer_init(mqtt_writer *writer, uint8_t *data, uint32_t capacity)
{
    if (writer != NULL) {
        writer->data = data;
        writer->capacity = capacity;
        writer->position = 0U;
        writer->status = MQTT_OK;
    }
}

void mqtt_write_byte(mqtt_writer *writer, uint8_t value)
{
    if (wire_room(writer, 1U)) {
        writer->data[writer->position] = value;
        writer->position += 1U;
    }
}

void mqtt_write_two_byte_integer(mqtt_writer *writer, uint16_t value)
{
    if (wire_room(writer, 2U)) {
        writer->data[writer->position] = (uint8_t)((uint32_t)value >> 8U);
        writer->data[writer->position + 1U] =
            (uint8_t)((uint32_t)value & 0xFFU);
        writer->position += 2U;
    }
}

void mqtt_write_four_byte_integer(mqtt_writer *writer, uint32_t value)
{
    if (wire_room(writer, 4U)) {
        writer->data[writer->position] = (uint8_t)(value >> 24U);
        writer->data[writer->position + 1U] = (uint8_t)((value >> 16U) & 0xFFU);
        writer->data[writer->position + 2U] = (uint8_t)((value >> 8U) & 0xFFU);
        writer->data[writer->position + 3U] = (uint8_t)(value & 0xFFU);
        writer->position += 4U;
    }
}

void mqtt_write_variable_byte_integer(mqtt_writer *writer, uint32_t value)
{
    if (value > MQTT_REMAINING_LENGTH_MAX) {
        mqtt_writer_refuse(writer);
    } else {
        /** @brief The bits not written yet. */
        uint32_t rest = value;
        /** @brief true once the last byte is written. */
        bool done = false;

        while (!done) {
            /** @brief The next byte. */
            uint32_t encoded = rest & MQTT_REMAINING_LENGTH_MASK;

            rest >>= 7U;
            if (rest > 0U) {
                encoded |= MQTT_REMAINING_LENGTH_CONTINUATION;
            } else {
                done = true;
            }
            mqtt_write_byte(writer, (uint8_t)encoded);
        }
    }
}

void mqtt_write_bytes(mqtt_writer *writer, const uint8_t *data, uint32_t length)
{
    if (wire_room(writer, length)) {
        writer->status =
            mqtt_bytes_copy(&writer->data[writer->position],
                            writer->capacity - writer->position, data, length);
        if (writer->status == MQTT_OK) {
            writer->position += length;
        }
    }
}

void mqtt_write_utf8(mqtt_writer *writer, const mqtt_utf8 *value)
{
    if (value == NULL) {
        mqtt_writer_refuse(writer);
    } else {
        mqtt_write_two_byte_integer(writer, value->length);
        mqtt_write_bytes(writer, value->data, (uint32_t)value->length);
    }
}

void mqtt_write_binary(mqtt_writer *writer, const mqtt_binary *value)
{
    if (value == NULL) {
        mqtt_writer_refuse(writer);
    } else {
        mqtt_write_two_byte_integer(writer, value->length);
        mqtt_write_bytes(writer, value->data, (uint32_t)value->length);
    }
}

uint32_t mqtt_variable_byte_integer_size(uint32_t value)
{
    /** @brief Number of bytes needed. */
    uint32_t size = 4U;

    if (value < 128U) {
        size = 1U;
    } else if (value < 16384U) {
        size = 2U;
    } else if (value < 2097152U) {
        size = 3U;
    } else {
        /* Four bytes is the most the encoding allows. */
    }
    return size;
}

mqtt_status mqtt_remaining_length_decode(const uint8_t *data, uint32_t length,
                                         uint32_t *value, uint32_t *used,
                                         bool *shortest)
{
    /** @brief The outcome, until a final byte is seen. */
    mqtt_status status = MQTT_INCOMPLETE;
    /** @brief The value so far. */
    uint32_t total = 0U;
    /** @brief Index of the next byte. */
    uint32_t index = 0U;
    /** @brief Position of the next seven bits. */
    uint32_t shift = 0U;

    if (((data == NULL) && (length > 0U)) || (value == NULL) ||
        (used == NULL) || (shortest == NULL)) {
        status = MQTT_INVALID_ARGUMENT;
    }
    while ((status == MQTT_INCOMPLETE) && (index < length)) {
        if (index == MQTT_REMAINING_LENGTH_BYTES) {
            status = MQTT_MALFORMED_PACKET;
        } else {
            /** @brief The current byte. */
            const uint32_t byte = (uint32_t)data[index];

            total |= (byte & MQTT_REMAINING_LENGTH_MASK) << shift;
            shift += 7U;
            index += 1U;
            if ((byte & MQTT_REMAINING_LENGTH_CONTINUATION) == 0U) {
                /* A last byte of zero after others is a longer form than the
                 * value needs. */
                *shortest = (byte != 0U) || (index == 1U);
                status = MQTT_OK;
            }
        }
    }
    if ((status == MQTT_INCOMPLETE) &&
        (length >= MQTT_REMAINING_LENGTH_BYTES)) {
        status = MQTT_MALFORMED_PACKET;
    }
    if (status == MQTT_OK) {
        *value = total;
        *used = index;
    }
    return status;
}

mqtt_status mqtt_variable_byte_integer_decode(const uint8_t *data,
                                              uint32_t length, uint32_t *value,
                                              uint32_t *used)
{
    /** @brief true if the value took the fewest bytes it can. */
    bool shortest = true;
    /** @brief The outcome. */
    mqtt_status status =
        mqtt_remaining_length_decode(data, length, value, used, &shortest);

    if ((status == MQTT_OK) && (!shortest)) {
        status = MQTT_MALFORMED_PACKET;
    }
    return status;
}

mqtt_utf8 mqtt_utf8_from_text(const char *text)
{
    /** @brief The view handed back. */
    mqtt_utf8 value = {0U, NULL};

    /** @brief Length of the text. */
    uint32_t length = 0U;

    /* Read at most one byte past the longest string, so a text with no NUL
     * isn't read without end. */
    if (mqtt_text_length(text, (uint32_t)UINT16_MAX + 1U, &length)) {
        value.length = (uint16_t)length;
        value.data = (const uint8_t *)text;
    }
    return value;
}

bool mqtt_utf8_equal_text(const uint8_t *data, uint32_t length,
                          const char *text)
{
    /** @brief Index of the byte being compared. */
    uint32_t index = 0U;
    /** @brief The answer so far. */
    bool equal = (text != NULL) && ((data != NULL) || (length == 0U));

    while (equal && (index < length)) {
        equal = (text[index] != '\0') && ((uint8_t)text[index] == data[index]);
        index += 1U;
    }
    return equal && (text[index] == '\0');
}

bool mqtt_utf8_equal(const mqtt_utf8 *left, const mqtt_utf8 *right)
{
    return (left != NULL) && (right != NULL) &&
           (left->length == right->length) &&
           mqtt_bytes_equal(left->data, right->data, (uint32_t)left->length);
}
