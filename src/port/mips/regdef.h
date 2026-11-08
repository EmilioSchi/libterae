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
 * libterae's own <regdef.h> for the MIPS Technologies memcpy.S and memset.S
 * in this folder, which include it when built without _LIBC. It names the
 * o32 registers those two files use, and no others. The assembler register
 * at is left out on purpose: the files write ".set at", which a macro of
 * that name would break.
 */

#ifndef MQTT_PORT_MIPS_REGDEF_H
#define MQTT_PORT_MIPS_REGDEF_H

#if !defined(_MIPS_SIM) || !defined(_ABIO32) || (_MIPS_SIM != _ABIO32)
#error "src/port/mips supports only the o32 ABI"
#endif

#define zero $0
#define v0 $2
#define v1 $3
#define a0 $4
#define a1 $5
#define a2 $6
#define a3 $7
#define t0 $8
#define t1 $9
#define t2 $10
#define t3 $11
#define t4 $12
#define t5 $13
#define t6 $14
#define t7 $15
#define t8 $24
#define t9 $25
#define ra $31

#endif
