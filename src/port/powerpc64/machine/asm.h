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
 * libterae's own <machine/asm.h> for FreeBSD's powerpc64 memcpy.S and
 * bcopy.S and NetBSD's bzero.S in this folder, for the ELFv2 ABI. FreeBSD's header of that name
 * carries a four-clause BSD licence, which cannot be combined with the GPL,
 * so it is not used and nothing here comes from it. This file defines only
 * what the two files take from it:
 *
 * ENTRY(name) opens a global function with a single entry point. None of
 * the routines reads the TOC pointer r2 or any global data, so there is no
 * TOC set-up and the local entry offset stays 0. It also marks the function
 * for the unwinder. END(name) closes it and records its size.
 * WEAK_REFERENCE(name, alias) makes the C name alias a global function at
 * the same address as the reserved upstream name. FreeBSD makes the alias
 * weak; here it is an ordinary global symbol, because a build without a C
 * library has no other definition for it to give way to.
 */

#ifndef MQTT_PORT_POWERPC64_MACHINE_ASM_H
#define MQTT_PORT_POWERPC64_MACHINE_ASM_H

#if !defined(_CALL_ELF) || (_CALL_ELF != 2)
#error "src/port/powerpc64 supports only the ELFv2 ABI"
#endif

    .abiversion 2

#if defined(MQTT_PORT_PLAIN_ENTRY)
/*
 * For NetBSD's bzero.S, which opens two entry points in a row, has no END,
 * and sizes its function by the ELFv1 entry name: no unwind information, and
 * .name defined as a local label at the same place.
 */
#define ENTRY(name) \
    .text; \
    .p2align 4; \
    .globl name; \
    .type name, @function; \
name: \
.name:
#else
#define ENTRY(name) \
    .text; \
    .p2align 4; \
    .globl name; \
    .type name, @function; \
name: \
    .cfi_startproc
#endif

#define END(name) \
    .cfi_endproc; \
    .size name, . - name

#define WEAK_REFERENCE(name, alias) \
    .globl alias; \
    .type alias, @function; \
    .set alias, name

#endif
