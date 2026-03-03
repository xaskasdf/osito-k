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
```
src/boot/vectors.S          Vector table (VECBASE, correct offsets)
src/boot/crt0.S             Startup: SP, VECBASE, BSS clear, call kernel_main
src/boot/nosdk_init.c       WDT disable, PLL 80MHz, UART pins, baud
src/kernel/context_switch.S Full ISR: save ctx → ISR stack → os_exception_handler → restore ctx → rfe
src/kernel/sched.cpp        Round-robin scheduler, task_create, task_yield (software int)
src/kernel/timer_tick.c     FRC1 100Hz, exception dispatcher, INUM 7/9 handling
src/kernel/task.h           TCB struct, context frame offsets, all kernel API declarations
src/kernel/sem.cpp          Counting semaphores
src/kernel/mq.cpp           Message queues (IPC)
src/kernel/timer_sw.cpp     Software timers (one-shot / periodic)
src/mem/pool_alloc.cpp      Fixed-block allocator (32B × 256 = 8KB)
src/mem/heap.cpp            General-purpose heap allocator (8KB, first-fit, coalescing)
src/fs/ositofs.cpp          Flash filesystem (SPI flash, 64KB-aligned sectors)
src/math/fixedpoint.h/cpp   Fixed-point 16.16 math (sin/cos/div/sqrt/lerp)
src/math/matrix3.h/cpp      3D vectors (vec3_t), 3×3 matrices (mat3_t), perspective projection
src/drivers/uart.cpp        UART0: polled TX, interrupt RX, ring buffer, mutex
src/drivers/gpio.cpp        GPIO read/write/mode
src/drivers/adc.cpp         SAR ADC (10-bit, A0 pin)
src/drivers/input.cpp       Joystick input (ADC + button, event queue)
src/drivers/font.cpp        4×6 bitmap font (ASCII 32-126)
src/drivers/video.cpp       Framebuffer 128×64, line drawing, text, UART streaming
src/forth/zforth.c          zForth core interpreter (MIT, adapted for no-libc)
src/forth/zforth.h          zForth API header (ctx struct, eval, push/pop)
src/forth/zfconf.h          zForth config: int32 cells, 2KB dict, 16-deep stacks
src/forth/zf_host.cpp       Host callbacks, core.zf bootstrap, REPL, file runner
src/forth/setjmp.h          jmp_buf typedef for Xtensa CALL0
src/forth/setjmp.S          setjmp/longjmp asm (6 regs, 24 bytes)
src/doom/doom.h             DOOM engine data structures (level, player, enemies, game state)
src/doom/doom_gen.cpp       Procedural level generator (4x4 grid, snake path, enemy placement)
src/doom/doom_render.cpp    2.5D wireframe renderer (walls, enemy sprites, weapon, HUD)
src/doom/doom_game.cpp      Game loop: input, movement, collision, shooting, AI, game over
src/shell/shell.cpp         Interactive shell (ps, mem, heap, fs, gpio, forth, run, doom, etc.)
src/main.cpp                kernel_main: init → create tasks → timer → sched_start

# x86-64 Bare-Metal AI OS (arch/x86/)
arch/x86/boot/efi_main.c            UEFI entry: GOP, serial, memory map, ExitBootServices
arch/x86/kernel/main.c              Post-boot kernel: PCI scan, NVMe init, mount OsitoFS
arch/x86/kernel/serial.c            COM1 UART (0x3F8, 115200 baud)
arch/x86/kernel/framebuffer.c       GOP 32bpp text console (8×16 font)
arch/x86/kernel/pci.c               PCIe enumeration (ECAM via MCFG, BAR size detection)
arch/x86/kernel/memory.c            Physical memory manager (bitmap, 4KB pages)
arch/x86/drivers/nvme.c             Minimal NVMe driver (admin+IO queues, read-only)
arch/x86/drivers/gpu.h              GPU types, MMIO register defines, Falcon defines, RPC IDs, VBIOS/BIT/FWSEC/WPR2 structs, PIO API, probe + GSP API
arch/x86/drivers/gpu.c              GPU probe Phase 1-3 + Phase 9 VBIOS read, BIT parse, FWSEC extraction
arch/x86/drivers/gsp.c              GSP Falcon driver: probe, firmware load, ELF parse, boot, queues, RPC, FWSEC-FRTS, PIO load (Phase 4-10 + X27)
arch/x86/fs/ositofs2.c              OsitoFS v2 bare-metal driver (mount, list, read)
arch/x86/fs/gpt.h                   GPT structs (UEFI spec) + API
arch/x86/fs/gpt.c                   GPT parser (name match + superblock magic probe)
arch/x86/fs/gguf.h                  GGUF types (tensor, model structs) + API
arch/x86/fs/gguf.c                  GGUF loader (in-memory parser, NVMe read, tensor table)
arch/x86/kernel/tensor.h            Tensor engine API (math, dequant, matvec, ops, RoPE, AVX2 dispatch)
arch/x86/kernel/tensor.c            Tensor engine impl (Q4_0/Q8_0, x87/SSE math, AVX2 detect, benchmark)
arch/x86/kernel/tensor_avx2.c       AVX2/FMA vectorized tensor ops (matvec_q4_0, rmsnorm, vec_add/mul)
arch/x86/kernel/inference.h         Llama inference API (state, weights, KV cache, forward pass)
arch/x86/kernel/inference.c         Transformer forward pass (embed, GQA attention, SwiGLU FFN, generate)
arch/x86/include/types.h            Freestanding types + MMIO + port I/O

# OsitoFS v2 Host Tools (tools/ositofs/)
include/common/ositofs2_format.h     On-disk format (shared header)
tools/ositofs/mkfs.c                 Format device with OsitoFS v2
tools/ositofs/write.c                Write files (GGUF auto-detect, layer index)
tools/ositofs/ls.c                   List files with model metadata
tools/ositofs/info.c                 Show filesystem info
tools/ositofs/gguf.c/h               GGUF parser (metadata + tensor offsets)
tools/ositofs/common.c/h             CRC32, block I/O, display helpers

# Documentation
docs/ositofs2-spec.md                OsitoFS v2 format specification
docs/bare-metal-ai-os.md             x86 bare-metal AI OS research & design
docs/x86-gpu-roadmap.md              GPU compute roadmap (X27-X40 + contingency)
```

