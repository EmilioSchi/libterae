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
 * libterae's stand-in for the Xtensa core's <xtensa/config/core-isa.h>,
 * which xtensa-asm.h in src/port/xtensa includes. That header is
 * generated for each Xtensa core and comes with its toolchain: ESP-IDF
 * has one for each ESP32. This one lets the folder build without it, for
 * any core, and sets only the four values memcpy.S, memset.S and
 * xtensa-asm.h read:
 *
 * - XCHAL_HAVE_BE: 1 for a big-endian core. It follows __XTENSA_EB__,
 *   which the compiler defines for one.
 * - XCHAL_HAVE_WINDOWED: 1 when the code is built for the windowed
 *   register ABI, so that the leaf functions start with entry and end
 *   with retw. It follows __XTENSA_WINDOWED_ABI__. Built for the call0
 *   ABI, they return with ret.
 * - XCHAL_HAVE_LOOPS: 1 when the core has the zero-overhead loop
 *   instructions. ESP32 and ESP32-S3 have them, and ESP32-S2 doesn't. It
 *   follows the compiler's __XCHAL_HAVE_LOOPS where the compiler gives
 *   one, as Espressif's does, and is 0 otherwise: the files then count
 *   with branches, which every core runs.
 * - XCHAL_INST_FETCH_WIDTH: how many bytes the core fetches at a time,
 *   which only aligns the loops. It follows __XCHAL_INST_FETCH_WIDTH where
 *   the compiler gives one, and is 4 otherwise, as on every ESP32.
 */

#ifndef LIBTERAE_PORT_XTENSA_CORE_ISA_H
#define LIBTERAE_PORT_XTENSA_CORE_ISA_H

#if defined(__XTENSA_EB__)
#define XCHAL_HAVE_BE 1
#else
#define XCHAL_HAVE_BE 0
#endif

#if defined(__XTENSA_WINDOWED_ABI__)
#define XCHAL_HAVE_WINDOWED 1
#else
#define XCHAL_HAVE_WINDOWED 0
#endif

#if defined(__XCHAL_HAVE_LOOPS)
#define XCHAL_HAVE_LOOPS __XCHAL_HAVE_LOOPS
#else
#define XCHAL_HAVE_LOOPS 0
#endif

#if defined(__XCHAL_INST_FETCH_WIDTH)
#define XCHAL_INST_FETCH_WIDTH __XCHAL_INST_FETCH_WIDTH
#else
#define XCHAL_INST_FETCH_WIDTH 4
#endif

#endif
