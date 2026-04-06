# Osito-K - Developer Notes

## What is this
Bare-metal preemptive kernel for ESP8266 (Wemos D1, Xtensa LX106 @ 80MHz).
No Espressif SDK — runs directly on hardware using `nosdk8266` approach.
A [naranjositos.tech](https://naranjositos.tech/) project.

## Build & Flash

```bash
# Linux:
export PATH="$PWD/tools/xtensa-lx106-elf/bin:$PATH"
make                    # default: all features enabled
make ENABLE_ELITE=0 ENABLE_FORTH=0 ENABLE_DOOM=1   # DOOM config
make flash              # flash via /dev/ttyUSB0

# Windows:
export PATH="/c/Users/xasko/osito-k/tools/xtensa-lx106-elf/bin:$PATH"
make PYTHON=py          # or run esptool manually
```

**x86-64 bare-metal build** (requires `gnu-efi`):
```bash
sudo apt install gnu-efi
make -C arch/x86            # builds arch/x86/build/ositok.efi
make -C tools/ositofs        # builds host tools (mkfs, write, ls, info)
```

**Feature flags** (see Makefile):
```
ENABLE_ELITE=1   Elite wireframe flight demo + ship models (~2.1KB IRAM)
ENABLE_FORTH=1   zForth scripting engine (~4.2KB IRAM)
ENABLE_DOOM=0    DOOM wireframe 2.5D engine (~3.5KB IRAM)
```

**Serial monitor**: 74880 baud (ROM bootloader uses ~52MHz APB, not 80MHz).
```bash
python3 tools/tviewer.py                    # terminal video bridge (SSH-safe)
python3 tools/tviewer.py /dev/ttyUSB0 74880 # explicit port/baud
python3 tools/console.py /dev/ttyUSB0       # text-only console
```

**Restore original firmware**: `python3 -m esptool --port /dev/ttyUSB0 write_flash 0x0 backup/wemos_d1_full_backup.bin`

## Critical Architecture Details

### Xtensa LX106 specifics
- **CALL0 ABI** — no register windows. a0=retaddr, a1=SP, a2-a7=args, a8-a15=temps.
- **Context frame**: 80 bytes (a0-a15 + PS + SAR + EPC1 + pad). Offsets in task.h and context_switch.S.
- **PS register**: INTLEVEL(0:3), EXCM(4), UM(5). EXCM=1 masks ALL level-1 exceptions regardless of INTLEVEL. `rsil` only changes INTLEVEL, NOT EXCM.
- **`rfe`** clears PS.EXCM atomically and jumps to EPC1.

### Vector table offsets (VECBASE = 0x40100000)
```
0x10  Debug/Level-2 exception
0x20  NMI/Level-3
0x30  KernelExceptionVector (PS.UM=0)
0x50  UserExceptionVector   (PS.UM=1)  ← FRC1 timer interrupts land here
0x70  DoubleExceptionVector
```
**Previous bug**: vectors.S had UserExceptionVector at 0x10 instead of 0x50. This caused ALL ISR crashes.

### Interrupt handling
- FRC1 timer → DPORT edge interrupt → INUM 9 → level-1 exception → VECBASE+0x50
- Software yield → `wsr intset` INUM 7 → same path → context switch
- UART RX → INUM 5 → ring buffer fill
- Must clear BOTH `FRC1_INT_CLR=1` AND `wsr intclear` for FRC1.
- Initial task PS = 0x30 (UM=1, EXCM=1). rfe clears EXCM → interrupts unmasked.

### Known issues
- `ets_strlen` ROM function crashes when called from preemptible task context. Use inline strlen instead.
- UART baud shows 74880 because ROM bootloader sets ~52MHz APB clock before our PLL init.
- Flash mode MUST be DOUT (QIO causes boot failure on Wemos D1).
- Image format MUST be version 1 (v2 doesn't boot on ESP8266).
- GCC 10.3 (earlephilhower) libgcc lacks Xtensa div/mul builtins. Makefile auto-finds GCC 8.4 libgcc via LIBGCC_COMPAT.

## Code Map

### ESP8266 (src/)
```
src/boot/          vectors.S, crt0.S, nosdk_init.c — startup + vector table
src/kernel/        context_switch.S, sched.cpp, timer_tick.c, task.h, sem.cpp, mq.cpp
src/mem/           pool_alloc.cpp (32B×256), heap.cpp (8KB first-fit)
src/fs/            ositofs.cpp — SPI flash filesystem
src/math/          fixedpoint.h/cpp (16.16), matrix3.h/cpp (3D vectors/matrices)
src/drivers/       uart.cpp, gpio.cpp, adc.cpp, input.cpp, font.cpp, video.cpp
src/forth/         zforth.c, zf_host.cpp, setjmp.S — Forth interpreter
src/doom/          doom_gen.cpp, doom_render.cpp, doom_game.cpp — 2.5D engine
src/shell/         shell.cpp — interactive shell
src/main.cpp       kernel_main entry point
```

### x86-64 Bare-Metal AI OS (arch/x86/)
```
boot/              boot_efi.c — UEFI bootloader (loads kernel.elf from ESP)
                   efi_main.c — Legacy monolithic EFI entry (deprecated)
include/           boot_info.h — Boot protocol struct (bootloader↔kernel)
kernel.ld          — Kernel linker script (base 0x2000000)
kernel/            main.c, serial.c, framebuffer.c, pci.c, memory.c, heap.c
                   idt.c, isr_stubs.S, paging.c — IDT + 4-level page tables
                   syscall.c, syscall_entry.S — SYSCALL/SYSRET + 100+ Linux syscalls
                   elf.c, process.c, setjmp.S — ELF64 loader + process subsystem
                   keyboard.c, terminal.c — PS/2 + line editor
                   net.c, net.h — TCP/IP stack (ARP, IPv4, ICMP, UDP, TCP, DNS)
                   crypto.c, tls.c — TLS 1.2 (SHA-256, AES-128-GCM, X25519)
                   http.c, claude.c — HTTPS client + Claude API
                   tokenizer.c — BPE tokenizer (Llama 3)
                   tensor.c, tensor_avx2.c, inference.c — Llama forward pass
                   smp.c, ap_trampoline.S — Multi-core SMP
                   dynlink.c — Dynamic linker (dl_open/sym/close)
                   git.c, zlib.c — Git VCS (SHA-1, DEFLATE)
                   shell.c — 19+ builtins (incl. build, kexec)
                   kexec_tramp.S — kexec trampoline (segment copy + jump)
                   shm.c, compositor.c, display.c, input_events.c, memcompress.c
                   dhcp.c — DHCP client (discover/offer/request/ack + renewal)
                   ntp.c — NTP time sync (pool.ntp.org)
                   ipv6.c — IPv6 link-local + ICMPv6
                   mdns.c — mDNS responder (osito-k.local)
                   kmod.c — Kernel module loader (.ko ELF64)
                   wayland.c — Wayland display protocol stub
                   socket.c — BSD socket API (AF_INET TCP/UDP)
                   timers.c — alarm/setitimer POSIX timers
                   pty.c — Pseudo-terminal subsystem
                   sysv_ipc.c — System V shared memory + semaphores
                   vt.c — Virtual terminal multiplexer (4 VTs)
                   blkdev.c — Block device abstraction layer
                   strace.c — Syscall tracer (per-process)
                   caps.c — Capabilities + cgroups
                   klog.c — Kernel log ring buffer (dmesg)
                   random.c — RNG with entropy pool + RDRAND
                   oom.c — OOM killer + swap stub
                   ns.c — Namespace isolation (containers)
                   panic.c — Kernel panic + watchdog
                   sysfs.c — /sys virtual filesystem
                   netfilter.c — Packet filtering (iptables-like)
                   crypto2.c — ChaCha20, Poly1305, SHA-512, HKDF
                   sched_rt.c — RT scheduler (FIFO, RR, Deadline)
                   dm.c — Device mapper (dm-linear)
                   power.c — ACPI shutdown/reboot + CPU freq
                   tls13.c — TLS 1.3 client handshake
                   sshd.c — SSH-2 server (version exchange, KEXINIT)
                   evdev.c — Input event device (keyboard + mouse)
                   sysctl.c — Kernel parameter tuning (/proc/sys)
                   fuse.c — Filesystem in Userspace (request/response)
                   kprof.c — Sampling profiler (RIP histogram)
                   msync.c — MAP_SHARED writeback coherence
                   io_uring.c — Async I/O (SQ/CQ rings)
                   bpf.c — eBPF virtual machine
                   kthread.c — Kernel thread lifecycle
                   initramfs.c — CPIO archive loader
                   trace.c — Kernel tracepoints (ftrace-like)
                   seccomp.c — Syscall sandboxing
                   workqueue.c — Deferred work execution
drivers/           nvme.c — NVMe read/write
                   gpu.c, gsp.c — NVIDIA GPU + GSP Falcon (Phases 1-10)
                   sass.c, gmmu.c — SASS kernels + GPU MMU
                   gpu_tensor.c, gpu_inference.c — GPU compute dispatch
                   gpu_display.c — GPU display engine
                   ahci.c, ccp.c, xhci.c — SATA, AMD TRNG, USB 3.x
                   hda.c — Intel HD Audio (PCM playback, codec init)
                   virtio.c — Virtio PCI transport (split virtqueue)
                   virtio_blk.c — Virtio block device (read/write)
                   virtio_net.c — Virtio network (RX/TX queues)
                   usb_storage.c — USB mass storage (BBB/SCSI)
win32/             pe.c, winexec.c — PE32 loader + execution
                   compat32.c, int2e_stub.S — 32→64 mode switching (INT 0x2E)
                   dllloader.c — 15 DLL shims (kernel32, msvcrt, user32, etc.)
                   ntsyscall.c, handle.c — NT syscalls + handle table
fs/                ositofs2.c, gpt.c, gguf.c — OsitoFS v2 (R/W + block reclaim) + GPT + GGUF
                   fat32.c — FAT32 R/W (LFN, create, delete, cluster alloc)
                   tmpfs.c — RAM filesystem (/tmp, 128 files, 64MB)
                   ext2.c — ext2/ext3/ext4 read-only (extents, indirect blocks)
                   iso9660.c — ISO 9660 CD-ROM read-only
                   exfat.c — exFAT read-only (USB/SD >32GB)
                   ntfs.c — NTFS read-only (MFT, run lists, B+ tree index)
                   udf.c — UDF read-only (DVD/Blu-ray)
                   squashfs.c — SquashFS read-only (zlib decompression)
                   hfsplus.c — HFS+ read-only (catalog B-tree, big-endian)
                   btrfs.c — Btrfs read-only (chunk tree, extent data)
                   apfs.c — APFS read-only (container + volume superblock)
libc/              crt.c, syscall.S, tcclib.c, math.c — CRT + extended libc
                   qjs_main.c, qjs_headers/ — QuickJS REPL
                   ositok.h — single-header libc for self-compiled programs
test/              various test programs + qjs.elf
```

### Shared GUI (gui/)
```
gui/               Cross-architecture graphical desktop (elementaryOS-inspired)
                   gui.h — shared header: surface_t, color palette, layout constants
                   gui_draw.c — primitives: rect, alpha blend, gradient, rounded rect, circle, multi-layer shadows
                   gui_text.c — 8x16 bitmap font + UTF-8 Unicode + AA text + Latin-1 Supplement (96 glyphs)
                   gui_panel.c — top panel (Wingpanel style) with NTP-synced clock
                   gui_dock.c — bottom dock (Plank style) with hover effects + active indicators
                   gui_window.c — window decorations: title bar, traffic-light buttons, shadow
                   gui_desktop.c — orchestrator: gradient bg + panel + demo windows + dock
```

### Host Tools (tools/)
```
tools/ositofs/     mkfs.c — format (--block-size, --label)
                   write.c — batch write, --from-list manifest, --overwrite
                   read.c — extract with wildcards (*.ext, prefix*, *)
                   ls.c — list with timestamps, block_size display
                   info.c — filesystem info + fragmentation + file stats
                   delete.c — batch delete with wildcards, --dry-run
                   rename.c — metadata-only rename
                   fsck.c — consistency check + --repair + --verbose CRC verify
                   defrag.c — offline compaction, --dry-run, --verbose
                   common.c/h — shared I/O, runtime block_size, superblock backup
include/common/    ositofs2_format.h — shared on-disk format (configurable block_size 64K-1M)
```

### AArch64 Port (arch/arm/)
```
arch/arm/          SM8350 (ROG Phone 5) bare-metal port — see docs/aarch64-detail.md
                   kernel/gui_task.c — desktop GUI scheduler task (renders to splash FB)
```

## Roadmap

### ESP8266 (Xtensa LX106)

| Feature | Description | Status |
|---------|-------------|--------|
| F1-F5   | Kernel, scheduler, drivers, FS, heap, font, framebuffer | Done |
| F6      | Fixed-point 16.16 math library (sin/cos/div/sqrt) | Done |
| F7      | 3D vectors, matrices, perspective projection | Done |
| F8      | Wireframe renderer — vertex/edge → rotate → project → draw | Done |
| F9      | Ship models — Cobra, Sidewinder, Coriolis, Viper, Asp, Shuttle | Done |
| F10     | Game loop + HUD — flight, starfield, radar, joystick | Done |
| **zF**  | **zForth** — replaced BASIC+VM, saved ~4KB IRAM | Done |
| **F12** | **DOOM wireframe** — 2.5D BSP engine, procedural levels | Done |
| **F11** | **Spreadsheet engine** — cell grid, formula parser, cursor UI | Next |

### x86-64 Bare-Metal AI OS

| Feature | Description | Status |
|---------|-------------|--------|
| X1-X8   | UEFI boot, serial, framebuffer, PCI, NVMe, OsitoFS, GPU detect | Done |
| X9-X11  | Ethernet (I211/e1000e), network stack (ARP/IPv4/UDP), QEMU infra | Done |
| X12-X15 | GPT parser, GGUF loader, tensor engine, Llama inference | Done |
| X16-X26 | GPU MMIO probe → FWSEC-FRTS (Phases 1-10) | Done |
| X27-X31 | Falcon PIO, GBL FWSEC, WPR2/Radix3, GSP 2-stage boot, SEC2 | Done |
| X32-X36 | RM alloc/control, channel+GPFIFO, compute bind, CE DMA, QMD | Done |
| X37-X42 | SASS kernels, GMMU, GPU tensor ops, GPU inference, PTX pipeline | Done |
| X-CPU1  | AVX2/FMA tensor ops (~4-8x speedup) | Done |
| X-CPU2  | NVMe write + OsitoFS v2 create/write/delete | Done |
| X-CPU3  | UDP prompt server (port 7777, inference dispatch) | Done |
| X-OS1   | IDT + exceptions + APIC timer | Done |
| X-OS2   | 4-level paging (identity map, 2MB large pages) | Done |
| X-OS3   | Heap allocator (kmalloc/kfree, auto-grow) | Done |
| X-OS4   | SYSCALL/SYSRET interface (Linux ABI) | Done |
| X-OS5   | ELF64 loader (PT_LOAD, stack setup) | Done |
| X-OS6   | Process subsystem (exec/exit/waitpid) | Done |
| X-OS7   | Terminal line editor (readline, history) | Done |
| X-OS8   | PS/2 keyboard (scancode set 1, IRQ 1) | Done |
| X-OS9   | Mini shell (18+ builtins) | Done |
| X-OS10  | File I/O syscalls + GDT relocation fix | Done |
| X-OS11  | TCC cross-compilation | Done |
| X-OS12  | Minimal CRT (printf, malloc, POSIX I/O) | Done |
| X-OS13  | TCC in-OS compilation | Done |
| X-NET1-5| ICMP, TCP, DNS, TLS 1.2+Crypto, HTTP client | Done |
| X-CL1-5 | Claude API, REPL, tool use (file/exec/search) | Done |
| X-TOK1  | BPE tokenizer (Llama 3, 128K vocab) | Done |
| X-INF1-3| GPU inference dispatch, VRAM-resident activations+weights | Done |
| X-SMP   | Multi-core AP startup (INIT-SIPI-SIPI) | Done |
| X-PIPE  | Pipes, dup2, signals, shell redirection | Done |
| X-DYN   | Dynamic linking (dl_open/dl_sym/dl_close) | Done |
| X-JS    | QuickJS JavaScript engine (ES2020+, BigInt) | Done |
| X-GIT   | Git VCS (SHA-1, zlib, standard objects) | Done |
| X-SCHED | Preemptive scheduler (APIC timer, RSP-swap) | Done |
| X-MMAP  | mmap/munmap/mprotect | Done |
| X-VFS   | Virtual filesystem (/dev, /proc) | Done |
| X-MUSL  | musl libc port (+20 syscalls) | Done |
| X-FORK  | fork/wait4/getppid + busybox ash | Done |
| X-THREAD| clone(CLONE_THREAD) + futex | Done |
| X-EDIT  | Kilo text editor (ANSI CSI, termios) | Done |
| X-HTTPD | HTTP file server (TCP listen/accept) | Done |
| X-SELF  | Self-hosting: TCC builds kernel.elf in-OS + kexec boots it | Done |
| X-QOS   | QoS priority scheduler (5 classes) | Done |
| X-CCP   | AMD CCP TRNG driver | Done |
| X-AHCI  | SATA AHCI driver | Done |
| X-XHCI  | xHCI USB 3.x driver + HID keyboard/mouse | Done |
| X-RETINA| Display pipeline (GPU page flip, compositor, animations, AA text) | Done |
| X-WIN32 | Windows PE32 compat layer (15 DLL shims) | WIP |
| X-PGTBL | Per-process page tables (CR3 switch on context switch) | Done |
| X-W32THR| Win32 real threading (CreateThread → sched_spawn) | Done |
| X-OSFS3 | OsitoFS v2 overhaul (9 tools, block_size, timestamps, hash, CRC, fsck) | Done |
| X-DHCP  | DHCP client (auto IP, gateway, DNS, lease renewal) | Done |
| X-NTP   | NTP time sync (pool.ntp.org, UTC offset, panel clock) | Done |
| X-TCP2  | TCP fast retransmit + timeout backoff (RFC 5681) | Done |
| X-IPV6  | IPv6 link-local + ICMPv6 + neighbor discovery | Done |
| X-MDNS  | mDNS responder (osito-k.local on UDP 5353) | Done |
| X-FS12  | 12 filesystems: FAT32 R/W, tmpfs, ext2/3/4, ISO9660, exFAT, NTFS, UDF, SquashFS, HFS+, Btrfs, APFS | Done |
| X-VFS2  | VFS mount table (/tmp, /fat, /ext2, /iso) + syscall dispatch | Done |
| X-VIRTIO| Virtio PCI transport (split virtqueue, device negotiation) | Done |
| X-KMOD  | Kernel module loader (.ko ELF64, symbol resolution, relocations) | Done |
| X-WL    | Wayland display protocol stub (surface create/commit/destroy) | Done |
| X-AUDIO | HDA audio /dev/dsp + beep command | Done |
| X-DOCKER| Dockerfile + Firecracker microVM containerization | Done |
| X-SOCK  | BSD Socket API (socket/bind/listen/accept/connect/send/recv) | Done |
| X-EPOLL | epoll + eventfd + poll syscalls | Done |
| X-PTY   | Pseudo-terminal pairs (/dev/ptmx + /dev/pts/N) | Done |
| X-TIMER | Timer subsystem (alarm, setitimer, SIGALRM delivery) | Done |
| X-PROCFS| Full procfs (/proc/cpuinfo, meminfo, uptime, mounts, filesystems) | Done |
| X-FB    | Framebuffer /dev/fb0 (mmap for direct pixel access) | Done |
| X-POWER | ACPI shutdown/reboot + CPU freq query + MWAIT detection | Done |
| X-IPC   | System V IPC (shmget/shmat/shmdt + semaphores) | Done |
| X-VT    | Virtual terminal multiplexer (Alt+F1-F4, 4 VTs) | Done |
| X-BLKDEV| Block device abstraction (NVMe/AHCI/virtio/USB unified API) | Done |
| X-STRACE| Syscall tracer (per-process logging with decoded args) | Done |
| X-CAPS  | Linux capabilities (38 caps, per-process effective/permitted) | Done |
| X-CGROUP| cgroups basic (CPU quota + memory limits per group) | Done |
| X-NS    | Namespaces (PID/mount/net/UTS/IPC isolation for containers) | Done |
| X-KLOG  | Kernel log ring buffer (64KB dmesg, syslog syscall) | Done |
| X-RNG   | Random number generator (entropy pool, RDRAND, /dev/random) | Done |
| X-OOM   | OOM killer (score-based, PID 0/1 protected) + swap stub | Done |
| X-PANIC | Kernel panic (register dump, stack trace, watchdog) | Done |
| X-SYSFS | sysfs /sys (block devices, net, kernel info) | Done |
| X-NF    | Netfilter packet filtering (32 rules, accept/drop) | Done |
| X-CRYPTO2| ChaCha20-Poly1305 + SHA-512 + HKDF-SHA256 | Done |
| X-SCHED2| RT scheduler (SCHED_FIFO, SCHED_RR, SCHED_DEADLINE) | Done |
| X-DM    | Device mapper (dm-linear, foundation for LVM/dm-crypt) | Done |
| X-VBLK  | Virtio block driver (read/write via split virtqueue) | Done |
| X-VNET  | Virtio network driver (RX/TX queues, MAC config) | Done |
| X-USBMS | USB mass storage (BBB protocol, SCSI READ/INQUIRY) | Done |
| X-TLS13 | TLS 1.3 client (X25519 key share, ChaCha20-Poly1305) | Done |
| X-SSHD  | SSH server (protocol exchange, KEXINIT, session mgmt) | Done |
| X-EVDEV | Input event device (evdev, Linux struct input_event) | Done |
| X-SYSCTL| Kernel sysctl (10+ tunable parameters, read/write) | Done |
| X-FUSE  | FUSE userspace filesystem (request/response queues) | Done |
| X-KPROF | Kernel profiler (sampling via timer ISR, histogram) | Done |
| X-MSYNC | msync + MAP_SHARED coherence (writeback to disk) | Done |
| X-IOURING| io_uring async I/O (SQ/CQ ring buffers) | Done |
| X-BPF   | eBPF virtual machine (ALU64, JMP, LDX, helper calls) | Done |
| X-KTHR  | Kernel threads API (kthread_create/stop) | Done |
| X-INITRD| initramfs loader (cpio newc → tmpfs extraction) | Done |
| X-TRACE | Kernel tracepoints (sched/syscall/irq/net/fs/mm) | Done |
| X-SECCOMP| Seccomp syscall filtering (strict + BPF filter) | Done |
| X-WQ    | Workqueue subsystem (deferred + delayed work) | Done |
| **Phase 0** | **Kernel/bootloader separation** (boot.efi + kernel.elf) | **Done** |
| **Phase 1** | **TCC cross-compiles kernel from host** | **Done** |
| **Phase 2** | **TCC compiles kernel inside OsitoK** (62 .c → 733KB ELF) | **Done** |
| **Phase 3** | **kexec: load + boot self-compiled kernel** | **Done** |
| **Phase 4** | **Self-built kernel boots directly from UEFI** (verified in QEMU) | **Done** |

> Full GPU roadmap (X27-X40 + contingency): see [docs/x86-gpu-roadmap.md](docs/x86-gpu-roadmap.md)
> Full OS roadmap (Tiers 0-9): see [docs/os-selfhost-roadmap.md](docs/os-selfhost-roadmap.md)
> Binary compatibility roadmap: see [docs/binary-compat-roadmap.md](docs/binary-compat-roadmap.md)
> Paths to Claude analysis: see [docs/paths-to-claude-on-ositok.md](docs/paths-to-claude-on-ositok.md)
> Kernel/bootloader separation + self-compiling road: see [docs/kernel-separation.md](docs/kernel-separation.md)
> Reference material (WRK, NT, DOS source): see [docs/reference-material.md](docs/reference-material.md)
> OsitoFS v2 host tools (9 tools): see `tools/ositofs/`
> Filesystem roadmap (12 current + 15 planned): see [docs/filesystem-roadmap.md](docs/filesystem-roadmap.md)

**Tier 8: Hardware Boot** — Boot OsitoK on real hardware (AMD Ryzen 7 5800X + RTX 3090).

### Hardware Boot Plan (target: WD SN740 512GB NVMe)

| Step | Task | Description | Deps |
|------|------|-------------|------|
| **H1** | **xHCI USB driver** | AMD 400/Matisse xHCI (`1022:43d5`, `1022:149c`). Enumerate ports, configure endpoints, USB HID for keyboard+mouse | None |
| **H2** | **USB HID input** | Parse HID reports, scancode→ASCII, integrate with terminal line editor | H1 |
| **H3** | **NVMe SN740 bring-up** | Test OsitoK NVMe driver with SN740 (`15b7:5016`, DRAM-less). May need CMB/SQ-in-CMB support | None |
| **H4** | **Flash to NVMe** | Create GPT on nvme1n1: ESP partition (boot.efi + kernel.elf) + OsitoFS partition | H3 |
| **H5** | **UEFI GOP display** | Verify framebuffer works on RTX 3090 GOP output (already obtained by boot.efi) | None |
| **H6** | **Real hardware boot** | Boot from nvme1n1 via BIOS boot menu. Serial header for debug if needed | H1-H5 |
| **H7** | **Nouveau modesetting** | Native display init for RTX 3090 (GA102). Resolution control, cursor | H6 |

**Critical path**: H1 → H2 → H6 (xHCI is the blocker — no keyboard = no interaction)

## Detailed Feature Documentation

For implementation details, API specifics, register-level documentation, and debugging notes:

- **[docs/x86-features-detail.md](docs/x86-features-detail.md)** — All x86-64 feature descriptions (X9-X42, X-OS*, X-NET*, X-CL*, X-WIN32, etc.)
- **[docs/esp8266-detail.md](docs/esp8266-detail.md)** — Math library API, zForth integration, resource budget, DOOM/Elite details
- **[docs/aarch64-detail.md](docs/aarch64-detail.md)** — AArch64/SM8350 (ROG Phone 5) port details
- **[docs/x86-gpu-roadmap.md](docs/x86-gpu-roadmap.md)** — GPU compute roadmap (X27-X40)
- **[docs/os-selfhost-roadmap.md](docs/os-selfhost-roadmap.md)** — OS self-hosting tiers 0-9

## Language
The user speaks Spanish. Communicate in Spanish when appropriate.

<!-- gitnexus:start -->
# GitNexus — Code Intelligence

This project is indexed by GitNexus as **osito-k** (76066 symbols, 93233 relationships, 76 execution flows). Use the GitNexus MCP tools to understand code, assess impact, and navigate safely.

> If any GitNexus tool warns the index is stale, run `npx gitnexus analyze` in terminal first.

## Always Do

- **MUST run impact analysis before editing any symbol.** Before modifying a function, class, or method, run `gitnexus_impact({target: "symbolName", direction: "upstream"})` and report the blast radius (direct callers, affected processes, risk level) to the user.
- **MUST run `gitnexus_detect_changes()` before committing** to verify your changes only affect expected symbols and execution flows.
- **MUST warn the user** if impact analysis returns HIGH or CRITICAL risk before proceeding with edits.
- When exploring unfamiliar code, use `gitnexus_query({query: "concept"})` to find execution flows instead of grepping. It returns process-grouped results ranked by relevance.
- When you need full context on a specific symbol — callers, callees, which execution flows it participates in — use `gitnexus_context({name: "symbolName"})`.

## When Debugging

1. `gitnexus_query({query: "<error or symptom>"})` — find execution flows related to the issue
2. `gitnexus_context({name: "<suspect function>"})` — see all callers, callees, and process participation
3. `READ gitnexus://repo/osito-k/process/{processName}` — trace the full execution flow step by step
4. For regressions: `gitnexus_detect_changes({scope: "compare", base_ref: "main"})` — see what your branch changed

## When Refactoring

- **Renaming**: MUST use `gitnexus_rename({symbol_name: "old", new_name: "new", dry_run: true})` first. Review the preview — graph edits are safe, text_search edits need manual review. Then run with `dry_run: false`.
- **Extracting/Splitting**: MUST run `gitnexus_context({name: "target"})` to see all incoming/outgoing refs, then `gitnexus_impact({target: "target", direction: "upstream"})` to find all external callers before moving code.
- After any refactor: run `gitnexus_detect_changes({scope: "all"})` to verify only expected files changed.

## Never Do

- NEVER edit a function, class, or method without first running `gitnexus_impact` on it.
- NEVER ignore HIGH or CRITICAL risk warnings from impact analysis.
- NEVER rename symbols with find-and-replace — use `gitnexus_rename` which understands the call graph.
- NEVER commit changes without running `gitnexus_detect_changes()` to check affected scope.

## Tools Quick Reference

| Tool | When to use | Command |
|------|-------------|---------|
| `query` | Find code by concept | `gitnexus_query({query: "auth validation"})` |
| `context` | 360-degree view of one symbol | `gitnexus_context({name: "validateUser"})` |
| `impact` | Blast radius before editing | `gitnexus_impact({target: "X", direction: "upstream"})` |
| `detect_changes` | Pre-commit scope check | `gitnexus_detect_changes({scope: "staged"})` |
| `rename` | Safe multi-file rename | `gitnexus_rename({symbol_name: "old", new_name: "new", dry_run: true})` |
| `cypher` | Custom graph queries | `gitnexus_cypher({query: "MATCH ..."})` |

## Impact Risk Levels

| Depth | Meaning | Action |
|-------|---------|--------|
| d=1 | WILL BREAK — direct callers/importers | MUST update these |
| d=2 | LIKELY AFFECTED — indirect deps | Should test |
| d=3 | MAY NEED TESTING — transitive | Test if critical path |

## Resources

| Resource | Use for |
|----------|---------|
| `gitnexus://repo/osito-k/context` | Codebase overview, check index freshness |
| `gitnexus://repo/osito-k/clusters` | All functional areas |
| `gitnexus://repo/osito-k/processes` | All execution flows |
| `gitnexus://repo/osito-k/process/{name}` | Step-by-step execution trace |

## Self-Check Before Finishing

Before completing any code modification task, verify:
1. `gitnexus_impact` was run for all modified symbols
2. No HIGH/CRITICAL risk warnings were ignored
3. `gitnexus_detect_changes()` confirms changes match expected scope
4. All d=1 (WILL BREAK) dependents were updated

## CLI

- Re-index: `npx gitnexus analyze`
- Check freshness: `npx gitnexus status`
- Generate docs: `npx gitnexus wiki`

<!-- gitnexus:end -->
