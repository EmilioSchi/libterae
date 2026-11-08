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
 * Provides memset for i386 (System V i386 ABI, cdecl).
 *
 * From musl, src/string/i386/memset.s, at commit
 * 5a56ad13e1e0790c06122978aad2a14d6b9efc1a, taken from the
 * github.com/kraj/musl mirror. It is kept unmodified: it carries no notice,
 * and musl's licence is in NOTICE in this folder.
 *
 * Why assembly: GCC and clang emit calls to memcpy, memmove, memset and
 * memcmp on their own, for struct copies and initialisers, so a build with
 * no C library must define these symbols. libterae's C can't: it would have
 * to declare names that the C standard reserves. A separate assembly file,
 * called only through the C calling convention, defines them instead, and
 * keeps the assembly apart from the C code.
 *
 * musl's file doesn't say that the stack needn't be executable, so this file
 * does, after it.
 */

#include "memset.s"

	.section .note.GNU-stack, "", @progbits
