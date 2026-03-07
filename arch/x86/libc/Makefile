#
# OsitoK x86-64 — Minimal libc for userspace programs
#
# Usage:
#   make                    # build crt.o + syscall.o + tcclib.o
#   make hello_c.elf        # build test program
#   make tcc.elf            # build TCC compiler for OsitoK
#
# Link your program:
#   tcc -c -nostdlib -nostdinc myapp.c -o myapp.o
#   ld -nostdlib -static -no-pie -e _start -Ttext=0x401000 \
#      crt.o syscall.o myapp.o -o myapp.elf
#

TCC ?= tcc
CC  ?= gcc
LD  ?= ld

BUILD  := ../build
LIBC   := $(BUILD)/libc

.PHONY: all clean

all: $(LIBC)/crt.o $(LIBC)/syscall.o $(LIBC)/tcclib.o

$(LIBC):
	mkdir -p $(LIBC)

$(LIBC)/crt.o: crt.c | $(LIBC)
	$(CC) -c -O2 -std=gnu11 -fno-builtin -fno-stack-protector -fno-pie \
	    -nostdinc -ffreestanding $< -o $@

$(LIBC)/syscall.o: syscall.S | $(LIBC)
	$(CC) -c -nostdlib $< -o $@

$(LIBC)/tcclib.o: tcclib.c | $(LIBC)
	$(CC) -c -O2 -std=gnu11 -fno-builtin -fno-stack-protector -fno-pie \
	    -nostdinc -ffreestanding $< -o $@

clean:
	rm -rf $(LIBC)

# ── Build test program ──
TEST := ../test

$(TEST)/hello_c.elf: $(LIBC)/crt.o $(LIBC)/syscall.o $(TEST)/hello_c.c
	$(TCC) -c -nostdlib -nostdinc $(TEST)/hello_c.c -o $(BUILD)/hello_c.o
	$(LD) -nostdlib -static -no-pie -e _start -Ttext=0x401000 \
	    $(LIBC)/crt.o $(LIBC)/syscall.o $(BUILD)/hello_c.o -o $@

hello_c.elf: $(TEST)/hello_c.elf

# ── Build TCC for OsitoK ──
TCC_OBJ := $(BUILD)/tcc.o

$(TEST)/tcc.elf: $(LIBC)/crt.o $(LIBC)/syscall.o $(LIBC)/tcclib.o $(TCC_OBJ)
	$(LD) -nostdlib -static -no-pie -e _start -Ttext=0x401000 \
	    $(LIBC)/crt.o $(LIBC)/syscall.o $(LIBC)/tcclib.o $(TCC_OBJ) -o $@
	@ls -lh $@

tcc.elf: $(TEST)/tcc.elf
