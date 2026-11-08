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
 * memmove for 32-bit Arm: ARMv7-A, with the hard-float EABI of
 * armv7-linux-musleabihf.
 *
 * Source: Android's bionic, libc/arch-arm/generic/bionic/memmove.S at commit
 * 731631f300090436d7f5df80d50b6275c8c60a93, under the BSD-2-Clause licence
 * in its header. It sits in this folder unchanged as memmove.S: see NOTICE.
 * It includes <private/bionic_asm.h>, which here is libterae's own
 * private/bionic_asm.h, because bionic's pulls in Linux and bionic headers.
 *
 * Why assembly (MISRA C:2012 Directive 4.2): GCC and clang emit calls to
 * memcpy, memmove, memset and memcmp for struct copies, large initialisers
 * and some loops, even with -ffreestanding, so a build without a C library
 * must define these symbols. C cannot define them without declaring names
 * that the C standard reserves, so they come from assembly files that hold
 * nothing else, as Directive 4.3 asks.
 *
 * What the code needs: ARM state and little-endian byte order, with integer
 * registers only. It has conditional instructions outside IT blocks, which
 * only ARM state accepts, and it sets no state itself, so the .arm below
 * sets it, whatever -marm or -mthumb says. It returns by loading pc with
 * LDM, which interworks, so Thumb callers can use it. Every word it loads
 * or stores is aligned: it reads a misaligned source in aligned words and
 * merges them with shifts, which assume little-endian order.
 *
 * Overlap: it copies backwards when the destination lies above the source,
 * comparing the addresses as unsigned, and forwards otherwise. It returns at
 * once when the two are equal.
 *
 * Unwinding: the macros in private/bionic_asm.h give each function DWARF CFI,
 * which describes only the state at entry, as in bionic, because the file
 * doesn't describe the registers it pushes. They also give each function an
 * EHABI entry that says it cannot be unwound, so that the object needs no
 * personality routine: see private/bionic_asm.h.
 *
 * Configuration: none is needed, so no macro is set here.
 *
 * Renaming: upstream calls the entry point memmove_generic, and the define
 * below renames it. The copying is done by a helper, bsd_safe_memcpy, which
 * private/bionic_asm.h keeps local to the object, so memmove is the only
 * symbol it exports.
 */

.arm

#define memmove_generic memmove

#include "memmove.S"

/* Upstream leaves this out. It marks the stack as not executable. */
.section .note.GNU-stack,"",%progbits
