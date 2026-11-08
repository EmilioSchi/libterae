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
 * port_memcpy.S provides memcpy for 64-bit PowerPC with the ELFv2 ABI, as on
 * the big-endian powerpc64-linux-musl target. It needs only the base 64-bit
 * instruction set.
 *
 * Source: memcpy.S by Instituto de Pesquisas Eldorado, kept unchanged beside
 * this file. It comes from FreeBSD, lib/libc/powerpc64/string/memcpy.S at
 * commit 2321b3df1f0db4f839cb9dde7c0a9a0060d4bcfe, under BSD-3-Clause.
 * NOTICE in this folder gives its URL and checksum. This is the scalar
 * version: FreeBSD's memcpy_vsx.S and its run-time selection are not used.
 *
 * Why assembly: GCC and clang emit calls to memcpy, memmove and memset even
 * in freestanding code, so a build without a C library has to define them.
 * C could define them only by declaring names that the C standard reserves,
 * which libterae's C never does. A separate assembly file keeps assembly
 * out of the C code, and this comment records why it is used.
 *
 * Configuration: none. The file defines __memcpy and makes memcpy an alias
 * of it through WEAK_REFERENCE, from libterae's ELFv2 <machine/asm.h> in
 * this folder. It already ends with the .note.GNU-stack section.
 */

#include "memcpy.S"

/*
 * mqtt_port_memcpy is libterae's own name for memcpy, at the same address.
 * base_string.c calls it when the library is built with MQTT_PORT_MEMORY,
 * as libterae's C may not name memcpy itself.
 */
    .globl mqtt_port_memcpy
    .type mqtt_port_memcpy, %function
    .equ mqtt_port_memcpy, memcpy
