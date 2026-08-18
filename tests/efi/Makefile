#
# Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
# Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
#
# The Jitter RNG as an EFI application: the library, the arch/ backends and one
# program, linked against gnu-efi and nothing else. See README.md in this
# directory for what it demonstrates and how it is run.
#
# The library sources are compiled here rather than linked from a built
# library, as tests/gcd and tests/health do: what is wanted is the freestanding
# selection of every arch/ backend, and a library built for the host has made
# the opposite choice in each of them.
#
# Nix builds this through flake.nix: `nix build .#efi` and `.#efi-aarch64` build
# the images, and .#checks.x86_64-linux.efi-vm boots the x86_64 one. This
# Makefile is the same fallback the other test directories carry, for a tree
# with neither.
#
#   make GNUEFI=/usr                          # gnu-efi's default prefix
#   make EFI_ARCH=aarch64 GNUEFI=/path CROSS_COMPILE=aarch64-linux-gnu-
#

# x86_64 and aarch64, which are the two the firmware in the field is on.
# EFI_ARCH rather than ARCH so that cross-building reads as what it is; it
# defaults to the machine this runs on.
EFI_ARCH	?= $(shell uname -m)

# The cross toolchain prefix, empty for a native build. CC, LD and OBJCOPY may
# also be set individually, which is what a toolchain that does not follow the
# prefix convention needs.
CROSS_COMPILE	?=
CC		?= $(CROSS_COMPILE)gcc
LD		?= $(CROSS_COMPILE)ld
OBJCOPY		?= $(CROSS_COMPILE)objcopy

GNUEFI		?= /usr
EFIINC		?= $(GNUEFI)/include/efi
EFILIB		?= $(GNUEFI)/lib

ROOT		:= ../..

ifeq ($(EFI_ARCH),x86_64)

NAME		:= BOOTX64.EFI
GNUEFI_ARCH	:= x86_64
CRT0		:= $(EFILIB)/crt0-efi-x86_64.o
LDSCRIPT	:= $(EFILIB)/elf_x86_64_efi.lds

# EFI on x86_64 calls with the Microsoft convention, which is not the one this
# compiler uses by default, and it runs with no red zone because the firmware
# takes interrupts on the same stack.
ARCH_CFLAGS	:= -mno-red-zone -DGNU_EFI_USE_MS_ABI
ARCH_LDFLAGS	:=
# GCC needs this for that calling convention and Clang has no such option,
# rejecting it outright. Probed rather than tied to a compiler name, so a
# driver that spells itself something else still gets it right.
ARCH_CFLAGS	+= $(shell $(CC) -maccumulate-outgoing-args -E -x c /dev/null   \
		     > /dev/null 2>&1 && echo -maccumulate-outgoing-args)

# binutils can write PE for this architecture, so the ELF the linker produced
# is converted into one. --subsystem=10 is EFI_APPLICATION.
OBJCOPY_TARGET	:= --output-target pei-x86-64 --subsystem=10

else ifeq ($(EFI_ARCH),aarch64)

NAME		:= BOOTAA64.EFI
GNUEFI_ARCH	:= aarch64
CRT0		:= $(EFILIB)/crt0-efi-aarch64.o
LDSCRIPT	:= $(EFILIB)/elf_aarch64_efi.lds

# Nothing: EFI on aarch64 uses the same calling convention the compiler already
# emits, and there is no red zone to disable.
ARCH_CFLAGS	:=

# The firmware maps at 4 kB granularity here, and the linker's default maximum
# page size of 64 kB would leave the sections misaligned against it.
ARCH_LDFLAGS	:= -z common-page-size=4096 -z max-page-size=4096

# No out-of-line atomics, and this one is not optional. GCC 10 and later
# default to -moutline-atomics on aarch64, which turns an atomic access into a
# call to a libgcc helper - __aarch64_swp4_acq_rel for the one read-modify-write
# in arch/jitterentropy-arch-atomic.c - that picks the LSE or the LL/SC
# implementation at run time from a libgcc ifunc. A -nostdlib link has no
# libgcc, so the symbol stays undefined, its PLT slot points into nothing, and
# the first jent_atomic_exchange_int() during the startup lands on an
# undefined instruction. It cost a QEMU -d int trace to find, which is worth
# recording: "Synchronous Exception" on the console, ESR 0x0/0x2000000 (EC 0,
# unknown reason) at a link-time address, and one R_AARCH64_JUMP_SLOT against
# an undefined __aarch64_swp4_acq_rel in the image.
#
# With this the compiler emits the instructions inline instead, as it does for
# the kernel, which uses the same flag for the same reason.
#
# Probed rather than assumed: it is GCC 10 and later that has it.
ARCH_CFLAGS	+= $(shell $(CC) -mno-outline-atomics -E -x c /dev/null	       \
		     > /dev/null 2>&1 && echo -mno-outline-atomics)

