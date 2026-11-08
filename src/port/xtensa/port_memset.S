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
 * port_memset.S provides memset for Xtensa cores, among them Espressif's ESP32
 * (LX6), ESP32-S2 and ESP32-S3 (LX7), in either register ABI: windowed, as
 * ESP-IDF uses, or call0.
 *
 * Source: memset.S by Tensilica, kept unchanged beside this file. It comes from
 * newlib, newlib/libc/machine/xtensa/memset.S at commit
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
 * What the code does: it stores a byte and a 16-bit half as needed until the
 * destination is aligned to 4 bytes, then 16 bytes at a time, and finishes
 * with 8-, 4-, 2- and 1-byte stores, so it makes no misaligned store.
 *
 * Configuration: XTENSA_ESP32_PSRAM_CACHE_FIX stays undefined. ESP-IDF
 * defines it for the first ESP32 revisions used with external PSRAM, whose
 * cache needs a memory barrier between some stores; a build for such a
 * part defines it too. The file lacks the .note.GNU-stack section, so this
 * wrapper adds it.
 */

#include "memset.S"

    .section .note.GNU-stack, "", @progbits
