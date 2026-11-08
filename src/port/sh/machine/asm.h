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
 * libterae's stand-in for NetBSD's <machine/asm.h> for SuperH,
 * sys/arch/sh3/include/asm.h, which memcpy.S and memset.S in src/port/sh
 * include. NetBSD's own carries the University of California's licence with
 * its advertising clause, and pulls in more of NetBSD than the files need.
 *
 * It defines the two macros they use, with the meaning NetBSD's gives them
 * at commit 91af1a5842bfaed91ef5ba5178ffd758613c0e54:
 *
 * - _C_LABEL(name) is a C name as the assembler sees it. NetBSD adds an
 *   underscore unless the toolchain says it doesn't, and here the compiler
 *   says so through __USER_LABEL_PREFIX__: an underscore for sh-elf, as GCC
 *   defines it there, nothing for sh-linux.
 * - ENTRY(name) starts a global function in .text, aligned to 4 bytes.
 *
 * RCSID is used only when LIBC_SCCS is defined, which it isn't here.
 */

#ifndef LIBTERAE_PORT_SH_MACHINE_ASM_H
#define LIBTERAE_PORT_SH_MACHINE_ASM_H

#ifndef __USER_LABEL_PREFIX__
#define __USER_LABEL_PREFIX__
#endif

/* Joins two tokens after expanding them, so a prefix can be a macro. */
#define LIBTERAE_JOIN(a, b) LIBTERAE_JOIN_NOW(a, b)
#define LIBTERAE_JOIN_NOW(a, b) a##b

#define _C_LABEL(name) LIBTERAE_JOIN(__USER_LABEL_PREFIX__, name)

#define ENTRY(name) \
    .text; .align 2; .globl _C_LABEL(name); \
    .type _C_LABEL(name), @function; _C_LABEL(name):

#endif
