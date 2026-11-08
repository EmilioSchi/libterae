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
 * port_memmove.c provides memmove for 32-bit MIPS (o32), in C: no memmove in
 * assembly under a licence libterae can ship was found for it. NOTICE in this
 * folder says what was looked at. The code is libterae's own, in
 * src/port/generic, which says why it lies outside the MISRA C claim and how
 * it is compiled.
 */

#include "../generic/memmove.c"
