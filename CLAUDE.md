# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What is this
Multi-arch bare-metal OS, one tree per arch under `arch/<arch>/`. A [naranjositos.tech](https://naranjositos.tech/) project.
- **x86-64** (`arch/x86/`) — the primary active target: UEFI-booted kernel with a Linux-ABI syscall layer, Win32 PE (UT99/Steam) and DOS/DPMI compatibility layers, networking/TLS, and on-device LLM inference. Almost all recent commits land here.
- **ESP8266** (`arch/xtensa/`) — the original target: preemptive kernel for Wemos D1 (Xtensa LX106 @ 80MHz), no Espressif SDK (`nosdk8266` approach).
- **AArch64** (`arch/arm/`, SM8350 / ROG Phone 5) and **wasm32** (`arch/wasm/`, Emscripten, runs in the browser).
- Shared code: `gui/` (desktop GUI used by every arch), `include/common/` (on-disk formats such as OsitoFS). Host tools: `tools/` (`tools/ositofs/` builds/edits disk images).

The repo root also contains untracked/ignored debugging debris — `.tmp-*` screenshots and logs, `nvme_*.img` disk images, game files (WADs, UT99 DLLs), and stray QEMU build-tree directories (`hw/`, `accel/`, `block/`, `tcg/`…). None of it is project source; ignore it when exploring.

## Build & Flash

The root `Makefile` is a **delegator** — each arch has its own Makefile under `arch/<arch>/`:

```bash
make            # default → ESP8266/Xtensa firmware (arch/xtensa/build/osito.elf + osito0x00000.bin)
make x86        # x86-64 bare-metal OS → arch/x86/build/{boot.efi,kernel.elf}
make arm        # AArch64 (SM8350 / ROG Phone 5)
make wasm       # wasm32 hosted build
make flash      # flash ESP8266 via /dev/ttyUSB0
make clean-all  # wipe every arch's build/
```

CI (`.github/workflows/build.yml`) builds **all four arches** on push/PR to `master` and `experiment`: `make xtensa PYTHON=python3`, `make x86 CLANG=1 SKIP_MODEL=1`, `make arm CROSS_COMPILE=aarch64-linux-gnu-`, `make wasm` (emsdk). Build-only — CI runs no tests. `release.yml` (on `v*` tags) builds Xtensa only.

### ESP8266 (Xtensa)
```bash
# Linux:
export PATH="$PWD/arch/xtensa/tools/xtensa-lx106-elf/bin:$PATH"
make                    # default: all features enabled (arch/xtensa/build/osito.elf)
make ENABLE_ELITE=0 ENABLE_FORTH=0 ENABLE_DOOM=1   # DOOM config
make flash              # flash via /dev/ttyUSB0
# Windows: export PATH=".../arch/xtensa/tools/xtensa-lx106-elf/bin:$PATH"; make PYTHON=py
```

### x86-64 bare-metal AI OS — the primary active target
Two toolchains: the default is the **`x86_64-elf-gcc`** cross toolchain; **`CLANG=1`** uses `clang --target=x86_64-elf` + `ld.lld` + `llvm-objcopy` instead (what CI and the Windows/msys2 machine use — no cross-gcc needed). Both still need **gnu-efi** for `boot.efi`; the Makefile auto-detects it at `/usr/local` (macOS, ncroxon source build) or `/usr` (`sudo apt install gnu-efi`), or override `EFI_PREFIX`.
```bash
make -C arch/x86 CLANG=1    # → build/boot.efi + build/kernel.elf + lib/vulkan/libvulkan.a
make -C arch/x86 CLANG=1 build/boot.efi build/kernel.elf   # skip libvulkan
make -C arch/x86 minimal    # kernel-minimal.elf — fallback when the full kernel crashes (BSS/NVMe-DMA overlap)
make -C arch/x86 tcc        # kernel.elf via in-tree TCC (self-host verification)
make -C arch/x86 legacy     # monolithic ositok.efi (old single-PE path)
make -C tools/ositofs       # host FS tools: mkfs/write/read/ls/info/delete/rename/fsck/defrag
```
Build knobs (all default 0; a flags stamp in `build/` forces a kernel rebuild when they change):
- `SKIP_MODEL=1` — skip the boot-time GGUF model load (inference unavailable); faster boot loop.
- `QUIET=1` — compile out high-volume serial tracing. Every serial byte is a VM exit under QEMU/KVM, so this matters for UT99/Steam runs.
- Opt-in tracing: `COMPAT_TRACE`, `PE_LOADER_TRACE`, `WIN32_TRACE`, `WIN32_VM_TRACE`, `WIN32_LOOKUP_TRACE`, `WIN32_LATENCY_TRACE`, `WIN32_IF_AUDIT`, `IOCP_TRACE`, `DOS_TRACE`, `DOS_IRQ_TRACE`, plus `INTEL_*_DIAGNOSTIC` (real-hardware Intel GPU probes). See the comments in `arch/x86/Makefile`.

### Run x86-64 in QEMU (the main dev loop)
The runner scripts build an ESP FAT image (boot.efi → `BOOTX64.EFI`, plus `kernel.elf`) via `mtools`, boot it under OVMF/edk2 on `q35`/`-m 512M`/`-smp 4` with virtio-net (`hostfwd udp 7778→7777`, `tcp 50052→50052`), xHCI kbd+tablet, and attach `arch/x86/build/nvme.img` as NVMe if present (the UT99/DOOM image).
```bash
arch/x86/scripts/qemu-cocoa.sh                                # macOS: -accel hvf -display cocoa
arch/x86/scripts/qemu-test.sh [--no-build] [--no-gl] [--kill] # generic; virtio-gpu-gl by default
tools/smoke-test.sh [secs]                                    # headless boot, waits for DHCP/shell in the serial log
tools/deploy.sh <elf> [--gl] [--auto] [--tail]                # write an ELF into build/nvme.img and relaunch QEMU
```
**The serial log is the debugging lifeline**: `arch/x86/build/serial.log`. Exit QEMU with `Ctrl-A X`. Poke the in-OS UDP inference server from the host: `echo "hola osito" | nc -u localhost 7777`.

### Windows (this machine)
The checkout is `D:\projects\osito-k`; msys2 lives in `C:\msys64` (clang, lld, llvm-tools, mtools, qemu; gnu-efi installed by hand under msys2's `/usr/local`). There is **no WSL on this machine**. Details in `README-windows.md`.
- **Build + boot (TCG)**: run `.\build-windows.ps1` from PowerShell; it takes `-Display none|gtk|sdl`, `-Nvme <img>`, `-BuildOnly` and `-NoBuild`. The equivalent from an MSYS2 MINGW64 shell is `OK_BUILD=1 arch/x86/scripts/run-windows.sh`, with `OK_DISPLAY` and `OK_NVME`. Booted without NVMe, the kernel reaches the `osito>` prompt in under a minute. The NVMe image is attached read-write, so the guest can modify it.
- **No hardware acceleration**: WHPX cannot boot OVMF (a #GP in `PlatformPei` on an early `WRMSR`, before `boot.efi` runs). This is an upstream QEMU limitation with no command-line workaround; see `docs/windows-qemu-whpx.md`. `arch/x86/scripts/run-wsl-kvm.sh` (KVM) only works on a Linux/WSL host that has QEMU installed, and none is set up right now.
- **Native clang can't read symlinks created from WSL.** If the build fails with "no such file" on a file that exists, check `find <dir> -type l` and replace the symlinks with copies.

### AArch64 on the ROG Phone 5 (SM8350)
Full guide: `arch/arm/BUILDING-ROG5.md`. The phone is on this machine's USB (fastboot in `C:\platform-tools`); the toolchain and the boot-image pieces (shim, DTBs, `mkbootimg.py`, stock backups) live on the Mac `cortessamuel@192.168.1.7` in `~/phone_breaker2`.
- `arch/arm/scripts/rog5-boot.sh` ships the working tree to the Mac and builds it there with `aarch64-elf-gcc`. clang can't build `arch/arm` (no `__builtin_setjmp` on aarch64).
  - `--usb` is the normal dev loop (~15 s, no buttons): it sends the kernel to the running Osito-K over its USB console (`kload`) and checks that the new kernel answers.
  - `--flash` packs `boot.img` and flashes the active slot, for a kernel that must survive a reboot (~75 s, no buttons). If Osito-K is running, it sends `reboot bootloader` first (the restart reason goes into the PMIC over SPMI, `platform/sm8350/pmic_arb.c`). Then it waits for the new kernel and checks `uname`'s build stamp against the flashed image. Only a hung or pre-2026-10-29 kernel needs Power + Vol-Down. `fastboot boot` bootloops on this device.
  - `rog5-console.ps1 -Command <line>` runs one shell command on the phone and prints the answer.
  - `--virt-check` also boots the virt build in local QEMU.
- There is no UART output. Feedback comes from the USB console (`dmesg` replays the boot log) and from the screen: progress bars, a blinking heartbeat square per CPU, an orange/purple panic band with ESR/ELR/`_start` as bit cells, and a text console (pixel rows 700 and below) that mirrors everything `serial_*` prints. The panic band is the only clue when the kernel dies before USB is up; the guide explains how to decode it.
- MMU and caches are on (`platform/sm8350/mmu.c`, flat map). Anything a device DMAs to must come from `dma_alloc()` (uncached window), not the page allocator.
- SMP: tasks run on all 8 cores (`kernel/sched.c`, `kernel/smp.c`). `task_create` keeps a task on the boot CPU, as before SMP; `task_create_on(..., cpu)` places it elsewhere, or anywhere with `TASK_ANY_CPU`. Shared state needs a lock from `include/spinlock.h`. The scheduler, heap, page allocator and console already have one. Don't use `__atomic` builtins: they emit LSE, which virt's Cortex-A72 lacks.
- The hypervisor traps GIC MMIO and emulates it from the abort syndrome, so a load/store with writeback or a pair (`str w0, [x1], #4`, `stp`) becomes a synchronous external abort. Access the GIC (and any other region it might trap) only through the asm `mmio_rd32`/`mmio_wr32`/`mmio_rd64` in `include/aarch64.h`.
- Boot sets every CPU cluster to its top frequency through the EPSS hardware (`platform/sm8350/cpufreq.c`, shell `cpufreq`): A55 1.8 GHz, A78 2.42 GHz, X1 2.84 GHz. The bootloader leaves the big cores at 710/844 MHz.
  - The A78s and the X1 share a voltage rail, so lowering one alone changes nothing.
  - Never lower the A55s: it hung the machine twice, and `cpufreq` refuses it.
  - `platform/sm8350/thermal.c` reads the TSENS sensors (shell `temp`), and its task caps both big clusters together above 95 °C.
- Status 2026-10-29: boots to `shell>`, with the shell interactive over USB. The side USB-C port enumerates as a CDC-ACM serial port (`VID_1209&PID_0001`, COM3 on this machine), and you can drive it from PowerShell with `System.IO.Ports.SerialPort`.
- The kernel is a PIE linked at 0 that relocates itself in `start.S`, so it runs at any load address and stored pointers work. Linker-script symbols that C reads must be section-relative (an absolute symbol's GOT entry gets no relocation). After linking, `readelf -r kernel.elf` should show only `R_AARCH64_RELATIVE`.

### Tests
CI runs no tests. Three kinds of test exist in `arch/x86/`:
- **Host tests** (the majority): `arch/x86/scripts/test-*.sh`. Most of them compile an `arch/x86/test/*_host.c` harness against the real kernel sources with gcc and clang, often under ASan/UBSan, and run it natively. They need Linux. From this machine, run them on the `xasko@kerberus` WSL (Ubuntu-24.04) with `arch/x86/scripts/remote-test.sh <name>...`, e.g. `remote-test.sh boot-diag`. It syncs the tree incrementally over SSH and runs `test-<name>.sh` there.
  - Arguments for a single test go after `--`, e.g. `remote-test.sh dos-fpu-compare -- /tmp/out.log`.
  - Run arbitrary commands with `--exec`, e.g. `remote-test.sh --exec 'make -C arch/x86 serial-host-test'`.
  - kerberus's SSH shell is PowerShell 5.1, so send remote commands through this script rather than raw `ssh`.
  - kerberus still lacks `libmpfr-dev libgmp-dev lld llvm` (used by the `dos-fpu-*` and PE-building tests); installing them needs the user's sudo.
  - Some tests expect fixtures pre-generated into `arch/x86/build/`, which is not synced.
- **QEMU tests**: about 20 of the `test-*.sh` / `test-*-qemu.py` scripts boot the kernel and assert on the serial log. They need a built `kernel.elf` and often an `nvme.img`.
- **In-OS fixtures**: `arch/x86/scripts/build-*-test.sh` cross-build PE32/PE64 Win32 fixtures (`clang --target=*-windows-msvc` + `lld-link` + `llvm-dlltool`, so no Windows SDK is needed). `make -C arch/x86 dos-*-test` builds DOS `.COM` fixtures into `build/test/`. Linux-ABI ELF test programs (`hello_c`, `fork_test`, `mmap_test`…) and in-OS tools (`tcc.elf`, `qjs.elf`) are built with the minimal libc via `make -C arch/x86/libc <name>.elf` (uses `tcc` and the `x86_64-elf` binutils). Copy them onto the NVMe/ESP image with `tools/ositofs` and run them from the in-OS shell.

### ESP8266 feature flags & serial
**Feature flags** (see `arch/xtensa/Makefile`):
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

## Conventions
- **LF line endings** are enforced by `.gitattributes`. CRLF from Windows editors produces whole-file diffs.
- Commit subjects are subsystem-prefixed and action-oriented, e.g. `x86/win32: fix UT99 rendering, display modes and C++ catches` or `x86/dos: implement Ctrl-C callbacks across real mode and DPMI`.
- Architecture-specific code stays under `arch/<arch>/`. Only reusable on-disk formats and APIs move to `include/common/`.
- `AGENTS.md` repeats these guidelines for other agents. Keep it in sync when they change.

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

## Code layout
Per-file maps for every arch are in [docs/code-map.md](docs/code-map.md). Overview of `arch/x86/`, the tree you will touch most:
- `boot/boot_efi.c` — the UEFI loader. It reads `kernel.elf` from the ESP, fills the handoff struct in `include/boot_info.h`, and jumps to the kernel. `kernel.ld` links the kernel at 0x2000000.
- `kernel/` — the core kernel and most subsystems, roughly one flat file per subsystem: scheduler, paging, syscalls, net/TLS, shell, inference, compositor, and so on.
- `drivers/` — PCI device drivers: NVMe, AHCI, xHCI, virtio-*, HDA, NVIDIA GPU/GSP, Intel graphics.
- `fs/` — OsitoFS v2/v3 plus the foreign filesystems, all behind the VFS.
- `win32/` — the PE loader, DLL shims, NT syscalls and handle table. This is the Win32 compat layer for UT99 and Steam.
- `dos/` — DOS/DPMI execution: real-mode CPU, INT 21h/31h services, XMS/EMS, the x87 FPU.
- `lib/` — vendored third-party code (Mesa/OpenGL, DXVK, Vulkan, BearSSL, tinyssh, softfloat).
- `libc/` — the minimal userspace libc and in-OS tools (TCC, QuickJS).
- `test/` — host harnesses (`*_host.c`) and in-OS test programs. `scripts/` holds QEMU runners, test drivers and debug helpers.

## Roadmap & status
The feature tables live in [docs/roadmap.md](docs/roadmap.md): ESP8266 `F*`, x86 `X-*`, wasm `W-*`, AArch64 `A-*`, and the `H*` hardware-boot plan. When a feature lands or changes status, add or update its row there, not here.

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
