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
 * @file base_websocket.c
 * @brief Carrying MQTT over WebSocket.
 *
 * [RFC6455] The WebSocket Protocol.
 * @li Cited: 1.3, 4.1, 4.2.1, 4.2.2, 5.2, 5.3, 11.3.1, 11.3.5.
 * @li Source: www.rfc-editor.org/rfc/rfc6455
 *
 * The hash is SHA-1, as RFC 6455 says, and is not used for anything but this
 * handshake.
 */

#include <stddef.h>

#include "base_string.h"
#include "base_websocket.h"
#include "v5_websocket.h"

/** @brief A space. */
#define WEBSOCKET_SPACE 0x20U
/** @brief A carriage return. */
#define WEBSOCKET_CR 0x0DU
/** @brief A line feed. */
#define WEBSOCKET_LF 0x0AU
/** @brief A horizontal tab. */
#define WEBSOCKET_TAB 0x09U
/** @brief The last control character, which comes after every other. */
#define WEBSOCKET_DELETE 0x7FU
/** @brief The byte of the digit zero. */
#define WEBSOCKET_DIGIT_ZERO 0x30U
/** @brief The byte of the digit nine. */
#define WEBSOCKET_DIGIT_NINE 0x39U
/** @brief The byte of the first capital letter. */
#define WEBSOCKET_CAPITAL_A 0x41U
/** @brief The byte of the last capital letter. */
#define WEBSOCKET_CAPITAL_Z 0x5AU
/** @brief The byte of the first small letter. */
#define WEBSOCKET_SMALL_A 0x61U
/** @brief The byte of the last small letter. */
#define WEBSOCKET_SMALL_Z 0x7AU
/** @brief What takes a capital letter to its lower case form. */
#define WEBSOCKET_CAPITAL_TO_LOWER 0x20U
/** @brief The number of bytes of a key: 16 bytes in base64. */
#define WEBSOCKET_KEY_LENGTH 24U
/** @brief The number of digits of a key, before its two bytes of padding. */
#define WEBSOCKET_KEY_DIGITS 22U
/**
 * @brief The number a part of an HTTP version is counted up to. A larger one
 * is as good, as only how it compares with 1 matters.
 */
#define WEBSOCKET_NUMBER_LIMIT 1000U
/** @brief The number of bytes of the GUID RFC 6455 adds to the key. */
#define WEBSOCKET_GUID_LENGTH 36U
/** @brief More bytes than any header name or word this file looks for. */
#define WEBSOCKET_NAME_MAX 32U
/**
 * @brief The number of bytes the opening handshake hashes: a key, then the
 * GUID.
 */
#define WEBSOCKET_HASHED_LENGTH (WEBSOCKET_KEY_LENGTH + WEBSOCKET_GUID_LENGTH)
/** @brief The number of bytes in a block of SHA-1. */
#define WEBSOCKET_BLOCK_BYTES 64U
/** @brief The number of bytes in a SHA-1 digest. */
#define WEBSOCKET_DIGEST_BYTES 20U
/** @brief The number of bytes in the digest, in base64. */
#define WEBSOCKET_ACCEPT_BYTES 28U
/** @brief The first bits of a final frame. */
#define WEBSOCKET_FINAL 0x80U
/** @brief The bit that says the payload is masked. */
#define WEBSOCKET_MASKED 0x80U
/** @brief The bits of the opcode. */
#define WEBSOCKET_OPCODE_MASK 0x0FU
/** @brief The bits of the length in the second byte. */
#define WEBSOCKET_LENGTH_MASK 0x7FU
/** @brief The length value that says a 16-bit length follows. */
#define WEBSOCKET_LENGTH_16 126U
/** @brief The length value that says a 64-bit length follows. */
#define WEBSOCKET_LENGTH_64 127U
/** @brief The reserved bits, which must be clear with no extension agreed. */
#define WEBSOCKET_RESERVED 0x70U

/**
 * @brief Rotate a 32-bit value to the left.
 * @param[in] value The value.
 * @param[in] count The number of bits, from 1 to 31.
 * @return The result.
 */
static uint32_t websocket_rotate(uint32_t value, uint32_t count)
{
    return (value << count) | (value >> (32U - count));
}

/**
 * @brief Run one 64-byte block through SHA-1.
 * @param[in,out] state The state.
 * @param[in] block The block.
 */
