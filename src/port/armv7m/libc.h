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
 * libterae's stand-in for NuttX's libs/libc/libc.h, which the three NuttX
 * files in this folder include as "libc.h". NuttX's own header pulls in the
 * NuttX configuration and its C library's headers, which a build without a
 * C library doesn't have and these routines don't need.
 *
 * The files take one macro from it, ARCH_LIBCFUN, which gives each function
 * its name. NuttX can add a prefix there, so that its C versions stay
 * callable beside the assembly. Here each function keeps its standard name.
 *
 * Each file also holds code only when NuttX's build defines its own
 * LIBC_BUILD_ macro: LIBC_BUILD_MEMCPY and so on. The port_*.S wrapper that
 * includes the file defines it, so this header doesn't.
 */

#ifndef LIBTERAE_PORT_ARMV7M_LIBC_H
#define LIBTERAE_PORT_ARMV7M_LIBC_H

#define ARCH_LIBCFUN(name) name

#endif
