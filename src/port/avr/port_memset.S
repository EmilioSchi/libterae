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
 * port_memset.S provides memset for 8-bit AVR with the movw instruction: the
 * ATmega, ATxmega and newer ATtiny devices, with avr-gcc's calling
 * convention, which clang's AVR target follows.
 *
 * Source: memset.S from avr-libc, libc/string/memset.S at commit
 * 86f504ec286fd70ff219760f42e612fe8e52a40e, kept unchanged beside this
 * file, under the BSD-3-Clause licence in its header. It includes
 * "asmdef.h", which here is libterae's own stand-in. NOTICE in this folder
 * gives its URL and checksum.
 *
 * Why assembly: GCC and clang emit calls to memcpy, memmove, memset and
 * memcmp even in freestanding code, so a build without a C library has to
 * define them. C could define them only by declaring names that the C
 * standard reserves, which libterae's C never does. A separate assembly
 * file keeps assembly out of the C code, and this comment records why it
 * is used.
 *
 * What the code does: it stores the low byte of its value one byte at a time
 * through the X pointer.
 *
 * Configuration: none. The file lacks the .note.GNU-stack section, so this
 * wrapper adds it.
 */

#include "memset.S"

    .section .note.GNU-stack, "", @progbits