static void websocket_sha1_block(uint32_t *state, const uint8_t *block)
{
    /** @brief The message schedule. Each word is written before it is read. */
    uint32_t words[80];
    /** @brief The working values. */
    uint32_t work[5] = {state[0], state[1], state[2], state[3], state[4]};
    /** @brief Index of the step. */
    uint32_t step = 0U;

    while (step < 16U) {
        /** @brief Index of the word's first byte. */
        const uint32_t at = step * 4U;

        words[step] =
            ((uint32_t)block[at] << 24U) | ((uint32_t)block[at + 1U] << 16U) |
            ((uint32_t)block[at + 2U] << 8U) | (uint32_t)block[at + 3U];
        step += 1U;
    }
    while (step < 80U) {
        words[step] =
            websocket_rotate(words[step - 3U] ^ words[step - 8U] ^
                                 words[step - 14U] ^ words[step - 16U],
                             1U);
        step += 1U;
    }
    step = 0U;
    while (step < 80U) {
        /** @brief The function for this round. */
        uint32_t mixed = work[1] ^ work[2] ^ work[3];
        /** @brief The constant for this round. */
        uint32_t constant = 0xCA62C1D6U;

        if (step < 20U) {
            mixed = (work[1] & work[2]) | ((~work[1]) & work[3]);
            constant = 0x5A827999U;
        } else if (step < 40U) {
            constant = 0x6ED9EBA1U;
        } else if (step < 60U) {
            mixed =
                (work[1] & work[2]) | (work[1] & work[3]) | (work[2] & work[3]);
            constant = 0x8F1BBCDCU;
        } else {
            /* The last twenty rounds use the same function as the second
             * twenty. */
        }
        {
            /** @brief The value that comes in at the front. */
            const uint32_t next = websocket_rotate(work[0], 5U) + mixed +
                                  work[4] + constant + words[step];

            work[4] = work[3];
            work[3] = work[2];
            work[2] = websocket_rotate(work[1], 30U);
            work[1] = work[0];
            work[0] = next;
        }
        step += 1U;
    }
    step = 0U;
    while (step < 5U) {
        state[step] += work[step];
        step += 1U;
    }
}

/**
 * @brief Write the state out as the digest.
 * @param[in] state The state.
 * @param[out] digest The 20 bytes of the digest.
 */
static void websocket_digest_write(const uint32_t *state, uint8_t *digest)
{
    /** @brief Index of the word of the state. */
    uint32_t word;

    for (word = 0U; word < 5U; word += 1U) {
        digest[word * 4U] = (uint8_t)(state[word] >> 24U);
        digest[(word * 4U) + 1U] = (uint8_t)((state[word] >> 16U) & 0xFFU);
        digest[(word * 4U) + 2U] = (uint8_t)((state[word] >> 8U) & 0xFFU);
        digest[(word * 4U) + 3U] = (uint8_t)(state[word] & 0xFFU);
    }
}

/**
 * @brief Hash the key and the GUID with SHA-1, as the opening handshake asks.
 *
 * They are always WEBSOCKET_HASHED_LENGTH bytes, 60, which leaves too little of
 * their block for SHA-1's padding: a byte 0x80, zeros, and the length in bits
 * in the last 8 bytes. So the padding starts in the first block and ends in a
 * second.
 *
 * @param[in] text The WEBSOCKET_HASHED_LENGTH bytes.
 * @param[out] digest The WEBSOCKET_DIGEST_BYTES bytes of the hash.
 */
static void websocket_sha1(const uint8_t *text, uint8_t *digest)
{
    /** @brief The state. */
    uint32_t state[5] = {0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U,
                         0xC3D2E1F0U};
    /** @brief The block being hashed. */
    uint8_t block[WEBSOCKET_BLOCK_BYTES];
    /** @brief The length in bits. */
    const uint32_t bits = WEBSOCKET_HASHED_LENGTH * 8U;
    /** @brief Index of the padding byte being written. */
    uint32_t pad = WEBSOCKET_HASHED_LENGTH + 1U;

    mqtt_bytes_put(block, text, WEBSOCKET_HASHED_LENGTH);
    block[WEBSOCKET_HASHED_LENGTH] = 0x80U;
    while (pad < WEBSOCKET_BLOCK_BYTES) {
        block[pad] = 0U;
        pad += 1U;
    }
    websocket_sha1_block(state, block);
    /* Zeros up to the length, whose top six bytes are zero as well. */
    pad = 0U;
    while (pad < (WEBSOCKET_BLOCK_BYTES - 2U)) {
        block[pad] = 0U;
        pad += 1U;
    }
    block[WEBSOCKET_BLOCK_BYTES - 2U] = (uint8_t)(bits >> 8U);
    block[WEBSOCKET_BLOCK_BYTES - 1U] = (uint8_t)(bits & 0xFFU);
    websocket_sha1_block(state, block);
    websocket_digest_write(state, digest);
}

