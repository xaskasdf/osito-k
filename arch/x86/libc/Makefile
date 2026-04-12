#
# OsitoK x86-64 — Minimal libc for userspace programs
#
# Usage:
#   make                    # build crt.o + syscall.o + tcclib.o
#   make hello_c.elf        # build test program
#   make tcc.elf            # build TCC compiler for OsitoK
#   make qjs.elf            # build QuickJS interpreter for OsitoK
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
TEST   := ../test

# Common flags for freestanding libc objects
CFLAGS = -c -O2 -std=gnu11 -fno-builtin -fno-stack-protector -fno-pie \
         -nostdinc -ffreestanding

# QuickJS headers (shim layer providing stdlib/stdio/math/etc)
QJS_HEADERS = -isystem qjs_headers

# QuickJS source directory
QJS_SRC ?= /tmp/quickjs-2024-01-13

# QuickJS compile flags
QJS_CFLAGS = $(CFLAGS) $(QJS_HEADERS) -I$(QJS_SRC) \
             -DEMSCRIPTEN -DCONFIG_BIGNUM -DCONFIG_VERSION=\"2024-01-13\"

.PHONY: all clean

all: $(LIBC)/crt.o $(LIBC)/syscall.o $(LIBC)/tcclib.o $(LIBC)/malloc.o $(LIBC)/math.o

$(LIBC):
	mkdir -p $(LIBC)

$(LIBC)/crt.o: crt.c | $(LIBC)
	$(CC) $(CFLAGS) $(QJS_HEADERS) $< -o $@

$(LIBC)/malloc.o: malloc.c | $(LIBC)
	$(CC) $(CFLAGS) $(QJS_HEADERS) $< -o $@
$(LIBC)/syscall.o: syscall.S | $(LIBC)
	$(CC) -c -nostdlib $< -o $@

$(LIBC)/tcclib.o: tcclib.c | $(LIBC)
	$(CC) $(CFLAGS) $(QJS_HEADERS) $< -o $@

$(LIBC)/math.o: math.c | $(LIBC)
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -rf $(LIBC)

# ── Build test program ──

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

# ── Build QuickJS for OsitoK ──
QJS_OBJS = $(LIBC)/quickjs.o $(LIBC)/libbf.o $(LIBC)/cutils.o \
           $(LIBC)/libregexp.o $(LIBC)/libunicode.o $(LIBC)/qjs_main.o

$(LIBC)/quickjs.o: $(QJS_SRC)/quickjs.c | $(LIBC)
	$(CC) $(QJS_CFLAGS) $< -o $@

$(LIBC)/libbf.o: $(QJS_SRC)/libbf.c | $(LIBC)
	$(CC) $(QJS_CFLAGS) $< -o $@

$(LIBC)/cutils.o: $(QJS_SRC)/cutils.c | $(LIBC)
	$(CC) $(QJS_CFLAGS) $< -o $@

$(LIBC)/libregexp.o: $(QJS_SRC)/libregexp.c | $(LIBC)
	$(CC) $(QJS_CFLAGS) $< -o $@

$(LIBC)/libunicode.o: $(QJS_SRC)/libunicode.c | $(LIBC)
	$(CC) $(QJS_CFLAGS) $< -o $@

$(LIBC)/qjs_main.o: qjs_main.c | $(LIBC)
	$(CC) $(QJS_CFLAGS) $< -o $@

$(TEST)/qjs.elf: $(LIBC)/crt.o $(LIBC)/syscall.o $(LIBC)/tcclib.o \
                 $(LIBC)/math.o $(QJS_OBJS)
	$(LD) -nostdlib -static -no-pie -e _start -Ttext=0x401000 \
	    $(LIBC)/crt.o $(LIBC)/syscall.o $(LIBC)/tcclib.o $(LIBC)/math.o \
	    $(LIBC)/qjs_main.o \
	    --start-group \
	    $(LIBC)/quickjs.o $(LIBC)/libbf.o $(LIBC)/cutils.o \
	    $(LIBC)/libregexp.o $(LIBC)/libunicode.o \
	    --end-group \
	    -o $@
	@ls -lh $@

qjs.elf: $(TEST)/qjs.elf
