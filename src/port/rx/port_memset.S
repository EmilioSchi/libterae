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
 * port_memset.S provides memset for Renesas RX (RXv1, RXv2 and RXv3), with the
 * arguments in r1 to r3 and the result in r1, and with the leading
 * underscore that GCC's rx-elf target puts on C names.
 *
 * Source: memset.S by Nick Clifton, kept unchanged beside this file. It comes
 * from picolibc, libc/machine/rx/memset.S at commit
 * 29e3581fa0e921e378a6aad08c9374b097ae56c8. The file carries only its
 * copyright line, and picolibc's COPYING.picolibc puts it under the
 * three-clause BSD licence, given in NOTICE. It includes <picolibc.h>,
 * which here is libterae's own stand-in. NOTICE in this folder gives its
 * URL and checksum.
 *
 * Why assembly: GCC and clang emit calls to memcpy, memmove, memset and
 * memcmp even in freestanding code, so a build without a C library has to
 * define them. C could define them only by declaring names that the C
 * standard reserves, which libterae's C never does. A separate assembly
 * file keeps assembly out of the C code, and this comment records why it
 * is used.
 *
 * What the code does: it fills with the string instruction sstr.b, which
 * stores the low byte of r2 into r3 bytes from r1, and returns the
 * destination it saved.
 *
 * What the code needs: the RX string instructions. The file holds code only
 * when the compiler defines __RX_ALLOW_STRING_INSNS__, as GCC does unless
 * given -mno-allow-string-insns, so the check below stops a build that
 * disallows them rather than leave memset undefined. The string instructions
 * may read ahead of the bytes they are asked for, which is why some
 * programs disallow them around I/O registers.
 *
 * Configuration: none. The file lacks the .note.GNU-stack section, so this
 * wrapper adds it.
 */

#if !defined(__RX_ALLOW_STRING_INSNS__)
#error "src/port/rx needs the RX string instructions"
#endif

#include "memset.S"

    .section .note.GNU-stack, "", @progbits
