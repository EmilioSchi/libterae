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
 * Provides memcmp for i386 (System V i386 ABI, cdecl).
 *
 * From newlib, newlib/libc/machine/i386/memcmp.S, at commit
 * b097f95233d26da7af6887798297d1efadb75370, kept unmodified, with its
 * notice: see NOTICE in this folder. i386mach.h, which it includes, is
 * libterae's own stand-in for newlib's.
 *
 * Why assembly: GCC and clang emit calls to memcpy, memmove, memset and
 * memcmp on their own, for struct copies and initialisers, so a build with
 * no C library must define these symbols. libterae's C can't: it would have
 * to declare names that the C standard reserves. A separate assembly file,
 * called only through the C calling convention, defines them instead, and
 * keeps the assembly apart from the C code.
 *
 * Why newlib's: OpenBSD's i386 memcmp.S is shorter, and says it is public
 * domain, but FreeBSD ships the same code by the same author under a
 * company's copyright, with a licence that has an advertising clause, and
 * nothing found shows that company releasing it. newlib's notice names its
 * holder and grants the use plainly.
 *
 * newlib's file doesn't say that the stack needn't be executable, so this
 * file does, after it.
 */

#include "memcmp.S"

	.section .note.GNU-stack, "", @progbits
