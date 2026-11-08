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
 * memmove in C, for a build with no C library on a processor whose folder of
 * src/port has no memmove in assembly. It is libterae's own, and the files
 * named port_memmove.c in those folders include it.
 *
 * It lies outside the MISRA C claim, like the assembly. Defining memmove in
 * C declares a name the C standard reserves, which Rule 21.2 forbids in the
 * library's own code. And to know which way to copy it compares where the
 * two ranges are, which C allows only within one object: here the pointers
 * are converted to integers, which works on each processor src/port serves,
 * as each has one flat address space.
 *
 * It is compiled with -ffreestanding and -fno-builtin, and with
 * -fno-tree-loop-distribute-patterns by a compiler that takes it, so that
 * its loops are not turned into a call to memmove, that is, to itself.
 */

#include <stddef.h>
#include <stdint.h>

void *memmove(void *destination, const void *source, size_t length);

void *memmove(void *destination, const void *source, size_t length)
{
    /* Where the bytes go. */
    unsigned char *to = (unsigned char *)destination;
    /* Where they come from. */
    const unsigned char *from = (const unsigned char *)source;

    /* The subtraction wraps, so it is at least length unless the destination
     * starts inside the source: below the source, or past its end, a copy
     * from the front never overwrites a byte it has still to read. */
    if (((uintptr_t)to - (uintptr_t)from) >= (uintptr_t)length) {
        /* Index of the byte being copied. */
        size_t index = 0U;

        while (index < length) {
            to[index] = from[index];
            index += 1U;
        }
    } else {
        /* One past the byte being copied. */
        size_t index = length;

        while (index > 0U) {
            index -= 1U;
            to[index] = from[index];
        }
    }
    return destination;
}
