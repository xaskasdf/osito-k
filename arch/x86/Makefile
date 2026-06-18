# OsitoK x86-64 — Split Build: boot.efi + kernel.elf
#
# boot.efi: UEFI bootloader (~300 LOC, gnu-efi, -fpic)
# kernel.elf: Full kernel (50+ files, -fno-pie, no EFI restrictions)
#
# Prerequisites:
#   sudo apt install gnu-efi gcc
#
# Usage:
#   make              Build boot.efi + kernel.elf
#   make clean        Clean build artifacts
#
# Legacy monolithic build:
#   make legacy       Build ositok.efi (single PE32+ binary)

# Cross toolchain. `make CLANG=1` uses clang as a freestanding cross-compiler
# + lld (the Windows/msys2 path — no x86_64-elf-gcc needed). Default is the
# x86_64-elf GNU toolchain (Linux/macOS).
ifeq ($(CLANG),1)
CC       = clang --target=x86_64-elf
LD       = ld.lld
OBJCOPY  = llvm-objcopy
AR       = llvm-ar
else
CC       = x86_64-elf-gcc
LD       = x86_64-elf-ld
OBJCOPY  = x86_64-elf-objcopy
AR       = x86_64-elf-ar
endif

# gnu-efi paths. Auto-detect macOS (/usr/local via ncroxon/gnu-efi from
# source) vs Debian/Ubuntu (/usr via apt-get install gnu-efi). Override
# any var on the make command line to force a custom location.
ifneq ($(wildcard /usr/local/lib/elf_x86_64_efi.lds),)
    EFI_PREFIX ?= /usr/local
else
    EFI_PREFIX ?= /usr
endif
EFI_INC      ?= $(EFI_PREFIX)/include/efi
EFI_INC_ARCH ?= $(EFI_PREFIX)/include/efi/x86_64
EFI_LIB      ?= $(EFI_PREFIX)/lib
EFI_CRT      ?= $(EFI_PREFIX)/lib/crt0-efi-x86_64.o
EFI_LDS      ?= $(EFI_PREFIX)/lib/elf_x86_64_efi.lds

# Boot flags (gnu-efi, PIE — only for boot.efi)
BFLAGS   = -ffreestanding -fno-stack-protector -fshort-wchar \
           -mno-red-zone -Wall -Wextra -O2 \
           -I$(EFI_INC) -I$(EFI_INC_ARCH) -Iinclude \
           -I../../include/common \
           -DGNU_EFI_USE_MS_ABI -fpic

# Kernel flags — NO PIE, NO -fno-jump-tables (EFI restrictions lifted!)
KCFLAGS  = -std=c11 -ffreestanding -fno-stack-protector -mno-red-zone \
           -mcmodel=large \
           -Wall -Wextra -O2 -fno-pie -Iinclude -I../../include/common \
           -I../../gui -D__KERNEL_X86__=1 -MMD -MP

# Compat-layer diagnostic probes (panorama round). Gate:
#   make -C arch/x86 COMPAT_TRACE=1
# When enabled, int2e/dos-int dispatchers log CPU ID when non-BSP, and
# the #PF handler logs any fault whose RSP sits in IST1/IST2. Byte-identical
# to the default build when COMPAT_TRACE is unset or 0.
COMPAT_TRACE ?= 0
ifeq ($(COMPAT_TRACE),1)
  KCFLAGS += -DCOMPAT_TRACE=1
endif

# Dev fast-boot: skip the boot-time GGUF/LLM model load (brandon-tiny, ~20 MB)
# to shorten the build→boot→UT99 loop. Inference is unavailable in this build.
#   make -C arch/x86 CLANG=1 SKIP_MODEL=1
SKIP_MODEL ?= 0
ifeq ($(SKIP_MODEL),1)
  KCFLAGS += -DOK_SKIP_MODEL=1
endif

# Dev quiet: compile out the high-volume diagnostic serial logging (the printf
# %s-arg tracer, [NtReadFile] per-read spam, [INT2E] per-call spam). Under KVM
# every serial write is a VM-exit, so these dominate UT99 boot time (~80% of
# 172k log lines). With QUIET=1 the compute-heavy phases run fast.
#   make -C arch/x86 CLANG=1 QUIET=1
QUIET ?= 0
ifeq ($(QUIET),1)
  KCFLAGS += -DOK_QUIET=1
