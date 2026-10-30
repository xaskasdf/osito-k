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

# Physical native GGTT/GT takeover qualification; never enabled implicitly.
INTEL_GGTT_DIAGNOSTIC ?= 0
KCFLAGS += -DINTEL_GGTT_DIAGNOSTIC=$(INTEL_GGTT_DIAGNOSTIC)
INTEL_BCS_DIAGNOSTIC ?= 0
KCFLAGS += -DINTEL_BCS_DIAGNOSTIC=$(INTEL_BCS_DIAGNOSTIC)
INTEL_SCANOUT_DIAGNOSTIC ?= 0
KCFLAGS += -DINTEL_SCANOUT_DIAGNOSTIC=$(INTEL_SCANOUT_DIAGNOSTIC)

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

# Record only failed dynamic DLL/export lookups, including their original
# last-error value. Use for delayed-load failures without tracing successful
# calls in Steam/CEF.
WIN32_LOOKUP_TRACE ?= 0
ifeq ($(WIN32_LOOKUP_TRACE),1)
  KCFLAGS += -DK32_LOOKUP_FAILURE_TRACE=1
  # Optional exact return address: persist a targeted failure before the
  # application's fatal hook. Zero avoids disk I/O for ordinary probes.
  WIN32_LOOKUP_CHECKPOINT_CALLER ?= 0
  KCFLAGS += -DK32_LOOKUP_CHECKPOINT_CALLER=$(WIN32_LOOKUP_CHECKPOINT_CALLER)
endif

# Per-operation virtual-memory tracing is useful for allocator corruption
# investigations, but Steam/CEF can issue thousands of successful calls per
# second. Keep failures and aggregate VM statistics enabled in normal builds.
#   make -C arch/x86 CLANG=1 WIN32_VM_TRACE=1
WIN32_VM_TRACE ?= 0
ifeq ($(WIN32_VM_TRACE),1)
  KCFLAGS += -DWIN32_VM_TRACE=1
endif

# Bounded timing samples and cumulative counters, without per-allocation logs.
WIN32_LATENCY_TRACE ?= 0
ifeq ($(WIN32_LATENCY_TRACE),1)
  KCFLAGS += -DWIN32_LATENCY_TRACE=1
endif

# DOS API and MCB tracing is intentionally opt-in. Native DOS programs can
# issue these calls in tight loops, and COM1 output is expensive under QEMU.
#   make -C arch/x86 CLANG=1 DOS_TRACE=1
DOS_TRACE ?= 0
ifeq ($(DOS_TRACE),1)
  DOS_TRACE_FLAGS = -DDOS_DIAGNOSTICS=1
  KCFLAGS += $(DOS_TRACE_FLAGS)
endif

# Bounded in-memory IRQ/PIC/PIT observations, with no serial output.
# Read the sequence-tagged ring through a debugger on diagnostic builds.
DOS_IRQ_TRACE ?= 0
ifeq ($(DOS_IRQ_TRACE),1)
  KCFLAGS += -DDOS_IRQ_DIAGNOSTICS=1
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

# Linux/WSL UART fault-injection tests, independent of QEMU and guest files.
HOST_CC ?= cc
SERIAL_HOST_SANITIZERS ?=
.PHONY: serial-host-test
serial-host-test: | $(BUILD)/test
	$(HOST_CC) -std=c11 -ffreestanding -O2 -Wall -Wextra -Werror $(SERIAL_HOST_SANITIZERS) \
		-DSERIAL_HOST_TEST kernel/serial_tx.c kernel/serial_test.c -o $(BUILD)/test/serial-core-host
	$(BUILD)/test/serial-core-host
	$(HOST_CC) -std=c11 -ffreestanding -O2 -Wall -Wextra -Werror $(SERIAL_HOST_SANITIZERS) \
		-include test/serial_uart_host.h kernel/serial.c kernel/serial_tx.c \
		test/serial_uart_host.c -o $(BUILD)/test/serial-uart-host
	$(BUILD)/test/serial-uart-host

# ── Source files ──────────────────────────────────────────────

BOOT_SRC = boot/boot_efi.c
WPA_KERN_SRC = kernel/random_secure.c kernel/wpa2_crypto.c kernel/wpa2_rsn.c kernel/wpa2.c kernel/wpa2_selftest.c
DRIVER_KERN_SRC = kernel/driver_manager.c kernel/driver_store.c kernel/driver_firmware.c kernel/driver_services.c kernel/wlan_profiles.c

KERN_SRC = kernel/main.c kernel/serial.c kernel/serial_tx.c kernel/serial_test.c kernel/framebuffer.c \
           kernel/pci.c kernel/acpi_reset.c kernel/dma_diag.c kernel/memory.c kernel/mem_extern.c kernel/net.c kernel/kexec.c kernel/kernel_update.c fs/boot_kernel.c \
           kernel/dhcp.c kernel/ntp.c kernel/ipv6.c kernel/mdns.c \
           kernel/tensor.c \
           kernel/tensor_avx2.c kernel/inference.c kernel/idt.c \
           kernel/paging.c kernel/compat_iret.c kernel/heap.c kernel/syscall.c \
           kernel/elf.c kernel/process.c \
           kernel/keyboard.c kernel/terminal.c kernel/terminal_control.c kernel/shell.c kernel/pkg.c \
           kernel/crypto.c kernel/tls.c kernel/http.c kernel/claude.c \
           $(WPA_KERN_SRC) \
           kernel/tokenizer.c kernel/smp.c kernel/smp_work.c kernel/sched_signal.c kernel/spec_prefetch.c kernel/spec_analyze.c kernel/spec_tls.c kernel/dynlink.c \
           kernel/zlib.c kernel/git.c \
           kernel/shm.c kernel/compositor.c kernel/display.c kernel/display_source.c \
           kernel/input_events.c kernel/memcompress.c \
           kernel/kmod.c kernel/wayland.c kernel/pty.c \
           kernel/socket.c kernel/timers.c kernel/power.c kernel/thermal.c kernel/thermal_sample.c kernel/cpu_idle.c kernel/lockup.c kernel/pollobj.c \
           kernel/sysv_ipc.c kernel/vt.c \
           kernel/blkdev.c kernel/strace.c kernel/caps.c \
           kernel/klog.c kernel/boot_diag.c kernel/random.c kernel/oom.c kernel/ns.c \
           kernel/panic.c kernel/sysfs.c kernel/netfilter.c \
           kernel/crypto2.c kernel/sched_rt.c kernel/dm.c \
           kernel/evdev.c kernel/sysctl.c kernel/tls13.c kernel/tls13_server.c kernel/sshd.c \
           kernel/file_sync.c kernel/file_sync_fs.c \
           kernel/screen_capture.c kernel/screen_capture_gop.c $(DRIVER_KERN_SRC) \
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
ASM_SRC  = kernel/isr_stubs.S kernel/compat_iret_tail.S kernel/syscall_entry.S kernel/setjmp.S kernel/kexec_tramp.S kernel/msr_safe.S kernel/cpu_idle_wait.S
DRV_SRC  = drivers/nvme.c drivers/i211.c drivers/rtl8111.c drivers/gpu.c drivers/gsp.c \
           drivers/intel_gfx.c drivers/intel_gfx_display.c drivers/intel_gfx_display_source.c drivers/intel_scanout_test.c \
           drivers/intel_ggtt.c \
           drivers/intel_ggtt_space.c \
           drivers/intel_gt.c \
           drivers/intel_bcs.c drivers/intel_cs.c drivers/intel_rcs.c drivers/intel_rcs_draw.c drivers/intel_gen9_texture.c \
           drivers/intel_rcs_sampled.c drivers/intel_rcs_vertex.c drivers/intel_rcs_sampled_test.c drivers/intel_rcs_vertex_test.c drivers/intel_bcs_queue.c \
           drivers/intel_bcs_worker.c \
           drivers/intel_bcs_irq.c \
           drivers/intel_bcs_context.c \
           drivers/intel_gpu_buffer.c \
           drivers/intel_gpu_memory.c \
           drivers/intel_gpu_kmt.c \
           drivers/intel_gpu_contexts.c \
           drivers/intel_gpu_fences.c \
           drivers/intel_gpu_kmt_context.c \
           drivers/intel_gpu_kmt_render.c \
           drivers/intel_gpu_kmt_fence.c \
           drivers/intel_gpu_service.c \
           drivers/intel_gpu_capture.c drivers/intel_gen9_shader.c \
           drivers/intel_gpu_jobs.c \
           drivers/intel_gpu_executor.c \
           drivers/intel_gpu_bcs_owner.c \
           drivers/intel_gfx_driver.c \
           drivers/intel_gpu_cpu_view.c \
           drivers/intel_ppgtt.c \
           drivers/intel_gfx_memory.c \
           drivers/sass.c drivers/gmmu.c drivers/gpu_tensor.c drivers/gpu_inference.c \
           drivers/xhci.c drivers/hid_parser.c drivers/ps2.c drivers/hda.c \
           drivers/virtio.c drivers/virtio_blk.c drivers/virtio_net.c drivers/virtio_gpu.c \
           drivers/virtio_gpu_3d.c drivers/nvk_backend.c drivers/nvk_backend_res.c \
           drivers/usb_storage.c drivers/iwx.c drivers/iwx_fw.c drivers/iwx_scan.c drivers/iwx_link.c drivers/iwx_data.c
FS_SRC   = fs/ositofs2.c fs/ositofs3.c fs/gpt.c fs/gguf.c fs/vfs.c fs/fat32.c \
           fs/tmpfs.c fs/iso9660.c fs/ext2.c fs/exfat.c fs/ntfs.c \
           fs/udf.c fs/squashfs.c fs/hfsplus.c fs/btrfs.c fs/apfs.c
