```
+============================================================================+
|                                                                            |
|                         O S I T O - K   v 0 . 4                            |
|                                                                            |
|              MULTI-ARCHITECTURE BARE-METAL OPERATING SYSTEM                |
|                                                                            |
|         FOR XTENSA LX106 ▪ x86-64 ▪ AArch64 (SM8350) PROCESSORS            |
|                                                                            |
|                        OPERATOR'S REFERENCE MANUAL                         |
|                                                                            |
|                          REVISION 4 -- MAY 2026                            |
|                                                                            |
+============================================================================+
```

                            *** IMPORTANT NOTICE ***

      This manual describes the installation, configuration, and operation
      of the Osito-K Bare-Metal Operating System, Version 0.4. The system
      now spans three (3) processor architectures and approximately one
      hundred sixty thousand (160,000) lines of source code. The operator
      should not attempt to read this manual in its entirety in a single
      session. Improper exposure to kernel source code may result in
      unpredictable behavior of the operator.


## TABLE OF CONTENTS

```
  SECTION                                                            PAGE
  -------                                                            ----
  I.    SYSTEM OVERVIEW ........................................      1
  II.   SUPPORTED ARCHITECTURES ................................      2
  III.  QUICK INSTALLATION (PER ARCHITECTURE) ..................      3
  IV.   x86-64 BARE-METAL AI/GAMING OS .........................      4
  V.    XTENSA LX106 EMBEDDED KERNEL ...........................      5
  VI.   AArch64 (ROG PHONE 5 / SM8350) PORT ....................      6
  VII.  SELECTED FEATURES OF NOTE ..............................      7
  VIII. SOURCE TREE LAYOUT .....................................      8
  IX.   DOCUMENTATION INDEX ....................................      9
  X.    PROJECT STATISTICS .....................................     10
  XI.   WARRANTY AND DISCLAIMER ................................     11
```

---


## I. SYSTEM OVERVIEW

