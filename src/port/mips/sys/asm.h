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
 * libterae's own <sys/asm.h> for the MIPS Technologies memcpy.S and memset.S
 * in this folder, which include it when built without _LIBC. It defines only
 * what those two files take from it, for the o32 ABI:
 *
 * LEAF(symbol) opens a global leaf function that keeps no stack frame and
 * returns through ra. END(symbol) closes it and records its size. Both also
 * mark the function for the unwinder, which needs no further rules because
 * the functions never move the stack pointer or save a register. LEAF
 * names sp and ra by number, $29 and $31, because the preprocessor reads
 * $ra as $ followed by the macro ra from <regdef.h>.
 * PTR_ADDU and PTR_SUBU add and subtract pointers, which are 32 bits wide.
 * The files define L, PTR_ADDIU, PTR_SRA and PTR_LSA themselves.
 */

#ifndef MQTT_PORT_MIPS_SYS_ASM_H
#define MQTT_PORT_MIPS_SYS_ASM_H

#if !defined(_MIPS_SIM) || !defined(_ABIO32) || (_MIPS_SIM != _ABIO32)
#error "src/port/mips supports only the o32 ABI"
#endif

#define LEAF(symbol) \
    .text; \
    .globl symbol; \
    .p2align 2; \
    .type symbol, @function; \
    .ent symbol, 0; \
symbol: \
    .frame $29, 0, $31; \
    .cfi_startproc

#define END(symbol) \
    .cfi_endproc; \
    .end symbol; \
    .size symbol, . - symbol

#define PTR_ADDU addu
#define PTR_SUBU subu

#endif