## Math Library Summary

### fixedpoint.h — Fixed-point 16.16
- Types: `fix16_t` (int32_t), `angle_t` (uint8_t, 0-255 = 0°-360°)
- Inline: `fix_add`, `fix_sub`, `fix_mul`, `fix_neg`, `fix_abs`, `fix_lerp`, `fix_clamp`, `fix_dist_approx`
- Functions: `fix_sin`, `fix_cos`, `fix_div`, `fix_sqrt`, `fix_print`
- Macros: `FIX16(n)`, `FIX16_C(f)`, `FIX16_TO_INT(x)`, `FIX16_ROUND(x)`

### matrix3.h — 3D Vectors & Matrices
- Types: `vec3_t` (12B), `mat3_t` (36B)
- Inline: `vec3()`, `vec3_add/sub/neg/scale/dot/length`, `mat3_identity`
- Functions: `mat3_rotate_x/y/z`, `mat3_multiply`, `mat3_transform`, `project`
- `project()` uses `FIX16_ROUND`, screen center at (64,32), returns 0 if behind camera
- `mat3_multiply` uses internal temp buffer — safe even if out aliases a or b

## zForth Integration

Minimal Forth interpreter (github.com/zevv/zForth, MIT). Replaced BASIC+VM.
- **Config**: int32_t cells, 2KB dictionary, 16-deep data/return stacks
- **Context**: `zf_ctx` struct (~2.3KB in BSS), persistent across invocations
- **Bootstrap**: core.zf embedded as `static const char[]` — defines if/else/fi, do/loop, variables, s"
- **Syscalls**: emit(0), print(1), tell(2), fb-clear(128), fb-pixel(129), fb-line(130), fb-flush(131), fb-text(132), yield(133), ticks(134), delay(135), wire-render(136), wire-models(137)
- **wire-render** `( model rx ry rz -- )`: models 0=cube, 1=cobra, 2=sidewinder, 3=viper, 4=coriolis
- **setjmp/longjmp**: custom Xtensa CALL0 asm, saves a0/a1/a12-a15 (24 bytes)
- **Shell**: `forth` (REPL, Ctrl+C exit), `run <file.zf>` (execute from OsitoFS)

### Historical: BASIC + VM (removed)
Tiny BASIC interpreter and bytecode VM were the original scripting layer (F1-F5).
Replaced by zForth to save ~4KB IRAM. Original source preserved in git history
(commit 434697a..11d5a48). Could be restored as optional compile-time feature
by re-adding src/basic/ and src/vm/ to the Makefile.

## Resource Budget

### IRAM .text breakdown by subsystem
```
Subsystem                        IRAM (bytes)   Config flag
─────────────────────────────────────────────────────────────
Core (boot, kernel, scheduler)     ~4,800       always
Drivers (uart, gpio, adc, input)   ~2,200       always
Video (framebuffer, font)          ~2,600       always
Memory (pool, heap)                ~1,800       always
Filesystem (ositofs)               ~2,100       always
Math (fixedpoint, matrix3)         ~2,200       always
Shell                              ~2,600       always
────── subtotal core ──────       ~18,300
Elite (wire3d, ships, game)        ~2,100       ENABLE_ELITE
zForth (zforth, zf_host, setjmp)   ~4,200       ENABLE_FORTH
DOOM (gen, render, game, combat)   ~5,900       ENABLE_DOOM
```

### Build configurations
```
Config                              .text    .data    .bss    IRAM free
All features (Elite+Forth+DOOM)    ~30,500  ~15,700  ~37,800  ~2.3KB
Elite + Forth (default)             24,594    9,848   37,232   ~8.2KB
DOOM only (with combat)             24,274    5,908   36,432   ~8.5KB
Core only (no features)            ~18,300   ~5,800  ~35,400  ~14.5KB
```