Osito-K (from Spanish *osito*, "little bear") is a **bare-metal operating
system** authored without dependence on any vendor SDK or third-party
runtime. A [naranjositos.tech](https://naranjositos.tech/) project. It
began in 2025 as a 4 KB preemptive kernel for the ESP8266 microcontroller
and has, through methodical accretion, grown to encompass three
processor architectures, hundreds of kernel modules, an in-kernel large
language model inference engine, twelve filesystem drivers, a Vulkan
graphics stack, a Win32 PE32 compatibility layer, and a small library
of vintage 1990s video games running on top of the latter.

The unifying philosophy is the same on every platform:

  - **No SDK**. The system speaks to hardware registers directly.
  - **No floating-point reliance** for core paths. Fixed-point and
    integer math wherever practical.
  - **Inspectable behavior**. Every subsystem emits diagnostic output
    on a serial port. The operator may always determine what the
    system is doing and why.
  - **Self-hosting where feasible**. The x86-64 build can compile its
    own kernel from source while running, then `kexec` into the new
    image without rebooting.


## II. SUPPORTED ARCHITECTURES

```
+==========================================================================+
|                  PROCESSOR ARCHITECTURES SUPPORTED                       |
+==========================================================================+
|                                                                          |
|  ARCH         CPU FAMILY                  REPRESENTATIVE TARGET           |
|  ----         ----------                  --------------------            |
|                                                                          |
|  xtensa       Tensilica Xtensa LX106      Wemos D1 Mini (ESP8266)        |
|               80 MHz, 80+32 KB RAM        4 MB SPI flash                 |
|                                                                          |
|  x86-64       AMD Ryzen / Intel Core      Ryzen 7 5800X + RTX 3090       |
|               (UEFI), AVX2/FMA/F16C        32 GB DRAM, NVMe              |
|                                                                          |
|  arm64        ARMv8.2-A (SM8350)          ASUS ROG Phone 5 (in progress) |
|               Snapdragon 888              16 GB DRAM, UFS 3.1            |
|                                                                          |
+==========================================================================+
```

The same source tree builds for all three. Architecture-specific code
lives under `arch/<arch>/`; truly shared code (the GUI, OsitoFS on-disk
format, Unicode subsystem) lives at the top level under `gui/` and
`include/common/`.


## III. QUICK INSTALLATION (PER ARCHITECTURE)

The operator should select the appropriate procedure for the target
hardware. The three procedures are mutually independent.

```
+--- xtensa (ESP8266) -----------------------------------------------------+

    export PATH="$PWD/arch/xtensa/tools/xtensa-lx106-elf/bin:$PATH"
    make
    make flash    # via /dev/ttyUSB0 (or COMx on Windows)

+--- x86-64 ---------------------------------------------------------------+

    sudo apt install gnu-efi              # or brew install x86_64-elf-gcc
    make -C arch/x86                      # produces boot.efi + kernel.elf
    make -C tools/ositofs                 # host tools to populate FS image
    tools/smoke-test.sh                   # boot in QEMU headless 30 sec

+--- arm64 (SM8350) -------------------------------------------------------+

    make -C arch/arm                      # produces kernel.img
    # Flash to userdata partition via fastboot — see docs/aarch64-detail.md
```

Refer to the per-architecture section below for build flag inventory,
boot sequence, and known limitations.


## IV. x86-64 BARE-METAL AI/GAMING OS

The x86-64 build is the largest and most actively developed. It boots
via UEFI on physical hardware (verified on AMD Ryzen 7 5800X) and in
QEMU. The kernel ships approximately one hundred (100) modules, twenty
(20) device drivers, and sixteen (16) filesystem implementations.

**Selected capabilities:**

  - **In-kernel LLM inference**. Llama 3.2 1B forward pass on CPU
    (AVX2/FMA tensor kernels) and on GPU (NVIDIA Ampere via the kernel
    NVK backend, when GSP firmware is present). Exposed as the
    `sys_inference` syscall family (530-534) and as a UDP prompt
    server on port 7777. See `docs/x86-features-detail.md`.

  - **Win32 PE32 compatibility**. Twelve DLL shims (kernel32, msvcrt,
    user32, gdi32, advapi32, comctl32, comdlg32, ddraw, dsound, ntdll,
    ole32, shell32, winmm, wsock32) enable execution of unmodified
    Windows binaries in long-mode-thunked compatibility. Active
    debugging targets: Unreal Tournament '99, Grand Theft Auto V.
    See `docs/binary-compat-roadmap.md`.

  - **DOS-native execution.** VCPI server, INT 31h DPMI host, and
    long-mode-to-protected-mode descriptor synthesis allow `DOOM.EXE`
    and similar DOS4GW-protected-mode programs to run with the kernel
    serving as the DPMI/VCPI provider. See `docs/dos-native-status.md`.

  - **Vulkan + OpenGL.** Mesa 25.0.0 with Zink (OpenGL-on-Vulkan)
    ported into the OS; NVK userland ICD plus Venus protocol bridge
    for virtio-gpu hosts. Software rasterizer for guest-local present.
    See `docs/x86-gpu-roadmap.md`.

  - **Unicode subsystem**. Plan 9-style 4-layer architecture (libutf
    codec + PSF2 fonts from OsitoFS + LRU cache + width-aware
    renderer). ASCII/Latin-1 always available link-time; Cyrillic /
    Greek / CJK / Hangul on demand from `/fonts/*.psf`. See
    `docs/unicode-architecture.md`.

  - **Self-hosting compilation.** The kernel can rebuild itself from
    source while running (TCC in-OS) and `kexec` into the new image.
    Phases 0-4 of the kernel/bootloader separation are complete. See
    `docs/kernel-separation.md`.

  - **GPU compute pipeline.** Native GSP Falcon bring-up phases 1-10
    on physical Ampere, FWSEC-FRTS, WPR2/Radix3, GMMU, channels,
    GPFIFO, SASS kernel codegen. See `docs/x86-gpu-roadmap.md`.

  - **Twelve filesystems.** OsitoFS v2/v3 native, FAT32 R/W, tmpfs,
    ext2/3/4 RO, ISO 9660 RO, exFAT RO, NTFS RO, UDF RO, SquashFS RO,
    HFS+ RO, Btrfs RO, APFS RO. See `docs/filesystem-roadmap.md`.

  - **Networking.** ARP, IPv4, IPv6, ICMP, UDP, TCP (with fast
    retransmit), DNS, DHCP, NTP, mDNS, TLS 1.2, TLS 1.3, HTTPS client
    + server, BSD sockets, Wayland stub.

  - **Container primitives.** PID/mount/net/UTS/IPC namespaces,
    cgroups (CPU quota + memory limits), Linux capabilities (38
    caps), seccomp BPF.

  - **Linux kernel parity items.** dmesg, sysctl, sysfs, procfs,
    epoll, eventfd, FUSE, io_uring, eBPF VM, tracepoints, RCU, slab,
    workqueue, lockdep, PSI, kobject, kallsyms, kprof.

For the complete x86-64 capabilities table (140+ entries) see
`CLAUDE.md`. For implementation details see `docs/x86-features-detail.md`.


## V. XTENSA LX106 EMBEDDED KERNEL

The original Osito-K. A preemptive multitasking kernel for the
Espressif ESP8266 microcontroller, running without the Espressif SDK.

**Selected capabilities:**

  - Priority-based preemptive scheduling at 100 Hz with full register
    save/restore via the FRC1 timer interrupt at vector base 0x50.
  - Two-tier memory allocation: 256-block × 32-byte pool + 8 KB
    first-fit heap with coalescing.
  - Three IPC primitives: counting semaphores, message queues,
    software timers.
  - Flat filesystem on SPI flash, contiguous allocation, up to 128
    files in 3.8 MB.
  - 25+ built-in shell commands.
  - **Elite flight demo.** Wireframe 3D rendering of Cobra,
    Sidewinder, Viper, and Coriolis ship models on a 128×64
    framebuffer with fixed-point 16.16 math and 3×3 matrix rotation.
  - **DOOM (wireframe 2.5D).** A procedurally-generated BSP-style
    engine fits in approximately 3.5 KB of IRAM.
  - **zForth.** A complete Forth interpreter with hardware syscalls
    for framebuffer and scheduler control. Scripts persist on the
    filesystem.

**Build configuration flags** (passed on the `make` command line):

```
  ENABLE_ELITE=1   Elite flight demo + ship models  (~2.1 KB IRAM)
  ENABLE_FORTH=1   zForth interactive language       (~4.2 KB IRAM)
  ENABLE_DOOM=0    DOOM wireframe engine             (~3.5 KB IRAM)
```

For the original full operator's manual covering the embedded build
in detail (boot sequence, console commands, IPC primitives, file
operations, Elite controls, zForth syntax) see
`docs/esp8266-detail.md`. The serial console operates at 74880 baud
during ROM bootloader phase, 115200 baud during kernel operation.

      NOTE: The 80 MHz clock provides ample processing power for the
      simultaneous execution of up to eight (8) independent programs.
      The operator should not be concerned about resource exhaustion
      under normal workloads.


## VI. AArch64 (ROG PHONE 5 / SM8350) PORT

The newest of the three architectures. A bare-metal port to the
Qualcomm Snapdragon 888 (SM8350) running on a stock ASUS ROG Phone 5,
delivered as a kernel image flashable via fastboot.

**Selected capabilities:**

  - PL011 UART for early debug output.
  - GICv3 interrupt controller, GIC distributor + redistributor + ITS.
  - 4-level MMU paging with 4 KB granule, identity + higher-half map.
  - PCIe ECAM probing.
  - HDA audio (over SoC audio block), virtio-blk, virtio-net.
  - OsitoFS v2 with the same on-disk format as x86-64.
  - TCP/IP stack identical to x86-64 (cross-arch share).
  - Syscall/ELF/process subsystems.
  - Crypto / TLS / HTTPS client.
  - Multi-core SMP via PSCI CPU_ON.
  - NEON SIMD tensor kernels for inference.
  - GUI desktop task rendering to the splash framebuffer (same
    `gui/` cross-arch implementation as x86-64).

See `docs/aarch64-detail.md` and `docs/arm64-future-work.md`.


## VII. SELECTED FEATURES OF NOTE

This section highlights features whose implementation may be of
particular educational interest. The reader is referred to the
linked documents for full design rationale.

**Unicode (`docs/unicode-architecture.md`).** Four-layer Plan 9-style
design. The kernel ships a minimum boot font (ASCII + Latin-1, ~3 KB
link-time) and loads PSF2 font files from `/fonts/` on OsitoFS for
broader coverage. CJK 16×16 glyphs are advanced as two cells in
terminal column counting. The renderer falls through five tiers
gracefully — a kernel without a filesystem still boots and shows
español, français, deutsch correctly.

**In-kernel LLM inference (`docs/x86-features-detail.md`).** The
forward pass of Llama 3.2 1B runs in ring 0 with the model weights
mmapped from a GGUF file on OsitoFS. Tensor scratch space lives in a
dedicated 512 MB superpage arena (`tensor_arena`) on 256 × 2 MB
pages — measurably reduces TLB pressure during attention computation.
Boot-time CPUID dispatch picks AVX2/FMA, AVX-512, or scalar
implementations. PMU counters (3 fixed + 4 PMC) provide IPC and
cache miss measurements.

**Demand-paged ELF loader (`docs/x86-vfs-demand-paging.md`).** PT_LOAD
segments are not eagerly loaded; a VMA_FILE_ELF region records the
file mapping and the page-fault handler pulls in pages on first
touch. Reduces working set size for cold binaries and speeds up
exec on large ELFs.

**Self-optimizing kernel** (`docs/kernel-demencial.md` §10). Live
kernel-text patching after boot stabilizes. Static branch sites are
registered at boot; once 1000 ticks have elapsed without panic, the
kernel can patch hot branches into unconditional jumps. Disabled by
default behind a compile-time flag.

**Speculative I/O prefetch** (`docs/kernel-demencial.md` §7). The VFS
layer records "after opening A, B is opened next" patterns and, on
subsequent opens of A, dispatches a prefetch of B to an idle AP via
`smp_submit_ff`. Measurable wins during compilation workloads.

**Hardware breakpoints / watchpoints** (`docs/kernel-demencial.md`
§6). DR0–DR3 exposed via the `watch` / `unwatch` / `hwbp` shell
commands. Used during this project to confirm or rule out memory
corruption suspicions without external JTAG.

For the complete list of advanced kernel features see
`docs/kernel-demencial.md`.


## VIII. SOURCE TREE LAYOUT

```
osito-k/
├── arch/
│   ├── xtensa/             ESP8266 source tree (boot, kernel, drivers,
│   │                       OsitoFS, Forth, DOOM, Elite, shell, math,
│   │                       3D wireframe, framebuffer)
│   ├── x86/                x86-64 source tree:
│   │   ├── boot/             UEFI bootloader (boot.efi)
│   │   ├── kernel/           ~100 kernel modules
│   │   ├── drivers/          NVMe, NIC, GPU, GSP, USB, HDA, virtio, ...
│   │   ├── fs/               12 filesystem drivers
│   │   ├── win32/            PE32 compatibility layer
│   │   ├── dos/              DOS/DPMI host
│   │   ├── lib/utf/          Unicode codec (libutf)
│   │   ├── lib/vulkan/       Vulkan loader + ICDs (NVK + Venus)
│   │   ├── lib/opengl/       Mesa 25.0.0 + Zink for in-kernel OpenGL
│   │   ├── libc/             Minimal CRT + extended POSIX
│   │   └── test/             Userspace test programs
│   └── arm/                AArch64/SM8350 port
├── gui/                    Cross-architecture GUI (desktop, panel,
│                           dock, AA text rendering, NTP-synced clock)
├── include/common/         Shared headers (OsitoFS format, UTF-8 codec,
│                           font subsystem) — used by host tools too
├── tools/
│   ├── ositofs/            9 host tools (mkfs, write, ls, info, ...)
│   ├── font/               PSF2 generator from GNU Unifont
│   ├── smoke-test.sh       QEMU smoke test (30 sec headless)
│   └── smoke-screenshot.sh QEMU + screendump via monitor socket
├── docs/                   31+ Markdown design documents
├── CLAUDE.md               Developer notes and complete feature roster
└── README.md               This file
```


## IX. DOCUMENTATION INDEX

The `docs/` directory contains design rationale, implementation
notes, and roadmaps for the major subsystems. The operator may wish
to consult the following entry points:

```
+----------------------------------+--------------------------------------+
| DOCUMENT                         | TOPIC                                |
+----------------------------------+--------------------------------------+
| CLAUDE.md                        | Master feature table + dev notes     |
| docs/x86-features-detail.md      | Per-feature deep dives (x86-64)      |
| docs/kernel-demencial.md         | 10 advanced kernel features          |
| docs/kernel-diagram.md           | 5 Mermaid diagrams of architecture   |
| docs/kernel-architecture.md      | Subsystem-level overview             |
| docs/kernel-separation.md        | Self-hosting + kexec road            |
| docs/x86-gpu-roadmap.md          | GSP / Vulkan / NVK / Mesa            |
| docs/binary-compat-roadmap.md    | Win32 PE / DOS / GTAV / UT99         |
| docs/unicode-architecture.md     | UTF-8 + PSF2 fonts + renderer        |
| docs/filesystem-roadmap.md       | 12 current + 15 planned filesystems  |
| docs/x86-vfs-demand-paging.md    | VFS unified layer + ELF demand-page  |
| docs/os-selfhost-roadmap.md      | Self-hosting tiers 0-9               |
| docs/wsl-kernel-wishlist.md      | Future native WSL2 kernel spike      |
| docs/aarch64-detail.md           | ROG Phone 5 / SM8350 port            |
| docs/esp8266-detail.md           | Original ESP8266 manual              |
| docs/ositofs2-spec.md            | OsitoFS v2 on-disk format            |
| docs/ositofs3-spec.md            | OsitoFS v3 on-disk format            |
+----------------------------------+--------------------------------------+
```


## X. PROJECT STATISTICS

```
+==========================================================================+
|                                                                          |
|  ARCHITECTURES SUPPORTED ........................ 3                     |
|  TOTAL SOURCE LINES (kernel only) ............. ~165,000                |
|  KERNEL MODULES (x86-64) ....................... ~100                    |
|  DEVICE DRIVERS (x86-64) ....................... ~22                     |
|  FILESYSTEM DRIVERS ............................ 12 + tmpfs              |
|  DESIGN DOCUMENTS .............................. 31                      |
|  HOST TOOLS .................................... 14                      |
|                                                                          |
|  LARGEST CALLABLE BINARY (UT99) ................ ~30 MB                  |
|  LLM MODEL SIZE (Llama 3.2 1B Q4_0) ............ ~700 MB GGUF            |
|  UNIFONT 15 FULL BMP TIER ...................... 1.4 MB                 |
|  KERNEL .text + .data (x86-64 stripped) ........ ~6.5 MB                 |
|                                                                          |
|  ESP8266 BUILD (DOOM + Elite + Forth) .......... ~70 KB ELF              |
|  ESP8266 IRAM HEADROOM ......................... ~28 KB                  |
|                                                                          |
|  FLOATING-POINT UNITS REQUIRED FOR INFERENCE ... 0 (Q4_0 quantized)      |
|  BEARS HARMED IN THE MAKING OF THIS KERNEL ..... 0                       |
|  ORANGE CATS HARMED IN THE MAKING OF THIS ...... 0                       |
|                                                                          |
+==========================================================================+
```


## XI. WARRANTY AND DISCLAIMER

```
+============================================================================+
|                                                                            |
|  THIS SOFTWARE IS PROVIDED TO THE OPERATOR "AS IS" WITHOUT WARRANTY        |
|  OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE         |
|  WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE,          |
|  OR NON-INFRINGEMENT OF INTELLECTUAL PROPERTY RIGHTS.                      |
|                                                                            |
|  THE AUTHORS SHALL NOT BE HELD LIABLE FOR ANY DAMAGES ARISING FROM         |
|  THE USE OF THIS SOFTWARE, INCLUDING BUT NOT LIMITED TO DAMAGE TO          |
|  THE COMPUTING HARDWARE, LOSS OF DATA, LOSS OF PRODUCTIVITY, OR            |
|  EXISTENTIAL DREAD EXPERIENCED WHILE DEBUGGING EXCEPTION VECTORS           |
|  AT THREE O'CLOCK IN THE MORNING.                                          |
|                                                                            |
|  THE x86-64 KERNEL EXECUTES UNMODIFIED WINDOWS PE32 BINARIES VIA           |
|  A COMPATIBILITY LAYER. THE AUTHORS MAKE NO REPRESENTATIONS REGARDING      |
|  THE FITNESS OF SUCH BINARIES, THEIR LICENSING, OR THE WISDOM OF           |
|  EXECUTING SOFTWARE PRODUCED IN 1999 ON HARDWARE PRODUCED IN 2026.         |
|                                                                            |
|  THE FILESYSTEMS STORE DATA ON FLASH MEMORY (xtensa) AND NVMe              |
|  STORAGE (x86-64) WITH FINITE WRITE CYCLES. THE AUTHORS ACCEPT             |
|  NO RESPONSIBILITY FOR DATA LOSS DUE TO MEDIA WEAR, POWER                  |
|  INTERRUPTION DURING WRITE OPERATIONS, COSMIC RAY-INDUCED BIT FLIPS,       |
|  OR THE OPERATOR'S FAILURE TO MAINTAIN ADEQUATE BACKUPS OF                 |
|  IRREPLACEABLE FILES CONTAINING ~165,000 LINES OF KERNEL CODE.             |
|                                                                            |
|  THE LARGE LANGUAGE MODEL EMBEDDED IN THE x86-64 BUILD MAY GENERATE        |
|  TEXT WHICH IS FACTUALLY INCORRECT, GRAMMATICALLY UNUSUAL, OR              |
|  SYNTHESIZED FROM THE COMBINED LITERATURE OF THE PUBLIC INTERNET.          |
|  THE OPERATOR SHALL NOT RELY ON SUCH TEXT FOR MEDICAL, LEGAL, OR           |
|  NAVIGATIONAL DECISIONS.                                                   |
|                                                                            |
|  Osito-K IS A PROJECT OF NARANJOSITOS.TECH                                 |
|  https://naranjositos.tech/                                                |
|                                                                            |
+============================================================================+
```


---

```
  Osito-K v0.4
  Copyright (C) 2025-2026 naranjositos.tech
  All rights reserved.

  https://naranjositos.tech/

  Three architectures.  ~165,000 lines of source code.
  One LLM running in ring 0.  Twelve filesystems.
  Four spaceships.  No floating-point units required.

  No bears were harmed in the making of this kernel.
  No orange cats were harmed either.

  The 80 MHz clock speed is a nominal value.  Actual performance
  may vary depending on ambient temperature, cosmic ray flux, and
  the general disposition of the hardware on any given day.
```
