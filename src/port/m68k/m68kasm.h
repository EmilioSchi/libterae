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
 * libterae's stand-in for picolibc's and newlib's m68kasm.h, which memcpy.S
 * and memset.S in this folder include as "m68kasm.h". It gives the two
 * things those files take from it, with the meaning upstream gives them:
 *
 * - SYM(name) is a C name as the assembler sees it, with the compiler's
 *   __USER_LABEL_PREFIX__ in front: nothing for m68k-linux and most ELF
 *   toolchains, an underscore for a.out ones.
 * - d0 to d7, a0 to a6, fp and sp are the registers, with the compiler's
 *   __REGISTER_PREFIX__ in front: % for ELF, where GNU as needs it.
 *
 * GCC defines both prefixes. clang defines only the first, so where the
 * second is missing a register takes % on ELF, which GNU as rejects a
 * register without, and nothing elsewhere. A name missing its prefix takes
 * none, which is what an ELF toolchain uses.
 */

#ifndef LIBTERAE_PORT_M68K_M68KASM_H
#define LIBTERAE_PORT_M68K_M68KASM_H

#ifdef __USER_LABEL_PREFIX__
#define LIBTERAE_NAME_PREFIX __USER_LABEL_PREFIX__
#else
#define LIBTERAE_NAME_PREFIX
#endif

#ifdef __REGISTER_PREFIX__
#define LIBTERAE_REGISTER_PREFIX __REGISTER_PREFIX__
#elif defined(__ELF__)
#define LIBTERAE_REGISTER_PREFIX %
#else
#define LIBTERAE_REGISTER_PREFIX
#endif

/*
 * Joins two tokens after expanding them, so a prefix can be a macro. % and
 * a register don't make one C token, which a preprocessor reading assembly
 * allows.
 */
#define LIBTERAE_JOIN(a, b) LIBTERAE_JOIN_NOW(a, b)
#define LIBTERAE_JOIN_NOW(a, b) a##b

#define SYM(name) LIBTERAE_JOIN(LIBTERAE_NAME_PREFIX, name)
#define REG(name) LIBTERAE_JOIN(LIBTERAE_REGISTER_PREFIX, name)

#define d0 REG(d0)
#define d1 REG(d1)
#define d2 REG(d2)
#define d3 REG(d3)
#define d4 REG(d4)
#define d5 REG(d5)
#define d6 REG(d6)
#define d7 REG(d7)
#define a0 REG(a0)
#define a1 REG(a1)
#define a2 REG(a2)
#define a3 REG(a3)
#define a4 REG(a4)
#define a5 REG(a5)
#define a6 REG(a6)
#define fp REG(fp)
#define sp REG(sp)

#endif
