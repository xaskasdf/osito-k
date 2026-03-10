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
                   shell.c — 18+ builtins
                   shm.c, compositor.c, display.c, input_events.c, memcompress.c
drivers/           nvme.c — NVMe read/write
                   gpu.c, gsp.c — NVIDIA GPU + GSP Falcon (Phases 1-10)
                   sass.c, gmmu.c — SASS kernels + GPU MMU
                   gpu_tensor.c, gpu_inference.c — GPU compute dispatch
                   gpu_display.c — GPU display engine
                   ahci.c, ccp.c, xhci.c — SATA, AMD TRNG, USB 3.x
win32/             pe.c, winexec.c — PE32 loader + execution
                   compat32.c, int2e_stub.S — 32→64 mode switching (INT 0x2E)
                   dllloader.c — 15 DLL shims (kernel32, msvcrt, user32, etc.)
                   ntsyscall.c, handle.c — NT syscalls + handle table
fs/                ositofs2.c, gpt.c, gguf.c — OsitoFS v2 + GPT + GGUF
libc/              crt.c, syscall.S, tcclib.c, math.c — CRT + extended libc
                   qjs_main.c, qjs_headers/ — QuickJS REPL
                   ositok.h — single-header libc for self-compiled programs
test/              various test programs + qjs.elf
```

### Host Tools (tools/)
```
tools/ositofs/     mkfs.c, write.c, ls.c, info.c — OsitoFS v2 host tools
include/common/    ositofs2_format.h — shared on-disk format
```

### AArch64 Port (arch/arm/)
```
arch/arm/          SM8350 (ROG Phone 5) bare-metal port — see docs/aarch64-detail.md
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
| X-SELF  | Self-hosting C compilation (cc -run) | Done |
| X-QOS   | QoS priority scheduler (5 classes) | Done |
| X-CCP   | AMD CCP TRNG driver | Done |
| X-AHCI  | SATA AHCI driver | WIP |
| X-XHCI  | xHCI USB 3.x driver | WIP |
| X-RETINA| Display pipeline (compositor, shared memory) | WIP |
| X-WIN32 | Windows PE32 compat layer (15 DLL shims) | WIP |
| **Phase 0** | **Kernel/bootloader separation** (boot.efi + kernel.elf) | **Done** |
| Phase 1 | TCC cross-compiles kernel from host | Next |
| Phase 2 | TCC compiles kernel inside OsitoK | Planned |
| Phase 3 | Install + reboot self-compiled kernel | Planned |

> Full GPU roadmap (X27-X40 + contingency): see [docs/x86-gpu-roadmap.md](docs/x86-gpu-roadmap.md)
> Full OS roadmap (Tiers 0-9): see [docs/os-selfhost-roadmap.md](docs/os-selfhost-roadmap.md)
> Binary compatibility roadmap: see [docs/binary-compat-roadmap.md](docs/binary-compat-roadmap.md)
> Paths to Claude analysis: see [docs/paths-to-claude-on-ositok.md](docs/paths-to-claude-on-ositok.md)
> Kernel/bootloader separation + self-compiling road: see [docs/kernel-separation.md](docs/kernel-separation.md)

**Next**: Phase 1 — TCC cross-compiles kernel.elf from Linux host (verify TCC can produce working kernel).

## Detailed Feature Documentation

For implementation details, API specifics, register-level documentation, and debugging notes:

- **[docs/x86-features-detail.md](docs/x86-features-detail.md)** — All x86-64 feature descriptions (X9-X42, X-OS*, X-NET*, X-CL*, X-WIN32, etc.)
- **[docs/esp8266-detail.md](docs/esp8266-detail.md)** — Math library API, zForth integration, resource budget, DOOM/Elite details
- **[docs/aarch64-detail.md](docs/aarch64-detail.md)** — AArch64/SM8350 (ROG Phone 5) port details
- **[docs/x86-gpu-roadmap.md](docs/x86-gpu-roadmap.md)** — GPU compute roadmap (X27-X40)
- **[docs/os-selfhost-roadmap.md](docs/os-selfhost-roadmap.md)** — OS self-hosting tiers 0-9

## Language
The user speaks Spanish. Communicate in Spanish when appropriate.
