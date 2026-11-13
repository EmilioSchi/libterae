# Builds libterae. See docs/en/building-en.pdf.
#
#   make              the static library build/libterae.a
#   make OS=freertos PORT=auto CROSS_COMPILE=arm-none-eabi- CFLAGS='-mcpu=cortex-m4 -mthumb -O2' \
#        RTOS_INCLUDES='-I...'
#                     the library for another system and processor, in build/freertos-armv7m: libterae.a, and
#                     libterae_port.a, the memory functions the compiler calls, which a program links after
#                     it. Every target takes these:
#     OS=<os>         a folder of platform/: posix, linux, macos, win32, freertos or ucos3, or none for src/
#                     alone, for a program with sockets and a clock of its own. It defaults to this machine's
#                     system: macos on Darwin, linux on Linux, and posix anywhere else.
#     PORT=<arch>     a folder of src/port/, for a build with no C library: auto for the one that fits CC and
#                     CFLAGS, as src/port/select.in picks it and make print-port shows, or generic for the C
#                     versions, which any processor runs. Without PORT the C library gives them.
#     CROSS_COMPILE=  the prefix of a GNU toolchain's programs, as arm-none-eabi-, which then gives CC. AR is
#                     the one of CC's toolchain unless given.
#     WITH_TLS=yes    platform/ with TLS, through Mbed TLS, built with platform/mbedtls_config.h as its
#                     MBEDTLS_USER_CONFIG_FILE. The default is no. A program links Mbed TLS after libterae.a.
#     MBEDTLS_PREFIX= the folder Mbed TLS is installed in, whose include/ has its headers, when the compiler
#                     doesn't find them by itself.
#     BUILD=<dir>     where the build goes: build/ for this machine's system without PORT or TLS, and
#                     otherwise a folder of build/ named by TLS, OS and PORT, as build/tls,
#                     build/freertos-armv7m, build/none-avr or build/win32.
#   make port PORT=aarch64 CC='clang --target=aarch64-linux-musl'
#                     libterae_port.a alone.
#   make OS=posix     the library for another system. One this machine isn't sets CC too, as with
#                     CC='zig cc -target x86_64-windows-gnu'.
#   make PAGE_SIZE=4096 BUILD=build/page4096
#                     the library with pages of another size than this machine's: a power of two from 4096
#                     to 65536. The library and what uses it need the same one, so give it its own BUILD
#                     folder. A build for another system, as with CC='clang --target=...', sets the target's
#                     here too.
#   make print-port PORT=auto
#                     the folder of src/port/ that PORT=auto takes for CC and CFLAGS.
#   make shared       the library as a shared one, which a program finds when it starts, in lib/:
#                     libterae.<major>.dylib on macOS and libterae.so.<version> elsewhere, with the names a
#                     loader and a linker look for linked to it, and pkgconfig/libterae.pc, the flags a program
#                     is compiled and linked with. A build for another system, or with TLS, goes to a folder of
#                     lib/ named as its folder of build/ is: lib/win32 has libterae-<major>.dll and its import
#                     library terae.lib, and lib/tls a library with Mbed TLS inside it. It needs a system with
#                     a loader, so not freertos, ucos3 or none, and no PORT. LIBDIR=<dir> puts it elsewhere.
#   make print-version
#                     the version of the library, from include/base_library.h, as 1.0.5.