endif

# Flags for precompiled .o files that may be called from TCC code.
# TCC doesn't guarantee 16-byte stack alignment (SysV ABI requires it).
# -mincoming-stack-boundary=3 tells GCC the stack may be 8-byte aligned,
# so it inserts realignment in prologues before any movaps/movdqa.
# clang lacks -mincoming-stack-boundary; -mstackrealign forces the same realign.
ifeq ($(findstring clang,$(CC)),clang)
KCFLAGS_PRECOMP = $(KCFLAGS) -mstackrealign
else
KCFLAGS_PRECOMP = $(KCFLAGS) -mincoming-stack-boundary=3
endif

# AVX2/FMA/F16C flags for tensor_avx2.c only
AVXFLAGS = $(KCFLAGS_PRECOMP) -mavx2 -mfma -mf16c

# EFI link flags (for boot.efi only)
ifeq ($(CLANG),1)
# ld.lld: force ELF mode (not MinGW/COFF), drop the driver-only -nostdlib
BLDFLAGS = -m elf_x86_64 -shared -Bsymbolic -z norelro \
           -T $(EFI_LDS) \
           -L$(EFI_LIB) \
           $(EFI_CRT)
else
BLDFLAGS = -nostdlib -shared -Bsymbolic \
           -T $(EFI_LDS) \
           -L$(EFI_LIB) \
           $(EFI_CRT)
endif
BLIBS    = -lefi -lgnuefi

# Kernel link flags
ifeq ($(CLANG),1)
# Raw ld.lld: force ELF mode (not MinGW/COFF), drop the driver-only -nostdlib,
# and keep every PT_LOAD on its own page so boot.efi can AllocatePages each one
# (--no-rosegment folds read-only orphan sections into the text segment).
KLDFLAGS = -m elf_x86_64 -static -T kernel.ld -z noexecstack --no-rosegment
else
KLDFLAGS = -nostdlib -static -T kernel.ld -z noexecstack
endif

BUILD    = build

# ── Source files ──────────────────────────────────────────────

BOOT_SRC = boot/boot_efi.c

KERN_SRC = kernel/main.c kernel/serial.c kernel/framebuffer.c \
           kernel/pci.c kernel/memory.c kernel/mem_extern.c kernel/net.c \
           kernel/dhcp.c kernel/ntp.c kernel/ipv6.c kernel/mdns.c \
           kernel/tensor.c \
           kernel/tensor_avx2.c kernel/inference.c kernel/idt.c \
           kernel/paging.c kernel/heap.c kernel/syscall.c \
           kernel/elf.c kernel/process.c \
           kernel/keyboard.c kernel/terminal.c kernel/shell.c kernel/pkg.c \
           kernel/crypto.c kernel/tls.c kernel/http.c kernel/claude.c \
           kernel/tokenizer.c kernel/smp.c kernel/smp_work.c kernel/spec_prefetch.c kernel/spec_analyze.c kernel/spec_tls.c kernel/dynlink.c \
           kernel/zlib.c kernel/git.c \
           kernel/shm.c kernel/compositor.c kernel/display.c \
           kernel/input_events.c kernel/memcompress.c \
           kernel/kmod.c kernel/wayland.c kernel/pty.c \
           kernel/socket.c kernel/timers.c kernel/power.c \
           kernel/sysv_ipc.c kernel/vt.c \
           kernel/blkdev.c kernel/strace.c kernel/caps.c \
           kernel/klog.c kernel/random.c kernel/oom.c kernel/ns.c \
           kernel/panic.c kernel/sysfs.c kernel/netfilter.c \
           kernel/crypto2.c kernel/sched_rt.c kernel/dm.c \
           kernel/evdev.c kernel/sysctl.c kernel/tls13.c kernel/tls13_server.c kernel/sshd.c \
           kernel/fuse.c kernel/kprof.c kernel/msync.c \
           kernel/io_uring.c kernel/bpf.c kernel/kthread.c kernel/initramfs.c \
           kernel/trace.c kernel/seccomp.c kernel/workqueue.c \
           kernel/rcu.c kernel/slab.c kernel/pci_hotplug.c kernel/coredump.c \
           kernel/lockdep.c kernel/psi.c kernel/kobject.c \
           kernel/inotify.c kernel/kallsyms.c kernel/cpu_topology.c \
           kernel/usym.c kernel/crash_report.c kernel/kstate.c \
           kernel/audio_sched.c kernel/dma_sched.c kernel/tokenizer_trie.c \
           kernel/vdso_thunks.c \
           kernel/cpu_features.c kernel/perf.c kernel/dispatch.c \
           kernel/tensor_arena.c kernel/sys_inference.c \
           kernel/pred_sched.c kernel/io_predict.c \
           kernel/hwbp.c kernel/self_optimize.c kernel/nic.c \
           kernel/apipa.c kernel/rag.c kernel/cert_pin.c \
           kernel/rsa.c kernel/x509.c kernel/ecdsa_p256.c kernel/ecdsa_p384.c \
           kernel/http_plain.c kernel/ocsp.c kernel/crl.c \
           kernel/inferconnect.c kernel/inferconnect_rpc.c kernel/inferconnect_client.c \
           kernel/inferconnect_oict.c kernel/inference_shims.c kernel/agent_simple.c \
           kernel/cluster.c kernel/cluster_rendezvous.c
