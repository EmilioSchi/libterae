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
 * @file v311_utf8.h
 * @brief Code points in UTF-8 encoded strings.
 *
 * Encoded strings MUST NOT contain code points U+D800 to U+DFFF, or U+0000
 * (null). They SHOULD NOT contain the control characters listed here, or
 * anything Unicode defines as a non-character. The bytes 0xEF 0xBB 0xBF always
 * mean U+FEFF, and a packet receiver MUST NOT ignore or remove them.
 *
 * Unicode has 66 non-characters: a run of 32, and the last two code points of
 * each of the 17 planes. A code point is one of those last two if ANDing it
 * with MQTT311_UTF8_NONCHARACTER_PLANE_END gives that same value.
 *
 * [V311] MQTT Version 3.1.1 Plus Errata 01, OASIS Standard, 10 December 2015,
 * 81 pages.
 * @li Cited: 1.5.3, p.13-14.
 * @li Source: docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/
 *
 * [UCD] Unicode Character Database, PropList.txt, version 18.0.0, 7 August
 * 2026.
 * @li Cited: "Noncharacter_Code_Point".
 * @li Source: www.unicode.org/Public/UCD/latest/ucd/PropList.txt
 */
#ifndef MQTT_V311_UTF8_H
#define MQTT_V311_UTF8_H

/**
 * @brief First of the surrogate code points, which strings must not contain.
 */
#define MQTT311_UTF8_SURROGATE_MIN          0xD800U
/** @brief Last of the surrogate code points, which strings must not contain. */
#define MQTT311_UTF8_SURROGATE_MAX          0xDFFFU
/** @brief Null code point, which strings must not contain. */
#define MQTT311_UTF8_NULL                   0x0000U
/** @brief First control character in the lower range. */
#define MQTT311_UTF8_C0_MIN                 0x0001U
/** @brief Last control character in the lower range. */
#define MQTT311_UTF8_C0_MAX                 0x001FU
/** @brief First control character in the upper range. */
#define MQTT311_UTF8_C1_MIN                 0x007FU
/** @brief Last control character in the upper range. */
#define MQTT311_UTF8_C1_MAX                 0x009FU
/** @brief First of the run of non-characters. */
#define MQTT311_UTF8_NONCHARACTER_MIN       0xFDD0U
/** @brief Last of the run of non-characters. */
#define MQTT311_UTF8_NONCHARACTER_MAX       0xFDEFU
/** @brief Bits that are set in the last two code points of every plane. */
#define MQTT311_UTF8_NONCHARACTER_PLANE_END 0xFFFEU
/** @brief ZERO WIDTH NO-BREAK SPACE. */
#define MQTT311_UTF8_ZWNBSP                 0xFEFFU

#endif /* MQTT_V311_UTF8_H */
