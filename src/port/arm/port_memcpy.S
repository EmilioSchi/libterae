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
 * memcpy for 32-bit Arm: ARMv7-A, with the hard-float EABI of
 * armv7-linux-musleabihf.
 *
 * Source: Arm Optimized Routines, string/arm/memcpy.S, with
 * string/arm/asmdefs.h, both at commit
 * 20859d3fd9e797dde592c489c8ef7d383f778620, taken under the MIT option of
 * their licence. Both sit in this folder unchanged: see NOTICE. This
 * asmdefs.h is AOR's header for 32-bit Arm. AOR's AArch64 header has the
 * same name and sits in src/port/aarch64/. memcpy.S includes it in quotes,
 * which finds the one in its own folder first, so the two never meet.
 *
 * Why assembly (MISRA C:2012 Directive 4.2): GCC and clang emit calls to
 * memcpy, memmove, memset and memcmp for struct copies, large initialisers
 * and some loops, even with -ffreestanding, so a build without a C library
 * must define these symbols. C cannot define them without declaring names
 * that the C standard reserves, so they come from assembly files that hold
 * nothing else, as Directive 4.3 asks.
 *
 * What the code needs: ARMv6 or later, in ARM state, which the file sets
 * itself with .arm. Thumb callers reach it through interworking. It loads
 * and stores words at unaligned addresses, so alignment checking (SCTLR.A)
 * must be off. It picks its code from the compiler's macros: NEON when
 * __ARM_NEON__ is defined, as clang does by default for this target, VFP
 * when floating-point instructions are allowed without NEON, and integer
 * registers otherwise. The VFP and NEON code uses only d0 to d7, which the
 * procedure call standard lets a function change.
 *
 * Overlap: it copies forwards only, and each store writes bytes it has just
 * loaded from the same offset. So memcpy with dst == src, which GCC requires
 * to work, leaves memory as it was. Any other overlap needs memmove, from
 * port_memmove.S.
 *
 * Branch protection: asmdefs.h emits BTI and PAC instructions only when the
 * compiler defines __ARM_FEATURE_BTI_DEFAULT or __ARM_FEATURE_PAC_DEFAULT,
 * which happens only for M-profile targets. ARMv7-A has neither feature, so
 * the object has none and needs none.
 *
 * Unwinding: ENTRY and END in asmdefs.h give the function DWARF CFI and an
 * EHABI unwind entry, which IS_LEAF below marks as one that cannot unwind.
 *
 * Configuration: IS_LEAF is defined, so END adds .cantunwind. Without it,
 * the EHABI entry is a compact one, and the assembler makes the object
 * refer to its personality routine, __aeabi_unwind_cpp_pr0, which comes
 * from an unwinder library that a build without a C library may not have.
 * The function is a leaf that calls nothing, and the entry wouldn't be
 * right anyway, since the function moves the stack pointer without telling
 * EHABI. BROKEN_ASM_NEON_ALIGN, a workaround for old releases of GNU as,
 * stays undefined.
 *
 * Renaming: upstream calls the entry point __memcpy_arm, and the define
 * below renames it.
 */

#define IS_LEAF 1

#define __memcpy_arm memcpy

#include "memcpy.S"

/* Upstream leaves this out. It marks the stack as not executable. */
.section .note.GNU-stack,"",%progbits

/*
 * mqtt_port_memcpy is libterae's own name for memcpy, at the same address.
 * base_string.c calls it when the library is built with MQTT_PORT_MEMORY,
 * as libterae's C may not name memcpy itself.
 */
    .globl mqtt_port_memcpy
    .type mqtt_port_memcpy, %function
    .equ mqtt_port_memcpy, memcpy
