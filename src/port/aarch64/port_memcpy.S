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
 * memcpy and memmove for AArch64. One upstream entry point provides both, so
 * this one wrapper defines the two. It is named after memcpy, the primary,
 * and there is no port_memmove.S for this architecture.
 *
 * Source: Arm Optimized Routines, string/aarch64/memcpy-advsimd.S, with
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
 * What the code needs: ARMv8.0-A with Advanced SIMD, and unaligned access,
 * which works only with the MMU on, on Normal memory, with alignment
 * checking (SCTLR_ELx.A) off.
 *
 * Overlap: the file says it handles overlaps, for memmove. Copies of up to
 * 128 bytes load every byte before they store any. Longer copies run
 * backwards when the destination starts inside the source, and return at
 * once when the two are equal. So memcpy with dst == src, which GCC requires
 * to work, leaves memory as it was.
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
 * Renaming: upstream calls the entry points __memcpy_aarch64_simd and
 * __memmove_aarch64_simd, and the defines below rename them, as FreeBSD does
 * when it builds these routines. This works for ELF, whose ENTRY_ALIAS uses
 * the name as given, but not for Mach-O, whose form pastes an underscore on.
 * The memmove label comes just before the 64-byte .align of memcpy's entry,
 * so the two share an address only because nothing goes into .text before
 * the include. Upstream gives memmove no .size, so its symbol has size 0.
 */

#define __memcpy_aarch64_simd memcpy
#define __memmove_aarch64_simd memmove

#include "memcpy-advsimd.S"

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
