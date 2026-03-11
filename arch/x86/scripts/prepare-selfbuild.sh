#!/bin/bash
#
# OsitoK x86-64 — Prepare NVMe image for kernel self-build (Phase 2)
#
# Creates an OsitoFS v2 disk image containing:
#   - All kernel .c source files (TCC-compilable, flat names)
#   - All .h header files (flat names)
#   - Pre-compiled GCC .o files (tensor, asm, compat32)
#   - tcc.elf compiler binary
#   - entry_alias.o (_start → kernel_entry for TCC linker)
#
# Usage:
#   ./scripts/prepare-selfbuild.sh           # Create 512MB image
#   ./scripts/prepare-selfbuild.sh --size N  # Create N MB image
#
# After running, use qemu-test.sh which auto-detects build/nvme.img.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
X86_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$X86_DIR/build"
TOOLS_DIR="$X86_DIR/../../tools/ositofs"
NVME_IMG="$BUILD_DIR/nvme.img"
IMG_SIZE_MB=512

GREEN='\033[0;32m'
CYAN='\033[0;36m'
RED='\033[0;31m'
NC='\033[0m'

info()  { echo -e "${GREEN}[+]${NC} $*"; }
step()  { echo -e "${CYAN}  →${NC} $*"; }
error() { echo -e "${RED}[!]${NC} $*"; exit 1; }

# Parse args
while [ $# -gt 0 ]; do
    case "$1" in
        --size) IMG_SIZE_MB="$2"; shift 2;;
        *) error "Unknown arg: $1";;
    esac
done

# ── Check prerequisites ──────────────────────────────────────

[ -f "$TOOLS_DIR/mkfs.ositofs" ]   || error "Build tools first: make -C tools/ositofs"
[ -f "$TOOLS_DIR/ositofs-write" ]  || error "Build tools first: make -C tools/ositofs"
[ -f "$X86_DIR/test/tcc.elf" ]     || error "tcc.elf not found at test/tcc.elf"

# ── Build kernel with GCC first (need .o files) ─────────────

info "Building kernel with GCC (for precompiled .o files)..."
make -C "$X86_DIR" -j$(nproc) 2>&1 | tail -3

# Compile entry_alias.S
info "Compiling entry_alias.o..."
gcc -c -o "$BUILD_DIR/kernel/entry_alias.o" "$X86_DIR/kernel/entry_alias.S"

# ── Create NVMe image ───────────────────────────────────────

info "Creating ${IMG_SIZE_MB}MB NVMe image..."
dd if=/dev/zero of="$NVME_IMG" bs=1M count="$IMG_SIZE_MB" status=none
"$TOOLS_DIR/mkfs.ositofs" "$NVME_IMG" --label "OsitoK-SelfBuild"

WRITE="$TOOLS_DIR/ositofs-write"
FILE_COUNT=0

write_file() {
    local path="$1"
    local name="${2:-$(basename "$path")}"
    if [ ! -f "$path" ]; then
        echo -e "  ${RED}SKIP${NC} $name (not found: $path)"
        return
    fi
    "$WRITE" "$NVME_IMG" "$path" --name "$name" 2>/dev/null
    FILE_COUNT=$((FILE_COUNT + 1))
    step "$name ($(stat -c%s "$path") bytes)"
}

# ── TCC compiler binary ─────────────────────────────────────

info "Writing tcc.elf..."
write_file "$X86_DIR/test/tcc.elf"

# ── TCC-compilable kernel .c files ──────────────────────────
# (Everything except tensor.c, tensor_avx2.c, compat32.c, msvcrt_shim.c)

info "Writing kernel .c sources..."

# kernel/
for f in main.c serial.c framebuffer.c pci.c memory.c net.c \
         inference.c idt.c paging.c heap.c syscall.c elf.c \
         process.c keyboard.c terminal.c shell.c crypto.c \
         tls.c http.c claude.c tokenizer.c smp.c dynlink.c \
         zlib.c git.c shm.c compositor.c display.c \
         input_events.c memcompress.c; do
    write_file "$X86_DIR/kernel/$f"
