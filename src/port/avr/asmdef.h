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
 * libterae's stand-in for avr-libc's common/asmdef.h, which the four
 * avr-libc files in this folder include as "asmdef.h". avr-libc's own
 * header includes the config.h its build generates, <avr/common.h> and
 * sectionname.h, which a build without a C library doesn't have.
 *
 * It defines only what the four files use, with the meaning avr-libc's
 * header gives it at commit 86f504ec286fd70ff219760f42e612fe8e52a40e:
 *
 * - _U(name) is the name itself.
 * - __tmp_reg__ is r0, the register avr-gcc lets any code change.
 * - XL, XH, ZL and ZH are the halves of the X and Z pointers, r26 to r31.
 * - XJMP is jmp where the device has it, and rjmp otherwise.
 * - ENTRY starts a global function in .text, and ENDFUNC ends it, setting
 *   its size as avr-libc's FUNCTION macro does.
 * - X_movw copies a register pair. avr-libc's macro also handles devices
 *   without the movw instruction, by working out the register numbers.
 *   This one needs movw, and the check below stops the build on a device
 *   without it: the classic AT90S cores of avr2 and the reduced cores of
 *   avrtiny.
 * - TINY_WEAK_ALIAS adds a name only on the reduced cores, so here it adds
 *   none.
 *
 * OPTIMIZE_SPEED, which avr-libc's configuration sets, stays undefined, so
 * each file takes its shorter loop.
 */

#ifndef LIBTERAE_PORT_AVR_ASMDEF_H
#define LIBTERAE_PORT_AVR_ASMDEF_H

#if !defined(__AVR_HAVE_MOVW__) || defined(__AVR_TINY__)
#error "src/port/avr needs an AVR with the movw instruction"
#endif

#define _U(name) name

#define __tmp_reg__ r0

#define XL r26
#define XH r27
#define ZL r30
#define ZH r31

#if defined(__AVR_HAVE_JMP_CALL__)
#define XJMP jmp
#else
#define XJMP rjmp
#endif

.macro ENTRY name
    .text
    .global \name
    .type \name, @function
    .size \name, .L__END - \name
\name:
.endm

.macro ENDFUNC
.L__END:
.endm

.macro X_movw dst, src
    movw \dst, \src
.endm

.macro TINY_WEAK_ALIAS new, old
.endm

#endif
