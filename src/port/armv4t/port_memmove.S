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
 * memmove for any 32-bit Arm with ARM state, from Armv4T on, in either byte
 * order.
 *
 * Source: newlib, newlib/libc/machine/arm/aeabi_memmove-arm.S at commit
 * b097f95233d26da7af6887798297d1efadb75370, kept unchanged in this folder:
 * see NOTICE. It is ARM Ltd's, under the BSD-3-Clause licence in its
 * header.
 *
 * Why assembly (MISRA C:2012 Directive 4.2): GCC and clang emit calls to
 * memcpy, memmove, memset and memcmp for struct copies, large initialisers
 * and some loops, even with -ffreestanding, so a build without a C library
 * must define these symbols. C cannot define them without declaring names
 * that the C standard reserves, so they come from assembly files that hold
 * nothing else, as Directive 4.3 asks.
 *
 * What the code needs: ARM state, which the file sets itself with .arm, and
 * integer registers only. It copies one byte at a time, so it needs no
 * alignment and works in either byte order, and is slow on large copies.
 * It returns with BX, which interworks, so Thumb callers can use it.
 *
 * Overlap: it copies backwards when the destination starts inside the
 * source, comparing the addresses as unsigned, and forwards otherwise. It
 * never changes r0, so it returns the destination as memmove must, which
 * the run-time ABI's __aeabi_memmove that it was written as doesn't need.
 *
 * Configuration: newlib builds the file from aeabi_memmove-soft.S, which
 * first defines ASM_ALIAS, a macro that the file uses to add the names
 * __aeabi_memmove4 and __aeabi_memmove8. Here ASM_ALIAS is defined to do
 * nothing, as port_aeabi_memcpy.S already gives those names.
 *
 * Renaming: upstream calls the entry point __aeabi_memmove, and the define
 * below renames it.
 */

.macro ASM_ALIAS new old
.endm

#define __aeabi_memmove memmove

#include "aeabi_memmove-arm.S"

/* Upstream leaves this out. It marks the stack as not executable. */
.section .note.GNU-stack,"",%progbits
