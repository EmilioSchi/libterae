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
 * memset for the Arm M profile with Thumb-2: Armv7-M and Armv7E-M
 * (Cortex-M3, M4, M7) and Armv8-M Mainline (Cortex-M33, M55, M85).
 *
 * Source: arch_memset.S from Apache NuttX, libs/libc/machine/arm/armv7-m at
 * commit 134ba33ca70120afc82bc8e5798950e232166fdb, kept unchanged in this
 * folder: see NOTICE. It is the Android Open Source Project's 2008 memset
 * for Arm, under the BSD-2-Clause licence in its header, which NuttX
 * assembles as Thumb-2. It includes "libc.h", which here is libterae's own
 * stand-in.
 *
 * Why assembly (MISRA C:2012 Directive 4.2): GCC and clang emit calls to
 * memcpy, memmove, memset and memcmp for struct copies, large initialisers
 * and some loops, even with -ffreestanding, so a build without a C library
 * must define these symbols. C cannot define them without declaring names
 * that the C standard reserves, so they come from assembly files that hold
 * nothing else, as Directive 4.3 asks.
 *
 * What the code needs: Thumb-2, which it sets itself with .thumb, and
 * integer registers only. It stores single bytes until the pointer is word
 * aligned, then aligned words, and the last few bytes as a halfword and a
 * byte, so it needs no unaligned access. Every byte of a word holds the same
 * value, so it works in either byte order. It returns by popping pc.
 *
 * Assembler option: the file has conditional instructions outside IT
 * blocks, written for ARM state. Thumb-2 needs an IT instruction before
 * each, which the assembler adds itself when given -mimplicit-it=always.
 * port.mk in this folder passes it, as -Wa,-mimplicit-it=always, which GNU
 * as and clang both take.
 *
 * Configuration: LIBC_BUILD_MEMSET makes the file hold its code, as NuttX's
 * build does.
 *
 * Renaming: none. ARCH_LIBCFUN in libc.h keeps the name memset.
 */

#if !defined(__ARM_ARCH_PROFILE) || (__ARM_ARCH_PROFILE != 'M') || \
    (__ARM_ARCH_ISA_THUMB < 2)
#error "src/port/armv7m needs an M-profile Arm with Thumb-2"
#endif

#define LIBC_BUILD_MEMSET 1

#include "arch_memset.S"

/* Upstream leaves this out. It marks the stack as not executable. */
.section .note.GNU-stack,"",%progbits
