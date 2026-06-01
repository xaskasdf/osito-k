# Osito-K - Developer Notes

## What is this
Bare-metal preemptive kernel for ESP8266 (Wemos D1, Xtensa LX106 @ 80MHz).
No Espressif SDK — runs directly on hardware using `nosdk8266` approach.
A [naranjositos.tech](https://naranjositos.tech/) project.

## Build & Flash

The root `Makefile` is a **delegator** — each arch has its own Makefile under `arch/<arch>/`:

```bash
make            # default → ESP8266/Xtensa firmware (build/osito.elf + osito0x00000.bin)
make x86        # x86-64 bare-metal OS → arch/x86/build/{boot.efi,kernel.elf}
make arm        # AArch64 (SM8350 / ROG Phone 5)
make wasm       # wasm32 hosted build
make flash      # flash ESP8266 via /dev/ttyUSB0
make clean-all  # wipe every arch's build/
```

### ESP8266 (Xtensa) — the CI-built default
```bash
# Linux:
export PATH="$PWD/arch/xtensa/tools/xtensa-lx106-elf/bin:$PATH"
make                    # default: all features enabled (build/osito.elf)
make ENABLE_ELITE=0 ENABLE_FORTH=0 ENABLE_DOOM=1   # DOOM config
make flash              # flash via /dev/ttyUSB0
# Windows: export PATH=".../arch/xtensa/tools/xtensa-lx106-elf/bin:$PATH"; make PYTHON=py
```
CI (`.github/workflows/build.yml`) builds **only** this Xtensa firmware on push/PR to `master`; x86/arm/wasm are not in CI.

### x86-64 bare-metal AI OS — the primary active target
Needs the **`x86_64-elf-gcc` cross toolchain + gnu-efi** (the bundled `tools/` toolchain is Xtensa-only). The Makefile auto-detects gnu-efi at `/usr/local` (macOS, ncroxon source build) or `/usr` (`sudo apt install gnu-efi`).
```bash
make -C arch/x86            # → build/boot.efi + build/kernel.elf + lib/vulkan/libvulkan.a
make -C arch/x86 minimal    # kernel-minimal.elf — fallback when the full kernel crashes (BSS/NVMe-DMA overlap)
make -C arch/x86 tcc        # kernel.elf via in-tree TCC (self-host verification)
make -C arch/x86 legacy     # monolithic ositok.efi (old single-PE path)
make -C arch/x86 COMPAT_TRACE=1   # +diagnostic probes in int2e/dos-int + #PF (byte-identical when unset/0)
make -C tools/ositofs       # host FS tools: mkfs/write/read/ls/info/delete/rename/fsck/defrag
```

### Run x86-64 in QEMU (the main dev loop)
The runner scripts build an ESP FAT image (boot.efi → `BOOTX64.EFI`, plus `kernel.elf`) via `mtools`, boot it under OVMF/edk2 on `q35`/`-m 512M`/`-smp 4` with virtio-net (`hostfwd udp 7778→7777`, `tcp 50052→50052`), xHCI kbd+tablet, and attach `arch/x86/build/nvme.img` as NVMe if present (the UT99/DOOM image).
```bash
arch/x86/scripts/qemu-cocoa.sh                                # macOS: -accel hvf -display cocoa
arch/x86/scripts/qemu-test.sh [--no-build] [--no-gl] [--kill] # generic; virtio-gpu-gl by default
.\build-windows.ps1                                           # Windows: WHPX + gtk (see below)
```
**The serial log is the debugging lifeline**: `arch/x86/build/serial.log`. Exit QEMU with `Ctrl-A X`. Poke the in-OS UDP inference server from the host: `echo "hola osito" | nc -u localhost 7777`.

### Windows (this machine)
The repo is rsync'd from a Mac to `C:\Users\xasko\osito-k`; build via msys2/mingw64 (`C:\msys64`). Full guide: **`README-windows.md`**.
```powershell
.\build-windows.ps1 -Check      # report missing deps
.\build-windows.ps1 -Install    # pacman: make, mtools, mingw-w64-x86_64-qemu
.\build-windows.ps1 -BuildOnly  # compile only
.\build-windows.ps1             # build + QEMU (WHPX; add -Accel tcg if no Hypervisor Platform)
```
`x86_64-elf-gcc` and `gnu-efi` are **not in pacman** — install manually (prebuilt cross-tools or crosstool-ng) and add their `bin/` to the mingw64 PATH.

### Tests
There is no host unit-test harness. `arch/x86/test/*.c` (hello_c, fork_test, thread_test, mmap_test, vfs_test, test_linux_abi, kilo, qjs, tcc, selfbuild…) compile to ELF and run **inside the booted OS** — load them onto the NVMe/ESP image and run from the in-OS shell, or exercise the network stack via the UDP server above.

**Feature flags** (see Makefile):
```
ENABLE_ELITE=1   Elite wireframe flight demo + ship models (~2.1KB IRAM)
ENABLE_FORTH=1   zForth scripting engine (~4.2KB IRAM)
ENABLE_DOOM=0    DOOM wireframe 2.5D engine (~3.5KB IRAM)
```

**Serial monitor**: 74880 baud (ROM bootloader uses ~52MHz APB, not 80MHz).
```bash
python3 arch/xtensa/scripts/tviewer.py                    # terminal video bridge (SSH-safe)
python3 arch/xtensa/scripts/tviewer.py /dev/ttyUSB0 74880 # explicit port/baud
python3 arch/xtensa/scripts/console.py /dev/ttyUSB0       # text-only console
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

### ESP8266 (arch/xtensa/)
```
arch/xtensa/boot/          vectors.S, crt0.S, nosdk_init.c — startup + vector table
arch/xtensa/kernel/        context_switch.S, sched.cpp, timer_tick.c, task.h, sem.cpp, mq.cpp
arch/xtensa/mem/           pool_alloc.cpp (32B×256), heap.cpp (8KB first-fit)
arch/xtensa/fs/            ositofs.cpp — SPI flash filesystem
arch/xtensa/math/          fixedpoint.h/cpp (16.16), matrix3.h/cpp (3D vectors/matrices)
arch/xtensa/drivers/       uart.cpp, gpio.cpp, adc.cpp, input.cpp, font.cpp, video.cpp
arch/xtensa/forth/         zforth.c, zf_host.cpp, setjmp.S — Forth interpreter
arch/xtensa/doom/          doom_gen.cpp, doom_render.cpp, doom_game.cpp — 2.5D engine
arch/xtensa/shell/         shell.cpp — interactive shell
arch/xtensa/main.cpp       kernel_main entry point
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
                   rcu.c — Read-Copy-Update (lock-free reads)
                   slab.c — Slab allocator (kmem_cache)
                   pci_hotplug.c — PCI device hotplug detection
                   coredump.c — Crash dump (registers + stack)
                   lockdep.c — Lock dependency validator
                   psi.c — Pressure Stall Information
                   kobject.c — Unified device model tree
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

> **Documentation refresh — last audit 2026-05-17** (TCP/TLS/PKI overhaul session: X-TLS13 row clarified (real handshake e2e, AES-128-GCM not ChaCha), new rows X-PKI / X-OCSP / X-CRL / X-AIA / X-SACK / X-RFC7323 / X-RTO / X-TLS-NEG, all marked Done; see `docs/session-2026-05-16-17-net-pki-overhaul.md` for the full chronology, RFC table, and primitive sizes. Prior 2026-04-25 audit covered Hardware Boot H1-H6 and the X-WIN32 / X-VGPU / X-MESA / X-DXVK / X-VK / X-DOS / X-MEMCOMP / X-VDSO / X-SPEC / X-KALL / X-AUDSCHED / X-DMASCHED / X-INOTIFY / X-CPUTOP / X-PERF / X-KSTATE / X-GUI / A-ARM64 entries.)

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
| X-WIN32 | Windows PE32 compat layer (12+ DLL shims: kernel32, msvcrt, user32, gdi32, advapi32, comctl32, comdlg32, ddraw, dsound, ntdll, ole32, shell32, winmm, wsock32). Active: UT99/GTAV/Engine.dll debugging, SEH dispatch, IAT patching, INT 0x2E thunks | WIP |
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
| X-TLS13 | TLS 1.3 client real (X25519 + AES-128-GCM + HKDF, Finished MAC, full handshake e2e contra CF) | Done |
| X-PKI   | RFC 5280 chain validation: link sig verify (P-256/P-384/RSA-SHA256/RSA-SHA384) + constraints (BasicConstraints/KeyUsage/pathLen) + validity window + SAN/CN match (RFC 6125) + cert_pin (static/dynamic/operator) | Done |
| X-OCSP  | OCSP client (RFC 6960): live query + responder sig verify + stapling (RFC 6066 §8) | Done |
| X-CRL   | CRL client (RFC 5280 §5): download + parse + serial check + sig verify (informative-mode fallback when OCSP errors) | Done |
| X-AIA   | AIA chase end-to-end: caIssuers URL extract + http_plain fetch + chain extend (recursive up to depth 3) + osfs2 cache | Done |
| X-SACK  | RFC 2018 SACK: SACK_PERMITTED negotiate + multi-block emission (up to 4) + RFC 6675 §4 prefix advance | Done |
| X-RFC7323| TCP window scaling + timestamps + PAWS (full RFC 7323) | Done |
| X-RTO   | RFC 6298 Jacobson RTT smoothing + dynamic RTO (SRTT/RTTVAR) via TSecr | Done |
| X-TLS-NEG| HTTP dispatch: try TLS 1.3 first, fallback TLS 1.2 (transparent to caller) | Done |
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
| X-RCU   | RCU read-copy-update (lock-free reads, grace periods) | Done |
| X-SLAB  | Slab allocator (kmem_cache, bitmap-based O(1) alloc) | Done |
| X-PCIHP | PCI hotplug detection (rescan, add/remove callbacks) | Done |
| X-CORE  | Kernel coredump (register dump, stack trace, code bytes) | Done |
| X-LOCKD | Lock dependency validator (deadlock detection) | Done |
| X-PSI   | Pressure Stall Information (CPU/memory/IO pressure) | Done |
| X-KOBJ  | Unified device model (kobject tree, sysfs foundation) | Done |
| X-VFSU  | VFS unified layer (osfs2/osfs3 dual-dispatch, vfs_node_t) | Done |
| X-OSFS3D| OsitoFS v3 driver (inodes, extents, hierarchical dirs, 1MB blocks) | Done |
| X-DEMAND| Demand-paged ELF loader (header read + VMA_FILE_ELF + page-fault dispatch) | Done |
| X-ETXTBSY| ETXTBSY on write-open of running binaries (proc_is_executing) | Done |
| X-PIPEBLK| Pipes block by default (O_NONBLOCK honored) | Done |
| X-HIGHMEM| High-memory allocator for kernel paging structures (avoid ELF collision) | Done |
| **Phase 0** | **Kernel/bootloader separation** (boot.efi + kernel.elf) | **Done** |
| **Phase 1** | **TCC cross-compiles kernel from host** | **Done** |
| **Phase 2** | **TCC compiles kernel inside OsitoK** (62 .c → 733KB ELF) | **Done** |
| **Phase 3** | **kexec: load + boot self-compiled kernel** | **Done** |
| **Phase 4** | **Self-built kernel boots directly from UEFI** (verified in QEMU) | **Done** |
| X-CPUF  | Centralized CPU feature detection (`cpu_features.h`), vendor/cache/PMU/SIMD caps | Done |
| X-PMU   | Hardware perf counters (3 fixed + 4 PMCs: L1d/L2/TLB/branch miss), RDPMC via CR4.PCE | Done |
| X-TARENA| Superpage tensor arena (512MB on 256 × 2MB pages) for scratch + KV cache | Done |
| X-DISP  | Boot-time CPUID dispatch table (AVX2/AVX-512 select, ERMS memcpy) | Done |
| X-SYSINF| `sys_inference` syscall family (530-534) + `ositok.h` `oi_*` wrappers | Done |
| X-ASLRL | ASLR-lite stack jitter (0-4080B, RDRAND-driven) for cache diversification | Done |
| X-PREDS | Markov predictive scheduling + kernel_rsp/FPU prefetch (`pred_record`/`pred_prewarm`) | Done |
| X-IOPRE | Speculative I/O prefetch: pattern table hooked into `vfs_find`, APs prefetch via `smp_submit_ff` | Done |
| X-SGTX  | Zero-copy scatter-gather TX on I211 (`i211_send_sg`, legacy descriptor chaining) | Done |
| X-HWBP  | Hardware breakpoints (DR0-DR3), `watch`/`unwatch`/`hwbp` shell commands | Done |
| X-SELF  | Self-optimizing kernel (dry-run v1: static branch site registration + `self_opt apply`) | Done |
| X-MEMCOMP| Memory compression (LZ-style page packing for cold pages) | Done |
| X-VDSO  | VDSO thunks (gettimeofday/clock_gettime fast path, no syscall) | Done |
| X-SPEC  | Speculative decoding (spec_analyze/spec_prefetch/spec_tls — branch-pattern + TLS prewarm) | Done |
| X-KALL  | kallsyms — runtime symbol table for stack traces and `usym` user-symbol resolver | Done |
| X-AUDSCHED| Audio-aware scheduler (low-latency PCM playback, HDA frame deadlines) | Done |
| X-DMASCHED| DMA scheduler (NVMe/AHCI request batching + priority queues) | Done |
| X-INOTIFY| inotify (file watch events: create/delete/modify) | Done |
| X-CPUTOP| CPU topology detection (sockets/cores/threads, NUMA hints) | Done |
| X-PERF  | perf events subsystem (counter sampling + ring buffer) | Done |
| X-KSTATE| Kernel state snapshotting (boot-time fast restore, crash_report integration) | Done |
| X-PIPES | Shell pipes (`cmd1 \| cmd2 \| ...`, hasta 8 etapas, stdin via memory buffer; `grep`/`head`/`tail` aceptan stdin) | Done |
| X-NICSTAT| `nic_stats` shell command — snapshot live I211 (ISR, IMS, ICR, RDH, RDT, GPRC, irq_pending) | Done |
| X-PCIMSI| PCI MSI capability ECAM-aligned read (offset 0x04 dword en lugar de 0x06 unaligned) — fix MSI bring-up | Done |
| X-RDTFIX| I211 RX RDT off-by-one (Linux igb-style: `RDT = next_to_use`, slot recién re-armado visible para HW) | Done |
| X-MSIFIX| I211 MSI delivery: programar GPIE con PBA \| EIAME \| NSICR — sin EIAME el chip ignora re-arm del IMS post-22 IRQs | Done |
| X-OFTP  | OsitoK File Transfer Protocol over UDP (`kdownload <ip> <port> <file>` + `tools/oftp-server.py` con NAK retransmit) | Done |
| X-KUPDATE| `kupdate` shell command — DNS + TLS 1.2 + HTTP GET + osfs2_write + cmd_kexec; default `https://naranjositos.tech/k/x86_64/stable/kernel.elf`, `--channel <ch>` y `--no-kexec` opcionales, valida ELF magic antes de kexec | Done (pending server-side upload) |
| X-NET-TXR| Mac↔OsitoK reply-path TX bug — `osito> ping` funciona pero `net_poll → handle_*` no llega al wire (instrumentado, hipótesis abiertas) | WIP |
| X-VGPU  | virtio-gpu 2D + 3D driver (resource create, transfer, virgl-style) | Done |
| X-MESA  | Mesa 25.0.0 in-OS port: util/c11/include + gallium aux + compiler/{glsl,nir,spirv} + zink + mesa/main + state_tracker (libGL.a super-archive) | WIP |
| X-DXVK  | DXVK 2.4 in-OS port: util/spirv/vulkan + dxbc/dxvk core + d3d11/dxgi/d3d10 (com_stub IUnknown) | WIP |
| X-VK    | Vulkan stack: NVK userland ICD + Venus protocol (encoder/decoder, phys-dev queries, cmd buffers) + software rasterizer for guest-local present | WIP |
| X-DOS   | DOS-native execution: VCPI server, EMS stubs, INT 31h DPMI, GDT[3]/[4] DOS4GW aliases, synth descriptors, INT 67h, LMSW PE switch detect, native FAR JMP/RETF transfer | WIP |
| X-GUI   | Shared cross-arch GUI (gui/): elementaryOS-inspired desktop, AA text, Wingpanel, Plank dock, window decorations, NTP clock | Done |
| X-UTF   | Unicode subsystem (Plan 9-style 4 capas): libutf codec + PSF2 font loader + LRU cache + width-aware renderer + Latin-1 boot fallback. Cobertura: ASCII/Latin-1 link-time + Cyrillic/Greek/CJK/Hangul desde /fonts/*.psf en OsitoFS. Implementado en fork osito-x; ver [docs/unicode-architecture.md](docs/unicode-architecture.md) para port a mainline | WIP |
| A-ARM64 | AArch64/SM8350 (ROG Phone 5) bare-metal port: PL011 UART, GICv3, MMU paging, PCI ECAM, HDA, virtio-blk + OsitoFS v2, virtio-net + TCP/IP, syscall+ELF+process, crypto/TLS/HTTPS, multi-core SMP via PSCI, NEON SIMD tensor ops, GUI desktop task | Done |
| W-WASM  | WebAssembly/Emscripten port (`arch/wasm/`): kernel + shell + LLM inference + OsitoFS + GUI compositor + Quake 2 side-module via dlopen. **55/103 kernel modules** compilados (53%), 11 filesystems read-only, 5 protocolos de red bridgeados, persistencia IndexedDB+localStorage, PWA installable. Assets en R2 (`wasm.naranjositos.tech`) con CORS + COOP/COEP cross-origin isolation. `dlopen` valida arquitectura para no colgar Asyncify con ELF nativo. Ver [docs/wasm-port.md](docs/wasm-port.md) | Done |
| W-CC    | In-browser C/C++ compilation (`cc` shell command): clang.wasm 30M + lld.wasm 19M + wasi-sysroot 9M (libc, libc++) hosted on R2; lazy-loaded Web Worker; bridge `EM_JS(js_cc_kick/done/drain)` + Asyncify polling. Soporta `cc src.c` (tcc -run style), `cc src.c -o out.wasm` (guarda a OsitoFS), `exec out.wasm` (branch WASI en `proc_exec` que detecta presencia de `dylink.0` section vs WASI). IndexedDB cache (~57 MB toolchain) para arranque instantáneo en visitas siguientes. Ver [docs/wasm-cc.md](docs/wasm-cc.md) | Done |
| W-BRANDON | brandon-arch port en WASM: TinyLlama-derivative con block_sharing + DenseFormer DWA + Value Residual Learning + register tokens. SPM tokenizer drop-in. Default model: brandon-tiny-10m-instruct.f16.gguf (21 MB). Recipe sampling auto-aplicado (temp 0.7 + penalty 1.2 + ngram 3). Speed: 64 ms/tok con pre-dequant F16→F32 al boot + F16C/AVX2 nativo. Bug crítico fixed: `matvec` sin case F16 corrompía a ceros silenciosamente. Ver [docs/brandon-tiny-integration.md](docs/brandon-tiny-integration.md) | Done |
| W-NET   | WASM browser networking via JS bridges: `curl <url>` (JS fetch), `claude` REPL (Anthropic API + SSE streaming token-by-token), `ws open/send/recv/close/list` (WebSocket primitive), `tcp connect <host> <port>` (CF Worker proxy template-substituted), `https <host> [path]` (TLS 1.2 sobre WS-tunneled TCP), `crypto sha256/512`. tls.c + tls13.c linkeados; net_tcp_send/recv bridgeados a wasm_ws_*. CF Worker `tools/tcp-proxy-worker.js`. | Done |
| W-FS    | 12 filesystems montables en WASM via `mount-fs <type> <url>`: ositofs2 (primary), iso9660, fat32, ext2/3/4, exfat, ntfs, squashfs, hfsplus, btrfs, apfs, udf. **Multi-aux real** (one-of-each-type concurrent) — separate backing buffers indexed by FS type name, dispatch via g_active_aux_slot. VFS auto-mount: `cat /iso/foo`, `ls /fat/`, `cat /aux/file` (last-active wildcard). `umount [type]`. | Done |
| W-PERSIST | Persistencia browser-side: OsitoFS image se guarda a IndexedDB (`osito-fs/img/main`) en cada flush al final de cada comando shell; restore al boot antes de mount. Sampling config (temp/penalty/ngram/proxy URL) en localStorage. Survives reload — `git commit`, archivos `cc -o`, mounts y tunes persisten entre sesiones. | Done |
| W-PWA   | OsitoK installable como PWA: `manifest.json` (standalone display, theme color, ícono 192/512), Service Worker `sw.js` (cache-first para osito.{html,js,wasm}, network-first con cache fallback para R2 GGUF/img). Funciona offline tras primera carga. | Done |

> Full GPU roadmap (X27-X40 + contingency): see [docs/x86-gpu-roadmap.md](docs/x86-gpu-roadmap.md)
> Full OS roadmap (Tiers 0-9): see [docs/os-selfhost-roadmap.md](docs/os-selfhost-roadmap.md)
> Binary compatibility roadmap: see [docs/binary-compat-roadmap.md](docs/binary-compat-roadmap.md)
> Paths to Claude analysis: see [docs/paths-to-claude-on-ositok.md](docs/paths-to-claude-on-ositok.md)
> Kernel/bootloader separation + self-compiling road: see [docs/kernel-separation.md](docs/kernel-separation.md)
> Reference material (WRK, NT, DOS source): see [docs/reference-material.md](docs/reference-material.md)
> OsitoFS v2 host tools (9 tools): see `tools/ositofs/`
> Filesystem roadmap (12 current + 15 planned): see [docs/filesystem-roadmap.md](docs/filesystem-roadmap.md)
> VFS + demand paging + ETXTBSY architecture (2026-04-12 sweep): see [docs/x86-vfs-demand-paging.md](docs/x86-vfs-demand-paging.md)

**Tier 8: Hardware Boot** — Boot OsitoK on real hardware (AMD Ryzen 7 5800X + RTX 3090). **RESOLVED** (commit `3c02b49`: H1-H6 hardware boot working with USB keyboard).

### Hardware Boot Plan (target: WD SN740 512GB NVMe)

| Step | Task | Description | Deps | Status |
|------|------|-------------|------|--------|
| **H1** | **xHCI USB driver** | AMD 400/Matisse xHCI (`1022:43d5`, `1022:149c`). Enumerate ports, configure endpoints, USB HID for keyboard+mouse | None | Done |
| **H2** | **USB HID input** | Parse HID reports, scancode→ASCII, integrate with terminal line editor | H1 | Done |
| **H3** | **NVMe SN740 bring-up** | Test OsitoK NVMe driver with SN740 (`15b7:5016`, DRAM-less). May need CMB/SQ-in-CMB support | None | Done |
| **H4** | **Flash to NVMe** | Create GPT on nvme1n1: ESP partition (boot.efi + kernel.elf) + OsitoFS partition | H3 | Done |
| **H5** | **UEFI GOP display** | Verify framebuffer works on RTX 3090 GOP output (already obtained by boot.efi) | None | Done |
| **H6** | **Real hardware boot** | Boot from nvme1n1 via BIOS boot menu. Serial header for debug if needed | H1-H5 | Done |
| **H7** | **Nouveau modesetting** | Native display init for RTX 3090 (GA102). Resolution control, cursor | H6 | Pending |
| **H8** | **Documentation refresh** | Audit features marked WIP/pending vs actual implementation, update CLAUDE.md tables and roadmap docs | None | In progress (last audit 2026-04-25) |

**Critical path resolved**: H1 → H2 → H6 worked (USB keyboard unlocked interaction). Remaining: H7 native modesetting + ongoing H8 doc maintenance.

## Detailed Feature Documentation

For implementation details, API specifics, register-level documentation, and debugging notes:

- **[docs/x86-features-detail.md](docs/x86-features-detail.md)** — All x86-64 feature descriptions (X9-X42, X-OS*, X-NET*, X-CL*, X-WIN32, etc.)
- **[docs/x86-network-stack.md](docs/x86-network-stack.md)** — Sesión bring-up red bare-metal (B450-F + I211): pipes, kdownload/OFTP, nic_stats, MSI fixes (PCI cap aligned + GPIE.EIAME), RDT off-by-one, reply-path TX bug + workaround
- **[docs/session-2026-05-16-17-net-pki-overhaul.md](docs/session-2026-05-16-17-net-pki-overhaul.md)** — 26-commit TCP + TLS + PKI overhaul: RFC 2018 SACK (multi-block), RFC 7323 timestamps + PAWS, RFC 6298 Jacobson RTT, TLS 1.3 client real (X25519 + AES-128-GCM + HKDF), RSA-2048, ECDSA P-256/P-384, X.509 chain validation (link sig + constraints + validity + SAN + pin + AIA + OCSP + CRL), OCSP stapling client, 1.3-preferred HTTP dispatch with 1.2 fallback
- **[docs/kernel-demencial.md](docs/kernel-demencial.md)** — 10 features avanzadas: cpu_features, PMU counters, superpage tensor arena, multipath dispatch, sys_inference syscall, ASLR lite, predictive scheduling, speculative I/O, zero-copy SG TX, hardware breakpoints, self-optimizing kernel
- **[docs/unicode-architecture.md](docs/unicode-architecture.md)** — Subsistema Unicode 4 capas estilo Plan 9: libutf codec, PSF2 font loader desde OsitoFS, LRU cache, renderer width-aware (CJK 16×16), boot font Latin-1 link-time. Implementado y validado en fork osito-x; documento describe diseño completo + plan de port a mainline
- **[docs/kernel-diagram.md](docs/kernel-diagram.md)** — 5 diagramas Mermaid: arquitectura completa (111 archivos en 12 subsistemas), secuencia de boot (22 pasos), dispatch de syscalls, forward pass de inference, y mapa de integración de las 10 features
- **[docs/esp8266-detail.md](docs/esp8266-detail.md)** — Math library API, zForth integration, resource budget, DOOM/Elite details
- **[docs/aarch64-detail.md](docs/aarch64-detail.md)** — AArch64/SM8350 (ROG Phone 5) port details
- **[docs/wasm-port.md](docs/wasm-port.md)** — WebAssembly/Emscripten port: build, R2 asset hosting + CORS, subsystem map, dlopen arch-validation
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
