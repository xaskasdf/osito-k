# OsitoK — WebAssembly Architecture
#
# Compiles the x86-64 kernel to WASM via Emscripten.
# Hardware is shimmed: serial→JS terminal, keyboard→JS events,
# paging/IDT/SMP/NVMe/GPU → no-op stubs.
# Asyncify enables blocking kb_getchar() without freezing the browser.
#
# Usage:
#   make          → build/osito.js + build/osito.wasm + build/osito.html
#   make run      → emrun build/osito.html
#   make clean    → remove build/

CC      := emcc
BUILDDIR := build

# ── Include paths ────────────────────────────────────────────────
# wasm/include MUST come before x86/include to shadow types.h
# (wasm types.h no-ops port I/O and provides Emscripten-compatible defs)
INCLUDES := \
    -I include \
    -I ../x86/kernel \
    -I ../x86/fs \
    -I ../x86/drivers \
    -I ../../gui \
    -idirafter ../x86/include

# ── Compiler flags ───────────────────────────────────────────────
CFLAGS := \
    -Os \
    -std=gnu11 \
    -DWASM_BUILD=1 \
    $(INCLUDES) \
    -Wno-unused-function \
    -Wno-unused-variable

# ── Emscripten link flags ─────────────────────────────────────────
EMFLAGS := \
    -s WASM=1 \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s INITIAL_MEMORY=268435456 \
    -s MAIN_MODULE=1 \
    -s ASYNCIFY=1 \
    -s 'ASYNCIFY_IMPORTS=["emscripten_sleep"]' \
    -s 'EXPORTED_FUNCTIONS=["_main","_wasm_kb_push","_wasm_get_mem_used_kb","_wasm_compositor_frame","_input_set_mouse_abs","_input_post_mouse_button"]' \
    -s 'EXPORTED_RUNTIME_METHODS=["ccall","cwrap"]' \
    --shell-file shell.html

# ── x86 sources compiled directly ────────────────────────────────
# These are hardware-independent C99/C11 files.
# Excluded: main.c (replaced by boot/wasm_init.c),
#           serial.c, keyboard.c, framebuffer.c (replaced by WASM drivers),
#           memory.c, heap.c (replaced by hal/mem.c + malloc),
#           idt.c, paging.c, smp.c, syscall.c (no-ops in hal/stubs.c),
#           process.c, elf.c (stubbed — paging_map_page not available),
#           tensor_avx2.c (AVX2 intrinsics not supported in WASM),
#           net.c, http.c, tls.c, claude.c (network stubbed for MVP),
#           all *.S assembly files (replaced by libc setjmp or no-ops),
#           all drivers/*.c (GPU/NVMe/xHCI/HDA/i211 → no-ops in stubs.c)
X86_SRCS := \
    ../x86/kernel/shell.c \
    ../x86/kernel/terminal.c \
    ../x86/kernel/tensor.c \
    ../x86/kernel/inference.c \
    ../x86/kernel/tokenizer.c \
    ../x86/kernel/compositor.c \
    ../x86/kernel/input_events.c \
    ../x86/kernel/shm.c \
    ../x86/kernel/framebuffer.c \
    ../x86/fs/gguf.c \
    ../x86/fs/ositofs2.c

# GUI library (architecture-independent rendering)
GUI_SRCS := \
    ../../gui/gui_draw.c \
    ../../gui/gui_text.c \
    ../../gui/gui_panel.c \
    ../../gui/gui_dock.c \
    ../../gui/gui_window.c \
    ../../gui/gui_desktop.c \
    ../../gui/gui_anim.c

# ── WASM-specific HAL + drivers ───────────────────────────────────
WASM_SRCS := \
    boot/wasm_init.c \
    hal/mem.c \
    hal/stubs.c \
    drivers/serial.c \
    drivers/kb.c \
    drivers/display.c

ALL_SRCS := $(X86_SRCS) $(GUI_SRCS) $(WASM_SRCS)

# ── Build targets ─────────────────────────────────────────────────

.PHONY: all run clean

all: $(BUILDDIR)/osito.html

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/osito.html: $(ALL_SRCS) shell.html | $(BUILDDIR)
	$(CC) $(CFLAGS) $(EMFLAGS) $(ALL_SRCS) -o $(BUILDDIR)/osito.html

run: all
	emrun $(BUILDDIR)/osito.html

clean:
	rm -rf $(BUILDDIR)