ASM_SRC  = kernel/isr_stubs.S kernel/syscall_entry.S kernel/setjmp.S kernel/kexec_tramp.S
DRV_SRC  = drivers/nvme.c drivers/i211.c drivers/rtl8111.c drivers/gpu.c drivers/gsp.c \
           drivers/sass.c drivers/gmmu.c drivers/gpu_tensor.c drivers/gpu_inference.c \
           drivers/xhci.c drivers/hid_parser.c drivers/hda.c \
           drivers/virtio.c drivers/virtio_blk.c drivers/virtio_net.c drivers/virtio_gpu.c \
           drivers/virtio_gpu_3d.c drivers/nvk_backend.c drivers/nvk_backend_res.c \
           drivers/usb_storage.c
FS_SRC   = fs/ositofs2.c fs/ositofs3.c fs/gpt.c fs/gguf.c fs/vfs.c fs/fat32.c \
           fs/tmpfs.c fs/iso9660.c fs/ext2.c fs/exfat.c fs/ntfs.c \
           fs/udf.c fs/squashfs.c fs/hfsplus.c fs/btrfs.c fs/apfs.c
WIN32_SRC = win32/win32_init.c win32/compat32.c win32/pe.c win32/winexec.c \
            win32/handle.c win32/ntsyscall.c win32/ntprocess.c win32/ntsync.c \
            win32/dllloader.c win32/wdbg.c win32/win32_abi.c \
            win32/kernel32_shim.c win32/ntdll_shim.c win32/msvcrt_shim.c \
            win32/user32_shim.c win32/gdi32_shim.c win32/advapi32_shim.c \
            win32/ddraw_shim.c win32/dsound_shim.c \
            win32/ole32_shim.c win32/shell32_shim.c \
            win32/comctl32_shim.c win32/comdlg32_shim.c \
            win32/winmm_shim.c win32/wsock32_shim.c \
            win32/ole2.c win32/msi.c win32/cab.c \
            win32/zip.c win32/msix.c win32/installer.c
WIN32_ASM = win32/int2e_stub.S
DOS_ASM  = dos/dos_int_stub.S
DOS_SRC  = dos/dos_init.c dos/dos_exec.c dos/dos_loader.c \
           dos/dos_mem.c dos/cpu8086.c dos/dos_int.c \
           dos/dos_api.c dos/dos_bios.c dos/dos_vga.c dos/dos_io.c \
           dos/dos_dpmi.c dos/dos_vcpi.c dos/dos_jit.c
GUI_SRC  = ../../gui/gui_draw.c ../../gui/gui_text.c \
           ../../gui/gui_panel.c ../../gui/gui_dock.c \
           ../../gui/gui_desktop.c ../../gui/gui_window.c \
           ../../gui/gui_anim.c

# ── Object files ──────────────────────────────────────────────

