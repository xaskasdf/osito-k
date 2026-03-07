#
# OsitoK x86-64 — Minimal libc for userspace programs
#
# Usage:
#   make                    # build crt.o + syscall.o
#   make hello_c.elf        # build test program
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

all: $(LIBC)/crt.o $(LIBC)/syscall.o

$(LIBC):
	mkdir -p $(LIBC)

$(LIBC)/crt.o: crt.c | $(LIBC)
	$(TCC) -c -nostdlib -nostdinc $< -o $@

$(LIBC)/syscall.o: syscall.S | $(LIBC)
	$(CC) -c -nostdlib $< -o $@

clean:
	rm -rf $(LIBC)

# ── Build test program ──
TEST := ../test

$(TEST)/hello_c.elf: $(LIBC)/crt.o $(LIBC)/syscall.o $(TEST)/hello_c.c
	$(TCC) -c -nostdlib -nostdinc $(TEST)/hello_c.c -o $(BUILD)/hello_c.o
	$(LD) -nostdlib -static -no-pie -e _start -Ttext=0x401000 \
	    $(LIBC)/crt.o $(LIBC)/syscall.o $(BUILD)/hello_c.o -o $@

hello_c.elf: $(TEST)/hello_c.elf