/**
 * @brief Write bytes in base64.
 * @param[in] data The bytes.
 * @param[in] length Number of bytes, a multiple of 3 plus 2.
 * @param[out] text Where to put the result.
 */
static void websocket_base64(const uint8_t *data, uint32_t length,
                             uint8_t *text)
{
    /** @brief The alphabet. */
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    /** @brief Index of the next byte to read. */
    uint32_t in = 0U;
    /** @brief Index of the next byte to write. */
    uint32_t out = 0U;

    while (in < length) {
        /** @brief The bytes taken: three, or two at the end. */
        const uint32_t second =
            ((in + 1U) < length) ? (uint32_t)data[in + 1U] : 0U;
        /** @brief The last byte of the group, if there is one. */
        const uint32_t third =
            ((in + 2U) < length) ? (uint32_t)data[in + 2U] : 0U;
        /** @brief The group, as one number. */
        const uint32_t group =
            ((uint32_t)data[in] << 16U) | (second << 8U) | third;

        text[out] = (uint8_t)alphabet[(group >> 18U) & 63U];
        text[out + 1U] = (uint8_t)alphabet[(group >> 12U) & 63U];
        text[out + 2U] = (uint8_t)alphabet[(group >> 6U) & 63U];
        text[out + 3U] = ((in + 2U) < length) ? (uint8_t)alphabet[group & 63U] :
                                                (uint8_t)'=';
        in += 3U;
        out += 4U;
    }
}

/**
 * @brief Decide whether a character of a header's name matches, whatever its
 * case.
 * @param[in] byte The byte from the request.
 * @param[in] lower The lower case letter or other character to match.
 * @return true if it matches.
 */
static bool websocket_same(uint8_t byte, char lower)
{
    /** @brief The byte in lower case, if it is a capital letter. */
    const uint32_t folded =
        ((byte >= WEBSOCKET_CAPITAL_A) && (byte <= WEBSOCKET_CAPITAL_Z)) ?
            ((uint32_t)byte + WEBSOCKET_CAPITAL_TO_LOWER) :
            (uint32_t)byte;

    return folded == (uint32_t)(uint8_t)lower;
}

/**
 * @brief Decide whether a byte is a space or a tab, which may stand around a
 * header's value without being part of it.
 * @param[in] byte The byte.
 * @return true if it is.
 */
static bool websocket_blank(uint8_t byte)
{
    return (byte == WEBSOCKET_SPACE) || (byte == WEBSOCKET_TAB);
}

/**
 * @brief Find a header line in the request.
 *
 * RFC 6455 section 1.3 leaves out the spaces around the key's value, and the
 * same is done for every header.
 *
 * @param[in] request The bytes of the HTTP request.
 * @param[in] length Number of bytes in request.
 * @param[in] name The name of the header, with its colon, in lower case.
 * @param[out] start Index where its value starts, after any spaces and tabs.
 * @param[out] end Index where its value ends, before any spaces and tabs.
 * @return true if it was there.
 */
static bool websocket_find_header(const uint8_t *request, uint32_t length,
                                  const char *name, uint32_t *start,
                                  uint32_t *end)
{
    /** @brief Number of bytes in the name. */
    uint32_t name_length = 0U;
    /** @brief The index of the start of a line. */
    uint32_t line = 0U;
    /** @brief The answer. */
    bool found = false;
    /** @brief true once the name is measured, which it always is. */
    const bool named = mqtt_text_length(name, WEBSOCKET_NAME_MAX, &name_length);

    while (named && (!found) && ((line + name_length) < length)) {
        /** @brief Index of the character being compared. */
        uint32_t index = 0U;

        while ((index < name_length) &&
               websocket_same(request[line + index], name[index])) {
            index += 1U;
        }
        if (index == name_length) {
            /** @brief The cursor over the value. */
            uint32_t cursor = line + name_length;

            while ((cursor < length) && websocket_blank(request[cursor])) {
                cursor += 1U;
            }
            *start = cursor;
            while ((cursor < length) && (request[cursor] != WEBSOCKET_CR) &&
                   (request[cursor] != WEBSOCKET_LF)) {
                cursor += 1U;
            }
            while ((cursor > *start) && websocket_blank(request[cursor - 1U])) {
                cursor -= 1U;
            }
            *end = cursor;
            found = true;
        } else {
            /* On to the next line. */
            while ((line < length) && (request[line] != WEBSOCKET_LF)) {
                line += 1U;
            }
            line += 1U;
        }
    }
    return found;
}

