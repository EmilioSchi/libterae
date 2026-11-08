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
 * libterae's stand-in for bionic's libc/private/bionic_asm.h, for the two
 * bionic files in the folder above, memmove.S and memcmp.S. They include it
 * as <private/bionic_asm.h>, so it sits in a private/ subfolder and the
 * assembler is given -I src/port/arm. Bionic's own header is not used: it
 * includes <features.h> and <asm/unistd.h>, which a build without a C
 * library doesn't have and these routines don't need.
 *
 * It defines only the three macros the two files use, ENTRY, ENTRY_PRIVATE
 * and END, with the directives that bionic's bionic_asm.h and
 * bionic_asm_arm.h emit on 32-bit Arm at commit
 * 731631f300090436d7f5df80d50b6275c8c60a93. ENTRY starts a global function,
 * aligned to 64 bytes, with DWARF CFI and an EHABI unwind entry. END closes
 * both and sets the function's size.
 *
 * Two things differ from bionic on purpose.
 *
 * ENTRY_PRIVATE: bionic makes the symbol global and hidden. Here it stays
 * local to the object, because its only user, bsd_safe_memcpy in memmove.S,
 * is called from the same file. The object then exports memmove alone, and
 * no name outside the reserved namespace can clash with a symbol of the
 * program it is linked into.
 *
 * END adds .cantunwind, so each EHABI entry says the function cannot be
 * unwound. Without it, the entry is a compact one, and the assembler makes
 * the object refer to its personality routine, __aeabi_unwind_cpp_pr0,
 * which comes from an unwinder library that a build without a C library
 * may not have. Bionic's entries wouldn't be right anyway: the two files
 * push registers without telling EHABI, so the entries describe only the
 * state on entry.
 */

#ifndef MQTT_PORT_BIONIC_ASM_H
#define MQTT_PORT_BIONIC_ASM_H

#define ENTRY(name)          \
    .text;                   \
    .globl name;             \
    .balign 64;              \
    .type name, %function;   \
    name:                    \
    .fnstart;                \
    .cfi_startproc;

#define ENTRY_PRIVATE(name)  \
    .text;                   \
    .balign 64;              \
    .type name, %function;   \
    name:                    \
    .fnstart;                \
    .cfi_startproc;

#define END(name)            \
    .cfi_endproc;            \
    .size name, . - name;    \
    .cantunwind;             \
    .fnend

#endif /* MQTT_PORT_BIONIC_ASM_H */