WIN32_SRC = win32/win32_init.c win32/compat32.c win32/pe.c win32/winexec.c \
	win32/handle.c win32/filelock.c win32/filecache.c win32/ntsyscall.c win32/ntprocess.c win32/ntjob.c win32/ntsync.c \
            win32/dllloader.c win32/unwind64.c win32/wdbg.c win32/if_audit.c win32/win32_abi.c \
             win32/wintime.c win32/kernel32_shim.c win32/osversion.c win32/timer_queue.c win32/profile.c \
             win32/ntdll_shim.c win32/delay_load.c \
            win32/msvcrt_shim.c win32/crt_secure.c win32/crt_float.c win32/crt_math.c win32/crt_args.c win32/crt_startup.c win32/crt_exit.c \
             win32/user32_shim.c win32/user32_text.c win32/user32_controls.c win32/user32_listbox.c win32/gdi32_shim.c win32/opengl32_shim.c \
             win32/libusb_shim.c \
             win32/dxgi_shim.c win32/d3dkmt.c win32/d3dkmt_cpu.c win32/d3dkmt_wait.c win32/d3dkmt_registry.c win32/version_shim.c \
            win32/advapi32_shim.c win32/scm.c \
            win32/ddraw_shim.c win32/dsound_shim.c win32/dinput8_shim.c \
            win32/ole32_shim.c win32/variant.c win32/iproute.c win32/ipinterface.c win32/oleacc_shim.c \
             win32/crypt32_shim.c win32/imagehlp_shim.c win32/secur32_shim.c win32/authenticode.c win32/urlmon_shim.c win32/advpack_shim.c win32/rpcrt4_shim.c win32/cabinet_shim.c win32/setupapi_inf.c win32/mmio_shim.c win32/wow64.c win32/installengine.c win32/shell32_shim.c win32/shell_fileop.c \
            win32/comctl32_shim.c win32/propertysheet.c win32/comdlg32_shim.c \
            win32/richedit_shim.c \
            win32/winmm_shim.c win32/wsock32_shim.c \
            win32/ole2.c win32/msi.c win32/msi_condition.c win32/msi_package.c win32/msi_plan.c \
            win32/msi_payload.c win32/msi_format.c win32/msi_registry.c win32/msi_apply.c \
            win32/msi_win32_io.c win32/msi_actions.c win32/msi_summary.c win32/msi_registration.c win32/msi_engine.c win32/msi_shim.c win32/msi_products.c win32/msi_discovery.c win32/msi_components.c win32/msi_install.c win32/msi_selftest.c win32/msi_engine_test.c \
            win32/fold_string.c win32/short_path.c win32/binary_type.c \
            win32/cab.c win32/cab_lzx.c win32/setupapi_cab.c \
            win32/zip.c win32/msix.c win32/installer.c
WIN32_ASM = win32/int2e_stub.S win32/context64.S win32/nt64_service.S
DOS_ASM  = dos/dos_int_stub.S dos/dos_fpu_irq_test.S
DOS_SRC  = dos/dos_init.c dos/dos_exec.c dos/dos_loader.c \
           dos/dos_mem.c dos/cpu8086.c dos/dos_int.c \
           dos/dos_api.c dos/dos_bios.c dos/dos_time.c dos/dos_find.c \
           dos/dos_vga.c dos/dos_vbe.c dos/dos_io.c dos/dos_mouse.c \
           dos/dos_audio.c dos/dos_fpu.c dos/dos_fpu_test.c dos/dos_a20_test.c \
           dos/dos_xms.c dos/dos_xms_test.c dos/dos_umb_test.c \
           dos/dos_dpmi.c dos/dos_vcpi.c dos/dos_jit.c dos/dos_paging.c
GUI_SRC  = ../../gui/gui_draw.c ../../gui/gui_text.c \
           ../../gui/gui_panel.c ../../gui/gui_dock.c \
           ../../gui/gui_desktop.c ../../gui/gui_window.c \
           ../../gui/gui_anim.c ../../gui/gui_theme.c ../../gui/gui_font.c \
           ../../gui/gui_widgets.c

# ── Object files ──────────────────────────────────────────────