/**
 * @brief Find a header that RFC 6455 section 11.3 lets appear only once.
 * @param[in] request The bytes of the HTTP request.
 * @param[in] length Number of bytes in request.
 * @param[in] name The name of the header, with its colon, in lower case.
 * @param[out] start Index where its value starts, after any spaces and tabs.
 * @param[out] end Index where its value ends, before any spaces and tabs.
 * @return true if it was there, and only once.
 */
static bool websocket_find_once(const uint8_t *request, uint32_t length,
                                const char *name, uint32_t *start,
                                uint32_t *end)
{
    /** @brief Where the value of a second one would start. */
    uint32_t again_start = 0U;
    /** @brief Where the value of a second one would end. */
    uint32_t again_end = 0U;
    /** @brief The answer. */
    bool found = websocket_find_header(request, length, name, start, end);

    if (found) {
        /* The end of the first value is inside its line, so no name starts
         * there, and the search goes on with the lines after it. */
        found = !websocket_find_header(&request[*end], length - *end, name,
                                       &again_start, &again_end);
    }
    return found;
}

/**
 * @brief Decide whether a list of words, separated by commas, has one in it.
 * @param[in] list The list.
 * @param[in] length Number of bytes in it.
 * @param[in] name The word. To ignore case, it has to be in lower case.
 * @param[in] ignore_case true to ignore the case of the list.
 * @return true if it has.
 */
static bool websocket_list_has(const uint8_t *list, uint32_t length,
                               const char *name, bool ignore_case)
{
    /** @brief Number of bytes in the name. */
    uint32_t name_length = 0U;
    /** @brief Index of the byte being looked at. */
    uint32_t index = 0U;
    /** @brief Number of bytes of the name this entry has matched so far. */
    uint32_t matched = 0U;
    /** @brief true once this entry cannot be the name. */
    bool different = false;
    /** @brief true once a space has followed this entry's text. */
    bool ended = false;
    /** @brief The answer. */
    bool found = false;
    /** @brief true once the name is measured, which it always is. */
    const bool named = mqtt_text_length(name, WEBSOCKET_NAME_MAX, &name_length);

    while (named && (!found) && (index <= length)) {
        /**
         * @brief The byte, with the end of the list acting as the end of an
         * entry.
         */
        const uint8_t byte = (index < length) ? list[index] : (uint8_t)',';

        if (byte == (uint8_t)',') {
            found = (!different) && (matched == name_length);
            matched = 0U;
            different = false;
            ended = false;
        } else if (websocket_blank(byte)) {
            ended = (matched > 0U) || different;
        } else if (ended || (matched >= name_length)) {
            different = true;
        } else if (ignore_case ? websocket_same(byte, name[matched]) :
                                 (byte == (uint8_t)name[matched])) {
            matched += 1U;
        } else {
            different = true;
        }
        index += 1U;
    }
    return found;
}

/**
 * @brief Decide whether a request has a header with a word in its list.
 * @param[in] request The bytes of the HTTP request.
 * @param[in] length Number of bytes in request.
 * @param[in] name The name of the header, with its colon, in lower case.
 * @param[in] word The word.
 * @param[in] ignore_case true to ignore the case of the list.
 * @param[in] once true if the header may appear only once.
 * @return true if it has.
 */
static bool websocket_header_has(const uint8_t *request, uint32_t length,
                                 const char *name, const char *word,
                                 bool ignore_case, bool once)
{
    /** @brief Where the value starts. */
    uint32_t start = 0U;
    /** @brief Where the value ends. */
    uint32_t end = 0U;
    /** @brief true if the header is there, as often as it may be. */
    bool there = false;
    /** @brief The answer. */
    bool found = false;

    if (once) {
        there = websocket_find_once(request, length, name, &start, &end);
    } else {
        there = websocket_find_header(request, length, name, &start, &end);
    }
    if (there) {
        found =
            websocket_list_has(&request[start], end - start, word, ignore_case);
    }
    return found;
}

/**
 * @brief Decide whether some text comes next in a request, and move past it if
 * it does.
 * @param[in] request The bytes of the HTTP request.
 * @param[in] length Number of bytes in request.
 * @param[in,out] cursor Index of the next byte. It moves past the text if the
 * text is there, and stays otherwise.
 * @param[in] text The text, which has to match exactly.
 * @return true if it is there.
 */
