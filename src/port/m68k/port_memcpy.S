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
 * port_memcpy.S provides memcpy for the Motorola 68000 family and NXP's
 * ColdFire, with the arguments on the stack as GCC passes them.
 *
 * Source: memcpy.S by mocom software, kept unchanged beside this file. It
 * comes from picolibc, libc/machine/m68k/memcpy.S at commit
 * 29e3581fa0e921e378a6aad08c9374b097ae56c8, under the permissive notice in
 * its header. It includes <picolibc.h> and "m68kasm.h", which here are
 * libterae's own stand-ins. NOTICE in this folder gives its URL and
 * checksum, and why picolibc's copy is taken rather than newlib's.
 *
 * Why assembly: GCC and clang emit calls to memcpy, memmove, memset and
 * memcmp even in freestanding code, so a build without a C library has to
 * define them. C could define them only by declaring names that the C
 * standard reserves, which libterae's C never does. A separate assembly
 * file keeps assembly out of the C code, and this comment records why it
 * is used.
 *
 * What the code needs: on ColdFire (__mcoldfire__) and the 68020 to 68060
 * it aligns the destination and copies long words, as those processors
 * accept a misaligned source. On the 68000 and 68010 it copies long words
 * only when both pointers are aligned to 4 bytes, and single bytes
 * otherwise. ColdFire has no dbra, and there the loops count with subq.l.
 * It returns the destination in both d0 and a0, so callers of either
 * convention find it.
 *
 * Overlap: it copies forwards, each store writing what it has just loaded
 * from the same offset, so memcpy with dst == src, which GCC requires to
 * work, leaves memory as it was.
 *
 * Configuration: none. The file marks the stack as not executable itself.
 */

#include "memcpy.S"

/*
 * mqtt_port_memcpy is libterae's own name for memcpy, at the same address.
 * base_string.c calls it when the library is built with MQTT_PORT_MEMORY,
 * as libterae's C may not name memcpy itself.
 */
    .globl SYM(mqtt_port_memcpy)
    .type SYM(mqtt_port_memcpy), @function
    .set SYM(mqtt_port_memcpy), SYM(memcpy)
