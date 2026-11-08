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
 * memcpy for the Arm M profile with Thumb-2: Armv7-M and Armv7E-M
 * (Cortex-M3, M4, M7) and Armv8-M Mainline (Cortex-M33, M55, M85).
 *
 * Source: arch_memcpy.S from Apache NuttX, libs/libc/machine/arm/armv7-m at
 * commit 134ba33ca70120afc82bc8e5798950e232166fdb, kept unchanged in this
 * folder: see NOTICE. It is ARM Ltd's memcpy-armv7m.S from newlib, under
 * the BSD-3-Clause licence in its header, with NuttX's naming macro in
 * place of newlib's headers. It includes "libc.h", which here is libterae's
 * own stand-in.
 *
 * Why assembly (MISRA C:2012 Directive 4.2): GCC and clang emit calls to
 * memcpy, memmove, memset and memcmp for struct copies, large initialisers
 * and some loops, even with -ffreestanding, so a build without a C library
 * must define these symbols. C cannot define them without declaring names
 * that the C standard reserves, so they come from assembly files that hold
 * nothing else, as Directive 4.3 asks.
 *
 * What the code needs: Thumb-2, which it sets itself with .thumb, and
 * integer registers only. Where the compiler defines
 * __ARM_FEATURE_UNALIGNED, as it does for these cores unless given
 * -mno-unaligned-access, it loads and stores words at unaligned addresses,
 * so the core must not trap them: CCR.UNALIGN_TRP must be clear, as it is
 * out of reset. Otherwise it copies a misaligned source a byte at a time.
 * It reads __ARM_ARCH_7EM__ only to pick between two equivalent loops, and
 * __ARM_BIG_ENDIAN to merge misaligned words in either byte order.
 *
 * Overlap: it copies forwards, and each store writes bytes it has just
 * loaded from the same offset, so memcpy with dst == src, which GCC
 * requires to work, leaves memory as it was. Any other overlap needs
 * memmove, from port_memmove.S.
 *
 * Configuration: LIBC_BUILD_MEMCPY makes the file hold its code, as NuttX's
 * build does. __OPT_BIG_BLOCK_SIZE and __OPT_MID_BLOCK_SIZE keep the
 * file's defaults, 64 and 16 bytes.
 *
 * Renaming: none. ARCH_LIBCFUN in libc.h keeps the name memcpy.
 */

#if !defined(__ARM_ARCH_PROFILE) || (__ARM_ARCH_PROFILE != 'M') || \
    (__ARM_ARCH_ISA_THUMB < 2)
#error "src/port/armv7m needs an M-profile Arm with Thumb-2"
#endif

#define LIBC_BUILD_MEMCPY 1

#include "arch_memcpy.S"

/* Upstream leaves this out. It marks the stack as not executable. */
.section .note.GNU-stack,"",%progbits

/*
 * mqtt_port_memcpy is libterae's own name for memcpy, at the same address.
 * base_string.c calls it when the library is built with MQTT_PORT_MEMORY,
 * as libterae's C may not name memcpy itself. .thumb_set gives the alias
 * the Thumb bit that memcpy has.
 */
    .globl mqtt_port_memcpy
    .type mqtt_port_memcpy, %function
    .thumb_set mqtt_port_memcpy, memcpy
