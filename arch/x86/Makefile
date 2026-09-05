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
#   make pxe          Build boot-pxe.efi with kernel.elf embedded
#   make clean        Clean build artifacts
#
# Legacy monolithic build:
#   make legacy       Build ositok.efi (single PE32+ binary)

.DEFAULT_GOAL := all

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

# Focused completion-port tracing. This remains useful with QUIET=1 and is
# deliberately bounded in kernel32_shim.c so Steam diagnostics stay readable.
#   make -C arch/x86 CLANG=1 QUIET=1 IOCP_TRACE=1
IOCP_TRACE ?= 0
ifeq ($(IOCP_TRACE),1)
  KCFLAGS += -DK32_IOCP_TRACE=1
endif

# General Win32 loader/API diagnostics. Keep these off in normal builds: CEF
# and games call these paths often enough for serial output to dominate QEMU.
#   make -C arch/x86 CLANG=1 WIN32_TRACE=1
WIN32_TRACE ?= 0
ifeq ($(WIN32_TRACE),1)
  KCFLAGS += -DK32_VERBOSE_DIAGNOSTICS=1 -DU32_API_DIAGNOSTICS=1 \
             -DNTSYNC_TRACE_DIAGNOSTICS=1
endif

# Per-operation virtual-memory tracing is useful for allocator corruption
# investigations, but Steam/CEF can issue thousands of successful calls per
# second. Keep failures and aggregate VM statistics enabled in normal builds.
#   make -C arch/x86 CLANG=1 WIN32_VM_TRACE=1
WIN32_VM_TRACE ?= 0
ifeq ($(WIN32_VM_TRACE),1)
  KCFLAGS += -DWIN32_VM_TRACE=1
endif

# DOS API and MCB tracing is intentionally opt-in. Native DOS programs can
# issue these calls in tight loops, and COM1 output is expensive under QEMU.
#   make -C arch/x86 CLANG=1 DOS_TRACE=1
DOS_TRACE ?= 0
ifeq ($(DOS_TRACE),1)
  DOS_TRACE_FLAGS = -DDOS_DIAGNOSTICS=1
  KCFLAGS += $(DOS_TRACE_FLAGS)
endif

# Diagnose native PE calls that return from a Win32 shim with interrupts
# disabled. This is intentionally opt-in because function instrumentation is
# too expensive for normal QEMU and hardware runs.
#   make -C arch/x86 CLANG=1 WIN32_IF_AUDIT=1
WIN32_IF_AUDIT ?= 0
ifeq ($(WIN32_IF_AUDIT),1)
  WIN32_IF_AUDIT_FLAGS = -finstrument-functions
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

# Detailed PE import tracing is intentionally opt-in. Steam and CEF resolve
# tens of thousands of symbols and each COM1 character causes a VM exit under
# QEMU. The normal build retains unresolved-import summaries and the diagnostic
# ring exposed by `pe-imports`, while this flag restores per-symbol output.
#   make -C arch/x86 CLANG=1 PE_LOADER_TRACE=1
PE_LOADER_TRACE ?= 0
ifeq ($(PE_LOADER_TRACE),1)
  KCFLAGS += -DPE_LOADER_TRACE=1
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
           kernel/klog.c kernel/boot_diag.c kernel/random.c kernel/oom.c kernel/ns.c \
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
           kernel/audio_sched.c kernel/pcm.c kernel/oss_audio.c kernel/dma_sched.c kernel/tokenizer_trie.c \
           kernel/vdso_thunks.c \
           kernel/cpu_features.c kernel/perf.c kernel/dispatch.c \
           kernel/tensor_arena.c kernel/sys_inference.c \
           kernel/pred_sched.c kernel/io_predict.c \
           kernel/hwbp.c kernel/self_optimize.c kernel/nic.c \
           kernel/apipa.c kernel/rag.c kernel/cert_pin.c kernel/certmgr.c \
           kernel/rsa.c kernel/x509.c kernel/ecdsa_p256.c kernel/ecdsa_p384.c \
           kernel/http_plain.c kernel/ocsp.c kernel/crl.c \
           kernel/inferconnect.c kernel/inferconnect_rpc.c kernel/inferconnect_client.c \
           kernel/inferconnect_oict.c kernel/inference_shims.c kernel/agent_simple.c \
           kernel/cluster.c kernel/cluster_rendezvous.c