static bool websocket_next_is(const uint8_t *request, uint32_t length,
                              uint32_t *cursor, const char *text)
{
    /** @brief Number of bytes in the text. */
    uint32_t text_length = 0U;
    /** @brief Number of bytes that match. */
    uint32_t index = 0U;
    /** @brief The answer: the text is measured first, which it always is. */
    bool found = mqtt_text_length(text, WEBSOCKET_NAME_MAX, &text_length);

    while (found && (index < text_length) && ((*cursor + index) < length) &&
           (request[*cursor + index] == (uint8_t)text[index])) {
        index += 1U;
    }
    found = found && (index == text_length);
    if (found) {
        *cursor += text_length;
    }
    return found;
}

/**
 * @brief Read the digits of one part of an HTTP version.
 * @param[in] request The bytes of the HTTP request.
 * @param[in] length Number of bytes in request.
 * @param[in,out] cursor Index of the first digit. It moves past the digits.
 * @param[out] value The number, or one of at least WEBSOCKET_NUMBER_LIMIT in
 * place of a larger one.
 * @return true if there was a digit.
 */
static bool websocket_number(const uint8_t *request, uint32_t length,
                             uint32_t *cursor, uint32_t *value)
{
    /** @brief Index of the first digit. */
    const uint32_t first = *cursor;

    *value = 0U;
    while ((*cursor < length) && (request[*cursor] >= WEBSOCKET_DIGIT_ZERO) &&
           (request[*cursor] <= WEBSOCKET_DIGIT_NINE)) {
        if (*value < WEBSOCKET_NUMBER_LIMIT) {
            *value = (*value * 10U) +
                     ((uint32_t)request[*cursor] - WEBSOCKET_DIGIT_ZERO);
        }
        *cursor += 1U;
    }
    return *cursor > first;
}

/**
 * @brief Decide whether a request starts with the line RFC 6455 sections 4.1
 * and 4.2.1 ask for: GET, a Request-URI, and HTTP/1.1 or a later version.
 *
 * Any Request-URI is taken, as MQTT is the only service there is.
 *
 * @param[in] request The bytes of the HTTP request.
 * @param[in] length Number of bytes in request.
 * @return true if it does.
 */
static bool websocket_request_line(const uint8_t *request, uint32_t length)
{
    /** @brief Index of the byte being looked at. */
    uint32_t cursor = 0U;
    /** @brief The part of the version before its dot. */
    uint32_t major = 0U;
    /** @brief The part of the version after its dot. */
    uint32_t minor = 0U;
    /** @brief The answer so far: the method comes first. */
    bool valid = websocket_next_is(request, length, &cursor, "GET ");

    if (valid) {
        /** @brief Index where the Request-URI starts. */
        const uint32_t target = cursor;

        /* It has neither spaces nor control characters. */
        while ((cursor < length) && (request[cursor] > WEBSOCKET_SPACE) &&
               (request[cursor] < WEBSOCKET_DELETE)) {
            cursor += 1U;
        }
        valid = cursor > target;
    }
    if (valid) {
        valid = websocket_next_is(request, length, &cursor, " HTTP/");
    }
    if (valid) {
        valid = websocket_number(request, length, &cursor, &major);
    }
    if (valid) {
        valid = websocket_next_is(request, length, &cursor, ".");
    }
    if (valid) {
        valid = websocket_number(request, length, &cursor, &minor);
    }
    if (valid) {
        valid = websocket_next_is(request, length, &cursor, "\r\n");
    }
    return valid && ((major > 1U) || ((major == 1U) && (minor >= 1U)));
}

/**
 * @brief Decide whether a request has all that RFC 6455 section 4.2.1 asks of
 * an opening, apart from its key.
 *
 * It has to offer MQTT's subprotocol too, which is the one the reply selects:
 * RFC 6455 section 4.2.2 and [MQTT-6.0.0-3], [MQTT-6.0.0-4].
 *
 * @param[in] request The bytes of the HTTP request.
 * @param[in] length Number of bytes in request.
 * @return true if it has.
 */
static bool websocket_is_opening(const uint8_t *request, uint32_t length)
{
    /** @brief Where the value of the Host header starts. */
    uint32_t host_start = 0U;
    /** @brief Where the value of the Host header ends. */
    uint32_t host_end = 0U;
    /** @brief The answer: the request line comes first. */
    bool valid = websocket_request_line(request, length);

    if (valid) {
        /* It names this server, which is not known here, so it only has to
         * be there. */
        valid = websocket_find_header(request, length, "host:", &host_start,
                                      &host_end) &&
                (host_end > host_start);
    }
    if (valid) {
        valid = websocket_header_has(request, length, "upgrade:", "websocket",
                                     true, false);
    }
    if (valid) {
        valid = websocket_header_has(request, length, "connection:", "upgrade",
                                     true, false);
    }
    if (valid) {
        valid = websocket_header_has(request, length, "sec-websocket-version:",
                                     MQTT_WEBSOCKET_VERSION, false, true);
    }
    if (valid) {
        valid = websocket_header_has(request, length, "sec-websocket-protocol:",
                                     MQTT5_WEBSOCKET_SUBPROTOCOL, false, false);
    }
    return valid;
}

