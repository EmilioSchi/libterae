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
 * The run-time ABI's fills for any 32-bit Arm with ARM state: __aeabi_memset
 * and __aeabi_memclr, each with its 4 and 8 forms for aligned pointers.
 *
 * Source: __aeabi_memset.s from musl, src/string/arm, at commit
 * 7813e9119eb5fe681b6750b8a033a0490bca0941, taken from the
 * github.com/kraj/musl mirror and kept unchanged in this folder. It carries
 * no notice, and musl's MIT licence is in NOTICE.
 *
 * Why it is here: on a bare-metal Arm target (arm-none-eabi), clang calls
 * these names, which the Arm run-time ABI defines, in place of memset for
 * clearing structs. src/ calls __aeabi_memclr4 or __aeabi_memclr8 at every
 * level. GCC calls the standard name. So a build without a C library needs
 * both sets.
 *
 * What the code needs: the file sets no state, so the .arm below sets ARM
 * state, whatever -marm or -mthumb says. It stores one byte at a time, so
 * it needs no alignment, and is slow on large fills. It changes only r0 to
 * r2, which the run-time ABI allows, and returns with BX, which interworks.
 * The run-time ABI orders __aeabi_memset's arguments as destination,
 * length, value, unlike memset, and the file follows it.
 *
 * Renaming: none. Upstream already uses the run-time ABI's names.
 */

.arm
#include "__aeabi_memset.s"

/* Upstream leaves this out. It marks the stack as not executable. */
.section .note.GNU-stack,"",%progbits