# CROSS_COMPILE names a GNU toolchain by the prefix of its programs, for CC unless it is given.
ifneq ($(CROSS_COMPILE),)
ifeq ($(origin CC),default)
CC := $(CROSS_COMPILE)gcc
endif
endif
CC       ?= cc
# The archiver of CC's own toolchain, as this machine's may write no index for another processor's objects:
# clang's llvm-ar where clang has one beside it, or else the ar GCC names. Other compilers name the one on the
# PATH.
ifeq ($(origin AR),default)
AR_LLVM := $(wildcard $(shell $(CC) -print-prog-name=llvm-ar 2>/dev/null))
AR := $(or $(AR_LLVM),$(shell $(CC) -print-prog-name=ar 2>/dev/null),ar)
endif
CFLAGS   ?= -O2 -g
STRICT   := -std=c99 -pedantic -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion
# The page size of the system this runs on, from what uname says, unless PAGE_SIZE gives one: 16 KiB on
# Apple's ARM processors and on LoongArch, 64 KiB on Linux on POWER, 4 KiB elsewhere. See base_page.h.
HOST_OS  := $(shell uname -s)
HOST_CPU := $(shell uname -m)
# The folder of platform/ for the system this runs on, and the one built for.
HOST_PLATFORM := $(if $(filter Darwin,$(HOST_OS)),macos,$(if $(filter Linux,$(HOST_OS)),linux,posix))
OS       ?= $(HOST_PLATFORM)
# The folders of src/port/, and the one the build takes. PORT=auto asks the compiler, with the build's CC and
# CFLAGS, which folder fits the processor: src/port/select.in names it from the macros the compiler defines.
PORT_FOLDERS := $(patsubst src/port/%/,%,$(dir $(wildcard src/port/*/NOTICE)))
ifeq ($(PORT),auto)
PORT_PICKED := $(subst ",,$(strip $(shell $(CC) $(CFLAGS) -E -P -x c src/port/select.in 2>/dev/null)))
ifeq ($(PORT_PICKED),)
$(error PORT=auto: $(CC) couldn't read src/port/select.in, so the processor isn't known: name a folder instead)
endif
override PORT := $(PORT_PICKED)
endif
ifneq ($(PORT),)
ifeq ($(filter $(PORT),$(PORT_FOLDERS)),)
$(error PORT=$(PORT) names no folder of src/port/: use auto, or one of $(PORT_FOLDERS))
endif
endif
# WITH_TLS=yes builds platform/ with TLS, and no, the default, without it.
WITH_TLS ?= no
ifneq ($(filter-out yes no,$(WITH_TLS)),)
$(error WITH_TLS=$(WITH_TLS): use yes or no)
endif
# A build for another system or processor than this machine's, or with TLS, has folders of its own, as
# build/freertos-armv7m or build/tls, so that two builds never share objects.
OS_TAG   := $(if $(filter $(HOST_PLATFORM),$(OS)),,-$(OS))
PORT_TAG := $(if $(PORT),-$(PORT))
TLS_TAG  := $(if $(filter yes,$(WITH_TLS)),tls)
BUILD_TAG := $(patsubst -%,%,$(TLS_TAG)$(OS_TAG)$(PORT_TAG))
BUILD_DEFAULT := build$(if $(BUILD_TAG),/$(BUILD_TAG))
BUILD    ?= $(BUILD_DEFAULT)
# make shared puts the shared library in a folder of lib/ named as the build's folder of build/ is, or in a
# folder lib/ inside BUILD when BUILD is given, as a build with another page size is.
LIBDIR   ?= $(if $(filter $(BUILD_DEFAULT),$(BUILD)),lib$(if $(BUILD_TAG),/$(BUILD_TAG)),$(BUILD)/lib)
# The library's version, major.minor.patch, from include/base_library.h, where a program reads it too.
version_part = $(shell sed -n 's/^.define MQTT_LIBRARY_VERSION_$(1) \([0-9][0-9]*\)U$$/\1/p' include/base_library.h)
VERSION_MAJOR := $(call version_part,MAJOR)
VERSION_MINOR := $(call version_part,MINOR)
VERSION_PATCH := $(call version_part,PATCH)
ifneq ($(words $(VERSION_MAJOR) $(VERSION_MINOR) $(VERSION_PATCH)),3)
$(error include/base_library.h gives no version in MQTT_LIBRARY_VERSION_MAJOR, _MINOR and _PATCH)
endif
VERSION  := $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_PATCH)
PAGE_SIZE ?= $(if $(and $(filter Darwin,$(HOST_OS)),$(filter arm64 aarch64,$(HOST_CPU))),16384,$\
             $(if $(and $(filter Linux,$(HOST_OS)),$(filter ppc64 ppc64le,$(HOST_CPU))),65536,$\
             $(if $(filter loongarch64,$(HOST_CPU)),16384,4096)))
# Sizes a small system needs smaller than the defaults, each left to its header unless set here or by the
# system's folder: WAIT_SOCKETS for MQTT_PLATFORM_WAIT_SOCKETS, SOCKETS for MQTT_PLATFORM_SOCKETS, the size
# of a table of handles, TLS_SESSIONS for MQTT_NET_TLS_SESSIONS, TLS_POOL_BYTES for MQTT_TLS_POOL_BYTES and
# NET_EVENTS for MQTT_NET_EVENTS.
WAIT_SOCKETS ?= $(WAIT_SOCKETS_$(OS))
SOCKETS      ?= $(SOCKETS_$(OS))
NET_EVENTS   ?= $(NET_EVENTS_$(OS))
TLS_SESSIONS ?= $(TLS_SESSIONS_$(OS))
SIZES     = $(if $(WAIT_SOCKETS),-DMQTT_PLATFORM_WAIT_SOCKETS=$(WAIT_SOCKETS)U) \
            $(if $(SOCKETS),-DMQTT_PLATFORM_SOCKETS=$(SOCKETS)U) \
            $(if $(TLS_SESSIONS),-DMQTT_NET_TLS_SESSIONS=$(TLS_SESSIONS)U) \
            $(if $(TLS_POOL_BYTES),-DMQTT_TLS_POOL_BYTES=$(TLS_POOL_BYTES)UL) \
            $(if $(NET_EVENTS),-DMQTT_NET_EVENTS=$(NET_EVENTS)U)
# They go everywhere the headers are read, as they set how much memory the server is given.
INCLUDES  = -Iinclude -Iplatform -DMQTT_PAGE_SIZE=$(PAGE_SIZE)U $(SIZES)
# src/ calls nothing outside itself. Without -ffreestanding the compiler may turn its byte loops into
# calls to memcpy and memset. With PORT set, base_string.c copies with src/port/$(PORT)'s memcpy instead of
# its own loop, so the library then needs $(BUILD)/libterae_port.a. src/port/generic's memcpy is the same
# loop as base_string.c's own, so PORT=generic leaves it as it is.
LIBRARY  := -ffreestanding $(if $(filter-out generic,$(PORT)),-DMQTT_PORT_MEMORY)

SRC      := $(wildcard src/*.c)
OBJ      := $(SRC:src/%.c=$(BUILD)/src/%.o)
# platform/ holds what doesn't depend on the system, and a folder for each system holds what does. linux/
# and macos/ use posix/ for everything they don't do their own way: they wait with their own sets of
# sockets, epoll and kqueue, through platform_wait_set.c.
# platform_wait.c waits in tiers on an array of entries, as poll takes them, and a system's folder keeps the
# entries. Random bytes come from /dev/urandom in posix/, and from the kernel's own calls in linux/ and
# macos/.
PLATFORM_COMMON := $(addprefix platform/platform_,net_server.c net_store.c net_client.c net_runs.c tls.c \
                   parse.c)
# A store is kept in a folder of files through platform_store_file.c, on each system with files, and in a
# region of the board's flash through platform_store_flash.c on the real-time systems. posix/ syncs a file
# with fsync, and macos/ with F_FULLFSYNC, as fsync there doesn't reach the drive's own cache.
STORE_FILE := platform/platform_store_file.c
POSIX_SRC := $(STORE_FILE) $(addprefix platform/posix/posix_,socket.c options.c signal.c clock.c wait_one.c \
             probe.c file.c)
POSIX_POLL := platform/platform_wait.c platform/posix/posix_poll.c
OS_SRC_posix := $(POSIX_SRC) $(POSIX_POLL) $(addprefix platform/posix/posix_,random.c sync.c)
OS_SRC_linux := $(POSIX_SRC) platform/platform_wait_set.c platform/posix/posix_sync.c \
                $(addprefix platform/linux/linux_,epoll.c random.c)
OS_SRC_macos := $(POSIX_SRC) platform/platform_wait_set.c \
                $(addprefix platform/macos/macos_,kqueue.c random.c sync.c)
# win32/ waits with WSAPoll through platform_wait.c, keeps its SOCKETs in a table of platform_slots.c, and
# has no processes to copy or signals to interrupt with. A build for it sets CC, as with
# CC='zig cc -target x86_64-windows-gnu' or x86-windows-gnu for 32 bits.
OS_SRC_win32 := platform/platform_wait.c platform/platform_slots.c platform/platform_probe_none.c \
                $(STORE_FILE) $(addprefix platform/win32/win32_,socket.c system.c poll.c random.c file.c)
# freertos/ uses FreeRTOS+TCP's sockets, waits with its socket sets through platform_wait_set.c, and keeps
# its Socket_t in a table of platform_slots.c. A build for it sets CC and CFLAGS for the processor, and
# RTOS_INCLUDES to where the kernel's and FreeRTOS+TCP's headers and the program's configuration are.
OS_SRC_freertos := platform/platform_wait_set.c platform/platform_slots.c platform/platform_probe_none.c \
                   platform/platform_store_flash.c \
                   $(addprefix platform/freertos/freertos_,socket.c system.c select.c random.c flash.c)
# ucos3/ uses uC/TCP-IP's own sockets, whose numbers are the handles, and waits with NetSock_Sel through
# platform_wait.c, as it takes sets of sockets as select does. It is built as freertos/ is.
OS_SRC_ucos3 := platform/platform_wait.c platform/platform_probe_none.c platform/platform_store_flash.c \
                $(addprefix platform/ucos3/ucos3_,socket.c system.c select.c random.c flash.c)
# What each folder is compiled with: platform/ itself with no system at all, and posix/ with POSIX alone.
DEFS_platform :=
DEFS_posix    := -D_POSIX_C_SOURCE=200809L
DEFS_linux    := -D_POSIX_C_SOURCE=200809L
DEFS_macos    := -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE
DEFS_win32    := -U_WIN32_WINNT -D_WIN32_WINNT=0x0600 -DWIN32_LEAN_AND_MEAN
DEFS_freertos  = $(RTOS_INCLUDES)
DEFS_ucos3     = $(RTOS_INCLUDES)
# A small system's sizes, unless the command line gives others.
WAIT_SOCKETS_freertos := 32
SOCKETS_freertos      := 48
NET_EVENTS_freertos   := 32
TLS_SESSIONS_freertos := 1
WAIT_SOCKETS_ucos3    := 32
NET_EVENTS_ucos3      := 32
TLS_SESSIONS_ucos3    := 1
ifeq ($(filter $(OS),posix linux macos win32 freertos ucos3 none),)
$(error OS=$(OS) names no folder of platform/: use posix, linux, macos, win32, freertos or ucos3, or none)
endif
# OS=none builds src/ alone, for a program with sockets and a clock of its own: nothing of platform/, and so
# no TLS, which is in platform/.
PLATFORM := $(if $(filter none,$(OS)),,$(PLATFORM_COMMON) $(OS_SRC_$(OS)))
ifeq ($(OS)$(WITH_TLS),noneyes)
$(error OS=none builds no platform/, where TLS is: use WITH_TLS=yes with a system's folder)
endif
PLATFORM_OBJ := $(PLATFORM:platform/%.c=$(BUILD)/platform/%.o)
# A shared library is found by a loader, which a system without one doesn't have, and it is loaded beside the
# C library, whose memory functions src/port/'s would take the place of for the whole program.
ifneq ($(filter shared,$(MAKECMDGOALS)),)
ifneq ($(filter freertos ucos3 none,$(OS)),)
$(error make shared: OS=$(OS) has no loader to find a shared library, so a firmware links libterae.a)
endif
ifneq ($(PORT),)
$(error make shared: PORT is for a program with no C library, and a shared library runs beside one: leave it out)
endif
endif
# With TLS, platform_tls.c has what base_tls.h declares, and without it each of those functions reports
# MQTT_TLS_ERROR. Mbed TLS's configuration is named by its whole path: Mbed TLS's own is also called
# mbedtls_config.h, and is found first by a bare name, as it sits beside the header that includes the user's.
TLS_FLAGS := $(if $(filter yes,$(WITH_TLS)),-DMQTT_WITH_TLS $(if $(MBEDTLS_PREFIX),-I$(MBEDTLS_PREFIX)/include) \
             -DMBEDTLS_USER_CONFIG_FILE=\"$(abspath platform/mbedtls_config.h)\")
PORT_SRC := $(wildcard src/port/$(PORT)/port_*.S)
# PORT=generic takes all four of src/port/generic's C files, which the other folders include one by one.
PORT_C   := $(if $(filter generic,$(PORT)),$(wildcard src/port/generic/mem*.c),\
                 $(wildcard src/port/$(PORT)/port_*.c))
# A folder's port.mk gives what its assembly needs from the assembler, as PORT_ASFLAGS.
PORT_ASFLAGS :=
-include $(if $(PORT),src/port/$(PORT)/port.mk)
PORT_OBJ := $(PORT_SRC:src/port/$(PORT)/%.S=$(BUILD)/port/$(PORT)/%.o) \
            $(PORT_C:src/port/$(PORT)/%.c=$(BUILD)/port/$(PORT)/%.c.o)
# With PORT naming a folder of assembly, libterae.a copies with its memcpy, so a program links
# libterae_port.a after it.
PORT_LIB  := $(if $(PORT),$(BUILD)/libterae_port.a)
# src/port/generic's C memory functions, where a folder has no assembly for them. GCC also needs
# -fno-tree-loop-distribute-patterns, which clang doesn't take, or it may turn their loops into calls to
# themselves.
PORT_CFLAGS := -std=c99 -ffreestanding -fno-builtin \
               $(shell $(CC) -fno-tree-loop-distribute-patterns -x c -c /dev/null -o /dev/null 2>/dev/null && \
                       echo -fno-tree-loop-distribute-patterns)
# The shared library is built from objects of its own, in $(BUILD)/shared, as code that runs at any address:
# -fPIC, which Windows doesn't take, as a DLL is moved by its relocations instead.
SHARED_DIR := $(BUILD)/shared
SHARED_OBJ := $(OBJ:$(BUILD)/%=$(SHARED_DIR)/%) $(PLATFORM_OBJ:$(BUILD)/%=$(SHARED_DIR)/%)
PIC      := $(if $(filter win32,$(OS)),,-fPIC)
# The file each system loads, and the names linked to it: the one a program records when it is linked, which
# changes with the major version, and the one a linker finds for -lterae. On Windows a program records the
# DLL's name from its import library, terae.lib, which -lterae finds with GNU ld, lld and zig alike, and
# which Microsoft's linker takes by name.
SHARED_FILE_posix    := libterae.so.$(VERSION)
SHARED_ALIASES_posix := libterae.so.$(VERSION_MAJOR) libterae.so
SHARED_FILE_linux    := $(SHARED_FILE_posix)
SHARED_ALIASES_linux := $(SHARED_ALIASES_posix)
SHARED_FILE_macos    := libterae.$(VERSION_MAJOR).dylib
SHARED_ALIASES_macos := libterae.dylib
SHARED_FILE_win32    := libterae-$(VERSION_MAJOR).dll
SHARED_FILE  := $(SHARED_FILE_$(OS))
# A program sees only the functions whose names start with mqtt, which are all those of include/: the rest
# stay inside the library, so that a function of the program with the same name can't take their place. On
# ELF systems a list for the linker says so, as GNU ld, gold, lld and mold take it, and on macOS a pattern.
# A DLL lists every function, which is safe, as Windows binds each call to the DLL it names. macOS also
# records the version, and the earliest one with everything a program linked now may use, which its loader
# checks.
SHARED_EXPORTS := $(SHARED_DIR)/exports.map
SHARED_MAP_posix := $(SHARED_EXPORTS)
SHARED_MAP_linux := $(SHARED_EXPORTS)
SHARED_LDFLAGS_posix = -shared -Wl,-soname,libterae.so.$(VERSION_MAJOR) -Wl,--version-script,$(SHARED_EXPORTS)
SHARED_LDFLAGS_linux = $(SHARED_LDFLAGS_posix)
SHARED_LDFLAGS_macos = -dynamiclib -install_name @rpath/$(SHARED_FILE_macos) \
                       -compatibility_version $(VERSION_MAJOR).$(VERSION_MINOR) -current_version $(VERSION) \
                       -Wl,-exported_symbol,'_mqtt*'
SHARED_LDFLAGS_win32 = -shared -Wl,--out-implib,$(LIBDIR)/terae.lib
# What the shared library is linked with, as nothing may be left for the program to give: Mbed TLS, from
# MBEDTLS_PREFIX's lib/ when it is given, whose archives then go inside it, and Windows' sockets and random
# bytes.
SHARED_LIBS := $(if $(filter yes,$(WITH_TLS)),$(if $(MBEDTLS_PREFIX),-L$(MBEDTLS_PREFIX)/lib) \
               -lmbedtls -lmbedx509 -lmbedcrypto) $(if $(filter win32,$(OS)),-lws2_32 -lbcrypt)
# The run-time search path a program on this system is linked with to find it in LIBDIR, for libterae.pc.
COMMA    := ,
SHARED_RPATH := $(if $(filter win32,$(OS)),,-Wl$(COMMA)-rpath$(COMMA)$${libdir})
# make can't see when the flags change, so each build folder keeps a file named by a checksum of them, and
# everything in it is built again when that changes: a folder can't keep objects built for another processor.
FLAGS_SEEN = $(CC) $(CFLAGS) $(STRICT) $(LIBRARY) $(INCLUDES) $(PLATFORM_DEFS) $(TLS_FLAGS) $(RTOS_INCLUDES) \
             $(DEFS_$(OS)) $(PORT_ASFLAGS) $(PORT_CFLAGS)
FLAGS_STAMP := $(BUILD)/flags-$(firstword $(shell printf '%s' '$(subst ','\'',$(FLAGS_SEEN))' | cksum))

.PHONY: all port print-port shared print-version clean

all: $(BUILD)/libterae.a $(PORT_LIB)

$(FLAGS_STAMP):
	@mkdir -p $(dir $@)
	@rm -f $(BUILD)/flags-*
	@touch $@

$(BUILD)/src/%.o: src/%.c $(wildcard include/*.h) $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(STRICT) $(LIBRARY) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Each file takes the definitions of its folder, as DEFS_<folder> gives them. PLATFORM_DEFS adds others.
$(BUILD)/platform/%.o: platform/%.c $(wildcard include/*.h) $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) -std=c99 $(DEFS_$(notdir $(patsubst %/,%,$(dir $<)))) -Wall -Wextra -Werror $(CFLAGS) $(INCLUDES) \
	    $(PLATFORM_DEFS) $(TLS_FLAGS) -c $< -o $@

$(SHARED_DIR)/src/%.o: src/%.c $(wildcard include/*.h) $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(STRICT) $(LIBRARY) $(CFLAGS) $(PIC) $(INCLUDES) -c $< -o $@

$(SHARED_DIR)/platform/%.o: platform/%.c $(wildcard include/*.h) $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) -std=c99 $(DEFS_$(notdir $(patsubst %/,%,$(dir $<)))) -Wall -Wextra -Werror $(CFLAGS) $(PIC) $(INCLUDES) \
	    $(PLATFORM_DEFS) $(TLS_FLAGS) -c $< -o $@

$(BUILD)/port/$(PORT)/%.o: src/port/$(PORT)/%.S $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(PORT_ASFLAGS) -Isrc/port/$(PORT) -c $< -o $@

$(BUILD)/port/$(PORT)/%.c.o: src/port/$(PORT)/%.c $(wildcard src/port/generic/*.c) $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(PORT_CFLAGS) $(CFLAGS) -c $< -o $@

# Not part of libterae.a: in a build with a C library these would replace its own functions.
port: $(PORT_LIB)
	@test -n "$(PORT)" || { echo "error: name a folder of src/port/, as in make port PORT=aarch64, or PORT=auto" >&2; \
	                        exit 2; }

$(BUILD)/libterae_port.a: $(PORT_OBJ)
	@rm -f $@
	$(AR) rcs $@ $^

$(BUILD)/libterae.a: $(OBJ) $(PLATFORM_OBJ)
	@rm -f $@
	$(AR) rcs $@ $^

# The shared library, the names linked to it, and what a program is built with to use it.
shared: $(LIBDIR)/$(SHARED_FILE) $(LIBDIR)/pkgconfig/libterae.pc

$(SHARED_EXPORTS): $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	printf '%s\n' '{' '  global: mqtt*;' '  local: *;' '};' > $@

$(LIBDIR)/$(SHARED_FILE): $(SHARED_OBJ) $(SHARED_MAP_$(OS)) $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) $(SHARED_LDFLAGS_$(OS)) $(SHARED_OBJ) $(SHARED_LIBS) -o $@
	for alias in $(SHARED_ALIASES_$(OS)); do ln -sf $(SHARED_FILE) $(LIBDIR)/$$alias || exit 1; done

# The page size and the sizes of the tables go in Cflags, as the program has to be compiled with them.
$(LIBDIR)/pkgconfig/libterae.pc: include/base_library.h $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	printf '%s\n' 'prefix=$(abspath .)' 'includedir=$${prefix}/include' 'libdir=$(abspath $(LIBDIR))' '' \
	    'Name: libterae' 'Description: MQTT 3.1, 3.1.1 and 5.0 Server and Client' 'Version: $(VERSION)' \
	    'Cflags: $(strip -I$${includedir} -DMQTT_PAGE_SIZE=$(PAGE_SIZE)U $(SIZES))' \
	    'Libs: -L$${libdir} -lterae $(SHARED_RPATH)' > $@

# The folder of src/port/ the build takes, as PORT=auto finds it for CC and CFLAGS.
print-port:
	@echo $(PORT)

print-version:
	@echo $(VERSION)

# A LIBDIR given to make is left alone, as it may be a folder that holds more than this library.
clean:
	rm -rf $(BUILD) $(if $(filter file,$(origin LIBDIR)),$(LIBDIR))
