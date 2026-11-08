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
 * libterae's stand-in for picolibc.h, which picolibc's build generates and
 * memcpy.S, memmove.S and memset.S in this folder include as <picolibc.h>.
 * It holds picolibc's configuration, and none of the three uses anything
 * from it, so this one is empty.
 */

#ifndef LIBTERAE_PORT_RX_PICOLIBC_H
#define LIBTERAE_PORT_RX_PICOLIBC_H

#endif