ASM_SRC  = kernel/isr_stubs.S kernel/syscall_entry.S kernel/setjmp.S kernel/kexec_tramp.S
DRV_SRC  = drivers/nvme.c drivers/i211.c drivers/rtl8111.c drivers/gpu.c drivers/gsp.c \
           drivers/intel_gfx.c \
           drivers/sass.c drivers/gmmu.c drivers/gpu_tensor.c drivers/gpu_inference.c \
           drivers/xhci.c drivers/hid_parser.c drivers/hda.c \
           drivers/virtio.c drivers/virtio_blk.c drivers/virtio_net.c drivers/virtio_gpu.c \
           drivers/virtio_gpu_3d.c drivers/nvk_backend.c drivers/nvk_backend_res.c \
           drivers/usb_storage.c
FS_SRC   = fs/ositofs2.c fs/ositofs3.c fs/gpt.c fs/gguf.c fs/vfs.c fs/fat32.c \
           fs/tmpfs.c fs/iso9660.c fs/ext2.c fs/exfat.c fs/ntfs.c \
           fs/udf.c fs/squashfs.c fs/hfsplus.c fs/btrfs.c fs/apfs.c
WIN32_SRC = win32/win32_init.c win32/compat32.c win32/pe.c win32/winexec.c \
            win32/handle.c win32/filelock.c win32/ntsyscall.c win32/ntprocess.c win32/ntsync.c \
            win32/dllloader.c win32/unwind64.c win32/wdbg.c win32/if_audit.c win32/win32_abi.c \
             win32/wintime.c win32/kernel32_shim.c win32/profile.c \
             win32/ntdll_shim.c \
            win32/msvcrt_shim.c win32/crt_float.c \
             win32/user32_shim.c win32/user32_text.c win32/user32_controls.c win32/user32_listbox.c win32/gdi32_shim.c win32/opengl32_shim.c \
             win32/libusb_shim.c \
             win32/dxgi_shim.c win32/version_shim.c \
            win32/advapi32_shim.c win32/scm.c \
            win32/ddraw_shim.c win32/dsound_shim.c win32/dinput8_shim.c \
            win32/ole32_shim.c win32/oleacc_shim.c \
             win32/crypt32_shim.c win32/authenticode.c win32/shell32_shim.c \
            win32/comctl32_shim.c win32/comdlg32_shim.c \
            win32/richedit_shim.c \
            win32/winmm_shim.c win32/wsock32_shim.c \
            win32/ole2.c win32/msi.c win32/cab.c \
            win32/zip.c win32/msix.c win32/installer.c
WIN32_ASM = win32/int2e_stub.S win32/context64.S
DOS_ASM  = dos/dos_int_stub.S
DOS_SRC  = dos/dos_init.c dos/dos_exec.c dos/dos_loader.c \
           dos/dos_mem.c dos/cpu8086.c dos/dos_int.c \
           dos/dos_api.c dos/dos_bios.c dos/dos_time.c dos/dos_find.c \
           dos/dos_vga.c dos/dos_vbe.c dos/dos_io.c dos/dos_mouse.c \
           dos/dos_audio.c \
           dos/dos_dpmi.c dos/dos_vcpi.c dos/dos_jit.c
GUI_SRC  = ../../gui/gui_draw.c ../../gui/gui_text.c \
           ../../gui/gui_panel.c ../../gui/gui_dock.c \
           ../../gui/gui_desktop.c ../../gui/gui_window.c \
           ../../gui/gui_anim.c

# ── Object files ──────────────────────────────────────────────

BOOT_OBJ  = $(BUILD)/boot_efi.o
BOOT_PXE_OBJ = $(BUILD)/boot_efi_pxe.o $(BUILD)/boot_pxe_kernel.o
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
KERNEL_C_OBJ = $(KERN_OBJ) $(DRV_OBJ) $(FS_OBJ) $(WIN32_OBJ) $(DOS_OBJ) $(GUI_OBJ)
KERNEL_FLAGS_ID := $(shell printf '%s\n' '$(CC) $(KCFLAGS) $(KCFLAGS_PRECOMP) $(AVXFLAGS) $(WIN32_IF_AUDIT_FLAGS)' | cksum | cut -d' ' -f1)
KERNEL_FLAGS_STAMP = $(BUILD)/.kernel-cflags-$(KERNEL_FLAGS_ID)

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
               drivers/intel_gfx.c \
               drivers/sass.c drivers/gmmu.c drivers/gpu_tensor.c \
               drivers/gpu_inference.c drivers/xhci.c drivers/hda.c