BOOT_OBJ  = $(BUILD)/boot_efi.o
KERN_OBJ  = $(KERN_SRC:%.c=$(BUILD)/%.o)
ASM_OBJ   = $(ASM_SRC:%.S=$(BUILD)/%.o)
DRV_OBJ   = $(DRV_SRC:%.c=$(BUILD)/%.o)
FS_OBJ    = $(FS_SRC:%.c=$(BUILD)/%.o)
WIN32_OBJ = $(WIN32_SRC:%.c=$(BUILD)/%.o)
WIN32_ASM_OBJ = $(WIN32_ASM:%.S=$(BUILD)/%.o)
DOS_OBJ   = $(DOS_SRC:%.c=$(BUILD)/%.o)
DOS_ASM_OBJ = $(DOS_ASM:%.S=$(BUILD)/%.o)
GUI_OBJ   = $(patsubst ../../gui/%.c,$(BUILD)/gui/%.o,$(GUI_SRC))

ALL_KERN_OBJ = $(KERN_OBJ) $(ASM_OBJ) $(DRV_OBJ) $(FS_OBJ) $(WIN32_OBJ) $(WIN32_ASM_OBJ) $(DOS_OBJ) $(DOS_ASM_OBJ) $(GUI_OBJ)

# ── Targets ───────────────────────────────────────────────────

# Minimal kernel: excludes all new subsystems from Apr 2026 session.
# Use when full kernel crashes (e.g., BSS overlap with NVMe DMA).
KERN_MINIMAL = kernel/main.c kernel/serial.c kernel/framebuffer.c \
               kernel/pci.c kernel/memory.c kernel/mem_extern.c kernel/net.c \
               kernel/tensor.c kernel/tensor_avx2.c kernel/inference.c \
               kernel/idt.c kernel/paging.c kernel/heap.c kernel/syscall.c \
               kernel/elf.c kernel/process.c \
               kernel/keyboard.c kernel/terminal.c kernel/shell.c kernel/pkg.c \
               kernel/crypto.c kernel/tls.c kernel/http.c kernel/claude.c \
               kernel/tokenizer.c kernel/smp.c kernel/dynlink.c \
               kernel/zlib.c kernel/git.c \
               kernel/shm.c kernel/compositor.c kernel/display.c \
               kernel/input_events.c kernel/memcompress.c
DRV_MINIMAL  = drivers/nvme.c drivers/i211.c drivers/rtl8111.c drivers/gpu.c drivers/gsp.c \
               drivers/sass.c drivers/gmmu.c drivers/gpu_tensor.c \
               drivers/gpu_inference.c drivers/xhci.c drivers/hda.c
FS_MINIMAL   = fs/ositofs2.c fs/ositofs3.c fs/gpt.c fs/gguf.c fs/vfs.c
KERN_MIN_OBJ = $(KERN_MINIMAL:%.c=$(BUILD)/%.o)
DRV_MIN_OBJ  = $(DRV_MINIMAL:%.c=$(BUILD)/%.o)
FS_MIN_OBJ   = $(FS_MINIMAL:%.c=$(BUILD)/%.o)
ALL_MIN_OBJ  = $(KERN_MIN_OBJ) $(ASM_OBJ) $(DRV_MIN_OBJ) $(FS_MIN_OBJ) \
               $(WIN32_OBJ) $(WIN32_ASM_OBJ) $(DOS_OBJ) $(DOS_ASM_OBJ) $(GUI_OBJ)

.PHONY: all clean check-deps legacy tcc tcc-clean minimal libvulkan

all: check-deps $(BUILD)/boot.efi $(BUILD)/kernel.elf libvulkan
	@echo ""
	@echo "Built: $(BUILD)/boot.efi + $(BUILD)/kernel.elf + lib/vulkan/libvulkan.a"

# ── Wave 2: Vulkan SDK library ────────────────────────────────
libvulkan:
	$(MAKE) -C lib/vulkan CC='$(CC)' AR='$(AR)'

minimal: check-deps $(BUILD)/boot.efi $(BUILD)/kernel-minimal.elf
	@echo ""
	@echo "Built MINIMAL: $(BUILD)/boot.efi + $(BUILD)/kernel-minimal.elf"
	@echo "Copy to kernel.elf: cp $(BUILD)/kernel-minimal.elf $(BUILD)/kernel.elf"

