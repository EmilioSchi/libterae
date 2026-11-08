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
 * port_memset.S provides memset for the Motorola 68000 family and NXP's
 * ColdFire, with the arguments on the stack as GCC passes them.
 *
 * Source: memset.S by mocom software, kept unchanged beside this file. It
 * comes from picolibc, libc/machine/m68k/memset.S at commit
 * 29e3581fa0e921e378a6aad08c9374b097ae56c8, under the permissive notice in
 * its header. It includes <picolibc.h> and "m68kasm.h", which here are
 * libterae's own stand-ins. NOTICE in this folder gives its URL and
 * checksum.
 *
 * Why assembly: GCC and clang emit calls to memcpy, memmove, memset and
 * memcmp even in freestanding code, so a build without a C library has to
 * define them. C could define them only by declaring names that the C
 * standard reserves, which libterae's C never does. A separate assembly
 * file keeps assembly out of the C code, and this comment records why it
 * is used.
 *
 * What the code needs: below 16 bytes it stores single bytes. Otherwise it
 * copies the low byte of the value into every byte of a long word, stores a
 * byte and a word as needed until the destination is aligned to 4 bytes,
 * then long words, so it makes no misaligned store on any of these
 * processors. ColdFire has no dbra, and there the loops count with subq.l.
 * It returns the destination in both d0 and a0.
 *
 * Configuration: none. The file marks the stack as not executable itself.
 */

#include "memset.S"