done

# drivers/
for f in nvme.c i211.c gpu.c gsp.c sass.c gmmu.c \
         gpu_tensor.c gpu_inference.c xhci.c; do
    write_file "$X86_DIR/drivers/$f"
done

# fs/
for f in ositofs2.c gpt.c gguf.c; do
    write_file "$X86_DIR/fs/$f"
done

# win32/ (except compat32.c, msvcrt_shim.c → GCC only)
for f in win32_init.c pe.c winexec.c handle.c ntsyscall.c \
         ntprocess.c ntsync.c dllloader.c kernel32_shim.c \
         ntdll_shim.c user32_shim.c gdi32_shim.c advapi32_shim.c \
         ddraw_shim.c dsound_shim.c ole32_shim.c shell32_shim.c \
         comctl32_shim.c comdlg32_shim.c winmm_shim.c wsock32_shim.c; do
    write_file "$X86_DIR/win32/$f"
done

# ── Header files ─────────────────────────────────────────────

info "Writing header files..."

# include/
for f in types.h boot_info.h stdint.h; do
    write_file "$X86_DIR/include/$f"
done

# include/common/
write_file "$X86_DIR/../../include/common/ositofs2_format.h"

# fs/*.h
for f in gguf.h gpt.h; do
    write_file "$X86_DIR/fs/$f"
done

# kernels/generated/*_code.h (SASS binary data)
for f in "$X86_DIR"/kernels/generated/*_code.h; do
    [ -f "$f" ] && write_file "$f"
done

# kernel/*.h
for f in inference.h tensor.h tls.h http.h tokenizer.h claude.h \
         smp.h crypto.h zlib.h git.h net.h; do
    write_file "$X86_DIR/kernel/$f"
done

# drivers/*.h
for f in i211.h gpu.h sass.h gpu_tensor.h gpu_inference.h \
         xhci.h ahci.h gpu_display.h; do
    write_file "$X86_DIR/drivers/$f"
done

# win32/*.h
for f in pe.h handle.h ntsyscall.h ntdll_shim.h kernel32_shim.h \
         msvcrt_shim.h advapi32_shim.h user32_shim.h gdi32_shim.h \
         wsock32_shim.h shell32_shim.h winmm_shim.h comctl32_shim.h \
         comdlg32_shim.h dllloader.h ddraw_shim.h ole32_shim.h \
         dsound_shim.h nttypes.h compat32.h; do
    write_file "$X86_DIR/win32/$f"
done

# ── Pre-compiled GCC .o files ────────────────────────────────
# (Files TCC can't compile: AVX2 intrinsics, lretq, __builtin_ms_va_list, assembly)

info "Writing pre-compiled GCC .o files..."

# Kernel files that need GCC
write_file "$BUILD_DIR/kernel/tensor.o"
write_file "$BUILD_DIR/kernel/tensor_avx2.o"

# Win32 files that need GCC
write_file "$BUILD_DIR/win32/compat32.o"
write_file "$BUILD_DIR/win32/msvcrt_shim.o"

# Assembly files (TCC assembler lacks iretq/lretq/sysret)
write_file "$BUILD_DIR/kernel/isr_stubs.o"
write_file "$BUILD_DIR/kernel/syscall_entry.o"
write_file "$BUILD_DIR/kernel/setjmp.o"
write_file "$BUILD_DIR/kernel/kexec_tramp.o"
write_file "$BUILD_DIR/win32/int2e_stub.o"

# TCC linker entry alias
write_file "$BUILD_DIR/kernel/entry_alias.o"

# ── Summary ──────────────────────────────────────────────────

info ""
info "NVMe image ready: $NVME_IMG"
info "  Files written: $FILE_COUNT"
info "  Image size: ${IMG_SIZE_MB}MB"
info ""
info "Test with: ./scripts/qemu-test.sh"
info "In OsitoK shell: build"

# List contents
echo ""
"$TOOLS_DIR/ositofs-ls" "$NVME_IMG" 2>/dev/null | tail -5
