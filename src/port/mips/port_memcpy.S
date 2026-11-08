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
 * port_memcpy.S provides memcpy for 32-bit MIPS with the o32 ABI, as on the
 * big-endian mips-linux-musl target. The code also handles little-endian
 * and, through its own branch, MIPS32 release 6.
 *
 * Source: memcpy.S by MIPS Technologies, kept unchanged beside this file.
 * It comes from newlib, newlib/libc/machine/mips/memcpy.S at commit
 * b097f95233d26da7af6887798297d1efadb75370, under BSD-3-Clause. NOTICE in
 * this folder gives its URL and checksum.
 *
 * Why assembly: GCC and clang emit calls to memcpy, memmove and memset even
 * in freestanding code, so a build without a C library has to define them.
 * C could define them only by declaring names that the C standard reserves,
 * which libterae's C never does. A separate assembly file keeps assembly
 * out of the C code, and this comment records why it is used.
 *
 * Configuration: the file is built without _LIBC and ANDROID_CHANGES, so it
 * takes <regdef.h> and <sys/asm.h> from libterae's shims in this folder, and
 * with DISABLE_PREFETCH, so it emits no prefetch at all. With _LIBC it would
 * prefetch the destination with the "prepare for store" hint, which zeroes a
 * cache line before the copy writes it, assumes 32-byte lines, and destroys
 * source bytes not yet read when the buffers overlap. GCC expects memcpy to
 * work when the destination is the source, so that hint must never be used.
 * The remaining prefetches are only hints, but they reach past the buffers.
 */

#if defined(_LIBC) || defined(ANDROID_CHANGES)
#error "build src/port/mips without _LIBC and ANDROID_CHANGES"
#endif

#if defined(RETURN_FIRST_PREFETCH) || defined(RETURN_LAST_PREFETCH)
#error "RETURN_FIRST_PREFETCH and RETURN_LAST_PREFETCH change the result"
#endif

#ifndef DISABLE_PREFETCH
#define DISABLE_PREFETCH 1
#endif

#include "memcpy.S"

    .section .note.GNU-stack, "", %progbits

/*
 * mqtt_port_memcpy is libterae's own name for memcpy, at the same address.
 * base_string.c calls it when the library is built with MQTT_PORT_MEMORY,
 * as libterae's C may not name memcpy itself.
 */
    .globl mqtt_port_memcpy
    .type mqtt_port_memcpy, %function
    .equ mqtt_port_memcpy, memcpy
