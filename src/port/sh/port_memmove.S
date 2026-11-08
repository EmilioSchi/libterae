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
 * port_memmove.S provides memmove for SuperH from SH-2 on: SH-2, SH-2A, SH-3
 * and
 * SH-4, in either byte order, with the arguments in r4 to r6 and the result
 * in r0, as GCC passes them. The code uses the delayed branches bt/s and
 * bf/s, which SH-1 lacks.
 *
 * Source: memmove.S by SHIMIZU Ryo, kept unchanged beside this file. It comes
 * from NetBSD, common/lib/libc/arch/sh3/string/memmove.S at commit
 * 91af1a5842bfaed91ef5ba5178ffd758613c0e54, under the BSD-3-Clause licence
 * in its header. It includes <machine/asm.h>, which here is libterae's own
 * stand-in. NOTICE in this folder gives its URL and checksum.
 *
 * Why assembly: GCC and clang emit calls to memcpy, memmove, memset and
 * memcmp even in freestanding code, so a build without a C library has to
 * define them. C could define them only by declaring names that the C
 * standard reserves, which libterae's C never does. A separate assembly
 * file keeps assembly out of the C code, and this comment records why it
 * is used.
 *
 * What the code does: memmove.S defines MEMMOVE and includes memcpy.S, which
 * then names its code memmove. That code handles any overlap, as described
 * in port_memcpy.S.
 *
 * Configuration: none. The file lacks the .note.GNU-stack section, so this
 * wrapper adds it.
 */

#include "memmove.S"

    .section .note.GNU-stack, "", @progbits
