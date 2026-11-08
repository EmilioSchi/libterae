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
 * port_memset.S provides memset for 64-bit PowerPC with the ELFv2 ABI, as on
 * the big-endian powerpc64-linux-musl target. It needs only the base 64-bit
 * instruction set: aligned doubleword stores, and bytes for the rest.
 *
 * Source: bzero.S by The NetBSD Foundation, kept unchanged beside this file.
 * It comes from NetBSD, lib/libc/arch/powerpc64/string/bzero.S at commit
 * 270c7710164d464dd162bea4f1ab5282ebe7efc3, under BSD-2-Clause. NOTICE in
 * this folder gives its URL and checksum.
 *
 * Why assembly: GCC and clang emit calls to memcpy, memmove and memset even
 * in freestanding code, so a build without a C library has to define them.
 * On this target they merge runs of zero stores into memset from -O1 up. C
 * could define it only by declaring names that the C standard reserves,
 * which libterae's C never does. A separate assembly file keeps assembly
 * out of the C code, and this comment records why it is used.
 *
 * Configuration: the file opens two entry points with ENTRY, bzero and then
 * memset, which bzero falls into, and has no END: it sizes memset by the
 * ELFv1 entry name .memset. MQTT_PORT_PLAIN_ENTRY asks libterae's
 * <machine/asm.h> for an ENTRY that opens no unwind information and also
 * defines that name, as a local label. bzero is renamed __bzero, a name C
 * reserves, as nothing in libterae calls it.
 */

#define MQTT_PORT_PLAIN_ENTRY 1
#define bzero __bzero

#include "bzero.S"

    .section .note.GNU-stack,"",%progbits
