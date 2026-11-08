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
 * memset in C, for a build with no C library on a processor whose folder of
 * src/port has no memset in assembly. It is libterae's own, and the files
 * named port_memset.c in those folders include it.
 *
 * It lies outside the MISRA C claim, like the assembly. Defining memset in
 * C declares a name the C standard reserves, which Rule 21.2 forbids in the
 * library's own code, and no other name will do: the compiler calls it by
 * this one, for clearing structs and runs of stores it merges.
 *
 * It stores one byte at a time, so it needs no alignment and works in either
 * byte order, on any processor. C converts value to unsigned char before
 * storing it, as the standard says memset does.
 *
 * It is compiled with -ffreestanding and -fno-builtin, and with
 * -fno-tree-loop-distribute-patterns by a compiler that takes it, so that
 * its loop is not turned into a call to memset, that is, to itself.
 */

#include <stddef.h>

void *memset(void *destination, int value, size_t length);

void *memset(void *destination, int value, size_t length)
{
    /* Where the bytes go. */
    unsigned char *to = (unsigned char *)destination;
    /* The byte stored. */
    const unsigned char byte = (unsigned char)value;
    /* Index of the byte being stored. */
    size_t index = 0U;

    while (index < length) {
        to[index] = byte;
        index += 1U;
    }
    return destination;
}