BOOT_OBJ  = $(BUILD)/boot_efi.o
BOOT_PXE_OBJ = $(BUILD)/boot_efi_pxe.o $(BUILD)/boot_pxe_kernel.o
KERN_OBJ  = $(KERN_SRC:%.c=$(BUILD)/%.o)
ASM_OBJ   = $(ASM_SRC:%.S=$(BUILD)/%.o)
DRV_OBJ   = $(DRV_SRC:%.c=$(BUILD)/%.o)
FS_OBJ    = $(FS_SRC:%.c=$(BUILD)/%.o)
WIN32_SRC += $(wildcard win32/math/*.c)
WIN32_OBJ = $(WIN32_SRC:%.c=$(BUILD)/%.o)
WIN32_ASM_OBJ = $(WIN32_ASM:%.S=$(BUILD)/%.o)
DOS_OBJ   = $(DOS_SRC:%.c=$(BUILD)/%.o)
DOS_NUMERIC_SRC = dos/dos_fpu_softfloat.cc dos/dos_fpu_state.cc $(wildcard lib/softfloat3e/*.cc lib/softfloat3e/8086-SSE/*.cc lib/bochs-transcendental/*.cc)
DOS_NUMERIC_OBJ = $(DOS_NUMERIC_SRC:%.cc=$(BUILD)/%.o)
DOS_OBJ += $(DOS_NUMERIC_OBJ)
DOS_ASM_OBJ = $(DOS_ASM:%.S=$(BUILD)/%.o)
GUI_OBJ   = $(patsubst ../../gui/%.c,$(BUILD)/gui/%.o,$(GUI_SRC))
WPA_CRYPTO_SRC = lib/bearssl/src/hash/sha1.c lib/bearssl/src/mac/hmac.c \
                 lib/bearssl/src/hash/md5.c lib/bearssl/src/hash/sha2small.c lib/bearssl/src/hash/sha2big.c \
                 lib/bearssl/src/codec/dec32be.c lib/bearssl/src/codec/enc32be.c \
                 lib/bearssl/src/codec/dec32le.c lib/bearssl/src/codec/enc32le.c \
                 lib/bearssl/src/codec/dec64be.c lib/bearssl/src/codec/enc64be.c \
                 lib/bearssl/src/symcipher/aes_ct.c lib/bearssl/src/symcipher/aes_ct_dec.c \
                 lib/bearssl/src/symcipher/aes_ct_cbcdec.c
WPA_CRYPTO_OBJ = $(WPA_CRYPTO_SRC:%.c=$(BUILD)/%.o)
SSH_SRC = $(wildcard lib/tinyssh/upstream/*.c) lib/tinyssh/osito_port.c
SSH_OBJ = $(SSH_SRC:%.c=$(BUILD)/%.o)
SSH_HEADERS = $(wildcard lib/tinyssh/*.h lib/tinyssh/upstream/*.h lib/tinyssh/upstream/cryptoint/*.h lib/tinyssh/port/*.h lib/tinyssh/port/sys/*.h)

ALL_KERN_OBJ = $(KERN_OBJ) $(ASM_OBJ) $(DRV_OBJ) $(FS_OBJ) $(WIN32_OBJ) $(WIN32_ASM_OBJ) $(DOS_OBJ) $(DOS_ASM_OBJ) $(GUI_OBJ)
KERNEL_C_OBJ = $(KERN_OBJ) $(DRV_OBJ) $(FS_OBJ) $(WIN32_OBJ) $(DOS_OBJ) $(GUI_OBJ)
ALL_KERN_OBJ += $(WPA_CRYPTO_OBJ)
KERNEL_C_OBJ += $(WPA_CRYPTO_OBJ)
ALL_KERN_OBJ += $(SSH_OBJ)
KERNEL_C_OBJ += $(SSH_OBJ)
KERNEL_FLAGS_ID := $(shell printf '%s\n' '$(CC) $(KCFLAGS) $(KCFLAGS_PRECOMP) $(AVXFLAGS) $(WIN32_IF_AUDIT_FLAGS)' | cksum | cut -d' ' -f1)
KERNEL_FLAGS_STAMP = $(BUILD)/.kernel-cflags-$(KERNEL_FLAGS_ID)

# ── Targets ───────────────────────────────────────────────────

# Minimal kernel: excludes all new subsystems from Apr 2026 session.
# Use when full kernel crashes (e.g., BSS overlap with NVMe DMA).
KERN_MINIMAL = kernel/main.c kernel/serial.c kernel/framebuffer.c kernel/lockup.c \
               $(WPA_KERN_SRC) \
               kernel/pci.c kernel/acpi_reset.c kernel/dma_diag.c kernel/memory.c kernel/mem_extern.c kernel/net.c kernel/kexec.c kernel/kernel_update.c fs/boot_kernel.c \
               kernel/tensor.c kernel/tensor_avx2.c kernel/inference.c \
               kernel/idt.c kernel/paging.c kernel/compat_iret.c kernel/heap.c kernel/syscall.c \
               kernel/elf.c kernel/process.c \
               kernel/keyboard.c kernel/terminal.c kernel/terminal_control.c kernel/shell.c kernel/pkg.c kernel/sshd.c \
               kernel/file_sync.c kernel/file_sync_fs.c \
               kernel/screen_capture.c kernel/screen_capture_gop.c $(DRIVER_KERN_SRC) \
               kernel/crypto.c kernel/tls.c kernel/http.c kernel/claude.c \
               kernel/tokenizer.c kernel/smp.c kernel/dynlink.c \
               kernel/zlib.c kernel/git.c \
               kernel/shm.c kernel/compositor.c kernel/display.c kernel/display_source.c \
               kernel/input_events.c kernel/memcompress.c
DRV_MINIMAL  = drivers/nvme.c drivers/i211.c drivers/rtl8111.c drivers/gpu.c drivers/gsp.c \
               drivers/intel_gfx.c \
               drivers/intel_ggtt.c \
               drivers/intel_ggtt_space.c \
               drivers/intel_gt.c \
               drivers/intel_bcs.c drivers/intel_cs.c drivers/intel_rcs.c drivers/intel_rcs_draw.c drivers/intel_gen9_texture.c \
               drivers/intel_rcs_sampled.c drivers/intel_rcs_vertex.c drivers/intel_rcs_sampled_test.c drivers/intel_rcs_vertex_test.c drivers/intel_bcs_queue.c \
               drivers/intel_bcs_context.c \
               drivers/intel_gpu_buffer.c \
               drivers/intel_gpu_memory.c \
               drivers/intel_ppgtt.c \
               drivers/intel_gfx_memory.c \
               drivers/sass.c drivers/gmmu.c drivers/gpu_tensor.c \
               drivers/gpu_inference.c drivers/xhci.c drivers/hda.c drivers/iwx.c drivers/iwx_fw.c drivers/iwx_scan.c drivers/iwx_link.c drivers/iwx_data.c
FS_MINIMAL   = fs/ositofs2.c fs/ositofs3.c fs/gpt.c fs/gguf.c fs/vfs.c
KERN_MIN_OBJ = $(KERN_MINIMAL:%.c=$(BUILD)/%.o)
DRV_MIN_OBJ  = $(DRV_MINIMAL:%.c=$(BUILD)/%.o)
FS_MIN_OBJ   = $(FS_MINIMAL:%.c=$(BUILD)/%.o)
ALL_MIN_OBJ  = $(KERN_MIN_OBJ) $(ASM_OBJ) $(DRV_MIN_OBJ) $(FS_MIN_OBJ) \
               $(WIN32_OBJ) $(WIN32_ASM_OBJ) $(DOS_OBJ) $(DOS_ASM_OBJ) $(GUI_OBJ) $(WPA_CRYPTO_OBJ) $(SSH_OBJ)

.PHONY: all clean check-deps legacy tcc tcc-clean minimal libvulkan pxe \
	dos-dpmi-test dos-vbe-native-test dos-audio-native-test dos-flags-test dos-debug-test dos-string-io-test dos-exception-test dos-vga-test \
	dos-record-service-test dos-real-exception-test dos-callback-entry-test dos-exec-primary-test \
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

# Standalone PE system utilities; installed on disk, not embedded in kernel.elf.
# Requires clang, llvm-dlltool and lld-link for both guest architectures.
.PHONY: win32-system-tools
win32-system-tools:
	bash scripts/build-win32-system-tools.sh arch/x86/$(BUILD)/win32-system-tools

dos-dpmi-test: $(BUILD)/test/dpmi_native.com

dos-vbe-native-test: $(BUILD)/test/dpmi_vbe.com

dos-audio-native-test: $(BUILD)/test/dpmi_audio.com

dos-flags-test: $(BUILD)/test/dpflags16.com $(BUILD)/test/dpflags32.com
dos-flags-test: $(BUILD)/test/dpnif16.com $(BUILD)/test/dpnif32.com

.PHONY: dos-bios-move-test
dos-bios-move-test: $(addprefix $(BUILD)/test/,bmovrm.com bmov16.com bmov32.com bmovs32.com)

$(BUILD)/test/bmovrm.o: test/bios_move.S | $(BUILD)/test
	as --32 --defsym MODE=0 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/bmov16.o: test/bios_move.S | $(BUILD)/test
	as --32 --defsym MODE=1 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/bmov32.o: test/bios_move.S | $(BUILD)/test
	as --32 --defsym MODE=2 --defsym STACK32=1 -o $@ $<
$(BUILD)/test/bmovs32.o: test/bios_move.S | $(BUILD)/test
	as --32 --defsym MODE=2 --defsym STACK32=0 -o $@ $<
$(addprefix $(BUILD)/test/,bmovrm.com bmov16.com bmov32.com bmovs32.com): %.com: %.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

dos-rm-arguments-test: $(BUILD)/test/dpargs16.com $(BUILD)/test/dpargs32.com

dos-record-service-test: $(BUILD)/test/dprs16.com $(BUILD)/test/dprs32.com \
	$(BUILD)/test/dprss16.com $(BUILD)/test/dprss32.com

dos-state-recovery-test: $(BUILD)/test/dpsr16.com $(BUILD)/test/dpsr32.com \
	$(BUILD)/test/dpsrs16.com $(BUILD)/test/dpsrs32.com

dos-callback-recovery-test: $(BUILD)/test/dpcr16.com $(BUILD)/test/dpcr32.com \
	$(BUILD)/test/dpcrs16.com $(BUILD)/test/dpcrs32.com

dos-callback-register-test: $(BUILD)/test/dprg16.com $(BUILD)/test/dprg32.com \
	$(BUILD)/test/dprgs16.com $(BUILD)/test/dprgs32.com

dos-real-exception-test: $(BUILD)/test/dpre16.com $(BUILD)/test/dpre32.com \
                         $(BUILD)/test/dpres16.com $(BUILD)/test/dpres32.com

dos-callback-entry-test: $(BUILD)/test/dpce16.com $(BUILD)/test/dpce32.com \
	$(BUILD)/test/dpces16.com $(BUILD)/test/dpces32.com

.PHONY: dos-client-resources-test
.PHONY: dos-client-routing-test
.PHONY: dos-raw-entry-test
.PHONY: dos-raw-client-test
.PHONY: dos-exec-unwind-test
dos-exec-unwind-test: $(addprefix $(BUILD)/test/,dxuc16.com dxuc32.com dxup16.com dxup32.com \
	dxus16.com dxus32.com dxun16.com dxun32.com dxuns16.com dxuns32.com \
	dxul16.com dxul32.com dxuls16.com dxuls32.com dxum16.com dxum32.com dxums16.com dxums32.com)

$(BUILD)/test/dxul16.o: test/dpmi_exec_unwind.S | $(BUILD)/test
	as --32 --defsym LOADONLY=1 --defsym CHILD=0 --defsym CHILD32=0 --defsym PM32=0 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/dxul32.o: test/dpmi_exec_unwind.S | $(BUILD)/test
	as --32 --defsym LOADONLY=1 --defsym CHILD=0 --defsym CHILD32=0 --defsym PM32=1 --defsym STACK32=1 -o $@ $<
$(BUILD)/test/dxuls16.o: test/dpmi_exec_unwind.S | $(BUILD)/test
	as --32 --defsym LOADONLY=1 --defsym CHILD=0 --defsym CHILD32=0 --defsym PM32=1 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/dxuls32.o: test/dpmi_exec_unwind.S | $(BUILD)/test
	as --32 --defsym LOADONLY=1 --defsym CHILD=0 --defsym CHILD32=0 --defsym PM32=0 --defsym STACK32=1 -o $@ $<
$(BUILD)/test/dxum16.o: test/dpmi_exec_unwind.S | $(BUILD)/test
	as --32 --defsym LOADONLY=1 --defsym CHILD=0 --defsym CHILD32=1 --defsym PM32=0 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/dxum32.o: test/dpmi_exec_unwind.S | $(BUILD)/test
	as --32 --defsym LOADONLY=1 --defsym CHILD=0 --defsym CHILD32=1 --defsym PM32=1 --defsym STACK32=1 -o $@ $<
$(BUILD)/test/dxums16.o: test/dpmi_exec_unwind.S | $(BUILD)/test
	as --32 --defsym LOADONLY=1 --defsym CHILD=0 --defsym CHILD32=1 --defsym PM32=1 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/dxums32.o: test/dpmi_exec_unwind.S | $(BUILD)/test
	as --32 --defsym LOADONLY=1 --defsym CHILD=0 --defsym CHILD32=1 --defsym PM32=0 --defsym STACK32=1 -o $@ $<

$(BUILD)/test/dxuc16.o: test/dpmi_exec_unwind.S | $(BUILD)/test
	as --32 --defsym CHILD=1 --defsym PM32=0 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/dxuc32.o: test/dpmi_exec_unwind.S | $(BUILD)/test
	as --32 --defsym CHILD=1 --defsym PM32=1 --defsym STACK32=1 -o $@ $<
$(BUILD)/test/dxup16.o: test/dpmi_exec_unwind.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym CHILD32=0 --defsym PM32=0 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/dxup32.o: test/dpmi_exec_unwind.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym CHILD32=0 --defsym PM32=1 --defsym STACK32=1 -o $@ $<
$(BUILD)/test/dxus16.o: test/dpmi_exec_unwind.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym CHILD32=0 --defsym PM32=1 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/dxus32.o: test/dpmi_exec_unwind.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym CHILD32=0 --defsym PM32=0 --defsym STACK32=1 -o $@ $<
$(BUILD)/test/dxun16.o: test/dpmi_exec_unwind.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym CHILD32=1 --defsym PM32=0 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/dxun32.o: test/dpmi_exec_unwind.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym CHILD32=1 --defsym PM32=1 --defsym STACK32=1 -o $@ $<
$(BUILD)/test/dxuns16.o: test/dpmi_exec_unwind.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym CHILD32=1 --defsym PM32=1 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/dxuns32.o: test/dpmi_exec_unwind.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym CHILD32=1 --defsym PM32=0 --defsym STACK32=1 -o $@ $<

$(addprefix $(BUILD)/test/,dxuc16.com dxuc32.com dxup16.com dxup32.com dxus16.com dxus32.com dxun16.com dxun32.com dxuns16.com dxuns32.com dxul16.com dxul32.com dxuls16.com dxuls32.com dxum16.com dxum32.com dxums16.com dxums32.com): %.com: %.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

dos-raw-client-test: $(addprefix $(BUILD)/test/,droc16.com droc32.com drop16.com drop32.com \
	dros16.com dros32.com dron16.com dron32.com drons16.com drons32.com)

$(BUILD)/test/droc16.o: test/dpmi_raw_client.S | $(BUILD)/test
	as --32 --defsym CHILD=1 --defsym PM32=0 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/droc32.o: test/dpmi_raw_client.S | $(BUILD)/test
	as --32 --defsym CHILD=1 --defsym PM32=1 --defsym STACK32=1 -o $@ $<
$(BUILD)/test/drop16.o: test/dpmi_raw_client.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym CHILD32=0 --defsym PM32=0 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/drop32.o: test/dpmi_raw_client.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym CHILD32=0 --defsym PM32=1 --defsym STACK32=1 -o $@ $<
$(BUILD)/test/dros16.o: test/dpmi_raw_client.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym CHILD32=0 --defsym PM32=1 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/dros32.o: test/dpmi_raw_client.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym CHILD32=0 --defsym PM32=0 --defsym STACK32=1 -o $@ $<
$(BUILD)/test/dron16.o: test/dpmi_raw_client.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym CHILD32=1 --defsym PM32=0 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/dron32.o: test/dpmi_raw_client.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym CHILD32=1 --defsym PM32=1 --defsym STACK32=1 -o $@ $<
$(BUILD)/test/drons16.o: test/dpmi_raw_client.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym CHILD32=1 --defsym PM32=1 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/drons32.o: test/dpmi_raw_client.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym CHILD32=1 --defsym PM32=0 --defsym STACK32=1 -o $@ $<

$(addprefix $(BUILD)/test/,droc16.com droc32.com drop16.com drop32.com dros16.com dros32.com dron16.com dron32.com drons16.com drons32.com): %.com: %.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

dos-raw-entry-test: $(addprefix $(BUILD)/test/,dprw16.com dprw32.com dprws16.com dprws32.com)

dos-client-routing-test: $(addprefix $(BUILD)/test/,dxcb16.com dxcb32.com \
	dxbp16.com dxbp32.com dxbs16.com dxbs32.com dxbn16.com dxbn32.com dxbns16.com dxbns32.com)

$(BUILD)/test/dxcb16.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=3 --defsym RESOURCES=4 -o $@ $<
$(BUILD)/test/dxcb32.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=3 --defsym CHILD32=1 --defsym RESOURCES=4 -o $@ $<
$(BUILD)/test/dxbp16.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym RESOURCES=4 --defsym PM32=0 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/dxbp32.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym RESOURCES=4 --defsym PM32=1 --defsym STACK32=1 -o $@ $<
$(BUILD)/test/dxbs16.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym RESOURCES=4 --defsym PM32=1 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/dxbs32.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym RESOURCES=4 --defsym PM32=0 --defsym STACK32=1 -o $@ $<
$(BUILD)/test/dxbn16.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym CHILD32=1 --defsym RESOURCES=4 --defsym PM32=0 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/dxbn32.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym CHILD32=1 --defsym RESOURCES=4 --defsym PM32=1 --defsym STACK32=1 -o $@ $<
$(BUILD)/test/dxbns16.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym CHILD32=1 --defsym RESOURCES=4 --defsym PM32=1 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/dxbns32.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym CHILD32=1 --defsym RESOURCES=4 --defsym PM32=0 --defsym STACK32=1 -o $@ $<

$(addprefix $(BUILD)/test/,dxcb16.com dxcb32.com dxbp16.com dxbp32.com dxbs16.com dxbs32.com dxbn16.com dxbn32.com dxbns16.com dxbns32.com): %.com: %.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

dos-client-resources-test: $(addprefix $(BUILD)/test/,dxcnew.com dxsnew.com \
	dxcp16.com dxcp32.com dxcs16.com dxcs32.com dxsp16.com dxsp32.com dxss16.com dxss32.com)

$(BUILD)/test/dxcnew.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=3 --defsym RESOURCES=1 -o $@ $<
$(BUILD)/test/dxsnew.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=3 --defsym RESOURCES=2 -o $@ $<
$(BUILD)/test/dxcp16.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym RESOURCES=1 --defsym PM32=0 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/dxcp32.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym RESOURCES=1 --defsym PM32=1 --defsym STACK32=1 -o $@ $<
$(BUILD)/test/dxcs16.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym RESOURCES=1 --defsym PM32=1 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/dxcs32.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym RESOURCES=1 --defsym PM32=0 --defsym STACK32=1 -o $@ $<
$(BUILD)/test/dxsp16.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym RESOURCES=2 --defsym PM32=0 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/dxsp32.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym RESOURCES=2 --defsym PM32=1 --defsym STACK32=1 -o $@ $<
$(BUILD)/test/dxss16.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym RESOURCES=2 --defsym PM32=1 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/dxss32.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym RESOURCES=2 --defsym PM32=0 --defsym STACK32=1 -o $@ $<

$(addprefix $(BUILD)/test/,dxcnew.com dxsnew.com dxcp16.com dxcp32.com dxcs16.com dxcs32.com dxsp16.com dxsp32.com dxss16.com dxss32.com): %.com: %.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

.PHONY: dos-client-resources32-test
dos-client-resources32-test: $(addprefix $(BUILD)/test/,dxc32.com dxs32.com \
	dxcn16.com dxcn32.com dxcns16.com dxcns32.com dxsn16.com dxsn32.com dxsns16.com dxsns32.com)

$(BUILD)/test/dxc32.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=3 --defsym CHILD32=1 --defsym RESOURCES=1 -o $@ $<
$(BUILD)/test/dxs32.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=3 --defsym CHILD32=1 --defsym RESOURCES=2 -o $@ $<
$(BUILD)/test/dxcn16.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym CHILD32=1 --defsym RESOURCES=1 --defsym PM32=0 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/dxcn32.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym CHILD32=1 --defsym RESOURCES=1 --defsym PM32=1 --defsym STACK32=1 -o $@ $<
$(BUILD)/test/dxcns16.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym CHILD32=1 --defsym RESOURCES=1 --defsym PM32=1 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/dxcns32.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym CHILD32=1 --defsym RESOURCES=1 --defsym PM32=0 --defsym STACK32=1 -o $@ $<
$(BUILD)/test/dxsn16.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym CHILD32=1 --defsym RESOURCES=2 --defsym PM32=0 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/dxsn32.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym CHILD32=1 --defsym RESOURCES=2 --defsym PM32=1 --defsym STACK32=1 -o $@ $<
$(BUILD)/test/dxsns16.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym CHILD32=1 --defsym RESOURCES=2 --defsym PM32=1 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/dxsns32.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym CHILD32=1 --defsym RESOURCES=2 --defsym PM32=0 --defsym STACK32=1 -o $@ $<

$(addprefix $(BUILD)/test/,dxc32.com dxs32.com dxcn16.com dxcn32.com dxcns16.com dxcns32.com dxsn16.com dxsn32.com dxsns16.com dxsns32.com): %.com: %.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

.PHONY: dos-client-real-stacks-test
dos-client-real-stacks-test: $(addprefix $(BUILD)/test/,dxrmc16.com dxrmc32.com \
	dxrmp16.com dxrmp32.com dxrms16.com dxrms32.com dxrmn16.com dxrmn32.com dxrmns16.com dxrmns32.com)

$(BUILD)/test/dxrmc16.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=3 --defsym RESOURCES=3 -o $@ $<
$(BUILD)/test/dxrmc32.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=3 --defsym CHILD32=1 --defsym RESOURCES=3 -o $@ $<
$(BUILD)/test/dxrmp16.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym RESOURCES=3 --defsym PM32=0 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/dxrmp32.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym RESOURCES=3 --defsym PM32=1 --defsym STACK32=1 -o $@ $<
$(BUILD)/test/dxrms16.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym RESOURCES=3 --defsym PM32=1 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/dxrms32.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym RESOURCES=3 --defsym PM32=0 --defsym STACK32=1 -o $@ $<
$(BUILD)/test/dxrmn16.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym CHILD32=1 --defsym RESOURCES=3 --defsym PM32=0 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/dxrmn32.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym CHILD32=1 --defsym RESOURCES=3 --defsym PM32=1 --defsym STACK32=1 -o $@ $<
$(BUILD)/test/dxrmns16.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym CHILD32=1 --defsym RESOURCES=3 --defsym PM32=1 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/dxrmns32.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=0 --defsym CHILD32=1 --defsym RESOURCES=3 --defsym PM32=0 --defsym STACK32=1 -o $@ $<

$(addprefix $(BUILD)/test/,dxrmc16.com dxrmc32.com dxrmp16.com dxrmp32.com dxrms16.com dxrms32.com dxrmn16.com dxrmn32.com dxrmns16.com dxrmns32.com): %.com: %.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

dos-exec-primary-test: $(BUILD)/test/dpxp16.com $(BUILD)/test/dpxp32.com \
	$(BUILD)/test/dpxps16.com $(BUILD)/test/dpxps32.com $(BUILD)/test/dpxreal.com \
	$(BUILD)/test/dpxleaf.com $(BUILD)/test/dpxnew.com

$(BUILD)/test/dpxp16.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym PM32=0 --defsym STACK32=0 --defsym CHILD=0 -o $@ $<

$(BUILD)/test/dpxp32.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=1 --defsym CHILD=0 -o $@ $<

$(BUILD)/test/dpxps16.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=0 --defsym CHILD=0 -o $@ $<

$(BUILD)/test/dpxps32.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym PM32=0 --defsym STACK32=1 --defsym CHILD=0 -o $@ $<

$(BUILD)/test/dpxreal.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=1 -o $@ $<

$(BUILD)/test/dpxleaf.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=2 -o $@ $<

$(BUILD)/test/dpxnew.o: test/dpmi_exec_primary.S | $(BUILD)/test
	as --32 --defsym CHILD=3 -o $@ $<

$(addprefix $(BUILD)/test/,dpxp16.com dpxp32.com dpxps16.com dpxps32.com dpxreal.com dpxleaf.com dpxnew.com): %.com: %.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

dos-extended-exception-test: $(BUILD)/test/dpee16.com $(BUILD)/test/dpee32.com \
	$(BUILD)/test/dpees16.com $(BUILD)/test/dpees32.com

.PHONY: dos-decode-test
dos-decode-test: $(addprefix $(BUILD)/test/,dpdec16.com dpdec32.com dpdecs16.com dpdecs32.com)

$(BUILD)/test/dpdec16.o: test/dpmi_decode.S | $(BUILD)/test
	as --32 --defsym PM32=0 --defsym STACK32=0 -o $@ $<

$(BUILD)/test/dpdec32.o: test/dpmi_decode.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=1 -o $@ $<

$(BUILD)/test/dpdecs16.o: test/dpmi_decode.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=0 -o $@ $<

$(BUILD)/test/dpdecs32.o: test/dpmi_decode.S | $(BUILD)/test
	as --32 --defsym PM32=0 --defsym STACK32=1 -o $@ $<

$(addprefix $(BUILD)/test/,dpdec16.com dpdec32.com dpdecs16.com dpdecs32.com): %.com: %.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

.PHONY: dos-native-irq-test
.PHONY: dos-a20-test
.PHONY: dos-xms-test
.PHONY: dos-umb-test
dos-umb-test: $(BUILD)/test/umblink.com
$(BUILD)/test/umblink.o: test/dos_umb.S | $(BUILD)/test
	as --32 -o $@ $<
$(BUILD)/test/umblink.com: $(BUILD)/test/umblink.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<
	test "$$(wc -c < $@)" -le 3584

dos-xms-test: $(addprefix $(BUILD)/test/,xmsrm.com xmsp16.com xmsp32.com xmsps16.com)

$(BUILD)/test/xmsrm.o: test/dos_xms.S | $(BUILD)/test
	as --32 --defsym MODE=0 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/xmsp16.o: test/dos_xms.S | $(BUILD)/test
	as --32 --defsym MODE=1 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/xmsp32.o: test/dos_xms.S | $(BUILD)/test
	as --32 --defsym MODE=2 --defsym STACK32=1 -o $@ $<
$(BUILD)/test/xmsps16.o: test/dos_xms.S | $(BUILD)/test
	as --32 --defsym MODE=2 --defsym STACK32=0 -o $@ $<
$(addprefix $(BUILD)/test/,xmsrm.com xmsp16.com xmsp32.com xmsps16.com): %.com: %.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

dos-a20-test: $(addprefix $(BUILD)/test/,a20rm.com a20p16.com a20p32.com a20ps16.com)

$(BUILD)/test/a20rm.o: test/dos_a20.S | $(BUILD)/test
	as --32 --defsym MODE=0 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/a20p16.o: test/dos_a20.S | $(BUILD)/test
	as --32 --defsym MODE=1 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/a20p32.o: test/dos_a20.S | $(BUILD)/test
	as --32 --defsym MODE=2 --defsym STACK32=1 -o $@ $<
$(BUILD)/test/a20ps16.o: test/dos_a20.S | $(BUILD)/test
	as --32 --defsym MODE=2 --defsym STACK32=0 -o $@ $<
$(addprefix $(BUILD)/test/,a20rm.com a20p16.com a20p32.com a20ps16.com): %.com: %.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

dos-native-irq-test: $(addprefix $(BUILD)/test/,dpirq32.com dpirqs16.com dpirqh32.com dpirqhs.com dpirqd32.com dpirqds.com dpirqoff.com)

$(BUILD)/test/dpirq32.o: test/dpmi_native_irq.S | $(BUILD)/test
	as --32 --defsym STACK32=1 -o $@ $<

$(BUILD)/test/dpirqs16.o: test/dpmi_native_irq.S | $(BUILD)/test
	as --32 --defsym STACK32=0 -o $@ $<

$(BUILD)/test/dpirqh32.o: test/dpmi_native_irq.S | $(BUILD)/test
	as --32 --defsym STACK32=1 --defsym HARDWARE=1 -o $@ $<

$(BUILD)/test/dpirqhs.o: test/dpmi_native_irq.S | $(BUILD)/test
	as --32 --defsym STACK32=0 --defsym HARDWARE=1 -o $@ $<

$(BUILD)/test/dpirqd32.o: test/dpmi_native_irq.S | $(BUILD)/test
	as --32 --defsym STACK32=1 --defsym HARDWARE=1 --defsym DIAGNOSTIC=1 -o $@ $<

$(BUILD)/test/dpirqds.o: test/dpmi_native_irq.S | $(BUILD)/test
	as --32 --defsym STACK32=0 --defsym HARDWARE=1 --defsym DIAGNOSTIC=1 -o $@ $<

$(BUILD)/test/dpirqoff.o: test/dpmi_native_irq.S | $(BUILD)/test
	as --32 --defsym STACK32=0 --defsym HARDWARE=1 --defsym DIAGNOSTIC=1 --defsym MASK_IRQ=1 -o $@ $<

$(addprefix $(BUILD)/test/,dpirq32.com dpirqs16.com dpirqh32.com dpirqhs.com dpirqd32.com dpirqds.com dpirqoff.com): %.com: %.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

.PHONY: dos-x87-routing-test
.PHONY: dos-rom-ownership-test
dos-rom-ownership-test: $(BUILD)/test/dprom16o.com $(BUILD)/test/dprom32o.com
$(BUILD)/test/dprom16o.o: test/dpmi_rom_owner.S | $(BUILD)/test
	as --32 --defsym PM32=0 -o $@ $<
$(BUILD)/test/dprom32o.o: test/dpmi_rom_owner.S | $(BUILD)/test
	as --32 --defsym PM32=1 -o $@ $<
$(BUILD)/test/dprom16o.com $(BUILD)/test/dprom32o.com: %.com: %.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

dos-x87-routing-test: $(BUILD)/test/dpmf16.com $(BUILD)/test/dpmf32.com

.PHONY: dos-x87-test
dos-x87-test: $(addprefix $(BUILD)/test/,dofprm.com dofp16.com dofp32.com dofpe32.com)

$(BUILD)/test/dofprm.o: test/dos_x87.S | $(BUILD)/test
	as --32 --defsym REAL=1 --defsym PM32=0 -o $@ $<
$(BUILD)/test/dofp16.o: test/dos_x87.S | $(BUILD)/test
	as --32 --defsym REAL=0 --defsym PM32=0 -o $@ $<
$(BUILD)/test/dofp32.o: test/dos_x87.S | $(BUILD)/test
	as --32 --defsym REAL=0 --defsym PM32=1 -o $@ $<
$(BUILD)/test/dofpe32.o: test/dos_x87_edges.S | $(BUILD)/test
	as --32 -o $@ $<
$(addprefix $(BUILD)/test/,dofprm.com dofp16.com dofp32.com dofpe32.com): %.com: %.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

.PHONY: dos-fp-state-test
.PHONY: dos-x87-diagnostic-test
dos-fp-state-test: dos-x87-diagnostic-test
dos-x87-diagnostic-test: $(addprefix $(BUILD)/test/,dpxd16.com dpxd32.com dpxdp16.com dpxdp32.com dpxdc.com)

$(BUILD)/test/dpxd16.o: test/dpmi_x87_diagnostic.S | $(BUILD)/test
	as --32 --defsym STACK32=0 --defsym PENDING=0 -o $@ $<
$(BUILD)/test/dpxd32.o: test/dpmi_x87_diagnostic.S | $(BUILD)/test
	as --32 --defsym STACK32=1 --defsym PENDING=0 -o $@ $<
$(BUILD)/test/dpxdp16.o: test/dpmi_x87_diagnostic.S | $(BUILD)/test
	as --32 --defsym STACK32=0 --defsym PENDING=1 -o $@ $<
$(BUILD)/test/dpxdp32.o: test/dpmi_x87_diagnostic.S | $(BUILD)/test
	as --32 --defsym STACK32=1 --defsym PENDING=1 -o $@ $<
$(BUILD)/test/dpxdc.o: test/dpmi_x87_diagnostic.S | $(BUILD)/test
	as --32 --defsym CHILD_ONLY=1 -o $@ $<
$(addprefix $(BUILD)/test/,dpxd16.com dpxd32.com dpxdp16.com dpxdp32.com dpxdc.com): %.com: %.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/dos/dos_int_stub.o: dos/dos_native_fp.h
$(BUILD)/dos/dos_fpu_irq_test.o: dos/dos_native_fp.h
$(BUILD)/kernel/isr_stubs.o: dos/dos_native_fp.h

dos-fp-state-test: $(addprefix $(BUILD)/test/,dpfp16.com dpfp32.com dpfpr16.com dpfpr32.com dpfpc32.com dpfm16.com dpfm32.com dpfmi32.com)
dos-fp-state-test: $(addprefix $(BUILD)/test/,dpcp16.com dpcp32.com dpcpc32.com)
dos-fp-state-test: $(addprefix $(BUILD)/test/,dpfd16.com dpfd32.com dpfdr16.com dpfdr32.com)

$(BUILD)/test/dpcp16.o: test/dpmi_coprocessor.S | $(BUILD)/test
	as --32 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/dpcp32.o: test/dpmi_coprocessor.S | $(BUILD)/test
	as --32 --defsym STACK32=1 -o $@ $<
$(BUILD)/test/dpcpc32.o: test/dpmi_coprocessor.S | $(BUILD)/test
	as --32 --defsym STACK32=1 --defsym CHILD_ONLY=1 -o $@ $<
$(addprefix $(BUILD)/test/,dpcp16.com dpcp32.com dpcpc32.com): %.com: %.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpfm16.o: test/dpmi_fp_memory.S | $(BUILD)/test
	as --32 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/dpfm32.o: test/dpmi_fp_memory.S | $(BUILD)/test
	as --32 --defsym STACK32=1 -o $@ $<
$(BUILD)/test/dpfmi32.o: test/dpmi_fp_memory.S | $(BUILD)/test
	as --32 --defsym STACK32=1 --defsym INTERPRETED=1 -o $@ $<
$(addprefix $(BUILD)/test/,dpfm16.com dpfm32.com dpfmi32.com): %.com: %.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpfpc32.o: test/dpmi_fp_state.S | $(BUILD)/test
	as --32 --defsym STACK32=1 --defsym RAW_ONLY=0 --defsym CHILD_ONLY=1 -o $@ $<

$(BUILD)/test/dpfp16.o: test/dpmi_fp_state.S | $(BUILD)/test
	as --32 --defsym STACK32=0 --defsym RAW_ONLY=0 -o $@ $<

$(BUILD)/test/dpfp32.o: test/dpmi_fp_state.S | $(BUILD)/test
	as --32 --defsym STACK32=1 --defsym RAW_ONLY=0 -o $@ $<

$(BUILD)/test/dpfpr16.o: test/dpmi_fp_state.S | $(BUILD)/test
	as --32 --defsym STACK32=0 --defsym RAW_ONLY=1 -o $@ $<

$(BUILD)/test/dpfpr32.o: test/dpmi_fp_state.S | $(BUILD)/test
	as --32 --defsym STACK32=1 --defsym RAW_ONLY=1 -o $@ $<

$(BUILD)/test/dpfd16.o: test/dpmi_fp_state.S | $(BUILD)/test
	as --32 --defsym STACK32=0 --defsym RAW_ONLY=0 --defsym DIAGNOSTIC_STATE=1 -o $@ $<
$(BUILD)/test/dpfd32.o: test/dpmi_fp_state.S | $(BUILD)/test
	as --32 --defsym STACK32=1 --defsym RAW_ONLY=0 --defsym DIAGNOSTIC_STATE=1 -o $@ $<
$(BUILD)/test/dpfdr16.o: test/dpmi_fp_state.S | $(BUILD)/test
	as --32 --defsym STACK32=0 --defsym RAW_ONLY=1 --defsym DIAGNOSTIC_STATE=1 -o $@ $<
$(BUILD)/test/dpfdr32.o: test/dpmi_fp_state.S | $(BUILD)/test
	as --32 --defsym STACK32=1 --defsym RAW_ONLY=1 --defsym DIAGNOSTIC_STATE=1 -o $@ $<
$(addprefix $(BUILD)/test/,dpfd16.com dpfd32.com dpfdr16.com dpfdr32.com): %.com: %.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(addprefix $(BUILD)/test/,dpfp16.com dpfp32.com dpfpr16.com dpfpr32.com dpfpc32.com): %.com: %.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmf16.o: test/dpmi_x87_route.S | $(BUILD)/test
	as --32 --defsym STACK32=0 -o $@ $<

$(BUILD)/test/dpmf32.o: test/dpmi_x87_route.S | $(BUILD)/test
	as --32 --defsym STACK32=1 -o $@ $<

$(addprefix $(BUILD)/test/,dpmf16.com dpmf32.com): %.com: %.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

dos-debug-test: $(BUILD)/test/dpdebug16.com $(BUILD)/test/dpdebug32.com \
	$(BUILD)/test/dpdr16.com $(BUILD)/test/dpdr32.com \
	$(BUILD)/test/dpdrs16.com $(BUILD)/test/dpdrs32.com \
	$(BUILD)/test/dpst16.com $(BUILD)/test/dpst32.com \
	$(BUILD)/test/dpsts16.com $(BUILD)/test/dpsts32.com \
	$(BUILD)/test/dpsth32.com $(BUILD)/test/dpsths16.com \
	$(BUILD)/test/dpsti32.com $(BUILD)/test/dpstis16.com

$(BUILD)/test/dpsti32.o: test/dpmi_step.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=1 --defsym IRET_HANDOFF=1 -o $@ $<

$(BUILD)/test/dpstis16.o: test/dpmi_step.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=0 --defsym IRET_HANDOFF=1 -o $@ $<

$(BUILD)/test/dpsth32.o: test/dpmi_step.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=1 --defsym HANDOFF=1 -o $@ $<

$(BUILD)/test/dpsths16.o: test/dpmi_step.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=0 --defsym HANDOFF=1 -o $@ $<

$(BUILD)/test/dpst16.o: test/dpmi_step.S | $(BUILD)/test
	as --32 --defsym PM32=0 --defsym STACK32=0 -o $@ $<

$(BUILD)/test/dpst32.o: test/dpmi_step.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=1 -o $@ $<

$(BUILD)/test/dpsts16.o: test/dpmi_step.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=0 -o $@ $<

$(BUILD)/test/dpsts32.o: test/dpmi_step.S | $(BUILD)/test
	as --32 --defsym PM32=0 --defsym STACK32=1 -o $@ $<

$(addprefix $(BUILD)/test/,dpst16.com dpst32.com dpsts16.com dpsts32.com dpsth32.com dpsths16.com dpsti32.com dpstis16.com): %.com: %.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpdr16.o: test/dpmi_debug_route.S | $(BUILD)/test
	as --32 --defsym PM32=0 --defsym STACK32=0 -o $@ $<

$(BUILD)/test/dpdr32.o: test/dpmi_debug_route.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=1 -o $@ $<

$(BUILD)/test/dpdrs16.o: test/dpmi_debug_route.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=0 -o $@ $<

$(BUILD)/test/dpdrs32.o: test/dpmi_debug_route.S | $(BUILD)/test
	as --32 --defsym PM32=0 --defsym STACK32=1 -o $@ $<

$(addprefix $(BUILD)/test/,dpdr16.com dpdr32.com dpdrs16.com dpdrs32.com): %.com: %.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

dos-string-io-test: $(BUILD)/test/dpstr16.com $(BUILD)/test/dpstr32.com

dos-exception-test: $(BUILD)/test/dpex16.com $(BUILD)/test/dpex32.com \
	$(BUILD)/test/dpxf16.com $(BUILD)/test/dpxf32.com $(BUILD)/test/dpfault.com \
	$(BUILD)/test/dpfalt32.com $(BUILD)/test/dpff16.com $(BUILD)/test/dpff32.com \
	$(BUILD)/test/dplock16.com $(BUILD)/test/dplock32.com \
	$(BUILD)/test/dpsw16.com $(BUILD)/test/dpsw32.com $(BUILD)/test/dpswn16.com $(BUILD)/test/dpswn32.com \
	$(BUILD)/test/dpstate16.com $(BUILD)/test/dpstate32.com \
	$(BUILD)/test/dpcbuf16.com $(BUILD)/test/dpcbuf32.com

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

$(BUILD)/test/dpvgamem.com: $(BUILD)/test/dpmi_vga_memory.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_vga_memory.o: test/dpmi_vga_memory.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/dpmi_audio.com: $(BUILD)/test/dpmi_audio.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_audio.o: test/dpmi_audio.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/dpflags16.com: $(BUILD)/test/dpmi_flags16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpflags32.com: $(BUILD)/test/dpmi_flags32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_flags16.o: test/dpmi_flags.S | $(BUILD)/test
	as --32 --defsym PM32=0 -o $@ $<

$(BUILD)/test/dpmi_flags32.o: test/dpmi_flags.S | $(BUILD)/test
	as --32 --defsym PM32=1 -o $@ $<

$(BUILD)/test/dpnif16.o: test/dpmi_native_if.S | $(BUILD)/test
	as --32 --defsym STACK32=0 -o $@ $<
$(BUILD)/test/dpnif32.o: test/dpmi_native_if.S | $(BUILD)/test
	as --32 --defsym STACK32=1 -o $@ $<
$(addprefix $(BUILD)/test/,dpnif16.com dpnif32.com): %.com: %.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpdebug16.com: $(BUILD)/test/dpmi_debug16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpdebug32.com: $(BUILD)/test/dpmi_debug32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_debug16.o: test/dpmi_debug.S | $(BUILD)/test
	as --32 --defsym PM32=0 -o $@ $<

$(BUILD)/test/dpmi_debug32.o: test/dpmi_debug.S | $(BUILD)/test
	as --32 --defsym PM32=1 -o $@ $<

$(BUILD)/test/dpstr16.com: $(BUILD)/test/dpmi_string_io16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpstr32.com: $(BUILD)/test/dpmi_string_io32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_string_io16.o: test/dpmi_string_io.S | $(BUILD)/test
	as --32 --defsym PM32=0 -o $@ $<

$(BUILD)/test/dpmi_string_io32.o: test/dpmi_string_io.S | $(BUILD)/test
	as --32 --defsym PM32=1 -o $@ $<

$(BUILD)/test/dpex16.com: $(BUILD)/test/dpmi_exception_defaults16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpex32.com: $(BUILD)/test/dpmi_exception_defaults32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_exception_defaults16.o: test/dpmi_exception_defaults.S | $(BUILD)/test
	as --32 --defsym PM32=0 --defsym FATAL_CHAIN=0 -o $@ $<

$(BUILD)/test/dpmi_exception_defaults32.o: test/dpmi_exception_defaults.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym FATAL_CHAIN=0 -o $@ $<

$(BUILD)/test/dpxf16.com: $(BUILD)/test/dpmi_exception_fatal16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpxf32.com: $(BUILD)/test/dpmi_exception_fatal32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_exception_fatal16.o: test/dpmi_exception_defaults.S | $(BUILD)/test
	as --32 --defsym PM32=0 --defsym FATAL_CHAIN=1 -o $@ $<

$(BUILD)/test/dpmi_exception_fatal32.o: test/dpmi_exception_defaults.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym FATAL_CHAIN=1 -o $@ $<

$(BUILD)/test/dpfault.com: $(BUILD)/test/dpmi_native_faults.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dplock16.com: $(BUILD)/test/dpmi_locked_stack16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dplock32.com: $(BUILD)/test/dpmi_locked_stack32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_locked_stack16.o: test/dpmi_locked_stack.S | $(BUILD)/test
	as --32 --defsym PM32=0 -o $@ $<

$(BUILD)/test/dpmi_locked_stack32.o: test/dpmi_locked_stack.S | $(BUILD)/test
	as --32 --defsym PM32=1 -o $@ $<

$(BUILD)/test/dpsw16.com: $(BUILD)/test/dpmi_software_frame16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpsw32.com: $(BUILD)/test/dpmi_software_frame32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpswn16.com: $(BUILD)/test/dpmi_software_frame_native16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpswn32.com: $(BUILD)/test/dpmi_software_frame_native32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_software_frame16.o: test/dpmi_software_frame.S | $(BUILD)/test
	as --32 --defsym PM32=0 --defsym STACK32=0 --defsym NATIVE_GATE=0 -o $@ $<

$(BUILD)/test/dpmi_software_frame32.o: test/dpmi_software_frame.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=1 --defsym NATIVE_GATE=0 -o $@ $<

$(BUILD)/test/dpmi_software_frame_native16.o: test/dpmi_software_frame.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=0 --defsym NATIVE_GATE=1 -o $@ $<

$(BUILD)/test/dpmi_software_frame_native32.o: test/dpmi_software_frame.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=1 --defsym NATIVE_GATE=1 -o $@ $<

$(BUILD)/test/dpsb16.com: $(BUILD)/test/dpmi_service_buffer16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dprs16.com: $(BUILD)/test/dpmi_record_service16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpsr16.com: $(BUILD)/test/dpmi_state_recovery16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpcr16.com: $(BUILD)/test/dpmi_callback_recovery16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dprg16.com: $(BUILD)/test/dpmi_callback_register16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dprg32.com: $(BUILD)/test/dpmi_callback_register32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dprgs16.com: $(BUILD)/test/dpmi_callback_register_stack16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dprgs32.com: $(BUILD)/test/dpmi_callback_register_stack32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_callback_register16.o: test/dpmi_callback_register.S | $(BUILD)/test
	as --32 --defsym PM32=0 --defsym STACK32=0 -o $@ $<

$(BUILD)/test/dpmi_callback_register32.o: test/dpmi_callback_register.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=1 -o $@ $<

$(BUILD)/test/dpmi_callback_register_stack16.o: test/dpmi_callback_register.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=0 -o $@ $<

$(BUILD)/test/dpmi_callback_register_stack32.o: test/dpmi_callback_register.S | $(BUILD)/test
	as --32 --defsym PM32=0 --defsym STACK32=1 -o $@ $<

$(BUILD)/test/dprw16.com: $(BUILD)/test/dpmi_raw_entry16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dprw32.com: $(BUILD)/test/dpmi_raw_entry32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dprws16.com: $(BUILD)/test/dpmi_raw_entry_stack16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dprws32.com: $(BUILD)/test/dpmi_raw_entry_stack32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_raw_entry16.o: test/dpmi_raw_entry.S | $(BUILD)/test
	as --32 --defsym PM32=0 --defsym STACK32=0 -o $@ $<

$(BUILD)/test/dpmi_raw_entry32.o: test/dpmi_raw_entry.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=1 -o $@ $<

$(BUILD)/test/dpmi_raw_entry_stack16.o: test/dpmi_raw_entry.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=0 -o $@ $<

$(BUILD)/test/dpmi_raw_entry_stack32.o: test/dpmi_raw_entry.S | $(BUILD)/test
	as --32 --defsym PM32=0 --defsym STACK32=1 -o $@ $<

$(BUILD)/test/dpce16.com: $(BUILD)/test/dpmi_callback_entry16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpce32.com: $(BUILD)/test/dpmi_callback_entry32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpces16.com: $(BUILD)/test/dpmi_callback_entry_stack16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpces32.com: $(BUILD)/test/dpmi_callback_entry_stack32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_callback_entry16.o: test/dpmi_callback_entry.S | $(BUILD)/test
	as --32 --defsym PM32=0 --defsym STACK32=0 -o $@ $<

$(BUILD)/test/dpmi_callback_entry32.o: test/dpmi_callback_entry.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=1 -o $@ $<

$(BUILD)/test/dpmi_callback_entry_stack16.o: test/dpmi_callback_entry.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=0 -o $@ $<

$(BUILD)/test/dpmi_callback_entry_stack32.o: test/dpmi_callback_entry.S | $(BUILD)/test
	as --32 --defsym PM32=0 --defsym STACK32=1 -o $@ $<

$(BUILD)/test/dpre16.com: $(BUILD)/test/dpmi_real_exception16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpre32.com: $(BUILD)/test/dpmi_real_exception32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpres16.com: $(BUILD)/test/dpmi_real_exception_stack16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpres32.com: $(BUILD)/test/dpmi_real_exception_stack32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_real_exception16.o: test/dpmi_real_exception.S | $(BUILD)/test
	as --32 --defsym PM32=0 --defsym STACK32=0 -o $@ $<

$(BUILD)/test/dpmi_real_exception32.o: test/dpmi_real_exception.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=1 -o $@ $<

$(BUILD)/test/dpmi_real_exception_stack16.o: test/dpmi_real_exception.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=0 -o $@ $<

$(BUILD)/test/dpmi_real_exception_stack32.o: test/dpmi_real_exception.S | $(BUILD)/test
	as --32 --defsym PM32=0 --defsym STACK32=1 -o $@ $<

$(BUILD)/test/dpee16.com: $(BUILD)/test/dpmi_extended_exception16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpee32.com: $(BUILD)/test/dpmi_extended_exception32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpees16.com: $(BUILD)/test/dpmi_extended_exception_stack16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpees32.com: $(BUILD)/test/dpmi_extended_exception_stack32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_extended_exception16.o: test/dpmi_extended_exception.S | $(BUILD)/test
	as --32 --defsym PM32=0 --defsym STACK32=0 -o $@ $<

$(BUILD)/test/dpmi_extended_exception32.o: test/dpmi_extended_exception.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=1 -o $@ $<

$(BUILD)/test/dpmi_extended_exception_stack16.o: test/dpmi_extended_exception.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=0 -o $@ $<

$(BUILD)/test/dpmi_extended_exception_stack32.o: test/dpmi_extended_exception.S | $(BUILD)/test
	as --32 --defsym PM32=0 --defsym STACK32=1 -o $@ $<

$(BUILD)/test/dpcr32.com: $(BUILD)/test/dpmi_callback_recovery32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpcrs16.com: $(BUILD)/test/dpmi_callback_recovery_stack16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpcrs32.com: $(BUILD)/test/dpmi_callback_recovery_stack32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_callback_recovery16.o: test/dpmi_callback_recovery.S | $(BUILD)/test
	as --32 --defsym PM32=0 --defsym STACK32=0 -o $@ $<

$(BUILD)/test/dpmi_callback_recovery32.o: test/dpmi_callback_recovery.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=1 -o $@ $<

$(BUILD)/test/dpmi_callback_recovery_stack16.o: test/dpmi_callback_recovery.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=0 -o $@ $<

$(BUILD)/test/dpmi_callback_recovery_stack32.o: test/dpmi_callback_recovery.S | $(BUILD)/test
	as --32 --defsym PM32=0 --defsym STACK32=1 -o $@ $<

$(BUILD)/test/dpsr32.com: $(BUILD)/test/dpmi_state_recovery32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpsrs16.com: $(BUILD)/test/dpmi_state_recovery_stack16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpsrs32.com: $(BUILD)/test/dpmi_state_recovery_stack32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_state_recovery16.o: test/dpmi_state_recovery.S | $(BUILD)/test
	as --32 --defsym PM32=0 --defsym STACK32=0 -o $@ $<

$(BUILD)/test/dpmi_state_recovery32.o: test/dpmi_state_recovery.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=1 -o $@ $<

$(BUILD)/test/dpmi_state_recovery_stack16.o: test/dpmi_state_recovery.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=0 -o $@ $<

$(BUILD)/test/dpmi_state_recovery_stack32.o: test/dpmi_state_recovery.S | $(BUILD)/test
	as --32 --defsym PM32=0 --defsym STACK32=1 -o $@ $<

$(BUILD)/test/dprs32.com: $(BUILD)/test/dpmi_record_service32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dprss16.com: $(BUILD)/test/dpmi_record_service_stack16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dprss32.com: $(BUILD)/test/dpmi_record_service_stack32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_record_service16.o: test/dpmi_record_service.S | $(BUILD)/test
	as --32 --defsym PM32=0 --defsym STACK32=0 -o $@ $<

$(BUILD)/test/dpmi_record_service32.o: test/dpmi_record_service.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=1 -o $@ $<

$(BUILD)/test/dpmi_record_service_stack16.o: test/dpmi_record_service.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=0 -o $@ $<

$(BUILD)/test/dpmi_record_service_stack32.o: test/dpmi_record_service.S | $(BUILD)/test
	as --32 --defsym PM32=0 --defsym STACK32=1 -o $@ $<

$(BUILD)/test/dprec16.com: $(BUILD)/test/dpmi_io_recovery16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dprec32.com: $(BUILD)/test/dpmi_io_recovery32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dprecs16.com: $(BUILD)/test/dpmi_io_recovery_stack16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dprecs32.com: $(BUILD)/test/dpmi_io_recovery_stack32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_io_recovery16.o: test/dpmi_io_recovery.S | $(BUILD)/test
	as --32 --defsym PM32=0 --defsym STACK32=0 -o $@ $<

$(BUILD)/test/dpmi_io_recovery32.o: test/dpmi_io_recovery.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=1 -o $@ $<

$(BUILD)/test/dpmi_io_recovery_stack16.o: test/dpmi_io_recovery.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=0 -o $@ $<

$(BUILD)/test/dpmi_io_recovery_stack32.o: test/dpmi_io_recovery.S | $(BUILD)/test
	as --32 --defsym PM32=0 --defsym STACK32=1 -o $@ $<

$(BUILD)/test/dpsb32.com: $(BUILD)/test/dpmi_service_buffer32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpsbs16.com: $(BUILD)/test/dpmi_service_buffer_stack16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpsbs32.com: $(BUILD)/test/dpmi_service_buffer_stack32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_service_buffer16.o: test/dpmi_service_buffer.S | $(BUILD)/test
	as --32 --defsym PM32=0 --defsym STACK32=0 -o $@ $<

$(BUILD)/test/dpmi_service_buffer32.o: test/dpmi_service_buffer.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=1 -o $@ $<

$(BUILD)/test/dpmi_service_buffer_stack16.o: test/dpmi_service_buffer.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=0 -o $@ $<

$(BUILD)/test/dpmi_service_buffer_stack32.o: test/dpmi_service_buffer.S | $(BUILD)/test
	as --32 --defsym PM32=0 --defsym STACK32=1 -o $@ $<

$(BUILD)/test/dpsbnp32.com: $(BUILD)/test/dpmi_service_buffer_loaded_np.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_service_buffer_loaded_np.o: test/dpmi_service_buffer.S | $(BUILD)/test
	as --32 --defsym PM32=1 --defsym STACK32=1 --defsym LOADED_NP=1 -o $@ $<

$(BUILD)/test/dpnseg16.com: $(BUILD)/test/dpmi_native_segments16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpnseg32.com: $(BUILD)/test/dpmi_native_segments32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_native_segments16.o: test/dpmi_native_segments.S | $(BUILD)/test
	as --32 --defsym STACK32=0 -o $@ $<

$(BUILD)/test/dpmi_native_segments32.o: test/dpmi_native_segments.S | $(BUILD)/test
	as --32 --defsym STACK32=1 -o $@ $<

$(BUILD)/test/dpstate16.com: $(BUILD)/test/dpmi_mode_state16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpstate32.com: $(BUILD)/test/dpmi_mode_state32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_mode_state16.o: test/dpmi_mode_state.S | $(BUILD)/test
	as --32 --defsym PM32=0 -o $@ $<

$(BUILD)/test/dpmi_mode_state32.o: test/dpmi_mode_state.S | $(BUILD)/test
	as --32 --defsym PM32=1 -o $@ $<

$(BUILD)/test/dpcbuf16.com: $(BUILD)/test/dpmi_callback_buffer16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpcbuf32.com: $(BUILD)/test/dpmi_callback_buffer32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_callback_buffer16.o: test/dpmi_callback_buffer.S | $(BUILD)/test
	as --32 --defsym PM32=0 -o $@ $<

$(BUILD)/test/dpmi_callback_buffer32.o: test/dpmi_callback_buffer.S | $(BUILD)/test
	as --32 --defsym PM32=1 -o $@ $<

$(BUILD)/test/dpmi_native_faults.o: test/dpmi_native_faults.S | $(BUILD)/test
	as --32 --defsym STACK32=0 --defsym FATAL_DEFAULT=0 -o $@ $<

$(BUILD)/test/dpfalt32.com: $(BUILD)/test/dpmi_native_faults32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_native_faults32.o: test/dpmi_native_faults.S | $(BUILD)/test
	as --32 --defsym STACK32=1 --defsym FATAL_DEFAULT=0 -o $@ $<

$(BUILD)/test/dpff16.com: $(BUILD)/test/dpmi_native_faults_fatal16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_native_faults_fatal16.o: test/dpmi_native_faults.S | $(BUILD)/test
	as --32 --defsym STACK32=0 --defsym FATAL_DEFAULT=1 -o $@ $<

$(BUILD)/test/dpff32.com: $(BUILD)/test/dpmi_native_faults_fatal32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_native_faults_fatal32.o: test/dpmi_native_faults.S | $(BUILD)/test
	as --32 --defsym STACK32=1 --defsym FATAL_DEFAULT=1 -o $@ $<

$(BUILD)/test/dosvga.com: $(BUILD)/test/dos_vga_text.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dos_vga_text.o: test/dos_vga_text.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/doscon.com: $(BUILD)/test/dos_console_input.o \
		test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dos_console_input.o: test/dos_console_input.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/dosjit.com: $(BUILD)/test/dos_jit_contract.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dos_jit_contract.o: test/dos_jit_contract.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/dosivt.com: $(BUILD)/test/dos_ivt_contract.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dos_ivt_contract.o: test/dos_ivt_contract.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/dprom16.com: $(BUILD)/test/dpmi_rom16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dprom32.com: $(BUILD)/test/dpmi_rom32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_rom16.o: test/dpmi_rom_chain.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/dpmi_rom32.o: test/dpmi_rom_chain.S | $(BUILD)/test
	as --32 --defsym CLIENT32=1 -o $@ $<

$(BUILD)/test/dosbits.com: $(BUILD)/test/dos_bitops.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dos_bitops.o: test/dos_bitops.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/dospop.com: $(BUILD)/test/dos_pop_contract.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dos_pop_contract.o: test/dos_pop_contract.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/dosioctl.com: $(BUILD)/test/dos_ioctl.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dos_ioctl.o: test/dos_ioctl.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/iochild.com: $(BUILD)/test/dos_ioctl_child.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dos_ioctl_child.o: test/dos_ioctl_child.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/dstdio.com: $(BUILD)/test/dos_stdio.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dos_stdio.o: test/dos_stdio.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/dscchild.com: $(BUILD)/test/dos_stdio_child.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dosbreak.com: $(BUILD)/test/dos_break.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dos_break.o: test/dos_break.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/dbchild.com: $(BUILD)/test/dos_break_child.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dos_break_child.o: test/dos_break_child.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/doscrit.com: $(BUILD)/test/dos_critical.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dos_critical.o: test/dos_critical.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/dcchild.com: $(BUILD)/test/dos_critical_child.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dos_critical_child.o: test/dos_critical_child.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/dpcrit16.com: $(BUILD)/test/dpmi_critical16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_critical16.o: test/dpmi_critical.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/dpargs16.com: $(BUILD)/test/dpmi_rm_arguments16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_rm_arguments16.o: test/dpmi_rm_arguments.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/dpargs32.com: $(BUILD)/test/dpmi_rm_arguments32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_rm_arguments32.o: test/dpmi_rm_arguments.S | $(BUILD)/test
	as --32 --defsym CLIENT32=1 -o $@ $<

$(BUILD)/test/dpcrit32.com: $(BUILD)/test/dpmi_critical32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_critical32.o: test/dpmi_critical.S | $(BUILD)/test
	as --32 --defsym CLIENT32=1 -o $@ $<

$(BUILD)/test/dpio16.com: $(BUILD)/test/dpmi_file_io16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_file_io16.o: test/dpmi_file_io.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/dpio32.com: $(BUILD)/test/dpmi_file_io32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_file_io32.o: test/dpmi_file_io.S | $(BUILD)/test
	as --32 --defsym CLIENT32=1 -o $@ $<

.PHONY: dos-file-pressure-test
dos-file-pressure-test: $(BUILD)/test/dpfio16.com $(BUILD)/test/dpfio32.com \
	$(BUILD)/test/dpfi16f.com $(BUILD)/test/dpfi32f.com

$(BUILD)/test/dpfio16.com: $(BUILD)/test/dpfio16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpfio32.com: $(BUILD)/test/dpfio32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpfi16f.com: $(BUILD)/test/dpfi16f.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpfi32f.com: $(BUILD)/test/dpfi32f.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpfio16.o: test/dpmi_file_pressure.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/dpfio32.o: test/dpmi_file_pressure.S | $(BUILD)/test
	as --32 --defsym CLIENT32=1 -o $@ $<

$(BUILD)/test/dpfi16f.o: test/dpmi_file_pressure.S | $(BUILD)/test
	as --32 --defsym RELEASE_ARENA=1 -o $@ $<

$(BUILD)/test/dpfi32f.o: test/dpmi_file_pressure.S | $(BUILD)/test
	as --32 --defsym CLIENT32=1 --defsym RELEASE_ARENA=1 -o $@ $<

$(BUILD)/test/dpcon16.com: $(BUILD)/test/dpmi_console16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_console16.o: test/dpmi_console.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/dpcon32.com: $(BUILD)/test/dpmi_console32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_console32.o: test/dpmi_console.S | $(BUILD)/test
	as --32 --defsym CLIENT32=1 -o $@ $<

$(BUILD)/test/dpbreak16.com: $(BUILD)/test/dpmi_break16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_break16.o: test/dpmi_break.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/dpbreak32.com: $(BUILD)/test/dpmi_break32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dpmi_break32.o: test/dpmi_break.S | $(BUILD)/test
	as --32 --defsym CLIENT32=1 -o $@ $<

$(BUILD)/test/doskey.com: $(BUILD)/test/dos_keyboard_rm.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dos_keyboard_rm.o: test/dos_keyboard_irq.S | $(BUILD)/test
	as --32 --defsym REALMODE=1 -o $@ $<

$(BUILD)/test/dpkey16.com: $(BUILD)/test/dos_keyboard16.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dos_keyboard16.o: test/dos_keyboard_irq.S | $(BUILD)/test
	as --32 -o $@ $<

$(BUILD)/test/dpkey32.com: $(BUILD)/test/dos_keyboard32.o test/dpmi_native.ld
	ld -m elf_i386 -T test/dpmi_native.ld -o $@ $<

$(BUILD)/test/dos_keyboard32.o: test/dos_keyboard_irq.S | $(BUILD)/test
	as --32 --defsym CLIENT32=1 -o $@ $<

$(BUILD)/test/dos_stdio_child.o: test/dos_stdio_child.S | $(BUILD)/test
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

.PHONY: dos-tsr-test
dos-tsr-test: $(addprefix $(BUILD)/test/,tsrtest.com tsrload.com tsrc31.com tsrc27.com tsrrom.com tsrmid.com dexmz.exe)

$(BUILD)/test/tsrtest.o: test/dos_tsr.S | $(BUILD)/test
	as --32 --defsym LOAD_ONLY=0 -o $@ $<
$(BUILD)/test/tsrload.o: test/dos_tsr.S | $(BUILD)/test
	as --32 --defsym LOAD_ONLY=1 -o $@ $<
$(BUILD)/test/tsrc31.o: test/dos_tsr_child.S | $(BUILD)/test
	as --32 --defsym TSR_KIND=31 -o $@ $<
$(BUILD)/test/tsrc27.o: test/dos_tsr_child.S | $(BUILD)/test
	as --32 --defsym TSR_KIND=27 -o $@ $<
$(BUILD)/test/tsrrom.o: test/dos_tsr_child.S | $(BUILD)/test
	as --32 --defsym TSR_KIND=28 -o $@ $<
$(BUILD)/test/tsrmid.o: test/dos_tsr_middle.S | $(BUILD)/test
	as --32 -o $@ $<
$(addprefix $(BUILD)/test/,tsrtest.com tsrload.com tsrc31.com tsrc27.com tsrrom.com tsrmid.com): %.com: %.o test/dpmi_native.ld
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

$(BUILD)/boot_efi.o: boot/boot_efi.c boot/boot_paging.h include/boot_trace.h | $(BUILD)
	$(CC) $(BFLAGS) -c -o $@ $<

$(BUILD)/boot_efi_pxe.o: boot/boot_efi.c boot/boot_paging.h include/boot_trace.h | $(BUILD)
	$(CC) $(BFLAGS) -DOK_PXE_EMBED_KERNEL=1 -c -o $@ $<

$(BUILD)/boot_pxe_kernel.o: boot/kernel_embed.S $(BUILD)/kernel.elf | $(BUILD)
	$(CC) -c -o $@ $<

# ── kernel.elf (kernel binary) ─────────────────────────────────

$(BUILD)/kernel.elf: $(ALL_KERN_OBJ) kernel.ld
	$(LD) $(KLDFLAGS) -o $@ $(filter-out kernel.ld,$^)
	@echo "Built: $@ ($$(stat -c%s $@) bytes)"

# Kernel C files
$(BUILD)/lib/tinyssh/%.o: lib/tinyssh/%.c $(SSH_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(KCFLAGS) -mgeneral-regs-only -fwrapv -Wno-unused-parameter \
		-Ilib/tinyssh/port -Ilib/tinyssh/upstream -Ilib/tinyssh/upstream/cryptoint -c -o $@ $<

$(BUILD)/kernel/serial.o $(BUILD)/kernel/serial_tx.o: KCFLAGS += -mgeneral-regs-only
$(BUILD)/kernel/wpa2.o $(BUILD)/kernel/wpa2_crypto.o $(BUILD)/kernel/wpa2_rsn.o $(BUILD)/kernel/wpa2_selftest.o $(BUILD)/kernel/random_secure.o: KCFLAGS += -Ilib/bearssl/port -mgeneral-regs-only
$(BUILD)/win32/advapi32_shim.o: KCFLAGS += -Ilib/bearssl/port

$(BUILD)/lib/bearssl/%.o: lib/bearssl/%.c
	@mkdir -p $(@D)
	$(CC) $(KCFLAGS) -mgeneral-regs-only -DBR_LE_UNALIGNED=0 -DBR_BE_UNALIGNED=0 \
		-Ilib/bearssl/port -Ilib/bearssl/inc -Ilib/bearssl/src -c -o $@ $<

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
$(BUILD)/win32/math/%.o: win32/math/%.c | $(BUILD)/win32
	@mkdir -p $(@D)
	$(CC) $(KCFLAGS) -ffp-contract=off -fno-tree-vectorize -fno-slp-vectorize -c -o $@ $<

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

# Integer-only, freestanding C++ arithmetic; use compiler standard headers.
$(DOS_NUMERIC_OBJ): $(BUILD)/%.o: %.cc
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -std=c11 -Iinclude -I../../include/common -I../../gui,$(KCFLAGS)) \
	    -x c++ -std=c++11 -fno-exceptions -fno-rtti -mgeneral-regs-only \
	    -Ilib -Ilib/softfloat3e -Ilib/softfloat3e/include -c -o $@ $<

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
GCC_ONLY_C += $(WPA_KERN_SRC)
GCC_ONLY_C += win32/crt_math.c $(wildcard win32/math/*.c)

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
GCC_WIN32_COMPAT += $(TCCDIR)/win32/crt_math.o \
                    $(patsubst %.c,$(TCCDIR)/%.o,$(wildcard win32/math/*.c))
GCC_ASM_OBJ       = $(ASM_SRC:%.S=$(TCCDIR)/%.o)
GCC_WIN32_ASM_OBJ = $(WIN32_ASM:%.S=$(TCCDIR)/%.o)

ALL_TCC_OBJ = $(TCC_KERN_OBJ) $(TCC_DRV_OBJ) $(TCC_FS_OBJ) $(TCC_WIN32_OBJ) $(TCC_DOS_OBJ) \
              $(GCC_TENSOR_OBJ) $(GCC_WIN32_COMPAT) $(GCC_ASM_OBJ) $(GCC_WIN32_ASM_OBJ) $(DOS_NUMERIC_OBJ)
ALL_TCC_OBJ += $(WPA_KERN_SRC:%.c=$(BUILD)/%.o) $(WPA_CRYPTO_OBJ)

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

$(TCCDIR)/win32/crt_math.o: win32/crt_math.c | $(TCCDIR)/win32
	$(CC) $(KCFLAGS_PRECOMP) -ffp-contract=off -Iwin32 -c -o $@ $<

$(TCCDIR)/win32/math/%.o: win32/math/%.c | $(TCCDIR)/win32
	@mkdir -p $(@D)
	$(CC) $(KCFLAGS_PRECOMP) -ffp-contract=off -c -o $@ $<

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
