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
 * @file base_utf8_check.c
 * @brief Checking the characters of a UTF-8 Encoded String.
 *
 * [RFC3629] UTF-8, a transformation format of ISO 10646.
 * @li Cited: 4, the table of well-formed sequences.
 * @li Source: www.rfc-editor.org/rfc/rfc3629
 */

#include <stddef.h>

#include "base_utf8_check.h"

/**
 * @brief Decide whether a character is one a sender should not use, which a
 * receiver may refuse.
 * @param[in] data The string.
 * @param[in] start Index of the character's first byte.
 * @param[in] size Number of bytes in the character, which has been found to be
 * well-formed.
 * @return true if it is a control character or a non-character.
 */
static bool utf8_is_unwanted(const uint8_t *data, uint32_t start, uint32_t size)
{
    /** @brief The character's first byte. */
    const uint32_t first = (uint32_t)data[start];
    /** @brief The character's second byte, for a character that has one. */
    const uint32_t second = (size > 1U) ? (uint32_t)data[start + 1U] : 0U;
    /** @brief The character's last byte. */
    const uint32_t last = (uint32_t)data[start + (size - 1U)];
    /** @brief The answer. */
    bool unwanted = false;

    if (size == 2U) {
        /* U+0080 to U+009F */
        unwanted = (first == 0xC2U) && (second <= 0x9FU);
    } else if (size == 3U) {
        /* U+FDD0 to U+FDEF, and U+FFFE and U+FFFF */
        unwanted = ((first == 0xEFU) && (second == 0xB7U) && (last >= 0x90U) &&
                    (last <= 0xAFU)) ||
                   ((first == 0xEFU) && (second == 0xBFU) && (last >= 0xBEU));
    } else if (size == 4U) {
        /* The last two values of each plane above the first. */
        unwanted = ((second & 0x0FU) == 0x0FU) &&
                   ((uint32_t)data[start + 2U] == 0xBFU) && (last >= 0xBEU);
    } else {
        /* U+0001 to U+001F, and U+007F */
        unwanted = (first < 0x20U) || (first == 0x7FU);
    }
    return unwanted;
}

/**
 * @brief Check a multi-byte sequence.
 * @param[in] data The string.
 * @param[in] length Number of bytes in data.
 * @param[in] start Index of the sequence's first byte, which is not ASCII.
 * @return Number of bytes in the sequence, or 0U if it is not well-formed.
 */
static uint32_t utf8_sequence_size(const uint8_t *data, uint32_t length,
                                   uint32_t start)
{
    /** @brief The first byte of the sequence. */
    const uint32_t first = (uint32_t)data[start];
    /**
     * @brief Number of bytes the first byte announces, or 0U if it is not a
     * valid start.
     */
    uint32_t size = 0U;
    /** @brief Lowest value allowed for the second byte. */
    uint32_t low = 0x80U;
    /** @brief Highest value allowed for the second byte. */
    uint32_t high = 0xBFU;

    if ((first >= 0xC2U) && (first <= 0xDFU)) {
        size = 2U;
    } else if ((first >= 0xE0U) && (first <= 0xEFU)) {
        size = 3U;
        if (first == 0xE0U) {
            low = 0xA0U;
        } else if (first == 0xEDU) {
            high = 0x9FU;
        } else {
            /* The other three-byte starts have the full range. */
        }
    } else if ((first >= 0xF0U) && (first <= 0xF4U)) {
        size = 4U;
        if (first == 0xF0U) {
            low = 0x90U;
        } else if (first == 0xF4U) {
            high = 0x8FU;
        } else {
            /* The other four-byte starts have the full range. */
        }
    } else {
        /* A continuation byte, an overlong start, or a byte beyond the Unicode
         * range. */
    }
    if ((size > 0U) && (size > (length - start))) {
        size = 0U;
    }
    if (size > 0U) {
        /** @brief The second byte of the sequence. */
        const uint32_t second = (uint32_t)data[start + 1U];

        if ((second < low) || (second > high)) {
            size = 0U;
        }
    }
    if (size > 2U) {
        if (((uint32_t)data[start + 2U] & 0xC0U) != 0x80U) {
            size = 0U;
        }
    }
    if (size > 3U) {
        if (((uint32_t)data[start + 3U] & 0xC0U) != 0x80U) {
            size = 0U;
        }
    }
    return size;
}

uint32_t mqtt_utf8_character_count(const uint8_t *data, uint32_t length)
{
    /** @brief Number of characters so far. */
    uint32_t count = 0U;
    /** @brief Index of the byte being looked at. */
    uint32_t index = 0U;
    /** @brief Number of bytes to look at: none if there are no bytes. */
    const uint32_t end = (data != NULL) ? length : 0U;

    while (index < end) {
        /* Every byte of a character but the first has 10 as its top two
         * bits. */
        if (((uint32_t)data[index] & 0xC0U) != 0x80U) {
            count += 1U;
        }
        index += 1U;
    }
    return count;
}

/**
 * @brief Check the bytes of a string.
 * @param[in] data The bytes.
 * @param[in] length Number of bytes in data.
 * @param[in] strict true to refuse the null character, and the characters a
 * receiver may refuse.
 * @return true if they pass.
 */
static bool utf8_check(const uint8_t *data, uint32_t length, bool strict)
{
    /** @brief The answer so far: false for a length with no bytes. */
    bool valid = (data != NULL) || (length == 0U);
    /** @brief Index of the next byte to look at. */
    uint32_t index = 0U;

    while (valid && (index < length)) {
        /** @brief The next byte. */
        const uint8_t byte = data[index];

        if (byte == 0U) {
            valid = !strict;
            index += 1U;
        } else if (byte < 0x80U) {
            valid = (!strict) || (!utf8_is_unwanted(data, index, 1U));
            index += 1U;
        } else {
            /** @brief Number of bytes in the sequence that starts here. */
            const uint32_t size = utf8_sequence_size(data, length, index);

            if (size == 0U) {
                valid = false;
            } else {
                valid = (!strict) || (!utf8_is_unwanted(data, index, size));
                index += size;
            }
        }
    }
    return valid;
}

bool mqtt_utf8_is_valid(const uint8_t *data, uint32_t length)
{
    return utf8_check(data, length, true);
}

bool mqtt_utf8_is_well_formed(const uint8_t *data, uint32_t length)
{
    return utf8_check(data, length, false);
}
