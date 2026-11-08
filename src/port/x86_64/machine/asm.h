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
 * Stands in for FreeBSD's <machine/asm.h> (sys/amd64/include/asm.h), which
 * memmove.S, memset.S and memcmp.S include. It is libterae's own, so that no
 * FreeBSD system header is needed, and it gives only what they use:
 *
 * - ENTRY(x) starts a global function x in .text, aligned to 16 bytes and
 *   padded with nop, and opens its call frame information.
 * - END(x) gives x its size and closes its call frame information.
 *
 * The routines push nothing and leave the stack pointer alone, so the frame
 * that .cfi_startproc describes holds for their whole length. FreeBSD's
 * profiling prologue, PIC macros and symbol name mapping are left out: the
 * symbols keep their C names, as ELF needs.
 */
#ifndef MQTT_PORT_MACHINE_ASM_H
#define MQTT_PORT_MACHINE_ASM_H

#define ENTRY(x) \
	.text; \
	.p2align 4, 0x90; \
	.globl x; \
	.type x, @function; \
	x:; \
	.cfi_startproc

#define END(x) \
	.size x, . - x; \
	.cfi_endproc

#endif /* MQTT_PORT_MACHINE_ASM_H */
