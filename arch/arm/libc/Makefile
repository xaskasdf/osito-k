#
# Makefile -- the Osito-K AArch64 libc, and the programs in ../apps
#
#   make -C arch/arm/libc                       # build/libc/libc.a + build/apps/*.elf
#   make -C arch/arm/libc CROSS=aarch64-elf-    # with the Mac's toolchain
#   make -C arch/arm/libc APPS="hello primes"   # only those
#
# Programs are static PIEs: the kernel (kernel/elf.c) loads them wherever
# the page allocator has room and applies their R_AARCH64_RELATIVE
# relocations, as start.S does for the kernel. `check` makes sure they have
# no other kind. Copy one to the phone with arch/arm/scripts/rog5-put.ps1
# and run it with `exec <name>`; arch/arm/scripts/rog5-app.sh does it all.
#

CROSS   ?= aarch64-linux-gnu-
CC      := $(CROSS)gcc
AR      := $(CROSS)ar
READELF := $(CROSS)readelf

BUILD   := ../build/libc
OUT     := ../build/apps
APPS    ?= $(basename $(notdir $(wildcard ../apps/*.c)))

# -fno-tree-loop-distribute-patterns: gcc would turn memcpy's and memset's
# own loops into calls to themselves. armv8-a: QEMU virt's A72 runs them too.
CFLAGS  := -O2 -ffreestanding -fPIE -fno-stack-protector -fno-tree-loop-distribute-patterns \
           -march=armv8-a -Wall -Wextra -Iinclude
LDFLAGS := -nostdlib -pie -Wl,--no-dynamic-linker -Wl,-z,text -Wl,-e,_start -Wl,-u,_start

LIBC_SRCS := sys.c string.c stdlib.c stdio.c
LIBC_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(LIBC_SRCS))
HEADERS   := $(wildcard include/*.h include/sys/*.h)
ELFS      := $(patsubst %,$(OUT)/%.elf,$(APPS))

.PHONY: all check clean

all: $(BUILD)/libc.a $(ELFS)

$(BUILD)/%.o: %.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/libc.a: $(LIBC_OBJS)
	$(AR) rcs $@ $^

$(OUT)/%.elf: ../apps/%.c $(BUILD)/libc.a $(HEADERS)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(BUILD)/libc.a -lgcc -o $@

# Each program: a PIE with RELATIVE relocations only
check: $(ELFS)
	@for f in $(ELFS); do \
	    $(READELF) -h $$f | grep -q 'DYN' || { echo "$$f: not a PIE"; exit 1; }; \
	    bad=$$($(READELF) -rW $$f | awk '/^[0-9a-f]+ /{print $$3}' | grep -vx R_AARCH64_RELATIVE | sort -u); \
	    if [ -n "$$bad" ]; then echo "$$f: relocations the kernel can't apply: $$bad"; exit 1; fi; \
	    echo "$$f: ok"; \
	done

clean:
	rm -rf $(BUILD) $(OUT)
