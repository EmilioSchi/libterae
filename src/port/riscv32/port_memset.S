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
 * port_memset.S provides memset for 32-bit RISC-V: RV32I and RV32E cores
 * such as SHAKTI's E-class and Espressif's ESP32-C series. It needs only
 * the base integer instructions. It stores single bytes until the pointer
 * is word aligned and then whole words, so it never makes a misaligned
 * store.
 *
 * Source: memset.S by SiFive, kept unchanged beside this file. It comes from
 * newlib, newlib/libc/machine/riscv/memset.S at commit
 * b097f95233d26da7af6887798297d1efadb75370, under the licence the file calls
 * the FreeBSD License, the two-clause BSD licence. It takes ENTRY and END
 * from newlib's newlib/libc/machine/riscv/sys/asm.h, kept unchanged as
 * sys/asm.h in this folder. NOTICE in this folder gives both URLs and
 * checksums, and the licence text.
 *
 * Why assembly: GCC and clang emit calls to memcpy, memmove and memset even
 * in freestanding code, so a build without a C library has to define them.
 * C could define them only by declaring names that the C standard reserves,
 * which libterae's C never does. A separate assembly file keeps assembly
 * out of the C code, and this comment records why it is used.
 *
 * Configuration: none. The file picks its code from the compiler's macros:
 * a byte loop when __OPTIMIZE_SIZE__ is defined, as at -Os, the vector
 * extension's stores when __riscv_vector is, and otherwise the word stores
 * described above, with the Zba, Zbkb and Zilsd instructions where the
 * target has them. The file lacks the .note.GNU-stack section, so this
 * wrapper adds it.
 */

#if __riscv_xlen != 32
#error "src/port/riscv32 needs a 32-bit RISC-V target"
#endif

#include "memset.S"

    .section .note.GNU-stack, "", %progbits