FS_MINIMAL   = fs/ositofs2.c fs/ositofs3.c fs/gpt.c fs/gguf.c fs/vfs.c
KERN_MIN_OBJ = $(KERN_MINIMAL:%.c=$(BUILD)/%.o)
DRV_MIN_OBJ  = $(DRV_MINIMAL:%.c=$(BUILD)/%.o)
FS_MIN_OBJ   = $(FS_MINIMAL:%.c=$(BUILD)/%.o)
ALL_MIN_OBJ  = $(KERN_MIN_OBJ) $(ASM_OBJ) $(DRV_MIN_OBJ) $(FS_MIN_OBJ) \
               $(WIN32_OBJ) $(WIN32_ASM_OBJ) $(DOS_OBJ) $(DOS_ASM_OBJ) $(GUI_OBJ)

.PHONY: all clean check-deps legacy tcc tcc-clean minimal libvulkan pxe \
	dos-dpmi-test dos-vbe-native-test dos-audio-native-test dos-vga-test \
	oss-audio-elf-test \
	oss-audio-execve-elf-test oss-audio-no-device-elf-test \
	shm-surface-elf-test console-input-elf-test dos-console-input-test \
	dos-extended-error-test dos-file-create-test dos-handle-table-test \
	dos-truename-test dos-exec-test dos-find-test dos-env-test dos-mouse-test \
	dos-vbe-test

$(KERNEL_FLAGS_STAMP): | $(BUILD)
	@rm -f $(BUILD)/.kernel-cflags $(BUILD)/.kernel-cflags-*
	@printf '%s\n' '$(CC) $(KCFLAGS) $(KCFLAGS_PRECOMP) $(AVXFLAGS) $(WIN32_IF_AUDIT_FLAGS)' > $@

$(KERNEL_C_OBJ): $(KERNEL_FLAGS_STAMP)

all: check-deps $(BUILD)/boot.efi $(BUILD)/kernel.elf libvulkan

dos-dpmi-test: $(BUILD)/test/dpmi_native.com

dos-vbe-native-test: $(BUILD)/test/dpmi_vbe.com

dos-audio-native-test: $(BUILD)/test/dpmi_audio.com

dos-vga-test: $(BUILD)/test/dosvga.com

dos-console-input-test: $(BUILD)/test/doscon.com

dos-extended-error-test: $(BUILD)/test/doserr.com

dos-file-create-test: $(BUILD)/test/dosfile.com

dos-handle-table-test: $(BUILD)/test/dosjft.com

dos-truename-test: $(BUILD)/test/dostrue.com

dos-exec-test: $(BUILD)/test/dosexec.com $(BUILD)/test/dexchild.com \
	$(BUILD)/test/dexmz.exe $(BUILD)/test/dexdpmi.com

dos-find-test: $(BUILD)/test/dosfind.com

dos-env-test: $(BUILD)/test/dosenv.com

dos-mouse-test: $(BUILD)/test/dosmouse.com

dos-vbe-test: $(BUILD)/test/dosvbe.com

oss-audio-elf-test: $(BUILD)/test/ossaud.elf

oss-audio-execve-elf-test: $(BUILD)/test/ossexec.elf \
	$(BUILD)/test/ossaud.elf

oss-audio-no-device-elf-test: $(BUILD)/test/ossnone.elf

shm-surface-elf-test: $(BUILD)/test/shmsurf.elf

console-input-elf-test: $(BUILD)/test/coninput.elf

$(BUILD)/test/dpmi_native.com: $(BUILD)/test/dpmi_native.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_native.o: test/dpmi_native.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/dpmi_vbe.com: $(BUILD)/test/dpmi_vbe.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_vbe.o: test/dpmi_vbe.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/dpmi_audio.com: $(BUILD)/test/dpmi_audio.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_audio.o: test/dpmi_audio.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/dosvga.com: $(BUILD)/test/dos_vga_text.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dos_vga_text.o: test/dos_vga_text.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/doscon.com: $(BUILD)/test/dos_console_input.o \
		test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dos_console_input.o: test/dos_console_input.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/dosioctl.com: $(BUILD)/test/dos_ioctl.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dos_ioctl.o: test/dos_ioctl.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/iochild.com: $(BUILD)/test/dos_ioctl_child.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dos_ioctl_child.o: test/dos_ioctl_child.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/doserr.com: $(BUILD)/test/dos_extended_error.o \
		test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dos_extended_error.o: test/dos_extended_error.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/dosfile.com: $(BUILD)/test/dos_file_create.o \
		test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dos_file_create.o: test/dos_file_create.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/dosjft.com: $(BUILD)/test/dos_handle_table.o \
		test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dos_handle_table.o: test/dos_handle_table.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/dostrue.com: $(BUILD)/test/dos_truename.o \
		test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dos_truename.o: test/dos_truename.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/dosexec.com: $(BUILD)/test/dos_exec_contract.o \
		test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dos_exec_contract.o: test/dos_exec_contract.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/dexchild.com: $(BUILD)/test/dos_exec_child.o \
		test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dos_exec_child.o: test/dos_exec_child.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/dexmz.exe: $(BUILD)/test/dos_exec_mz_child.o test/dos_mz.ld
	ld -m elf_i386 -T test/dos_mz.ld -o $@ $<

