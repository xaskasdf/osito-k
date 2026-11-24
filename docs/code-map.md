# OsitoK — Code Map

> Per-directory file map for each architecture, moved out of `CLAUDE.md` on 2026-10-28. Not exhaustive: `ls` is the source of truth, and newer subtrees such as `arch/x86/dos/` and `arch/x86/lib/` are not listed here yet.

## ESP8266 (arch/xtensa/)
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

## x86-64 Bare-Metal AI OS (arch/x86/)
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

## Shared GUI (gui/)
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

## Host Tools (tools/)
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

## AArch64 Port (arch/arm/)
```
arch/arm/          SM8350 (ROG Phone 5) bare-metal port — see aarch64-detail.md
                   kernel/gui_task.c — desktop GUI scheduler task (renders to splash FB)
```
