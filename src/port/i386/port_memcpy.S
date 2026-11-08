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
 * Provides memcpy and memmove for i386 (System V i386 ABI, cdecl). memcpy
 * is the primary, and names this file.
 *
 * From musl, src/string/i386/memcpy.s and src/string/i386/memmove.s, at
 * commit 5a56ad13e1e0790c06122978aad2a14d6b9efc1a, taken from the
 * github.com/kraj/musl mirror. Both are kept unmodified: they carry no
 * notice, and musl's licence is in NOTICE in this folder.
 *
 * The two share this file because memmove.s isn't complete on its own: when
 * the destination doesn't start inside the source, it jumps to
 * __memcpy_fwd, a hidden second name for memcpy that memcpy.s defines.
 * Assembled together, they leave no symbol undefined. memcpy copies forward,
 * storing each byte where it read it from, so it also works when the source
 * and the destination are the same, as GCC expects.
 *
 * Why assembly: GCC and clang emit calls to memcpy, memmove, memset and
 * memcmp on their own, for struct copies and initialisers, so a build with
 * no C library must define these symbols. libterae's C can't: it would have
 * to declare names that the C standard reserves. A separate assembly file,
 * called only through the C calling convention, defines them instead, and
 * keeps the assembly apart from the C code.
 *
 * musl's files don't say that the stack needn't be executable, so this file
 * does, after them.
 */

#include "memcpy.s"
#include "memmove.s"

	.section .note.GNU-stack, "", @progbits

/*
 * mqtt_port_memcpy is libterae's own name for memcpy, at the same address.
 * base_string.c calls it when the library is built with MQTT_PORT_MEMORY,
 * as libterae's C may not name memcpy itself.
 */
    .globl mqtt_port_memcpy
    .type mqtt_port_memcpy, %function
    .equ mqtt_port_memcpy, memcpy