/**
 * @brief Find the end of a request's headers, which is an empty line.
 * @param[in] request The bytes of the HTTP request.
 * @param[in] length Number of bytes in request.
 * @return Index of the byte after the empty line, or zero if it has not come.
 */
static uint32_t websocket_headers_end(const uint8_t *request, uint32_t length)
{
    /** @brief Index of the byte after the end of the request's headers. */
    uint32_t over = 0U;
    /** @brief Index being looked at. */
    uint32_t index = 0U;

    while ((over == 0U) && ((index + 3U) < length)) {
        if ((request[index] == WEBSOCKET_CR) &&
            (request[index + 1U] == WEBSOCKET_LF) &&
            (request[index + 2U] == WEBSOCKET_CR) &&
            (request[index + 3U] == WEBSOCKET_LF)) {
            over = index + 4U;
        }
        index += 1U;
    }
    return over;
}

/**
 * @brief Write the reply that accepts an opening.
 * @param[in] key The value of the request's key, WEBSOCKET_KEY_LENGTH bytes
 * that websocket_key_valid has taken.
 * @param[out] reply Where the reply goes.
 * @param[in] capacity Number of bytes reply can hold.
 * @param[out] reply_length Number of bytes in the reply.
 * @return MQTT_OK, or MQTT_BUFFER_TOO_SMALL.
 */
static mqtt_status websocket_reply(const uint8_t *key, uint8_t *reply,
                                   uint32_t capacity, uint32_t *reply_length)
{
    /** @brief The value RFC 6455 adds to the key. */
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    /** @brief The first lines of the reply. */
    static const char head[] =
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Protocol: " MQTT5_WEBSOCKET_SUBPROTOCOL "\r\n"
        "Sec-WebSocket-Accept: ";
    /** @brief The end of the reply. */
    static const char tail[] = "\r\n\r\n";
    /** @brief The text to hash, which is written in full. */
    uint8_t text[WEBSOCKET_HASHED_LENGTH];
    /** @brief The hash, which websocket_sha1 writes in full. */
    uint8_t digest[WEBSOCKET_DIGEST_BYTES];
    /** @brief Number of bytes in the first lines of the reply. */
    const uint32_t head_length = (uint32_t)sizeof(head) - 1U;
    /** @brief Number of bytes in the end of the reply. */
    const uint32_t tail_length = (uint32_t)sizeof(tail) - 1U;
    /** @brief Number of bytes in the reply. */
    const uint32_t total = head_length + WEBSOCKET_ACCEPT_BYTES + tail_length;
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;

    if (total > capacity) {
        status = MQTT_BUFFER_TOO_SMALL;
    } else {
        mqtt_bytes_put(text, key, WEBSOCKET_KEY_LENGTH);
        mqtt_bytes_put(&text[WEBSOCKET_KEY_LENGTH], (const uint8_t *)guid,
                       WEBSOCKET_GUID_LENGTH);
        websocket_sha1(text, digest);
        /* total fits in capacity, so each part does. */
        mqtt_bytes_put(reply, (const uint8_t *)head, head_length);
        websocket_base64(digest, WEBSOCKET_DIGEST_BYTES, &reply[head_length]);
        mqtt_bytes_put(&reply[head_length + WEBSOCKET_ACCEPT_BYTES],
                       (const uint8_t *)tail, tail_length);
        *reply_length = total;
    }
    return status;
}

/**
 * @brief Decide whether a byte is one of the 64 digits of base64.
 * @param[in] byte The byte.
 * @return true if it is.
 */
static bool websocket_base64_digit(uint8_t byte)
{
    return ((byte >= WEBSOCKET_CAPITAL_A) && (byte <= WEBSOCKET_CAPITAL_Z)) ||
           ((byte >= WEBSOCKET_SMALL_A) && (byte <= WEBSOCKET_SMALL_Z)) ||
           ((byte >= WEBSOCKET_DIGIT_ZERO) && (byte <= WEBSOCKET_DIGIT_NINE)) ||
           (byte == (uint8_t)'+') || (byte == (uint8_t)'/');
}

/**
 * @brief Decide whether a key is 16 bytes in base64, as RFC 6455 section
 * 4.2.1 asks: 22 digits, then two bytes of padding.
 *
 * The bits the last digit has to spare are not looked at, as they do not
 * change the 16 bytes. The example key in RFC 6455 section 4.1 has them set.
 *
 * @param[in] key The value of the key.
 * @param[in] length Number of bytes in key.
 * @return true if it is.
 */