$(BUILD)/kernel-minimal.elf: $(ALL_MIN_OBJ) kernel.ld
	$(LD) $(KLDFLAGS) -o $@ $(filter-out kernel.ld,$^)
	@echo "Built: $@ ($$(stat -c%s $@) bytes) [MINIMAL — no new subsystems]"

check-deps:
	@true

# ── boot.efi (UEFI bootloader) ────────────────────────────────

$(BUILD)/boot.efi: $(BUILD)/boot.so
	$(OBJCOPY) -j .text -j .rodata -j .sdata -j .data -j .dynamic -j .dynsym \
	           -j .rel -j .rela -j .reloc \
	           --target=efi-app-x86_64 $< $@ 2>/dev/null || \
	python3 scripts/elf2efi.py $< $@
	@echo "Built: $@ ($$(stat -c%s $@ 2>/dev/null || stat -f%z $@ 2>/dev/null || wc -c < $@) bytes)"

$(BUILD)/boot.so: $(BOOT_OBJ)
	$(LD) $(BLDFLAGS) -o $@ $^ $(BLIBS)

$(BUILD)/boot_efi.o: boot/boot_efi.c | $(BUILD)
	$(CC) $(BFLAGS) -c -o $@ $<

# ── kernel.elf (kernel binary) ─────────────────────────────────

$(BUILD)/kernel.elf: $(ALL_KERN_OBJ) kernel.ld
	$(LD) $(KLDFLAGS) -o $@ $(filter-out kernel.ld,$^)
	@echo "Built: $@ ($$(stat -c%s $@) bytes)"

# Kernel C files
$(BUILD)/kernel/%.o: kernel/%.c | $(BUILD)/kernel
	$(CC) $(KCFLAGS) -c -o $@ $<

# GCC-only precompiled files: use KCFLAGS_PRECOMP (TCC stack compat)
$(BUILD)/kernel/tensor.o: kernel/tensor.c | $(BUILD)/kernel
	$(CC) $(KCFLAGS_PRECOMP) -c -o $@ $<

# tensor_avx2.c needs AVX2/FMA flags (AVXFLAGS already uses KCFLAGS_PRECOMP)
$(BUILD)/kernel/tensor_avx2.o: kernel/tensor_avx2.c | $(BUILD)/kernel
	$(CC) $(AVXFLAGS) -c -o $@ $<

# Assembly files
$(BUILD)/kernel/%.o: kernel/%.S | $(BUILD)/kernel
	$(CC) -c -o $@ $<

# Drivers — disable auto-vectorization to prevent GCC from generating
# SSE instructions for MMIO struct copies (KVM can't emulate MMIO→XMM)
$(BUILD)/drivers/%.o: drivers/%.c | $(BUILD)/drivers
	$(CC) $(KCFLAGS) -fno-tree-vectorize -c -o $@ $<

# Filesystem
$(BUILD)/fs/%.o: fs/%.c | $(BUILD)/fs
	$(CC) $(KCFLAGS) -c -o $@ $<

# Win32 compatibility layer
$(BUILD)/win32/%.o: win32/%.c | $(BUILD)/win32
	$(CC) $(KCFLAGS) -Iwin32 -c -o $@ $<

# GCC-only Win32 precompiled files: use KCFLAGS_PRECOMP (TCC stack compat)
$(BUILD)/win32/compat32.o: win32/compat32.c | $(BUILD)/win32
	$(CC) $(filter-out -fno-pie,$(KCFLAGS_PRECOMP)) -fPIE -fno-jump-tables -Iwin32 -c -o $@ $<

$(BUILD)/win32/msvcrt_shim.o: win32/msvcrt_shim.c | $(BUILD)/win32
	$(CC) $(KCFLAGS_PRECOMP) -Iwin32 -c -o $@ $<

$(BUILD)/win32/ddraw_shim.o: win32/ddraw_shim.c | $(BUILD)/win32
	$(CC) $(KCFLAGS_PRECOMP) -Iwin32 -c -o $@ $<

$(BUILD)/win32/dsound_shim.o: win32/dsound_shim.c | $(BUILD)/win32
	$(CC) $(KCFLAGS_PRECOMP) -Iwin32 -c -o $@ $<

# Win32 assembly
$(BUILD)/win32/%.o: win32/%.S | $(BUILD)/win32
	$(CC) -c -o $@ $<

