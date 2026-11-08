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
 * memcpy in C, for a build with no C library on a processor that has no
 * folder of its own in src/port: make port PORT=generic builds it with the
 * other three files of this folder. It is libterae's own.
 *
 * It lies outside the MISRA C claim, like the assembly. Defining memcpy in
 * C declares a name the C standard reserves, which Rule 21.2 forbids in the
 * library's own code, and no other name will do: the compiler calls it by
 * this one, for struct copies and initialisers.
 *
 * It copies one byte at a time, forwards, so it needs no alignment and
 * works in either byte order, on any processor. Each store writes the byte
 * just read from the same offset, so memcpy with destination equal to
 * source, which GCC requires to work, leaves memory as it was.
 *
 * Unlike every folder with assembly, it gives no mqtt_port_memcpy, so a
 * build with PORT=generic doesn't define MQTT_PORT_MEMORY, and base_string.c
 * keeps its own loop, which is the same.
 *
 * It is compiled with -ffreestanding and -fno-builtin, and with
 * -fno-tree-loop-distribute-patterns by a compiler that takes it, so that
 * its loop is not turned into a call to memcpy, that is, to itself.
 */

#include <stddef.h>

void *memcpy(void *destination, const void *source, size_t length);

void *memcpy(void *destination, const void *source, size_t length)
{
    /* Where the bytes go. */
    unsigned char *to = (unsigned char *)destination;
    /* Where they come from. */
    const unsigned char *from = (const unsigned char *)source;
    /* Index of the byte being copied. */
    size_t index = 0U;

    while (index < length) {
        to[index] = from[index];
        index += 1U;
    }
    return destination;
}
