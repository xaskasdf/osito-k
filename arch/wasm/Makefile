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
GIT_REV   := $(shell cd $(CURDIR)/../.. && git rev-parse --short HEAD 2>/dev/null || echo unknown)
BUILD_TS  := $(shell date -u +%Y-%m-%dT%H:%M:%SZ)

CFLAGS := \
    -O3 \
    -std=gnu11 \
    -DWASM_BUILD=1 \
    -DOSITO_GIT_REV=\"$(GIT_REV)\" \
    -DOSITO_BUILD_TS=\"$(BUILD_TS)\" \
    -msimd128 \
    $(INCLUDES) \
    -Wno-unused-function \
    -Wno-unused-variable

# ── Emscripten link flags ─────────────────────────────────────────
EMFLAGS := \
    -s WASM=1 \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s INITIAL_MEMORY=536870912 \
    -s TOTAL_STACK=1048576 \
    -s MAIN_MODULE=1 \
    -s EMULATE_FUNCTION_POINTER_CASTS=1 \
    -s ASYNCIFY=1 \
    -s 'ASYNCIFY_IMPORTS=["emscripten_sleep"]' \
    -s 'EXPORTED_FUNCTIONS=["_main","_wasm_kb_push","_wasm_get_mem_used_kb","_wasm_compositor_frame","_input_set_mouse_abs","_input_post_mouse_button","_osito_chat_sync","_wasm_status_fs_label","_wasm_status_fs_files","_wasm_status_fs_free_mb"]' \
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
#           net.c, http.c, tls.c, claude.c, sshd.c, tls13.c, dhcp.c, ntp.c,
#           ipv6.c, mdns.c, apipa.c, netfilter.c, nic.c (network → stubs.c),
#           hwbp.c, self_optimize.c, spec_*.c, vdso_thunks.c, kmod.c,
#           coredump.c, crash_report.c, audio_sched.c, dma_sched.c,
#           pci_hotplug.c, smp_work.c, pred_sched.c, io_predict.c,
#           tensor_arena.c, sys_inference.c (x86-only / hw-bound → stubs.c),
#           wayland.c, evdev.c, pty.c, fuse.c, sshd.c (host-OS-bound → stubs)
#           all *.S assembly files (replaced by libc setjmp or no-ops),
#           all drivers/*.c (GPU/NVMe/xHCI/HDA/i211 → no-ops in stubs.c)
X86_SRCS := \
    ../x86/kernel/shell.c \
    ../x86/kernel/terminal.c \
    ../x86/kernel/tensor.c \
    ../x86/kernel/inference.c \
    ../x86/kernel/rag.c \
    ../x86/kernel/tokenizer.c \
    ../x86/kernel/tokenizer_trie.c \
    ../x86/kernel/compositor.c \
    ../x86/kernel/input_events.c \
    ../x86/kernel/shm.c \
    ../x86/kernel/framebuffer.c \
    ../x86/kernel/klog.c \
    ../x86/kernel/kallsyms.c \
    ../x86/kernel/usym.c \
    ../x86/kernel/sysctl.c \
    ../x86/kernel/sysv_ipc.c \
    ../x86/kernel/vt.c \
    ../x86/kernel/blkdev.c \
    ../x86/kernel/dm.c \
    ../x86/kernel/sysfs.c \
    ../x86/kernel/kobject.c \
    ../x86/kernel/inotify.c \
    ../x86/kernel/random.c \
    ../x86/kernel/oom.c \
    ../x86/kernel/psi.c \
    ../x86/kernel/ns.c \
    ../x86/kernel/caps.c \
    ../x86/kernel/strace.c \
    ../x86/kernel/lockdep.c \
    ../x86/kernel/seccomp.c \
    ../x86/kernel/kprof.c \
    ../x86/kernel/kstate.c \
    ../x86/kernel/trace.c \
    ../x86/kernel/io_uring.c \
    ../x86/kernel/bpf.c \
    ../x86/kernel/msync.c \
    ../x86/kernel/memcompress.c \
    ../x86/kernel/timers.c \
    ../x86/kernel/crypto.c \
    ../x86/kernel/crypto2.c \
    ../x86/kernel/sched_rt.c \
    ../x86/kernel/workqueue.c \
    ../x86/kernel/kthread.c \
    ../x86/kernel/slab.c \
    ../x86/kernel/socket.c \
    ../x86/kernel/rcu.c \
    ../x86/kernel/zlib.c \
    ../x86/kernel/git.c \
    ../x86/kernel/tls.c \
    ../x86/kernel/tls13.c \
    ../x86/kernel/wayland.c \
    ../x86/kernel/fuse.c \
    ../x86/kernel/evdev.c \
    ../x86/kernel/pty.c \
    ../x86/kernel/sshd.c \
    ../x86/kernel/initramfs.c \
    ../x86/fs/gguf.c \
    ../x86/fs/gpt.c \
    ../x86/fs/iso9660.c \
    ../x86/fs/fat32.c \
    ../x86/fs/ext2.c \
    ../x86/fs/exfat.c \
    ../x86/fs/ntfs.c \
    ../x86/fs/squashfs.c \
    ../x86/fs/hfsplus.c \
    ../x86/fs/btrfs.c \
    ../x86/fs/apfs.c \
    ../x86/fs/udf.c \
    ../x86/fs/ositofs2.c \
    ../x86/fs/vfs.c \
    ../x86/fs/tmpfs.c

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

# WASM first so our printf overrides win over libc's
ALL_SRCS := $(WASM_SRCS) $(X86_SRCS) $(GUI_SRCS)

# ── Build targets ─────────────────────────────────────────────────

.PHONY: all run clean

all: $(BUILDDIR)/osito.html

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/osito.html: $(ALL_SRCS) shell.html manifest.json sw.js | $(BUILDDIR)
	$(CC) $(CFLAGS) $(EMFLAGS) $(ALL_SRCS) -o $(BUILDDIR)/osito.html
	cp manifest.json $(BUILDDIR)/
	# Stamp the Service Worker cache name with the wasm md5 so each
	# new build invalidates the previous SW cache automatically.
	@WASM_HASH=$$(md5 -q $(BUILDDIR)/osito.wasm 2>/dev/null || md5sum $(BUILDDIR)/osito.wasm | cut -d' ' -f1); \
	sed "s/osito-k-v1/osito-k-$$WASM_HASH/" sw.js > $(BUILDDIR)/sw.js

run: all
	emrun $(BUILDDIR)/osito.html

clean:
	rm -rf $(BUILDDIR)