static bool websocket_key_valid(const uint8_t *key, uint32_t length)
{
    /** @brief Index of the byte being looked at. */
    uint32_t index = 0U;
    /** @brief The answer so far. */
    bool valid = (length == WEBSOCKET_KEY_LENGTH);

    while (valid && (index < WEBSOCKET_KEY_DIGITS)) {
        valid = websocket_base64_digit(key[index]);
        index += 1U;
    }
    return valid && (key[WEBSOCKET_KEY_DIGITS] == (uint8_t)'=') &&
           (key[WEBSOCKET_KEY_DIGITS + 1U] == (uint8_t)'=');
}

/**
 * @brief Do what mqtt_websocket_accept does, for arguments known to be valid.
 * @param[in] request The bytes of the HTTP request.
 * @param[in] length Number of bytes in request.
 * @param[out] reply Where to put the reply.
 * @param[in] capacity Number of bytes reply can hold.
 * @param[out] reply_length Number of bytes in the reply, on MQTT_OK.
 * @param[out] used Number of bytes the request took, on MQTT_OK.
 * @return MQTT_OK, MQTT_INCOMPLETE if the request has not all arrived, or
 * MQTT_MALFORMED_PACKET.
 */
static mqtt_status websocket_accept_body(const uint8_t *request,
                                         uint32_t length, uint8_t *reply,
                                         uint32_t capacity,
                                         uint32_t *reply_length, uint32_t *used)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_INCOMPLETE;
    /** @brief Index of the byte after the end of the request's headers. */
    const uint32_t over = websocket_headers_end(request, length);

    if (over != 0U) {
        /** @brief Where the key's value starts. */
        uint32_t key_start = 0U;
        /** @brief Where the key's value ends. */
        uint32_t key_end = 0U;

        /** @brief true if the request is an opening, and it has a key. */
        bool found = websocket_is_opening(request, over);

        status = MQTT_MALFORMED_PACKET;
        if (found) {
            found = websocket_find_once(request, over, "sec-websocket-key:",
                                        &key_start, &key_end);
        }
        if (found &&
            websocket_key_valid(&request[key_start], key_end - key_start)) {
            status = websocket_reply(&request[key_start], reply, capacity,
                                     reply_length);
            if (status == MQTT_OK) {
                *used = over;
            }
        }
    }
    return status;
}

mqtt_status mqtt_websocket_accept(const uint8_t *request, uint32_t length,
                                  uint8_t *reply, uint32_t capacity,
                                  uint32_t *reply_length, uint32_t *used)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if (((request != NULL) || (length == 0U)) && ((reply != NULL) ||
        (capacity == 0U)) && (reply_length != NULL) && (used != NULL)) {
        answer = websocket_accept_body(request, length, reply, capacity,
                                       reply_length, used);
    }
    return answer;
}

/**
 * @brief Do what mqtt_websocket_frame_read does, for arguments known to be
 * valid.
 * @param[in] data The bytes.
 * @param[in] length Number of bytes in data.
 * @param[out] frame The result, on MQTT_OK.
 * @return MQTT_OK, MQTT_INCOMPLETE, or MQTT_MALFORMED_PACKET.
 */
static mqtt_status websocket_frame_read_body(const uint8_t *data,
                                             uint32_t length,
                                             mqtt_websocket_frame *frame)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_INCOMPLETE;

    if (length >= 2U) {
        /** @brief The length field in the second byte. */
        const uint32_t code = (uint32_t)data[1] & WEBSOCKET_LENGTH_MASK;
        /** @brief Number of bytes the length takes after the second byte. */
        uint32_t extra = 0U;

        if (code == WEBSOCKET_LENGTH_16) {
            extra = 2U;
        } else if (code == WEBSOCKET_LENGTH_64) {
            extra = 8U;
        } else {
            /* The length is in the second byte. */
        }
        {
            /** @brief true if the payload is masked. */
            const bool masked = ((uint32_t)data[1] & WEBSOCKET_MASKED) != 0U;
            /** @brief Number of bytes in the header. */
            const uint32_t size = 2U + extra + (masked ? 4U : 0U);

            if (((uint32_t)data[0] & WEBSOCKET_RESERVED) != 0U) {
                status = MQTT_MALFORMED_PACKET;
            } else if (length >= size) {
                /** @brief The payload length so far. */
                uint64_t payload = (uint64_t)code;

                if (extra > 0U) {
                    /** @brief Index of the byte being read. */
                    uint32_t index = 0U;

                    payload = 0U;
                    while (index < extra) {
                        payload = (payload << 8U) | (uint64_t)data[2U + index];
                        index += 1U;
                    }
                }
                frame->opcode =
                    (uint8_t)((uint32_t)data[0] & WEBSOCKET_OPCODE_MASK);
                frame->final = ((uint32_t)data[0] & WEBSOCKET_FINAL) != 0U;
                frame->masked = masked;
                frame->size = size;
                frame->payload_length = payload;
                if (masked) {
                    /* The header was found to hold the four bytes. */
                    mqtt_bytes_put(frame->mask, &data[2U + extra], 4U);
                }
                /* A length takes the fewest bytes it can, and its top bit is
                 * zero: section 5.2. */
                if (((extra == 2U) &&
                     (payload < (uint64_t)WEBSOCKET_LENGTH_16)) ||
                    ((extra == 8U) && (payload <= (uint64_t)UINT16_MAX)) ||
                    ((payload >> 63U) != 0U)) {
                    status = MQTT_MALFORMED_PACKET;
                } else {
                    status = MQTT_OK;
                }
            } else {
                /* The rest of the header has not arrived. */
            }
        }
    }
    return status;
}

