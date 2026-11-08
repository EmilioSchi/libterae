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
 * memcpy for any 32-bit Arm with ARM state, from Armv4T on, in either byte
 * order: ARM7TDMI, ARM9, ARM11, and Cortex-R and Cortex-A cores such as the
 * big-endian Cortex-R4F of TI's TMS570 and the little-endian one of its
 * RM4x.
 *
 * Source: musl, src/string/arm/memcpy.S at commit
 * 7813e9119eb5fe681b6750b8a033a0490bca0941, taken from the
 * github.com/kraj/musl mirror and kept unchanged in this folder: see NOTICE.
 * It is the Android Open Source Project's 2008 memcpy, under the
 * BSD-2-Clause licence in its header, which musl changed to return in a way
 * every Arm since Armv4T accepts, to drop its prefetches, and to build for
 * big-endian.
 *
 * Why assembly (MISRA C:2012 Directive 4.2): GCC and clang emit calls to
 * memcpy, memmove, memset and memcmp for struct copies, large initialisers
 * and some loops, even with -ffreestanding, so a build without a C library
 * must define these symbols. C cannot define them without declaring names
 * that the C standard reserves, so they come from assembly files that hold
 * nothing else, as Directive 4.3 asks.
 *
 * What the code needs: ARM state, which the .arm below sets, whatever
 * -marm or -mthumb says, and integer registers only. Every word it loads
 * or stores is aligned: it reads a misaligned source in aligned words and
 * merges them with shifts, in the direction __ARMEB__ gives, so alignment
 * checking may be on. It returns with BX, which interworks, so Thumb
 * callers can use it on Armv4T too.
 *
 * Overlap: it copies forwards, and each store writes bytes it has just
 * loaded from the same offset, so memcpy with dst == src, which GCC
 * requires to work, leaves memory as it was. Any other overlap needs
 * memmove, from port_memmove.S.
 *
 * Unwinding: the file opens an EHABI entry with .fnstart and never closes
 * it, so the assembler emits no unwind table for it and the object needs
 * no personality routine.
 *
 * Configuration: none. Renaming: none, as upstream names it memcpy.
 */

.arm
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
