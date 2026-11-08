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
 * Stands in for newlib's newlib/libc/machine/i386/i386mach.h, which
 * memcmp.S includes. It is libterae's own, because newlib's carries no
 * copyright or licence, and it gives only what memcmp.S uses:
 *
 * - SYM(x) is the assembly name of the C function x. On ELF that is x
 *   itself, with no leading underscore.
 * - SOTYPE_FUNCTION(sym) marks sym as a function. newlib does this only when
 *   _I386MACH_NEED_SOTYPE_FUNCTION is defined; here it always does, as every
 *   target libterae builds for is ELF.
 * - A macro for each register memcmp.S names, since it writes them with no
 *   % prefix: eax, ebx, ecx, edx, esi, edi, ebp and esp, and the byte
 *   registers al and dl. Each gives the register's name with the prefix.
 *
 * memcmp.S includes this file as "i386mach.h", so the file next to it is
 * used before any on the include path. newlib's own copy must never be put
 * in this folder.
 */
#ifndef MQTT_PORT_I386MACH_H
#define MQTT_PORT_I386MACH_H

#define SYM(x) x

#define SOTYPE_FUNCTION(sym) .type SYM(sym), @function

#define eax %eax
#define ebx %ebx
#define ecx %ecx
#define edx %edx
#define esi %esi
#define edi %edi
#define ebp %ebp
#define esp %esp

#define al %al
#define dl %dl

#endif /* MQTT_PORT_I386MACH_H */
