# OsitoK — Roadmap & Feature Status

> Feature tables for every architecture, moved out of `CLAUDE.md` on 2026-10-28. When a feature lands or changes status, update its row here.

> **Documentation refresh — last audit 2026-05-17** (TCP/TLS/PKI overhaul session: X-TLS13 row clarified (real handshake e2e, AES-128-GCM not ChaCha), new rows X-PKI / X-OCSP / X-CRL / X-AIA / X-SACK / X-RFC7323 / X-RTO / X-TLS-NEG, all marked Done; see `session-2026-05-16-17-net-pki-overhaul.md` for the full chronology, RFC table, and primitive sizes. Prior 2026-04-25 audit covered Hardware Boot H1-H6 and the X-WIN32 / X-VGPU / X-MESA / X-DXVK / X-VK / X-DOS / X-MEMCOMP / X-VDSO / X-SPEC / X-KALL / X-AUDSCHED / X-DMASCHED / X-INOTIFY / X-CPUTOP / X-PERF / X-KSTATE / X-GUI / A-ARM64 entries.)

## ESP8266 (Xtensa LX106)

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

## x86-64 Bare-Metal AI OS

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
| X-MSI   | Native MSI/MSIX installer engine (mini-msiexec): OLE2/CFBF reader, MSI string-pool + _Columns + Directory/Component/File/Media/Registry tables, embedded CAB/MSZIP (zlib_inflate_raw cross-block window), ZIP/OPC + AppxManifest for MSIX. Extracts to OsitoFS via vfs_resolve(WIN32), applies Registry rows, writes uninstall manifest. Shell: `msi install`/`msiexec`/`msix` + `.msi`/`.msix` auto-detect in winexec. Validated end-to-end vs real test.msi+test.msix (host harness, byte-exact). Files: win32/{ole2,msi,cab,zip,msix,installer}.c | Done |
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
| X-TLS13 | TLS 1.3 client real (X25519 + AES-128-GCM + HKDF, Finished MAC, full handshake e2e contra CF). Since 2026-10-30 it authenticates the server: chain verified, CertificateVerify checked (ECDSA, RSA-PSS), revocation by OCSP/CRL, handshake messages reassembled across records; a failed authentication doesn't fall back to 1.2 | Done |
| X-PKI   | RFC 5280 chain validation, enforced since 2026-10-30: BearSSL 0.6's X.509 engine (`lib/bearssl`) against Mozilla's roots (`kernel/ca_roots.c`, `tools/ca-roots/gen-bearssl-anchors.py`): link signatures, constraints, validity, host name; SHA-1/MD5 refused. Before, x509.c's checks only logged their result and cert_pin knew three pins. TLS 1.2 checks the ServerKeyExchange signature and revocation (OCSP, else CRL) too, keys come from an HMAC-DRBG over RDRAND (`kernel/random_drbg.c`) | Done |
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
| X-UTF   | Unicode subsystem (Plan 9-style 4 capas): libutf codec + PSF2 font loader + LRU cache + width-aware renderer + Latin-1 boot fallback. Cobertura: ASCII/Latin-1 link-time + Cyrillic/Greek/CJK/Hangul desde /fonts/*.psf en OsitoFS. Implementado en fork osito-x; ver [docs/unicode-architecture.md](unicode-architecture.md) para port a mainline | WIP |
| A-ARM64 | AArch64/SM8350 (ROG Phone 5) bare-metal port: PL011 UART, GICv3, MMU paging, PCI ECAM, HDA, virtio-blk + OsitoFS v2, virtio-net + TCP/IP, syscall+ELF+process, crypto/TLS/HTTPS, multi-core SMP via PSCI, NEON SIMD tensor ops, GUI desktop task | Done |
| A-ROG5  | Boot on the physical ROG Phone 5 through the sm8350-boot shim. As of 2026-10-28 it boots to `shell>`, with timer IRQs and context switches running (blinking heartbeat). The shell is interactive over a polled DWC3 CDC-ACM gadget on the side USB-C port (a COM port on the PC). The screen shows progress bars, a bit-encoded panic band and a 2× text console. New kernels load over USB (`kload`) without flashing, and the kernel relocates itself (PIE linked at 0). MMU and caches are on: a flat map with an uncached DMA window and framebuffer. Tasks run on all 8 cores (4× A55, 3× A78, X1): one shared run queue, tasks pinned to a core or free to move, idle CPUs woken by IPI (4 µs to start a task on another core), unpinned busy tasks on the big cores and light ones on the A55s (per-task utilization); `cpubench` shows them in parallel. Every cluster runs at its top frequency (EPSS): 1.8 / 2.42 / 2.84 GHz; a thermal task reads the TSENS sensors (`temp`) and caps the big clusters together (they share a voltage rail) when they run hot. The Power and volume keys work over SPMI: the volume keys page the screen back through the console log, both held reboot into fastboot, Power prints a status. The console takes the whole panel; the progress bars and heartbeat squares are a debug mode (`SCREEN_DEBUG`, shell `screen`). UFS storage: reads anywhere (LUNs, GPT, raw blocks, `ufs`), writes only into `userdata`, which holds an OsitoFS v3 with directories (x86's driver; formatted in-kernel, `ufs migrate` converts a v2; `cd`, `mkdir`, `mv`, `rm`) mounted at boot, and the shell works like x86's: pipes, `>`/`>>`/`<`, `cp`/`head`/`tail`/`grep`/`wc`, history and Tab completion, plus `;`/`&&`/`||`, wildcards and here documents; files move over USB both ways (`put`, `get`, verified by CRC), `screenshot` saves the panel (`rog5-screenshot.ps1` brings it to the PC as a PNG), and C programs run from it (`exec`): a small libc and `rog5-app.sh` build them as static PIEs the kernel relocates. `desktop` shows the shared GUI in its phablet layout (portrait, 300%) with the console inside its Terminal window. The touchscreen works: its supplies are voted on through RPMh (AOP command DB + apps RSC), the FocalTech controller is read over bit-banged I2C (its QUP SE is GPI-DMA only), and taps and swipes drive the desktop. All 16 GB of RAM is in use, less carve-outs (15 GB high memory beyond the kernel's 208 MB). The local LLM runs: x86's inference stack with NEON kernels, SDOT on an 8-bit copy of the input for quantized weights, and matvecs split over the X1 and the three A78s, with models loaded from UFS into high memory (`model`, `chat`). SmolLM2-135M runs at ~100 tok/s and brandon-tiny at ~165 tok/s. USB networking: `net up` makes the gadget an ACM + CDC-NCM composite that Windows' inbox drivers take. DHCP or link-local addressing then lets PC and phone ping each other. Through Windows ICS the phone reaches the internet: DNS, NTP and HTTPS over TLS 1.3 or 1.2 that authenticates the server (BearSSL's X.509 engine against Mozilla's roots, the handshake signature checked, revocation by the leaf's CRL, keys from the hardware PRNG through an HMAC-DRBG). `rog5-boot.sh --flash` persists a build without buttons (`reboot bootloader` via the PMIC over SPMI). See [arch/arm/BUILDING-ROG5.md](../arch/arm/BUILDING-ROG5.md) | WIP |
| W-WASM  | WebAssembly/Emscripten port (`arch/wasm/`): kernel + shell + LLM inference + OsitoFS + GUI compositor + Quake 2 side-module via dlopen. **55/103 kernel modules** compilados (53%), 11 filesystems read-only, 5 protocolos de red bridgeados, persistencia IndexedDB+localStorage, PWA installable. Assets en R2 (`wasm.naranjositos.tech`) con CORS + COOP/COEP cross-origin isolation. `dlopen` valida arquitectura para no colgar Asyncify con ELF nativo. Ver [docs/wasm-port.md](wasm-port.md) | Done |
| W-CC    | In-browser C/C++ compilation (`cc` shell command): clang.wasm 30M + lld.wasm 19M + wasi-sysroot 9M (libc, libc++) hosted on R2; lazy-loaded Web Worker; bridge `EM_JS(js_cc_kick/done/drain)` + Asyncify polling. Soporta `cc src.c` (tcc -run style), `cc src.c -o out.wasm` (guarda a OsitoFS), `exec out.wasm` (branch WASI en `proc_exec` que detecta presencia de `dylink.0` section vs WASI). IndexedDB cache (~57 MB toolchain) para arranque instantáneo en visitas siguientes. Ver [docs/wasm-cc.md](wasm-cc.md) | Done |
| W-BRANDON | brandon-arch port en WASM: TinyLlama-derivative con block_sharing + DenseFormer DWA + Value Residual Learning + register tokens. SPM tokenizer drop-in. Default model: brandon-tiny-10m-instruct.f16.gguf (21 MB). Recipe sampling auto-aplicado (temp 0.7 + penalty 1.2 + ngram 3). Speed: 64 ms/tok con pre-dequant F16→F32 al boot + F16C/AVX2 nativo. Bug crítico fixed: `matvec` sin case F16 corrompía a ceros silenciosamente. Ver [docs/brandon-tiny-integration.md](brandon-tiny-integration.md) | Done |
| W-NET   | WASM browser networking via JS bridges: `curl <url>` (JS fetch), `claude` REPL (Anthropic API + SSE streaming token-by-token), `ws open/send/recv/close/list` (WebSocket primitive), `tcp connect <host> <port>` (CF Worker proxy template-substituted), `https <host> [path]` (TLS 1.2 sobre WS-tunneled TCP), `crypto sha256/512`. tls.c + tls13.c linkeados; net_tcp_send/recv bridgeados a wasm_ws_*. CF Worker `tools/tcp-proxy-worker.js`. | Done |
| W-FS    | 12 filesystems montables en WASM via `mount-fs <type> <url>`: ositofs2 (primary), iso9660, fat32, ext2/3/4, exfat, ntfs, squashfs, hfsplus, btrfs, apfs, udf. **Multi-aux real** (one-of-each-type concurrent) — separate backing buffers indexed by FS type name, dispatch via g_active_aux_slot. VFS auto-mount: `cat /iso/foo`, `ls /fat/`, `cat /aux/file` (last-active wildcard). `umount [type]`. | Done |
| W-PERSIST | Persistencia browser-side: OsitoFS image se guarda a IndexedDB (`osito-fs/img/main`) en cada flush al final de cada comando shell; restore al boot antes de mount. Sampling config (temp/penalty/ngram/proxy URL) en localStorage. Survives reload — `git commit`, archivos `cc -o`, mounts y tunes persisten entre sesiones. | Done |
| W-PWA   | OsitoK installable como PWA: `manifest.json` (standalone display, theme color, ícono 192/512), Service Worker `sw.js` (cache-first para osito.{html,js,wasm}, network-first con cache fallback para R2 GGUF/img). Funciona offline tras primera carga. | Done |

> Full GPU roadmap (X27-X40 + contingency): see [docs/x86-gpu-roadmap.md](x86-gpu-roadmap.md)
> Full OS roadmap (Tiers 0-9): see [docs/os-selfhost-roadmap.md](os-selfhost-roadmap.md)
> Binary compatibility roadmap: see [docs/binary-compat-roadmap.md](binary-compat-roadmap.md)
> Paths to Claude analysis: see [docs/paths-to-claude-on-ositok.md](paths-to-claude-on-ositok.md)
> Kernel/bootloader separation + self-compiling road: see [docs/kernel-separation.md](kernel-separation.md)
> Reference material (WRK, NT, DOS source): see [docs/reference-material.md](reference-material.md)
> OsitoFS v2 host tools (9 tools): see `tools/ositofs/`
> Filesystem roadmap (12 current + 15 planned): see [docs/filesystem-roadmap.md](filesystem-roadmap.md)
> VFS + demand paging + ETXTBSY architecture (2026-04-12 sweep): see [docs/x86-vfs-demand-paging.md](x86-vfs-demand-paging.md)

**Tier 8: Hardware Boot** — Boot OsitoK on real hardware (AMD Ryzen 7 5800X + RTX 3090). **RESOLVED** (commit `3c02b49`: H1-H6 hardware boot working with USB keyboard).

## Hardware Boot Plan (target: WD SN740 512GB NVMe)

| Step | Task | Description | Deps | Status |
|------|------|-------------|------|--------|
| **H1** | **xHCI USB driver** | AMD 400/Matisse xHCI (`1022:43d5`, `1022:149c`). Enumerate ports, configure endpoints, USB HID for keyboard+mouse | None | Done |
| **H2** | **USB HID input** | Parse HID reports, scancode→ASCII, integrate with terminal line editor | H1 | Done |
| **H3** | **NVMe SN740 bring-up** | Test OsitoK NVMe driver with SN740 (`15b7:5016`, DRAM-less). May need CMB/SQ-in-CMB support | None | Done |
| **H4** | **Flash to NVMe** | Create GPT on nvme1n1: ESP partition (boot.efi + kernel.elf) + OsitoFS partition | H3 | Done |
| **H5** | **UEFI GOP display** | Verify framebuffer works on RTX 3090 GOP output (already obtained by boot.efi) | None | Done |
| **H6** | **Real hardware boot** | Boot from nvme1n1 via BIOS boot menu. Serial header for debug if needed | H1-H5 | Done |
| **H7** | **Nouveau modesetting** | Native display init for RTX 3090 (GA102). Resolution control, cursor | H6 | Pending |
| **H8** | **Documentation refresh** | Audit features marked WIP/pending vs actual implementation, update the tables in this file and the other roadmap docs | None | In progress (last audit 2026-04-25) |

**Critical path resolved**: H1 → H2 → H6 worked (USB keyboard unlocked interaction). Remaining: H7 native modesetting + ongoing H8 doc maintenance.
