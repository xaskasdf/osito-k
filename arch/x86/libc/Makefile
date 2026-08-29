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

# Cross-toolchain: produces x86_64 ELF objects (host cc on macOS would
# emit Mach-O, which the cross-ld can't link).  Used by hello_gl.elf
# and crtgl.o (W4.8).
XCC ?= x86_64-elf-gcc
XLD ?= x86_64-elf-ld

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

all: $(LIBC)/crt.o $(LIBC)/syscall.o $(LIBC)/tcclib.o $(LIBC)/malloc.o $(LIBC)/math.o $(LIBC)/crtgl.o

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

# W4.8 — libc extras for linking against libGL.a (Mesa).  Use the
# cross-compiler so the .o is x86_64 ELF (host clang on macOS would
# emit Mach-O).
$(LIBC)/crtgl.o: crtgl.c | $(LIBC)
	$(XCC) $(CFLAGS) $(QJS_HEADERS) $< -o $@

clean:
	rm -rf $(LIBC)

# ── Build test program ──

$(TEST)/hello_c.elf: $(LIBC)/crt.o $(LIBC)/syscall.o $(TEST)/hello_c.c
	$(TCC) -c -nostdlib -nostdinc $(TEST)/hello_c.c -o $(BUILD)/hello_c.o
	$(LD) -nostdlib -static -no-pie -e _start -Ttext=0x401000 \
	    $(LIBC)/crt.o $(LIBC)/syscall.o $(BUILD)/hello_c.o -o $@

hello_c.elf: $(TEST)/hello_c.elf

# ── Build minimal OpenGL smoke test (W4.7c) ──
# Links against build/lib/opengl/libGL.a + build/lib/vulkan/libvulkan.a
# (and the supporting Mesa static libs needed by libGL).
GL_INC := -I../lib/opengl/include -I../lib/opengl/mesa/include
# libstdc++ + libsupc++ from cross sysroot — Mesa's GLSL frontend (compiler/glsl/*.cpp)
# and the new mesa/main/*.cpp / state_tracker/*.cpp TUs use std::vector,
# std::unordered_map, etc. plus the C++ ABI runtime (vtables, typeinfo).
SYSROOT_LIB := /Users/pc/ok-ported/toolchain/sysroot/usr/lib
GL_LIBS := $(BUILD)/lib/opengl/libGL.a \
           $(BUILD)/lib/opengl/libmesa_main.a \
           $(BUILD)/lib/opengl/libmesa_zink.a \
           $(BUILD)/lib/opengl/libmesa_gallium.a \
           $(BUILD)/lib/opengl/libmesa_compiler.a \
           $(BUILD)/lib/opengl/libmesa_util.a \
           $(BUILD)/lib/vulkan/libvulkan.a \
           $(SYSROOT_LIB)/libstdc++.a \
           $(SYSROOT_LIB)/libsupc++.a

# Use the cross-toolchain explicitly (matches how the prebuilt libGL.a was
# produced — system 'ld' on macOS doesn't speak the ELF flag set).
# (XCC/XLD already defined above.)

$(TEST)/hello_gl.elf: $(LIBC)/crt.o $(LIBC)/syscall.o $(LIBC)/math.o $(LIBC)/malloc.o $(LIBC)/tcclib.o $(LIBC)/crtgl.o $(TEST)/hello_gl.c
	$(XCC) -c -O2 -fno-pie -ffreestanding -nostdinc \
	    $(GL_INC) -isystem qjs_headers \
	    $(TEST)/hello_gl.c -o $(BUILD)/hello_gl.o
	$(XLD) -nostdlib -static -no-pie -e _start -Ttext=0x401000 \
	    --allow-multiple-definition \
	    $(LIBC)/crt.o $(LIBC)/syscall.o $(LIBC)/math.o $(LIBC)/malloc.o \
	    $(LIBC)/tcclib.o $(LIBC)/crtgl.o \
	    $(BUILD)/hello_gl.o \
	    --start-group $(GL_LIBS) --end-group \
	    -o $@
	@ls -lh $@

hello_gl.elf: $(TEST)/hello_gl.elf

# Real Venus user-space transport smoke test.  The public Vulkan archive
# intentionally exposes only host-validated entry points at this stage.
VK_INC := -I../lib/vulkan/include

$(TEST)/hello_vk.elf: $(LIBC)/crt.o $(LIBC)/syscall.o $(LIBC)/malloc.o \
                      $(LIBC)/tcclib.o $(TEST)/hello_vk.c \
                      $(BUILD)/lib/vulkan/libvulkan.a
	$(XCC) -c -O2 -fno-pie -ffreestanding -nostdinc \
	    $(VK_INC) -isystem qjs_headers \
	    $(TEST)/hello_vk.c -o $(BUILD)/hello_vk.o
	$(XLD) -nostdlib -static -no-pie -e _start -Ttext=0x401000 \
	    --allow-multiple-definition \
	    $(LIBC)/crt.o $(LIBC)/syscall.o $(LIBC)/malloc.o $(LIBC)/tcclib.o \
	    $(BUILD)/hello_vk.o $(BUILD)/lib/vulkan/libvulkan.a -o $@
	@ls -lh $@

hello_vk.elf: $(TEST)/hello_vk.elf

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