# pei-aarch64-little, exactly as x86_64 uses pei-x86-64: binutils does have a
# PE target for this architecture, whatever gnu-efi's own build rules assume -
# they fall back to a flat -O binary dump wherever objcopy has no such target,
# relying on their startup code carrying a hand-written PE header. That
# fallback does not produce a loadable image here (the firmware reports no
# bootable device), and there is no reason to take it when objcopy can write
# the real thing.
OBJCOPY_TARGET	:= --output-target pei-aarch64-little --subsystem=10

else
$(error EFI_ARCH must be x86_64 or aarch64, not $(EFI_ARCH))
endif

# The first two lines are what makes the build freestanding, and
# __STDC_HOSTED__ going to zero with -ffreestanding is what selects
# JENT_BAREMETAL in jitterentropy.h - no arch/ backend then reaches for an
# operating system.
#
# -O0 is mandatory rather than a preference: the entropy core refuses to
# compile at any other level (see the #error in src/jitterentropy-base.c),
# because the loops it times are what it measures.
CFLAGS		+= -ffreestanding -fno-stack-protector -fno-strict-aliasing
CFLAGS		+= -fpic -fshort-wchar $(ARCH_CFLAGS)
CFLAGS		+= -DJENT_STATIC_LIB
CFLAGS		+= -O0 -std=c11 -Wall -Wextra
CFLAGS		+= -I$(EFIINC) -I$(EFIINC)/$(GNUEFI_ARCH) -I$(ROOT) -I$(ROOT)/src

# -Bsymbolic and the gnu-efi script produce a relocatable ELF shared object;
# the crt0 relocates the image itself at entry, which is why .rela and the
# dynamic sections have to survive the conversion below.
LDFLAGS		+= -nostdlib -znocombreloc -shared -Bsymbolic -L$(EFILIB)
LDFLAGS		+= $(ARCH_LDFLAGS)
LDLIBS		:= -lefi -lgnuefi

# .rodata is kept explicitly: gnu-efi's own recipe predates its linker script
# emitting a .rodata section of its own, and without it every string literal in
# the image is absent - the application runs and prints uninitialized memory.
OBJCOPY_SECTIONS := -j .text -j .sdata -j .data -j .rodata -j .dynamic	       \
		    -j .dynsym -j .dynstr -j .rel -j .rela -j '.rel.*'	       \
		    -j '.rela.*' -j .areloc -j .reloc

SRCS		:= $(wildcard $(ROOT)/src/*.c) $(wildcard $(ROOT)/arch/*.c)     \
		   jitterentropy-efi.c
OBJS		:= $(notdir $(SRCS:.c=.o))

.PHONY: all clean esp

all: $(NAME)

%.o: $(ROOT)/src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: $(ROOT)/arch/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

jitterentropy.so: $(OBJS)
	$(LD) $(LDFLAGS) -T $(LDSCRIPT) $(CRT0) $(OBJS) -o $@ $(LDLIBS)

$(NAME): jitterentropy.so
	$(OBJCOPY) $(OBJCOPY_SECTIONS) $(OBJCOPY_TARGET) $< $@

# An EFI system partition laid out as a directory, which is what QEMU's
# -drive file=fat:rw:<dir> serves and what the firmware boots with no boot
# entry configured: EFI/BOOT/BOOT<arch>.EFI is the removable-media default
# path, and its name is why the two architectures can share one partition.
esp: $(NAME)
	mkdir -p esp/EFI/BOOT
	cp $(NAME) esp/EFI/BOOT/$(NAME)

clean:
	@- $(RM) $(OBJS) jitterentropy.so BOOTX64.EFI BOOTAA64.EFI
	@- $(RM) -r esp
