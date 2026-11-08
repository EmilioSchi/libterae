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
 * The run-time ABI's copies for the Arm M profile: __aeabi_memcpy and
 * __aeabi_memmove, each with its 4 and 8 forms for aligned pointers.
 *
 * Source: __aeabi_memcpy.s from musl, src/string/arm, at commit
 * 7813e9119eb5fe681b6750b8a033a0490bca0941, taken from the
 * github.com/kraj/musl mirror and kept unchanged in this folder. It carries
 * no notice, and musl's MIT licence is in NOTICE.
 *
 * Why it is here: on a bare-metal Arm target (arm-none-eabi), clang calls
 * these names, which the Arm run-time ABI defines, in place of memcpy and
 * memmove for struct copies and initialisers. GCC calls the standard names.
 * So a build without a C library needs both sets.
 *
 * What the code needs: the file sets no state, and its instructions are
 * Thumb as well as ARM, so it is assembled as Thumb for these cores. It
 * copies one byte at a time, so it needs no alignment and works in either
 * byte order, and is slow on large copies. It changes only r0 to r3, which
 * the run-time ABI allows. The memcpy entries copy forwards. The memmove
 * entries copy backwards when the destination lies above the source,
 * comparing them as unsigned, and forwards otherwise.
 *
 * Renaming: none. Upstream already uses the run-time ABI's names.
 */

#include "__aeabi_memcpy.s"

/* Upstream leaves this out. It marks the stack as not executable. */
.section .note.GNU-stack,"",%progbits
