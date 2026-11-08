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
 * memcmp for AArch64.
 *
 * Source: Arm Optimized Routines, string/aarch64/memcmp.S, with
 * string/aarch64/asmdefs.h, both at commit
 * 20859d3fd9e797dde592c489c8ef7d383f778620, taken under the MIT option of
 * their licence. Both sit in this folder unchanged: see NOTICE.
 *
 * Why assembly (MISRA C:2012 Directive 4.2): GCC and clang emit calls to
 * memcpy, memmove, memset and memcmp for struct copies, large initialisers
 * and some loops, even with -ffreestanding, so a build without a C library
 * must define these symbols. C cannot define them without declaring names
 * that the C standard reserves, so they come from assembly files that hold
 * nothing else, as Directive 4.3 asks.
 *
 * What the code needs: ARMv8.0-A with Advanced SIMD, which its loop for 160
 * bytes or more uses, and unaligned access. It loads 8 and 16 bytes at a
 * time from addresses it has not aligned, so alignment checking
 * (SCTLR_ELx.A) must be off, and the memory must be Normal memory with the
 * MMU on.
 *
 * Result: bytes compare as unsigned char, as C requires. The result is -1,
 * 0 or 1, except for lengths 1 and 3, where a difference found only in the
 * last byte gives the difference of the two bytes.
 *
 * Branch protection: the entry starts with a BTI landing pad (hint 34, a NOP
 * on cores before Armv8.5), and asmdefs.h adds a .note.gnu.property section
 * that marks the object as compatible with BTI and PAC. The code is a leaf
 * that never saves the return address, so it needs no PAC instructions. The
 * note is there because WANT_GNU_PROPERTY defaults to 1. A build may pass
 * -DWANT_GNU_PROPERTY=0 to leave it out.
 *
 * Configuration: none is needed, so no macro is set here.
 *
 * Renaming: upstream calls the entry point __memcmp_aarch64, and the define
 * below renames it, as FreeBSD does when it builds these routines.
 */

#define __memcmp_aarch64 memcmp

#include "memcmp.S"

/* Upstream leaves this out. It marks the stack as not executable. */
.section .note.GNU-stack,"",%progbits