# DOS compatibility layer
$(BUILD)/dos/%.o: dos/%.c | $(BUILD)/dos
	$(CC) $(KCFLAGS) -Idos -c -o $@ $<

# DOS assembly
$(BUILD)/dos/%.o: dos/%.S | $(BUILD)/dos
	$(CC) -c -o $@ $<

# GUI shared library
$(BUILD)/gui/%.o: ../../gui/%.c | $(BUILD)/gui
	$(CC) $(KCFLAGS) -c -o $@ $<

# ── TCC kernel build (Phase 1: verify TCC can produce working kernel) ────
#
# TCC compiles everything it can; GCC fills in for:
#   - tensor.c, tensor_avx2.c (SSE/AVX2 intrinsics + asm constraints)
#   - All .S files (iretq, lretq opcodes unknown to TCC assembler)
#   - win32/compat32.c (lretq), win32/msvcrt_shim.c (__builtin_ms_va_list)
#
# Usage: make tcc    (builds boot.efi with GCC + kernel.elf with TCC/GCC mix)

TCC      = tcc
TCCDIR   = $(BUILD)/tcc
TCCFLAGS = -c -nostdlib -Iinclude -I../../include/common -D__KERNEL_X86__=1

# Files that MUST use GCC (SSE/AVX asm constraints, special builtins)
GCC_ONLY_C = kernel/tensor.c kernel/tensor_avx2.c \
             win32/compat32.c win32/msvcrt_shim.c

# Everything else uses TCC
TCC_KERN_SRC = $(filter-out $(GCC_ONLY_C),$(KERN_SRC))
TCC_DRV_SRC  = $(DRV_SRC)
TCC_FS_SRC   = $(FS_SRC)
TCC_WIN32_SRC = $(filter-out $(GCC_ONLY_C),$(WIN32_SRC))
TCC_DOS_SRC   = $(DOS_SRC)

# TCC object files
TCC_KERN_OBJ  = $(TCC_KERN_SRC:%.c=$(TCCDIR)/%.o)
TCC_DRV_OBJ   = $(TCC_DRV_SRC:%.c=$(TCCDIR)/%.o)
TCC_FS_OBJ    = $(TCC_FS_SRC:%.c=$(TCCDIR)/%.o)
TCC_WIN32_OBJ = $(TCC_WIN32_SRC:%.c=$(TCCDIR)/%.o)
TCC_DOS_OBJ   = $(TCC_DOS_SRC:%.c=$(TCCDIR)/%.o)

# GCC-only object files (in TCC build dir)
GCC_TENSOR_OBJ    = $(TCCDIR)/kernel/tensor.o $(TCCDIR)/kernel/tensor_avx2.o
GCC_WIN32_COMPAT  = $(TCCDIR)/win32/compat32.o $(TCCDIR)/win32/msvcrt_shim.o
GCC_ASM_OBJ       = $(ASM_SRC:%.S=$(TCCDIR)/%.o)
GCC_WIN32_ASM_OBJ = $(WIN32_ASM:%.S=$(TCCDIR)/%.o)

ALL_TCC_OBJ = $(TCC_KERN_OBJ) $(TCC_DRV_OBJ) $(TCC_FS_OBJ) $(TCC_WIN32_OBJ) $(TCC_DOS_OBJ) \
              $(GCC_TENSOR_OBJ) $(GCC_WIN32_COMPAT) $(GCC_ASM_OBJ) $(GCC_WIN32_ASM_OBJ)

.PHONY: tcc tcc-clean

tcc: check-deps $(BUILD)/boot.efi $(TCCDIR)/kernel.elf
	@echo ""
	@echo "Built: $(BUILD)/boot.efi + $(TCCDIR)/kernel.elf (TCC kernel)"

$(TCCDIR)/kernel.elf: $(ALL_TCC_OBJ) kernel.ld
	$(LD) $(KLDFLAGS) -o $@ $(filter-out kernel.ld,$^)
	@echo "Built (TCC): $@ ($$(stat -c%s $@) bytes)"

# TCC rules for kernel C files
$(TCCDIR)/kernel/%.o: kernel/%.c | $(TCCDIR)/kernel
	$(TCC) $(TCCFLAGS) -o $@ $<

