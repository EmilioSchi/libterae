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
 * Provides memcmp for x86_64 (System V AMD64 ABI).
 *
 * From FreeBSD, lib/libc/amd64/string/memcmp.S, at commit
 * 2321b3df1f0db4f839cb9dde7c0a9a0060d4bcfe, kept unmodified, with its
 * notice: see NOTICE in this folder.
 *
 * Why assembly: GCC and clang emit calls to memcpy, memmove, memset and
 * memcmp on their own, for struct copies and initialisers, so a build with
 * no C library must define these symbols. libterae's C can't: it would have
 * to declare names that the C standard reserves. A separate assembly file,
 * called only through the C calling convention, defines them instead, and
 * keeps the assembly apart from the C code.
 *
 * memcmp.S has two variants, which FreeBSD chooses between when a program
 * starts. Here amd64_archlevel.h, libterae's own, chooses when this file is
 * assembled, and MQTT_PORT_ARCHLEVEL names the choice: the scalar variant,
 * which uses only general registers and never writes to the stack. The SSE2
 * one, baseline, stores below the stack pointer, which code built with
 * -mno-red-zone doesn't allow. machine/asm.h and machine/param.h are
 * libterae's own stand-ins for FreeBSD's too.
 */

#define MQTT_PORT_ARCHLEVEL scalar

#include "memcmp.S"