mqtt_status mqtt_websocket_frame_read(const uint8_t *data, uint32_t length,
                                      mqtt_websocket_frame *frame)
{
    /** @brief The answer. */
    mqtt_status answer = MQTT_INVALID_ARGUMENT;

    if ((frame != NULL) && ((data != NULL) || (length == 0U))) {
        answer = websocket_frame_read_body(data, length, frame);
    }
    return answer;
}

/**
 * @brief Do what mqtt_websocket_frame_write does, for arguments known to be
 * valid.
 * @param[out] header Where to put it, with room for MQTT_WEBSOCKET_HEADER_MAX
 * bytes.
 * @param[in] opcode The opcode.
 * @param[in] payload_length Number of bytes in the payload.
 * @return Number of bytes in the header.
 */
static uint32_t websocket_frame_write_body(uint8_t *header, uint8_t opcode,
                                           uint64_t payload_length)
{
    /** @brief Number of bytes in the header. */
    uint32_t size = 2U;

    header[0] = (uint8_t)(WEBSOCKET_FINAL | (uint32_t)opcode);
    if (payload_length < WEBSOCKET_LENGTH_16) {
        header[1] = (uint8_t)payload_length;
    } else if (payload_length <= (uint64_t)UINT16_MAX) {
        header[1] = (uint8_t)WEBSOCKET_LENGTH_16;
        header[2] = (uint8_t)(payload_length >> 8U);
        header[3] = (uint8_t)(payload_length & 0xFFU);
        size = 4U;
    } else {
        /** @brief Index of the byte being written. */
        uint32_t index = 0U;

        header[1] = (uint8_t)WEBSOCKET_LENGTH_64;
        while (index < 8U) {
            /** @brief Where this byte is in the length. */
            const uint32_t shift = 56U - (index * 8U);

            header[2U + index] = (uint8_t)((payload_length >> shift) & 0xFFU);
            index += 1U;
        }
        size = 10U;
    }
    return size;
}

uint32_t mqtt_websocket_frame_write(uint8_t *header, uint8_t opcode,
                                    uint64_t payload_length)
{
    /** @brief The answer. */
    uint32_t answer = 0U;

    if (header != NULL) {
        answer = websocket_frame_write_body(header, opcode, payload_length);
    }
    return answer;
}

/**
 * @brief Do what mqtt_websocket_unmask does, for arguments known to be valid.
 * @param[in,out] data The bytes.
 * @param[in] length Number of bytes.
 * @param[in] mask The frame's mask.
 * @param[in,out] position Where in the payload these bytes start. It moves on
 * by length.
 */
static void websocket_unmask_body(uint8_t *data, uint32_t length,
                                  const uint8_t *mask, uint32_t *position)
{
    /** @brief Where in the payload the byte is. */
    uint32_t at = *position;
    /** @brief Index of the byte. */
    uint32_t index = 0U;

    while (index < length) {
        data[index] =
            (uint8_t)((uint32_t)data[index] ^ (uint32_t)mask[at & 3U]);
        at += 1U;
        index += 1U;
    }
    *position = at;
}

void mqtt_websocket_unmask(uint8_t *data, uint32_t length, const uint8_t *mask,
                           uint32_t *position)
{
    if ((position != NULL) && (mask != NULL) && ((data != NULL) ||
        (length == 0U))) {
        websocket_unmask_body(data, length, mask, position);
    }
}