$(TCCDIR)/drivers/%.o: drivers/%.c | $(TCCDIR)/drivers
	$(TCC) $(TCCFLAGS) -o $@ $<

$(TCCDIR)/fs/%.o: fs/%.c | $(TCCDIR)/fs
	$(TCC) $(TCCFLAGS) -o $@ $<

$(TCCDIR)/win32/%.o: win32/%.c | $(TCCDIR)/win32
	$(TCC) $(TCCFLAGS) -Iwin32 -o $@ $<

$(TCCDIR)/dos/%.o: dos/%.c | $(TCCDIR)/dos
	$(TCC) $(TCCFLAGS) -Idos -o $@ $<

# GCC rules for files TCC can't handle (KCFLAGS_PRECOMP for TCC stack compat)
$(TCCDIR)/kernel/tensor.o: kernel/tensor.c | $(TCCDIR)/kernel
	$(CC) $(KCFLAGS_PRECOMP) -c -o $@ $<

$(TCCDIR)/kernel/tensor_avx2.o: kernel/tensor_avx2.c | $(TCCDIR)/kernel
	$(CC) $(AVXFLAGS) -c -o $@ $<

$(TCCDIR)/win32/compat32.o: win32/compat32.c | $(TCCDIR)/win32
	$(CC) $(KCFLAGS_PRECOMP) -Iwin32 -c -o $@ $<

$(TCCDIR)/win32/msvcrt_shim.o: win32/msvcrt_shim.c | $(TCCDIR)/win32
	$(CC) $(KCFLAGS_PRECOMP) -Iwin32 -c -o $@ $<

# Assembly always uses GCC (TCC assembler missing iretq/lretq)
$(TCCDIR)/kernel/%.o: kernel/%.S | $(TCCDIR)/kernel
	$(CC) -c -o $@ $<

$(TCCDIR)/win32/%.o: win32/%.S | $(TCCDIR)/win32
	$(CC) -c -o $@ $<

# TCC build directories
$(TCCDIR) $(TCCDIR)/kernel $(TCCDIR)/drivers $(TCCDIR)/fs $(TCCDIR)/win32 $(TCCDIR)/dos:
	mkdir -p $@

tcc-clean:
	rm -rf $(TCCDIR)

# ── Legacy monolithic build (backward compat) ──────────────────

LEGACY_EFI_OBJ = $(BUILD)/efi_main.o
LEGACY_CFLAGS  = -ffreestanding -fno-stack-protector -fshort-wchar \
                 -mno-red-zone -Wall -Wextra -O2 \
                 -I$(EFI_INC) -I$(EFI_INC_ARCH) -Iinclude \
                 -I../../include/common \
                 -DGNU_EFI_USE_MS_ABI -fpic
LEGACY_KCFLAGS = -std=c11 -ffreestanding -fno-stack-protector -mno-red-zone \
                 -Wall -Wextra -O2 -fPIE -fno-jump-tables -Iinclude -I../../include/common \
                 -D__KERNEL_X86__=1

legacy: check-deps $(BUILD)/ositok.efi

$(BUILD)/ositok.efi: $(BUILD)/ositok.so
	$(OBJCOPY) -j .text -j .rodata -j .sdata -j .data -j .dynamic -j .dynsym \
	           -j .rel -j .rela -j .reloc \
	           --target=efi-app-x86_64 $< $@
	@echo "Built: $@ ($$(stat -c%s $@) bytes)"

$(BUILD)/ositok.so: $(LEGACY_EFI_OBJ) $(ALL_KERN_OBJ)
	$(LD) -nostdlib -shared -Bsymbolic -T $(EFI_LDS) -L$(EFI_LIB) $(EFI_CRT) -o $@ $^ $(BLIBS)

$(BUILD)/efi_main.o: boot/efi_main.c | $(BUILD)/boot
	$(CC) $(LEGACY_CFLAGS) -c -o $@ $<

# ── Build directories ─────────────────────────────────────────

$(BUILD) $(BUILD)/boot $(BUILD)/kernel $(BUILD)/drivers $(BUILD)/fs $(BUILD)/win32 $(BUILD)/dos $(BUILD)/gui:
	mkdir -p $@

# Auto-generated header dependency files (produced by -MMD -MP)
-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)

clean:
	rm -rf $(BUILD)