### DRAM usage
```
sin_table 1KB + pool 8KB + heap 8KB + FS buffers ~4KB + stacks ~12KB
+ zf_ctx ~2.3KB (if ENABLE_FORTH) + doom_state ~1.5KB (if ENABLE_DOOM)
Total used: ~35-37KB of ~80KB available
```

### Other resources
```
Flash:   OsitoFS on SPI flash (4MB total)
Tasks:   idle, input, shell (3 of 8 slots used)
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
| X1      | UEFI boot (gnu-efi): GOP framebuffer, serial, ExitBootServices | Done |
| X2      | Kernel entry: COM1 serial (115200), GOP 32bpp text console (8×16 font) | Done |
| X3      | Physical memory manager (bitmap allocator, 4KB pages) | Done |
| X4      | PCIe enumeration (ECAM via MCFG ACPI table, legacy I/O fallback) | Done |
| X5      | NVMe driver (admin + IO queues, read-only) | Done |
| X6      | OsitoFS v2 bare-metal driver (mount, list, read) | Done |
| X7      | OsitoFS v2 host tools (mkfs, write, ls, info) + GGUF parser | Done |
| X8      | GPU detection (NVIDIA gen detect, GSP-shim placeholder) | Done |
| **X9**  | **Intel I211 Ethernet driver** (igb family, legacy descriptors, polling) | Done |
| **X10** | **Network stack** (ARP responder, IPv4, UDP send/recv, echo server) | Done |
| X11     | QEMU test infrastructure (OVMF + e1000e, serial log, UDP forward) | Done |
| **X12** | **GPT parser** (auto-find OsitoFS partition by name + magic probe) | Done |
| **X13** | **GGUF model loader** (load from OsitoFS into RAM, in-memory parser, tensor table) | Done |
| **X14** | **Tensor compute engine** (Q4_0/Q8_0 matvec, rmsnorm, softmax, SiLU, RoPE) | Done |
| **X15** | **Inference runtime** (Llama forward pass, GQA attention, SwiGLU, greedy decode) | Done |
| **X16** | **GPU MMIO probe** (chip ID, engines, PTIMER, Falcon detect — Phase 1) | Done |
| **X17** | **GPU VRAM discovery** (VRAM size, BAR1 R/W test, PRAMIN window — Phase 2) | Done |
| **X18** | **GPU PCI BAR sizes + gpu_write + PRAMIN window slide R/W** (Phase 3) | Done |
| **X19** | **GSP Falcon deep probe + firmware load to VRAM** (Phase 4) | Done |
| **X20** | **GSP boot** (ELF parse, BOOTVEC, CPUCTL start, mailbox handshake) — Phase 5 | Done |
| **X21** | **GSP message queues** (shared memory, TX/RX primitives, init args to VRAM) — Phase 6 | Done |
| **X22** | **GSP-RM RPC protocol** (function IDs, poll with timeout, init sequence) — Phase 7 | Done |
| **X23** | **GSP-RM init commands** (SET_SYSTEM_INFO, ALLOC_ROOT, ALLOC_DEVICE, INIT_POST_OBJGPU) — Phase 8 | Done |
| **X24** | **VBIOS read** (PRAMIN window read, ROM+PCIR parse, image chain enumeration) — Phase 9 | Done |
| **X25** | **BIT table parse** (BIT scan, Falcon ucode table, FWSEC extraction) — Phase 9 | Done |
| **X26** | **FWSEC-FRTS execution** (FWSEC upload to VRAM, Falcon boot, WPR2 creation attempt) — Phase 10 | Done |
| **X-CPU1** | **AVX2/FMA tensor ops** (runtime CPUID, vectorized matvec_q4_0 + rmsnorm, ~4-8x speedup) | Done |
| **X27** | **Falcon PIO Load** (IMEMC/IMEMD write/read, falcon_reset, falcon_boot, PIO self-test) | Done |
| **X28** | **GBL-based FWSEC-FRTS** (parse FWSEC header, PIO-load GBL to IMEM, DMEM descriptor, FBIF DMA, SEC2/GSP dispatch) | Done |
| **X29** | **WPR2 metadata + Radix3 page tables** (3-level PT build, RmRiscvUCodeDesc extract, GspFwWprMeta write to VRAM) | Done |
| **X30** | **Two-stage GSP boot** (Falcon DMA load, bootloader-first boot, WPR meta in mailboxes, legacy fallback) | Done |
| **X31** | **SEC2 booter load** (load booter.bin from OsitoFS, DMA to SEC2, boot with WPR meta, firmware→WPR2) | Done |
| **X32** | **RPC ID fix + generic RM alloc/control** (corrected rpc_global_enums IDs, RM_ALLOC func 103, subdevice + VASPACE) | Done |
| **X33** | **Channel + GPFIFO** (TSG alloc, GPFIFO channel alloc, ring buffer + instance/USERD memory) | Done |

> Full GPU roadmap (X27-X40 + contingency): see [docs/x86-gpu-roadmap.md](docs/x86-gpu-roadmap.md)

### F12: DOOM Wireframe 2.5D
Procedural level generator (4x4 grid, snake path connectivity) + wall-segment projection renderer.
Compile-time feature: `ENABLE_DOOM=1`. Disabled by default to save IRAM.
Controls: WASD=move, Q/E=strafe, A/D=turn, F/SPACE=shoot, M=minimap, R=new level, Ctrl+C=exit.
Uses fix16 math for all transforms. doom_state_t in static BSS (~1.5KB).

**Combat system**:
- **Enemies**: denemy_t (12B), max 8 per level, placed in rooms away from spawn
- **AI states**: IDLE → CHASE (speed 0.06/frame) → ATTACK (damage every ~0.3s) → HURT (stun) → DEAD
- **Shooting**: hitscan — project enemy to screen X, check if within ±12px of crosshair, closest hit wins
- **Sprites**: wireframe diamond scaled by distance, state-dependent visuals (shake on hurt, arms on attack, flat line when dead)
- **Player HP**: 10, enemy HP: 3. Damage flash (border), muzzle flash (radial lines)
- **Game over**: screen with kill count, R to restart, Ctrl+C to exit
- **Minimap**: enemies shown as X marks, dead enemies hidden

### F11: Spreadsheet Engine
Cell grid (e.g. 8×16), each cell holds number or formula string.
Formula parser: `=A1+B2*3`, cell references, basic operators (+−×÷).
Evaluation with dependency tracking (topological sort or mark-dirty).
UI: grid rendered on framebuffer, cursor navigation with joystick, cell editing via UART.

### X9: Intel I211 Ethernet Driver
Intel I211 Gigabit (igb family, PCI `8086:1539`). Legacy 16-byte RX/TX descriptors,
single queue, polling (no IRQs). Ring sizes: RX=128, TX=64, 2048B packet buffers.
Init: global reset → MAC read from RAL/RAH → RX/TX ring setup → RCTL/TCTL enable.
Compatible with e1000e in QEMU (Intel 82574L, same igb register set).

### X10: Minimal Network Stack
Static IP configuration, ARP table (16 entries), IPv4 with checksum verification.
UDP send/recv with port-based handler dispatch. Echo server on port 7777.
No ICMP, no TCP, no DHCP — minimal footprint for inference prompt delivery.

### X15: Inference Runtime
Complete Llama 3.2 1B transformer forward pass. Orchestrates tensor.h primitives over GGUF model data.
- **Architecture**: dim=2048, 16 layers, 32 heads, 8 KV heads (GQA 4:1), head_dim=64, ffn_dim=8192, vocab=128256
- **Forward pass**: embed → 16× (attn_norm → Q/K/V matvec → RoPE → KV cache → GQA attention → output proj → residual → ffn_norm → SwiGLU → down proj → residual) → final norm → logits
- **Decoding**: greedy argmax, BOS token 128000, EOS tokens 128001/128009
- **Memory**: scratch ~602KB + KV cache ~16MB (256 seq len) = ~17MB total
- **Performance**: ~5-10s/token CPU scalar @ 3GHz (demo functional)
- **Timing**: rdtsc per token, ms estimated @ 3GHz

### X16: GPU MMIO Probe (Phase 1)
Read-only BAR0 MMIO probe — no writes to GPU registers.
- **Chip ID**: PMC_BOOT_0 bits 31:20 → chip name lookup (Turing through Blackwell)
- **Engines**: PMC_ENABLE → PGRAPH, PFIFO, PFB, PTIMER, CE0, CE1
- **PTIMER**: 64-bit nanosecond timer (high-low-high rollover-safe read)
- **Falcons**: GSP (0x110000), SEC2 (0x087000), PMU (0x10A000) — HWCFG presence check
- **Safety**: Dead register (0xFFFFFFFF) and zero checks on BAR0 access
- **Pattern**: Static state, mmio_read32 wrapper (same as i211.c)

### X17: GPU VRAM Discovery (Phase 2)
VRAM size discovery, BAR1 read/write test, PRAMIN window probe.
- **VRAM size**: `NV_PFB_PRI_MMU_LOCAL_MEMORY_RANGE` (0x100CE0), bits 29:0 << 17 = bytes
- **BAR1 access**: Read 4 dwords at BAR1 base, check for dead registers
- **BAR1 R/W**: Write/read test at BAR1+32MB (away from GOP framebuffer), restore originals
- **PRAMIN**: Read 4 dwords at BAR0+0x700000 (1MB instance memory window)
- **Safety**: No writes to GPU control registers. BAR1 write test uses VRAM only. Skip on inaccessible BAR1.
- **Barriers**: wmb() after writes, rmb() before reads

### X18: GPU PCI BAR Sizes + gpu_write + PRAMIN Window R/W (Phase 3)
First active writes to GPU: PCI config space BAR size detection, PRAMIN window control.
- **pci_write32**: Write 32-bit to PCI config space (ECAM or legacy I/O CF8/CFC)
- **pci_read_bar_size**: Standard PCI BAR size detection (save → write 0xFFFFFFFF → read mask → restore). Supports 32-bit and 64-bit BARs.
- **gpu_write**: First GPU register write function (`mmio_write32` via BAR0)
- **PRAMIN window slide**: Write `NV_PBUS_BAR0_WINDOW` (0x001700) to move 1MB PRAMIN window to arbitrary VRAM offset. Test at VRAM+64MB: write 0xDEADBEEF/0x0517014B, read back, restore originals + window position.
- **Safety**: Only writes to PBUS_BAR0_WINDOW (well-documented control register) and VRAM data through PRAMIN. All originals saved and restored. BAR size detection during PCI scan (before gpu_init uses BARs).

### X19: GSP Falcon Deep Probe + Firmware Load to VRAM (Phase 4)
Deep probe of GSP Falcon microcontroller, load firmware blob to RAM, upload to VRAM.
- **Falcon probe**: HWCFG2 → decode IMEM/DMEM sizes, CPUCTL halted/stopped status, MAILBOX0/1 values
- **Firmware load**: `osfs2_find("gsp.bin")` → validate size (1–128MB) → `mem_alloc_aligned` → read in 1MB chunks
- **VRAM upload**: PRAMIN window slide (same X18 pattern) → write firmware 4 bytes/dword through 1MB window → verify first 4 dwords → restore window
- **Placement**: Firmware at VRAM+128MB (away from GOP framebuffer)
- **Timing**: rdtsc for upload measurement, estimated @ 3GHz
- **Safety**: Only reads Falcon status registers (no writes to CPUCTL/BOOTVEC/DMACTL). PRAMIN window save/restore. Only writes firmware data to VRAM. Graceful degradation: no GPU → skip VRAM upload; no gsp.bin → log and continue.
- **Does NOT boot GSP** — that is X20 (BOOTVEC, CPUCTL_STARTCPU, mailbox handshake)

### X20: GSP Falcon Boot — ELF Parse + Boot Sequence + Mailbox Handshake (Phase 5)
First attempt to boot the GSP Falcon microcontroller.
- **ELF64 parse**: Validate magic/class/endian, extract entry point + program headers from `gsp.fw_data` in RAM
- **Boot sequence**: Halt Falcon → clear mailboxes → set DMATRFBASE (VRAM offset >> 8) → set BOOTVEC (entry >> 8) → CPUCTL_STARTCPU
- **Mailbox poll**: Read MAILBOX0 in tight loop (~1s timeout @ 3GHz). Any non-zero value = firmware alive.
- **Diagnostics**: On timeout, dump CPUCTL (HALTED/STOPPED/RUNNING), MAILBOX0, MAILBOX1
- **Registers written**: CPUCTL (halt/start), BOOTVEC, DMATRFBASE, MAILBOX0/1 (clear to 0)
- **Safety**: Timeout-based, never hangs. On failure Falcon returns to HALTED. OS continues regardless.
- **Does NOT implement RPC** — that is X21+ (shared memory message queues, GSP-RM protocol)

### X21: GSP Shared Memory Message Queues (Phase 6)
Bidirectional host↔GSP communication via shared memory queues following NVIDIA `open-gpu-kernel-modules` architecture.
- **Shared memory**: 513KB region (0x81000 bytes) — PTE array + 2× (header + 63 pages data)
- **Layout**: PTE(4KB) + CPU queue header(4KB) + CPU queue data(252KB) + GSP queue header(4KB) + GSP queue data(252KB)
- **Headers**: MsgqTxHeader (32B: version, size, msgSize, msgCount, writePtr, flags, rxHdrOff, entryOff) + MsgqRxHeader (4B: readPtr)
- **Protocol**: Lock-free — producer writes TX writePtr, consumer reads RX readPtr. Modular % 63 indexing.
- **Messages**: Element header (48B: authTag, aad, XOR checksum, seqNum, elemCount) + RPC header (32B: version, "VRPC" signature, function, result, sequence)
- **Queue init args**: Written to VRAM+126MB via PRAMIN for GSP to find at boot
- **Mailboxes**: Now carry shared memory physical address (low 32 in MAILBOX0, high 32 in MAILBOX1)
- **Doorbell**: NV_PGSP_QUEUE_HEAD (0x110C00) write notifies GSP of new messages
- **Post-boot**: Attempts recv on status queue (no response expected without full boot chain)
- **Does NOT implement RPC commands** — that is X22+ (INIT command, ACK handling)

### X22: GSP-RM RPC Protocol (Phase 7)
RPC protocol layer on top of X21 message queues. Function IDs, polling, init sequence.
- **Function IDs**: Enum defines from `rpc_global_enums.h` — NOP(0), ALLOC_ROOT(2), GET_GSP_STATIC_INFO(68), GSP_RM_CONTROL(76), CONTINUATION_RECORD(0x43), etc.
- **Event IDs**: GSP_INIT_DONE(0x80), RUN_CPU_SEQUENCER(0x81), POST_EVENT(0x82)
- **gsp_rpc_poll**: Polls status queue with rdtsc-based timeout (~3GHz estimate). Returns function, result, and payload to caller. Never blocks indefinitely.
- **gsp_rpc_init**: Post-boot sequence — (1) poll for INIT_DONE event (2s timeout), (2) send GET_GSP_STATIC_INFO, (3) poll response and parse GPU name from `gsp_static_info_t`.
- **gsp_queue_recv**: Extended with `rpc_result` output parameter.
- **Graceful degradation**: Without full boot chain (FWSEC, WPR, radix3), GSP doesn't respond — timeouts logged, OS continues.
### X23: GSP-RM Init Commands (Phase 8)
5-step RM initialization sequence over X22 RPC protocol. Sends real hardware data (BAR addresses, PCI BDF, device ID).
- **Payload structs**: `gsp_system_info_t` (88B), `gsp_registry_table_t` (160B), `gsp_alloc_root_t` (16B), `gsp_alloc_device_t` (24B)
- **Handle constants**: `GSP_RM_CLIENT_HANDLE` (0xC1D00000), `GSP_RM_DEVICE_HANDLE` (0xDE1D0000)
- **PCI BDF**: `gpu_device_t` now carries `pci_bus`, `pci_dev`, `pci_func` (populated in `pci_add_device`)
- **Sequence**: (1) SET_SYSTEM_INFO (fire-and-forget) → (2) SET_REGISTRY (fire-and-forget) → (3) ALLOC_ROOT (poll 2s) → (4) ALLOC_DEVICE (poll 2s) → (5) INIT_POST_OBJGPU (poll 2s)
- **Graceful degradation**: Each step logs and continues on timeout. Without full boot chain, all polls timeout — expected behavior.
- **Integration**: Called from `gsp_boot()` after `gsp_rpc_init()`. Independent guard on `queues_ready`.
- **Does NOT implement RM control** — that is X24+ (secure boot chain, GPU compute)

### X24: VBIOS Read from VRAM (Phase 9)
Read VBIOS from VRAM via PRAMIN window, parse ROM+PCIR image chain.
- **Read method**: PRAMIN window at VRAM offset 0, reads 64KB initial then extends if needed (max 256KB)
- **ROM header**: 0xAA55 signature, PCIR offset field at byte 24
- **PCIR chain**: Each image has PCIR with vendor/device ID, code_type, image_length (512B units), last_image flag
- **Code types**: PciAt (0x00), UEFI (0x03), FwSec (0xE0) — FwSec images identified for X25/X26
- **Fallback**: Tries offset 0x1000 if no signature at offset 0
- **Safety**: Read-only VRAM access. PRAMIN window saved/restored. No GPU register writes.
- **Integration**: Called from `gpu_init()` after Phase 3 PRAMIN R/W test

### X25: BIT Table Parse + FWSEC Extraction (Phase 9)
Parse BIT (BIOS Information Table) from VBIOS, extract FWSEC blob.
- **BIT scan**: Linear scan for "BIT\0" signature in VBIOS data
- **Token 0x70**: Falcon Data token points to Falcon Ucode Table
- **Ucode table**: Header + descriptors with application_id (0x01=FWSEC), target_id (0x03=GSP), VBIOS offset
- **Fallback**: If no BIT found, uses PCIR code_type=0xE0 images from X24 directly
- **State**: `fwsec_state_t` holds pointer into VBIOS buffer (no separate allocation)
- **Safety**: Parse-only, no writes. Graceful on missing BIT or missing Falcon token.

### X26: FWSEC-FRTS Execution + WPR2 Creation (Phase 10)
Load FWSEC into GSP Falcon, execute FRTS command to create WPR2 region.
- **Upload**: FWSEC image → VRAM+192MB via PRAMIN (same pattern as gsp.bin upload)
- **Boot sequence**: Halt → DMATRFBASE → BOOTVEC=0 → MAILBOX0=0x15 (FRTS cmd) → STARTCPU
- **Poll**: Waits for mailbox change (2s timeout @ 3GHz)
- **WPR2 check**: Scans end-of-VRAM offsets (-4KB, -1MB, -2MB) for 0x57505232 "WPR2" magic
- **Post-execution**: Halts Falcon, ready for gsp.bin re-boot
- **Safety**: Timeout-based, never hangs. Falcon returns to HALTED on failure.
- **Known limitation**: Without full SEC2 bootstrap chain, FWSEC may not execute on all GPUs. Expected behavior for initial implementation.

### X-CPU1: AVX2/FMA Tensor Ops
Runtime-dispatched AVX2 vectorization of hot-path tensor operations.
- **Detection**: CPUID check for XSAVE+AVX+FMA+AVX2. Enables CR4.OSXSAVE and XCR0 bits for AVX state on bare-metal.
- **matvec_q4_0_avx2**: SIMD nibble unpack (vpunpckl/hbw) → cvtepu8_epi32 → cvtepi32_ps → vfmadd. 4 groups of 8 per Q4_0 block. ~4-8x speedup.
- **rmsnorm_avx2**: AVX2 FMA sum-of-squares + vectorized element-wise multiply.
- **vec_add/mul_avx2**: 8-wide AVX2 with scalar tail.
- **Dispatch**: `tensor_has_avx2()` cached check. Public functions (matvec_q4_0, rmsnorm, etc.) auto-dispatch.
- **Build**: `tensor_avx2.c` compiled with `-mavx2 -mfma` (AVXFLAGS in Makefile). All other files stay default ISA.
- **Benchmark**: tensor_benchmark() runs scalar vs AVX2 comparison on 2048×2048 matvec, reports speedup.

### X27: Falcon PIO Load
Programmed I/O access to Falcon IMEM/DMEM via IMEMC/IMEMD registers.
- **falcon_pio_load_imem(base, dst, data, size)**: Write dwords to IMEM. IMEMC = addr | (1<<24) auto-inc.
- **falcon_pio_load_dmem(base, dst, data, size)**: Write dwords to DMEM. Same pattern.
- **falcon_pio_read_imem/dmem**: Readback via auto-inc-on-read (bit 25).
- **falcon_reset(base)**: Halt + clear mailboxes + verify.
- **falcon_boot(base, boot_addr)**: BOOTVEC + STARTCPU.
- **falcon_pio_selftest(base)**: Write pattern to DMEM, readback verify, restore zeros.
- **Purpose**: Prerequisite for X28 (GBL load to IMEM for FWSEC-FRTS proper boot chain).

### X28: GBL-based FWSEC-FRTS Execution
Correct Turing+ boot sequence using Generic Bootloader (GBL) intermediary instead of direct DMATRFBASE approach.
- **FWSEC header parse**: `falcon_fw_hdr_t` at start of FWSEC blob — extracts bl_code_offset/size for GBL, os_code/data offsets for FWSEC payload
- **GBL extraction**: First from BIT Falcon ucode table (app_id=0x02), fallback to FWSEC internal header bootloader offsets
- **Target Falcon selection**: SEC2 (0x087000) for Turing, GSP (0x110000) for Ampere+. Based on `target_id` and GPU generation.
- **FBIF TRANSCFG**: Programs `NV_PFALCON_FBIF_TRANSCFG` (falcon_base+0x600) = 0x02 to enable DMA from coherent system memory
- **PIO sequence**: GBL microcode → IMEM via `falcon_pio_load_imem()`, `BootloaderDmemDescV2` → DMEM via `falcon_pio_load_dmem()`
- **DMEM descriptor**: Contains physical addresses of FWSEC code/data in system RAM (identity-mapped), FRTS command (0x15) as argv
- **Boot + poll**: `falcon_boot(base, 0)` starts GBL which DMA-loads FWSEC and executes FRTS. Poll mailbox 2s for completion.
- **Fallback**: If v2 fails, `gsp_fwsec_frts_legacy()` (old X26 VRAM+DMATRFBASE approach) runs automatically
- **Known limitation**: Without full SEC2 bootstrap chain on Turing, GBL may not complete DMA. Diagnostic output reveals which step failed.

### X29: WPR2 Metadata + Radix3 Page Tables
Prepare GSP boot environment: radix3 page tables, bootloader extraction, WPR metadata.
- **Radix3 page tables**: 3-level hierarchy (L0→L1→L2→firmware pages) mapping firmware at GSP virtual address 0. 512 entries per 4KB page. Identity-mapped (DMA addr = pointer). Allocated in system RAM.
- **RmRiscvUCodeDesc**: Descriptor at end of gsp.bin (`fw_size - sizeof(desc)`). Contains `bootloader_offset/size`, `riscv_elf_offset/size`, `manifest_offset`. Bootloader copied to page-aligned DMA-accessible RAM.
- **GspFwWprMeta v2**: Written to VRAM (end - 256KB) via PRAMIN. Contains radix3 L0 DMA addr, bootloader DMA addr + offsets, VRAM layout (fbSize, gspFwWprEnd). Magic 0x57505232 "WPR2" for verification.
- **Integration**: Called from `gsp_boot()` after `gsp_fwsec_frts()`, before DMATRFBASE/BOOTVEC boot.
- **Size calculation**: ~32MB gsp.bin → 8192 FW pages, 16 L2 pages, 1 L1 page = 73KB total PT.
- **Safety**: All allocations from system RAM. PRAMIN writes to end-of-VRAM (away from framebuffer/firmware). Verify via magic readback. Graceful degradation on any failure.
- **Does NOT change boot sequence** — that is X30 (two-stage bootloader-first boot via radix3).

### X30: Two-Stage GSP Boot
Falcon DMA load engine + bootloader-first boot sequence replacing direct firmware boot.
- **falcon_dma_load(base, src_phys, dst_off, size, to_imem)**: Chunked 256B DMA transfers via DMATRFBASE/DMATRFCMD. Programs FBIF TRANSCFG for coherent sysmem. Polls DMATRFCMD.IDLE per chunk (~10ms timeout).
- **Registers**: DMATRFMOFFS (+0x114) = dest offset, DMATRFFBOFFS (+0x11C) = src offset, DMATRFCMD (+0x118) = trigger with SIZE_256B|IMEM flags, DMATRFBASE/BASE1 = phys addr >> 8.
- **gsp_boot_v2**: (1) Reset GSP, (2) DMA-load bootloader to IMEM, (3) DMA-load BL params to DMEM, (4) BOOTVEC=0, (5) MAILBOX0/1 = WPR meta VRAM address, (6) STARTCPU, (7) poll 2s.
- **gsp_boot_legacy**: Preserved X20 direct DMATRFBASE+BOOTVEC boot as fallback.
- **gsp_boot dispatcher**: Tries v2 first, falls back to legacy on failure. RPC init (X22-X23) runs after either path.
- **Known limitation**: Without SEC2 booter chain, GSP bootloader may not complete on Turing. Full SEC2 coordination is X31.

### X31: SEC2 Booter Load
SEC2 falcon runs the "booter_load" firmware that DMA-copies GSP firmware into WPR2 via radix3.
- **gsp_load_booter()**: Loads "booter.bin" from OsitoFS (4KB–4MB). Optional — gracefully skipped if not present.
- **gsp_sec2_booter()**: (1) Reset SEC2, (2) DMA-load booter to SEC2 IMEM via `falcon_dma_load`, (3) BOOTVEC=0, (4) MAILBOX0/1 = WPR meta VRAM address, (5) STARTCPU, (6) poll HALTED + MAILBOX0==0 (3s timeout).
- **Success criteria**: SEC2 halts with MAILBOX0=0 — firmware loaded into WPR2 region.
- **Integration**: Runs in `gsp_boot()` after FWSEC-FRTS + radix3/WPR meta, before GSP boot.
- **Known limitation**: booter.bin is from NVIDIA firmware package (`/lib/firmware/nvidia/<ver>/gsp/booter_load-*.bin`). Must be added to OsitoFS disk. Without it, SEC2 step is skipped and boot continues with direct GSP boot.
- **Phase A complete**: X27-X31 implement the full GSP secure boot chain infrastructure. Hardware testing will determine which steps succeed on real GPUs.

### X32: RPC ID Fix + Generic RM Alloc/Control
Critical bug fix + Phase B foundation. RPC function IDs were wrong (compared against rpc_global_enums.h 535.113.01).
- **RPC ID corrections**: GET_GSP_STATIC_INFO 68→65, SET_REGISTRY 69→73, GSP_SET_SYSTEM_INFO 70→72, GSP_INIT_POST_OBJGPU 71→74, GSP_RM_ALLOC 77→103, CONTINUATION_RECORD 0x43→71. Events: INIT_DONE 0x80→0x1001, RUN_CPU_SEQUENCER 0x81→0x1002.
- **gsp_rm_alloc()**: Generic RM object allocator (func 103). Builds `rpc_rm_alloc_hdr_t` (32B) + inline class-specific params. Polls 2s for response.
- **gsp_rm_control()**: Generic RM control call (func 76). Builds `rpc_rm_ctrl_hdr_t` (24B) + inline params. Polls 2s.
- **ALLOC_SUBDEVICE**: Class 0x2080 (NV20_SUBDEVICE_0), 4B params (subDeviceId=0). Handle 0x5D1D0000, parent=device.
- **ALLOC_VASPACE**: Class 0x90F1 (FERMI_VASPACE_A), 48B params (externally-owned). Handle 0x90F10000, parent=device.
- **RM init sequence extended**: 5→7 steps (added ALLOC_SUBDEVICE + ALLOC_VASPACE after INIT_POST_OBJGPU).

### X33: Channel + GPFIFO
GPU compute channel allocation via RM. Foundation for pushing GPU commands.
- **Host memory**: GPFIFO ring (512 entries × 8B = 4KB), instance memory (RAMFC, 4KB), USERD (4KB). All page-aligned, DMA-accessible.
- **TSG allocation**: KEPLER_CHANNEL_GROUP_A (0xA06C) via `gsp_rm_alloc()`. Parent=device, links to VASPACE.
- **Channel allocation**: Generation-specific class (Turing 0xC46F / Ampere 0xC56F / Ada 0xC76F). Parent=TSG. Carries GPFIFO offset, instanceMem, userdMem, ramfcMem descriptors.
- **GPFIFO entry format**: 8 bytes — address[39:2] + length in dwords. `gpfifo_make_entry()` helper.
- **channel_state_t**: Tracks GPFIFO ring, gp_put index, inst/userd memory, RM handles, chan_class.
- **Integration**: Called from `gsp_boot()` after `gsp_rm_init()`.

## Language
The user speaks Spanish. Communicate in Spanish when appropriate.
