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
 * port_memcpy.S provides memcpy for 64-bit RISC-V, as on the riscv64-linux-musl
 * target (RV64GC, lp64d). It needs only RV64I, and it never makes a
 * misaligned load or store.
 *
 * Source: memcpy.S by Strahinja Stanisic, kept unchanged beside this file.
 * It comes from FreeBSD, lib/libc/riscv/string/memcpy.S at commit
 * 2321b3df1f0db4f839cb9dde7c0a9a0060d4bcfe, under BSD-2-Clause. It takes
 * ENTRY and END from FreeBSD's sys/riscv/include/asm.h, kept unchanged as
 * machine/asm.h in this folder. NOTICE in this folder gives both URLs and
 * checksums, and the licence text.
 *
 * Why assembly: GCC and clang emit calls to memcpy, memmove and memset even
 * in freestanding code, so a build without a C library has to define them.
 * C could define them only by declaring names that the C standard reserves,
 * which libterae's C never does. A separate assembly file keeps assembly
 * out of the C code, and this comment records why it is used.
 *
 * Configuration: none. The file lacks the .note.GNU-stack section, so this
 * wrapper adds it, to mark that the code needs no executable stack.
 */

#if __riscv_xlen != 64
#error "src/port/riscv64 needs a 64-bit RISC-V target"
#endif

#include "memcpy.S"

    .section .note.GNU-stack, "", %progbits

/*
 * mqtt_port_memcpy is libterae's own name for memcpy, at the same address.
 * base_string.c calls it when the library is built with MQTT_PORT_MEMORY,
 * as libterae's C may not name memcpy itself.
 */
    .globl mqtt_port_memcpy
    .type mqtt_port_memcpy, %function
    .equ mqtt_port_memcpy, memcpy
