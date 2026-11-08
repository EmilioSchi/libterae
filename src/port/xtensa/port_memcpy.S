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

/*
 * port_memcpy.S provides memcpy for Xtensa cores, among them Espressif's ESP32
 * (LX6), ESP32-S2 and ESP32-S3 (LX7), in either register ABI: windowed, as
 * ESP-IDF uses, or call0.
 *
 * Source: memcpy.S by Tensilica, kept unchanged beside this file. It comes from
 * newlib, newlib/libc/machine/xtensa/memcpy.S at commit
 * b097f95233d26da7af6887798297d1efadb75370, under the MIT licence in its
 * header. Espressif's newlib-esp32 carries the same bytes. It includes
 * newlib's xtensa-asm.h, also kept unchanged here, which includes
 * <xtensa/config/core-isa.h>: in this folder that is libterae's own
 * stand-in, which says what it sets. NOTICE in this folder gives the URLs
 * and checksums.
 *
 * Why assembly: GCC and clang emit calls to memcpy, memmove, memset and
 * memcmp even in freestanding code, so a build without a C library has to
 * define them. C could define them only by declaring names that the C
 * standard reserves, which libterae's C never does. A separate assembly
 * file keeps assembly out of the C code, and this comment records why it
 * is used.
 *
 * What the code does: It aligns the destination with single-byte and 16-bit
 * stores, then copies 16 bytes at a time. A misaligned source is read in
 * aligned words and merged with the funnel shift (ssa8 and src), in the
 * order __XTENSA_EB__ gives, so the code makes no misaligned access.
 *
 * Overlap: it copies forwards, each store writing what it has just loaded
 * from the same offset, so memcpy with dst == src, which GCC requires to
 * work, leaves memory as it was.
 *
 * Configuration: XTENSA_ESP32_PSRAM_CACHE_FIX stays undefined. ESP-IDF
 * defines it for the first ESP32 revisions used with external PSRAM, whose
 * cache needs a memory barrier between some stores; a build for such a
 * part defines it too. The file lacks the .note.GNU-stack section, so this
 * wrapper adds it.
 */

#include "memcpy.S"

    .section .note.GNU-stack, "", @progbits

/*
 * mqtt_port_memcpy is libterae's own name for memcpy, at the same address.
 * base_string.c calls it when the library is built with MQTT_PORT_MEMORY,
 * as libterae's C may not name memcpy itself.
 */
    .globl mqtt_port_memcpy
    .type mqtt_port_memcpy, @function
    .set mqtt_port_memcpy, memcpy
