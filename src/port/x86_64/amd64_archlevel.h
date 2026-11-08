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
 * Stands in for FreeBSD's lib/libc/amd64/amd64_archlevel.h, which memcmp.S
 * includes. memcmp.S holds one memcmp for each x86_64 level it supports,
 * scalar and baseline (SSE2). FreeBSD picks one when the program starts,
 * through a gnu_indirect_function resolver that calls __archlevel_resolve in
 * its C library. libterae has no C library to call, so this file picks one
 * when the file is assembled instead:
 *
 * - ARCHFUNCS(func) makes func a global function that is another name for
 *   func_<level>, where level is MQTT_PORT_ARCHLEVEL, which the wrapper sets.
 * - ARCHFUNC and ENDARCHFUNCS, which build FreeBSD's table of variants, give
 *   nothing.
 * - ARCHENTRY(func, level) and ARCHEND(func, level) start and end a variant,
 *   func_<level>, as a local function, like FreeBSD's.
 *
 * Each variant is still assembled, but only the selected one can be called:
 * the others have local names, and nothing refers to them. No resolver,
 * indirect function or relocation is made.
 *
 * memcmp.S includes this file as "amd64_archlevel.h", so the file next to it
 * is used before any on the include path. FreeBSD's own copy must never be
 * put in this folder.
 */
#ifndef MQTT_PORT_AMD64_ARCHLEVEL_H
#define MQTT_PORT_AMD64_ARCHLEVEL_H

#include <machine/asm.h>

#ifndef MQTT_PORT_ARCHLEVEL
#error "MQTT_PORT_ARCHLEVEL must name the variant to use, such as scalar"
#endif

/* The name of a variant: func_level. */
#define MQTT_PORT_VARIANT(func, level) func ## _ ## level

/* The same, after the arguments are expanded. */
#define MQTT_PORT_SELECTED(func, level) MQTT_PORT_VARIANT(func, level)

#define ARCHFUNCS(func) \
	.globl func; \
	.type func, @function; \
	.set func, MQTT_PORT_SELECTED(func, MQTT_PORT_ARCHLEVEL)

#define ARCHFUNC(func, level)

#define ENDARCHFUNCS(func)

#define ARCHENTRY(func, level) \
	.text; \
	.p2align 4, 0x90; \
	.type MQTT_PORT_VARIANT(func, level), @function; \
	MQTT_PORT_VARIANT(func, level):; \
	.cfi_startproc

#define ARCHEND(func, level) \
	END(MQTT_PORT_VARIANT(func, level))

#endif /* MQTT_PORT_AMD64_ARCHLEVEL_H */
