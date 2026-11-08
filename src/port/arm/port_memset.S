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
 * memset for 32-bit Arm: ARMv7-A, with the hard-float EABI of
 * armv7-linux-musleabihf.
 *
 * Source: Arm Optimized Routines, string/arm/memset.S at commit
 * 20859d3fd9e797dde592c489c8ef7d383f778620, taken under the MIT option of
 * its licence. It includes no header, and sits in this folder unchanged:
 * see NOTICE.
 *
 * Why assembly (MISRA C:2012 Directive 4.2): GCC and clang emit calls to
 * memcpy, memmove, memset and memcmp for struct copies, large initialisers
 * and some loops, even with -ffreestanding, so a build without a C library
 * must define these symbols. C cannot define them without declaring names
 * that the C standard reserves, so they come from assembly files that hold
 * nothing else, as Directive 4.3 asks.
 *
 * What the code needs: ARMv7-A in Thumb state, which the file sets itself
 * with .thumb. It marks its entry as a Thumb function, so ARM-state callers
 * reach it through interworking. It uses integer registers only. It stores
 * single bytes until the pointer is 8-byte aligned and then whole aligned
 * words, so it needs no unaligned access.
 *
 * Branch protection: ARMv7-A has no BTI or PAC, and the file has none.
 *
 * Unwinding: the file has no DWARF CFI and no EHABI unwind entry, so a
 * debugger cannot unwind from inside it.
 *
 * Configuration: none is needed, so no macro is set here.
 *
 * Renaming: upstream calls the entry point __memset_arm, and the define
 * below renames it.
 */

#define __memset_arm memset

#include "memset.S"

/* Upstream leaves this out. It marks the stack as not executable. */
.section .note.GNU-stack,"",%progbits