$(BUILD)/test/dos_exec_mz_child.o: test/dos_exec_mz_child.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/dexdpmi.com: $(BUILD)/test/dpmi_native.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dosfind.com: $(BUILD)/test/dos_find_contract.o \
		test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dos_find_contract.o: test/dos_find_contract.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/dosenv.com: $(BUILD)/test/dos_env_contract.o \
		test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dos_env_contract.o: test/dos_env_contract.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/dosmouse.com: $(BUILD)/test/dos_mouse_contract.o \
		test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dos_mouse_contract.o: test/dos_mouse_contract.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/dosvbe.com: $(BUILD)/test/dos_vbe_contract.o \
		test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dos_vbe_contract.o: test/dos_vbe_contract.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/console_input_elf.o: test/console_input_elf.c | $(BUILD)/test
	$(CC) -std=c11 -ffreestanding -fno-stack-protector -mno-red-zone \
		-O2 -fno-pie -nostdlib -c -o $@ $<

$(BUILD)/test/coninput.elf: $(BUILD)/test/console_input_elf.o
	$(LD) -m elf_x86_64 -static -e _start -o $@ $<

$(BUILD)/test/oss_audio_elf.o: test/oss_audio_elf.c | $(BUILD)/test
	$(CC) -std=c11 -ffreestanding -fno-stack-protector -mno-red-zone \
		-O2 -fno-pie -nostdlib -c -o $@ $<

$(BUILD)/test/ossaud.elf: $(BUILD)/test/oss_audio_elf.o
	$(LD) -m elf_x86_64 -static -e _start -o $@ $<

$(BUILD)/test/oss_audio_no_device_elf.o: test/oss_audio_elf.c | $(BUILD)/test
	$(CC) -std=c11 -ffreestanding -fno-stack-protector -mno-red-zone \
		-O2 -fno-pie -nostdlib -DOSS_EXPECT_NO_DEVICE=1 -c -o $@ $<

$(BUILD)/test/ossnone.elf: $(BUILD)/test/oss_audio_no_device_elf.o
	$(LD) -m elf_x86_64 -static -e _start -o $@ $<

$(BUILD)/test/oss_audio_execve_elf.o: test/oss_audio_execve_elf.c | $(BUILD)/test
	$(CC) -std=c11 -ffreestanding -fno-stack-protector -mno-red-zone \
		-O2 -fno-pie -nostdlib -c -o $@ $<

$(BUILD)/test/ossexec.elf: $(BUILD)/test/oss_audio_execve_elf.o
	$(LD) -m elf_x86_64 -static -e _start -o $@ $<

$(BUILD)/test/shm_surface_elf.o: test/shm_surface_elf.c | $(BUILD)/test
	$(CC) -std=c11 -ffreestanding -fno-stack-protector -mno-red-zone \
		-O2 -fno-pie -nostdlib -c -o $@ $<

$(BUILD)/test/shmsurf.elf: $(BUILD)/test/shm_surface_elf.o
	$(LD) -m elf_x86_64 -static -e _start -o $@ $<

$(BUILD)/test:
	mkdir -p $@
	@echo ""
	@echo "Built: $(BUILD)/boot.efi + $(BUILD)/kernel.elf + lib/vulkan/libvulkan.a"

pxe: check-deps $(BUILD)/boot-pxe.efi
	@echo ""
	@echo "Built PXE: $(BUILD)/boot-pxe.efi (kernel.elf embedded)"

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

