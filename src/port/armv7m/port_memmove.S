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
 * memmove for the Arm M profile with Thumb-2: Armv7-M and Armv7E-M
 * (Cortex-M3, M4, M7) and Armv8-M Mainline (Cortex-M33, M55, M85).
 *
 * Source: arch_memmove.S from Apache NuttX, libs/libc/machine/arm/armv7-m at
 * commit 134ba33ca70120afc82bc8e5798950e232166fdb, kept unchanged in this
 * folder: see NOTICE. It is ARM Ltd's aeabi_memmove-thumb2.S from newlib,
 * under the BSD-3-Clause licence in its header, named memmove and with
 * plain pushes in place of newlib's prologue macros. It includes "libc.h",
 * which here is libterae's own stand-in.
 *
 * Why assembly (MISRA C:2012 Directive 4.2): GCC and clang emit calls to
 * memcpy, memmove, memset and memcmp for struct copies, large initialisers
 * and some loops, even with -ffreestanding, so a build without a C library
 * must define these symbols. C cannot define them without declaring names
 * that the C standard reserves, so they come from assembly files that hold
 * nothing else, as Directive 4.3 asks.
 *
 * What the code needs: Thumb-2, which it sets itself with .thumb, and
 * integer registers only. It copies one byte at a time, so it needs no
 * alignment and works in either byte order, and is slow on large copies.
 *
 * Overlap: it copies backwards when the destination starts inside the
 * source, comparing the addresses as unsigned, and forwards otherwise. It
 * returns the destination, which it never changes in r0.
 *
 * Configuration: LIBC_BUILD_MEMMOVE makes the file hold its code, as
 * NuttX's build does.
 *
 * Renaming: none. ARCH_LIBCFUN in libc.h keeps the name memmove.
 */

#if !defined(__ARM_ARCH_PROFILE) || (__ARM_ARCH_PROFILE != 'M') || \
    (__ARM_ARCH_ISA_THUMB < 2)
#error "src/port/armv7m needs an M-profile Arm with Thumb-2"
#endif

#define LIBC_BUILD_MEMMOVE 1

#include "arch_memmove.S"

/* Upstream leaves this out. It marks the stack as not executable. */
.section .note.GNU-stack,"",%progbits
