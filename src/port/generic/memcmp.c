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
 * memcmp in C, for a build with no C library on a processor whose folder of
 * src/port has no memcmp in assembly. It is libterae's own, and the files
 * named port_memcmp.c in those folders include it.
 *
 * It lies outside the MISRA C claim, like the assembly. Defining memcmp in
 * C declares a name the C standard reserves, which Rule 21.2 forbids in the
 * library's own code, and no other name will do: the compiler, and code
 * linked with libterae, call it by this one. libterae's own comparisons use
 * mqtt_bytes_equal instead, which takes the same time wherever the bytes
 * differ. This one stops at the first difference.
 *
 * It is compiled with -ffreestanding and -fno-builtin, and with
 * -fno-tree-loop-distribute-patterns by a compiler that takes it, so that
 * its loop is not turned into a call to memcmp, that is, to itself.
 */

#include <stddef.h>

int memcmp(const void *left, const void *right, size_t length);

int memcmp(const void *left, const void *right, size_t length)
{
    /* The bytes, as C compares them: unsigned char. */
    const unsigned char *a = (const unsigned char *)left;
    const unsigned char *b = (const unsigned char *)right;
    /* Index of the byte being compared. */
    size_t index = 0U;
    /* The difference of the first two bytes that differ, or 0. */
    int answer = 0;

    while ((answer == 0) && (index < length)) {
        answer = (int)a[index] - (int)b[index];
        index += 1U;
    }
    return answer;
}