$(BUILD)/boot-pxe.efi: $(BUILD)/boot-pxe.so
	$(OBJCOPY) -j .text -j .rodata -j .sdata -j .data -j .dynamic -j .dynsym \
	           -j .rel -j .rela -j .reloc \
	           --target=efi-app-x86_64 $< $@ 2>/dev/null || \
	python3 scripts/elf2efi.py $< $@
	@echo "Built: $@ ($$(stat -c%s $@ 2>/dev/null || stat -f%z $@ 2>/dev/null || wc -c < $@) bytes)"

$(BUILD)/boot.so: $(BOOT_OBJ)
	$(LD) $(BLDFLAGS) -o $@ $^ $(BLIBS)

$(BUILD)/boot-pxe.so: $(BOOT_PXE_OBJ)
	$(LD) $(BLDFLAGS) -o $@ $^ $(BLIBS)

$(BUILD)/boot_efi.o: boot/boot_efi.c | $(BUILD)
	$(CC) $(BFLAGS) -c -o $@ $<

$(BUILD)/boot_efi_pxe.o: boot/boot_efi.c | $(BUILD)
	$(CC) $(BFLAGS) -DOK_PXE_EMBED_KERNEL=1 -c -o $@ $<

$(BUILD)/boot_pxe_kernel.o: boot/kernel_embed.S $(BUILD)/kernel.elf | $(BUILD)
	$(CC) -c -o $@ $<

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

# Win32 compatibility layer — shims run on the guest thread's SIMD state.
$(BUILD)/win32/%.o: win32/%.c | $(BUILD)/win32
	$(CC) $(KCFLAGS) $(WIN32_IF_AUDIT_FLAGS) -fno-tree-vectorize -fno-slp-vectorize -Iwin32 -c -o $@ $<

# The profiling hooks themselves must never be instrumented.
$(BUILD)/win32/if_audit.o: win32/if_audit.c | $(BUILD)/win32
	$(CC) $(KCFLAGS) -fno-tree-vectorize -fno-slp-vectorize -Iwin32 -c -o $@ $<

# GCC-only Win32 precompiled files: use KCFLAGS_PRECOMP (TCC stack compat)
$(BUILD)/win32/compat32.o: win32/compat32.c | $(BUILD)/win32
	$(CC) $(filter-out -fno-pie,$(KCFLAGS_PRECOMP)) $(WIN32_IF_AUDIT_FLAGS) -fPIE -fno-jump-tables -Iwin32 -c -o $@ $<

$(BUILD)/win32/msvcrt_shim.o: win32/msvcrt_shim.c | $(BUILD)/win32
	$(CC) $(KCFLAGS_PRECOMP) $(WIN32_IF_AUDIT_FLAGS) -Iwin32 -c -o $@ $<

$(BUILD)/win32/opengl32_shim.o: win32/opengl32_shim.c | $(BUILD)/win32
	$(CC) $(KCFLAGS_PRECOMP) $(WIN32_IF_AUDIT_FLAGS) -fno-tree-vectorize -fno-slp-vectorize -Iwin32 -Ilib/opengl/mesa/include -c -o $@ $<

$(BUILD)/win32/ddraw_shim.o: win32/ddraw_shim.c | $(BUILD)/win32
	$(CC) $(KCFLAGS_PRECOMP) $(WIN32_IF_AUDIT_FLAGS) -Iwin32 -c -o $@ $<

$(BUILD)/win32/dsound_shim.o: win32/dsound_shim.c | $(BUILD)/win32
	$(CC) $(KCFLAGS_PRECOMP) $(WIN32_IF_AUDIT_FLAGS) -Iwin32 -c -o $@ $<

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
TCCFLAGS = -c -nostdlib -Iinclude -I../../include/common -D__KERNEL_X86__=1 \
           $(DOS_TRACE_FLAGS)

# Files that MUST use GCC (SSE/AVX asm constraints, special builtins)
GCC_ONLY_C = kernel/tensor.c kernel/tensor_avx2.c \
             win32/compat32.c win32/msvcrt_shim.c win32/opengl32_shim.c

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
GCC_WIN32_COMPAT  = $(TCCDIR)/win32/compat32.o $(TCCDIR)/win32/msvcrt_shim.o \
                    $(TCCDIR)/win32/opengl32_shim.o
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

$(TCCDIR)/win32/opengl32_shim.o: win32/opengl32_shim.c | $(TCCDIR)/win32
	$(CC) $(KCFLAGS_PRECOMP) -fno-tree-vectorize -fno-slp-vectorize -Iwin32 -Ilib/opengl/mesa/include -c -o $@ $<

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
