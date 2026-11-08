# What src/port/armv7m needs from the assembler, read by the Makefile.
#
# arch_memset.S has conditional instructions outside IT blocks. In Thumb-2
# each needs an IT before it, which the assembler adds when asked. GNU as and
# clang both take the option through -Wa. See port_memset.S.
PORT_ASFLAGS := -Wa,-mimplicit-it=always
