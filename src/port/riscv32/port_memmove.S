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
 * port_memmove.S provides memmove for 32-bit RISC-V: RV32I and RV32E cores
 * such as SHAKTI's E-class and Espressif's ESP32-C series. It copies one
 * byte at a time, so it needs only the base integer instructions and is
 * slow on large copies.
 *
 * Source: memmove-asm.S by SiFive, kept unchanged beside this file, the
 * same file src/port/riscv64 uses. It comes from newlib,
 * newlib/libc/machine/riscv/memmove-asm.S at commit
 * b097f95233d26da7af6887798297d1efadb75370, under the licence the file
 * calls the FreeBSD License, the two-clause BSD licence. It takes ENTRY and
 * END from newlib's newlib/libc/machine/riscv/sys/asm.h, kept unchanged as
 * sys/asm.h in this folder. NOTICE in this folder gives both URLs and
 * checksums, and the licence text.
 *
 * Why assembly: GCC and clang emit calls to memcpy, memmove and memset even
 * in freestanding code, so a build without a C library has to define them.
 * C could define them only by declaring names that the C standard reserves,
 * which libterae's C never does. A separate assembly file keeps assembly
 * out of the C code, and this comment records why it is used.
 *
 * Configuration: PREFER_SIZE_OVER_SPEED selects the byte loop, the only
 * part of the file that needs no vector extension. Without it the file
 * holds no code unless the target has that extension. The loop copies
 * forwards when the source lies above the destination and backwards
 * otherwise, comparing the addresses as unsigned numbers. The file lacks
 * the .note.GNU-stack section, so this wrapper adds it.
 */

#if __riscv_xlen != 32
#error "src/port/riscv32 needs a 32-bit RISC-V target"
#endif

#ifndef PREFER_SIZE_OVER_SPEED
#define PREFER_SIZE_OVER_SPEED 1
#endif

#include "memmove-asm.S"

    .section .note.GNU-stack, "", %progbits
