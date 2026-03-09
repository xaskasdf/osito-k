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
arch/x86/kernel/idt.c               IDT setup, exception handlers (#PF/#GP/#UD), APIC timer
arch/x86/kernel/isr_stubs.S         ISR entry stubs (save regs, call C handler, IRETQ)
arch/x86/kernel/paging.c            4-level page tables, identity map, CR3 switch, MMIO mapping
arch/x86/kernel/heap.c              Kernel heap allocator (kmalloc/kfree, first-fit, coalescing)
arch/x86/kernel/syscall.c           SYSCALL/SYSRET setup, dispatch table, fd table
arch/x86/kernel/syscall_entry.S     SYSCALL entry point (save regs, call C dispatch, SYSRETQ)
arch/x86/kernel/elf.c               ELF64 loader (PT_LOAD, stack setup, entry jump)
arch/x86/kernel/process.c           Process table, exec/exit/waitpid, PID allocation
arch/x86/drivers/nvme.c             Minimal NVMe driver (admin+IO queues, read/write)
arch/x86/drivers/gpu.h              GPU types, MMIO register defines, Falcon defines, RPC IDs, VBIOS/BIT/FWSEC/WPR2 structs, PIO API, probe + GSP API
arch/x86/drivers/gpu.c              GPU probe Phase 1-3 + Phase 9 VBIOS read, BIT parse, FWSEC extraction
arch/x86/drivers/gsp.c              GSP Falcon driver: probe, firmware load, ELF parse, boot, queues, RPC, FWSEC-FRTS, PIO load (Phase 4-10 + X27)
arch/x86/drivers/sass.h              SASS kernel types, 128-bit instruction encoding, control code helpers, kernel catalog
arch/x86/drivers/sass.c              Pre-encoded SASS kernels (NOP, S2R, NOP4, store_pattern, vec_add_f32), VRAM upload, dispatch
arch/x86/drivers/gmmu.c              GMMU page tables: 5-level identity map, instance block PDB config
arch/x86/drivers/gpu_tensor.h        GPU tensor ops API (VRAM alloc, upload/download, dispatch wrappers)
arch/x86/drivers/gpu_tensor.c        GPU tensor dispatch layer, PRAMIN data transfer, self-test, PTX docs
arch/x86/drivers/gpu_inference.h     GPU inference orchestration API (hybrid GPU/CPU forward pass)
arch/x86/drivers/gpu_inference.c     GPU-accelerated Llama forward pass, kernel dispatch table, benchmark
arch/x86/kernels/*.ptx               PTX kernels translated from ntransformer CUDA (gemv_q4_0, rmsnorm, softmax, elementwise, rope)
arch/x86/scripts/compile_kernels.sh  PTX→cubin→C array build pipeline (requires CUDA toolkit)
arch/x86/scripts/cubin2array.py      ELF parser: extracts SASS .text from cubin, generates C byte array header
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
arch/x86/kernel/idt.c               IDT setup, exception handlers, APIC timer, 8259 PIC remap
arch/x86/kernel/isr_stubs.S         ISR entry points (save GPRs, call C handler, IRETQ)
arch/x86/kernel/paging.c            4-level page tables, identity map, MMIO map, CR3 switch
arch/x86/kernel/heap.c              kmalloc/kfree first-fit heap (auto-grow, coalescing)
arch/x86/kernel/syscall.c           SYSCALL MSR setup, dispatch table, write/read/exit/brk
arch/x86/kernel/syscall_entry.S     SYSCALL entry point (arg shuffle, STI+JMP return)
arch/x86/kernel/elf.c               ELF64 loader (PT_LOAD, stack setup, entry jump)
arch/x86/kernel/process.c           Process table, exec/exit/waitpid, setjmp/longjmp lifecycle
arch/x86/kernel/setjmp.S            kern_setjmp/longjmp (RBX,RBP,R12-R15,RSP,RIP)
arch/x86/kernel/keyboard.c          PS/2 keyboard (scancode set 1, IRQ 1, ring buffer)
arch/x86/kernel/terminal.c          Line editor (readline, backspace, history, Ctrl shortcuts)
arch/x86/kernel/net.c               Network stack (ARP, IPv4, UDP, ICMP, TCP client, DNS resolver)
arch/x86/kernel/net.h               Network types + API (eth/arp/ip/udp/tcp structs)
arch/x86/kernel/crypto.h            Crypto primitives API (SHA-1, SHA-256, HMAC, AES-128-GCM, X25519)
arch/x86/kernel/crypto.c            Crypto implementation (all from scratch, no external libs)
arch/x86/kernel/zlib.h              zlib API (inflate/deflate for git objects)
arch/x86/kernel/zlib.c              DEFLATE inflate + deflate with LZ77 hash chain
arch/x86/kernel/git.h               Git VCS types + API (init/add/commit/log/status/diff/branch/checkout)
arch/x86/kernel/git.c               Git-compatible VCS (SHA-1 objects, zlib compression, OsitoFS storage)
arch/x86/kernel/tls.h               TLS 1.2 client types + API (tls_conn_t, connect/send/recv/close)
arch/x86/kernel/tls.c               TLS 1.2 client (ECDHE-RSA-AES128-GCM-SHA256, no cert verify)
arch/x86/kernel/http.h              HTTP client API (http_session_t, open/request/read_body/close)
arch/x86/kernel/http.c              HTTPS client (GET/POST, chunked transfer, streaming body)
arch/x86/kernel/claude.h            Claude API client types + API (chat, ask, streaming)
arch/x86/kernel/claude.c            Claude Messages API (JSON builder, SSE parser, streaming, multi-turn session)
arch/x86/kernel/tokenizer.h         BPE tokenizer API (tok_entry_t, tok_merge_t, tokenizer_t)
arch/x86/kernel/tokenizer.c         BPE encode/decode (FNV-1a hash, greedy+merge, GGUF vocab)
arch/x86/kernel/smp.h               SMP types (cpu_info_t, spinlock_t) + API
arch/x86/kernel/smp.c               Multi-core startup (MADT parse, trampoline, INIT-SIPI-SIPI)
arch/x86/kernel/ap_trampoline.S     AP trampoline source (16→32→64 mode transition)
arch/x86/kernel/dynlink.c            Dynamic linker (dl_open/dl_sym/dl_close, ELF relocation, kernel symbol export)
arch/x86/kernel/shell.c             Interactive shell (18 builtins, argv parser, ELF exec)
arch/x86/include/types.h            Freestanding types + MMIO + port I/O
arch/x86/libc/crt.c                 Minimal CRT (_start, printf, malloc, POSIX I/O wrappers)
arch/x86/libc/syscall.S             Raw SYSCALL instruction wrappers (__syscall1-4)
arch/x86/libc/Makefile              Build CRT + tcclib + link userspace ELFs
arch/x86/libc/tcclib.c              Extended libc for TCC/QuickJS (FILE*, fprintf, strtol, qsort, __udivti3, fesetround)
arch/x86/libc/math.c               Freestanding x87 FPU math library (35+ functions: sin/cos/exp/log/pow/sqrt/etc)
arch/x86/libc/qjs_main.c           QuickJS REPL wrapper (JS_NewRuntime2 custom allocator, console.log, file eval)
arch/x86/libc/qjs_headers/          Freestanding shim headers (stdlib/stdio/math/string/etc for QuickJS)
arch/x86/test/tiny.c                Minimal test C program for TCC compilation test
arch/x86/test/testmod.c              Test dynamic module (mod_hello, mod_add, mod_factorial, mod_square)
arch/x86/test/qjs.elf              QuickJS interpreter binary (1002KB, static ET_EXEC)

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
| **X34** | **Compute class bind + kernel dispatch** (pushbuffer encoding, SET_OBJECT, CTRL_BIND/SCHEDULE, semaphore fence) | Done |
| **X35** | **Copy Engine DMA** (CE class bind, H2D/D2H physical copy, semaphore fence, chunked transfers) | Done |
| **X36** | **Kernel completion + semaphore sync** (QMD build QMDV02_03, SEND_PCAS dispatch, kernel wait, CE result readback) | Done |
| **X37** | **SASS kernel infrastructure** (128-bit instruction encoding, pre-encoded NOP/S2R/NOP4 kernels, VRAM upload via PRAMIN, dispatch smoke test) | Done |
| **X38** | **GMMU page tables** (GP100+ MMU v2, 5-level identity map, constant buffer QMD, memory windows) | Done |
| **X39** | **GPU tensor ops** (store_pattern STG test, vec_add_f32 kernel, VRAM buffer mgmt, dispatch layer, PTX docs) | Done |
| **X40** | **GPU-accelerated inference** (hybrid GPU/CPU forward pass, kernel dispatch table, VRAM scratch, benchmark) | Done |
| **X41** | **PTX kernel compilation pipeline** (ntransformer CUDA→PTX translation, ptxas build, cubin ELF extract, C array embed) | Done |
| **X42** | **Compiled kernel dispatch** (CB0 parameter passing, gpu_dispatch_kernel helper, per-kernel wrappers for all 8 PTX kernels) | Done |
| **X-CPU2** | **NVMe write** (IO write command, nvme_write/write_bytes/flush, OsitoFS v2 create/write/delete) | Done |
| **X-CPU3** | **UDP prompt server** (port 7777, token ID input/output, inference dispatch, keep model alive) | Done |
| **X-OS1** | **IDT + Exceptions + APIC timer** (256-entry IDT, ISR stubs, #PF/#GP/#UD handlers with register dump, xAPIC periodic timer vector 32) | Done |
| **X-OS2** | **Paging** (4-level x86-64 page tables, identity map RAM+MMIO with 2MB large pages, CR3 switch, paging_map_mmio API) | Done |
| **X-OS3** | **Heap allocator** (kmalloc/kfree/kcalloc/krealloc, first-fit free list, block coalescing, auto-grow via page allocator) | Done |
| **X-OS4** | **Syscall interface** (SYSCALL/SYSRET via LSTAR/STAR/FMASK MSRs, dispatch table, write/read/exit, fd table with stdin/stdout/stderr) | Done |
| **X-OS5** | **ELF loader** (ELF64 validation, PT_LOAD segment loading, stack setup with argc/argv/envp, entry point jump) | Done |
| **X-OS6** | **Process subsystem** (process_t table, PID alloc, per-process FD table, proc_exec/exit/waitpid, kernel PID 0) | Done |
| **X-OS7** | **Terminal line editor** (readline with echo, backspace, insert mode, Ctrl+C/D/U/A/E/W, command history) | Done |
| **X-OS8** | **PS/2 keyboard driver** (scancode set 1→ASCII, shift/ctrl/caps, ring buffer, IRQ 1 via 8259 PIC vector 0x71) | Done |
| **X-OS9** | **Mini shell** (command parser, 12 builtins: help/uname/ps/mem/uptime/echo/ls/cat/exec/clear/reboot/halt) | Done |
| **X-OS10** | **File I/O syscalls + GDT fix** (open/close/read/lseek/fstat/brk/writev, OsitoFS fd table, GDT relocation, ET_EXEC memory lifecycle) | Done |
| **X-OS11** | **TCC cross-compilation** (TCC 0.9.28rc compiles C → .o, ld links with CRT → static ET_EXEC ELF for OsitoK) | Done |
| **X-OS12** | **Minimal CRT** (crt.c: _start, printf, malloc, open/close/read/write/lseek, strlen/memset/memcpy; syscall.S: raw SYSCALL wrappers) | Done |
| **X-OS13** | **TCC in-OS compilation** (TCC 0.9.28rc runs inside OsitoK, compiles .c → .o, extended libc: FILE*, fprintf, strtol, qsort, setjmp) | Done |
| **X-NET1** | **ICMP** (echo request/reply, ping command in shell, IP checksum verification) | Done |
| **X-NET4** | **TLS 1.2 + Crypto** (SHA-256, HMAC, AES-128-GCM, X25519, ECDHE-RSA handshake, gateway routing) | Done |
| **X-NET5** | **HTTP client** (GET/POST over HTTPS, chunked transfer-encoding, streaming body, curl command) | Done |
| **X-CL1** | **Claude API client** (Messages API, JSON builder, SSE streaming, apikey/ask commands) | Done |
| **X-CL2** | **Claude REPL** (multi-turn conversation, session history, sliding window, `claude` shell command) | Done |
| **X-CL3** | **Tool use: file read/write** (Anthropic tool_use protocol, SSE tool parsing, file_read/write/list, tool loop) | Done |
| **X-CL4** | **Tool: exec** (compile+run C code via TCC, execute ELF binaries, stdout capture) | Done |
| **X-CL5** | **Tool: search** (grep-like substring search across OsitoFS files, line-number matches) | Done |
| **X-NET2** | **TCP stack** (client-only, 3-way handshake, send/recv, FIN close, tcptest shell command) | Done |
| **X-NET3** | **DNS resolver** (UDP query to SLIRP DNS, A record parse, resolve shell command) | Done |
| **X-TOK1** | **BPE tokenizer** (Llama 3 BPE encode/decode, GGUF vocab extraction, FNV-1a hash, greedy+merge) | Done |
| **X-INF1** | **GPU inference dispatch** (matvec_q4_0 + rmsnorm + silu_mul + rope + vec_add on GPU, VRAM save/restore, CPU fallback) | Done |
| **X-INF2** | **VRAM-resident activations** (activations stay in VRAM between ops, only weights uploaded per dispatch, zero-transfer for silu_mul/rope/residual) | Done |
| **X-INF3** | **VRAM-resident weights** (all model weights uploaded to VRAM at boot, weight cache lookup, zero-transfer matvec/rmsnorm, GPU logits, GMMU 768MB identity map) | Done |
| **X-SMP** | **Multi-core AP startup** (ACPI MADT parse, INIT-SIPI-SIPI, 16→32→64 trampoline, AP LAPIC init, `cpus` shell command) | Done |
| **X-PIPE** | **Pipes, dup2, signals, shell redirection** (pipe() circular buffer, dup2(), kill(), sigaction(), shell `>` / `>>` operators) | Done |
| **X-DYN** | **Dynamic linking** (dl_open/dl_sym/dl_close, ET_DYN ELF loading, PT_DYNAMIC parse, R_X86_64_RELATIVE/GLOB_DAT/JUMP_SLOT relocations, kernel symbol export, `dl` shell command) | Done |
| **X-JS** | **QuickJS JavaScript engine** (QuickJS 2024-01-13 bare-metal port, x87 FPU math library, freestanding shim headers, `js` shell command, 1002KB ELF) | Done |
| **X-GIT** | **Git version control** (SHA-1 + zlib DEFLATE, standard git objects, init/add/commit/log/status/diff/branch/checkout, `git` shell command) | Done |
| **X-SCHED** | **Preemptive scheduler** (APIC timer round-robin, fake interrupt frame spawn, RSP-swap context switch in ISR stub, BSP-only guard for SMP safety, `sched` shell command) | Done |
| **X-MMAP** | **mmap/munmap/mprotect** (MAP_ANONYMOUS identity-mapped, VMA tracking, page-level protection, CRT wrappers, 6/6 QEMU tests pass) | Done |
| **X-VFS** | **Virtual filesystem layer** (/dev/null,zero,urandom,console + /proc/self/status,maps + getcwd/readlink/getdents64 syscalls, 7/7 QEMU tests pass) | Done |
| **X-MUSL** | **musl libc port** (cross-compiled musl 1.2.5 static libc, +20 syscalls: arch_prctl/set_tid_address/clock_gettime/getrandom/nanosleep/getpid/gettid/futex/fcntl/prlimit64/etc, 9/9 QEMU tests pass) | Done |
| **X-FORK** | **fork/wait4/getppid + busybox** (scheduler-based fork, wait4 memory clobber fix, execve /proc/self/exe, 8MB allocator boundary, ioctl/poll/vfork, busybox ash interactive, 3/3 QEMU tests pass) | Done |

> Full GPU roadmap (X27-X40 + contingency): see [docs/x86-gpu-roadmap.md](docs/x86-gpu-roadmap.md)
> Full OS roadmap (Tiers 0-9): see [docs/os-selfhost-roadmap.md](docs/os-selfhost-roadmap.md)
> Binary compatibility roadmap: see [docs/binary-compat-roadmap.md](docs/binary-compat-roadmap.md)
> Paths to Claude analysis: see [docs/paths-to-claude-on-ositok.md](docs/paths-to-claude-on-ositok.md)

**Tier 7+ (next)**: X-BUSYBOX (static busybox binary execution),
X-EDIT (port kilo editor), X-HTTPD (TCP server), X-SELF (self-hosting kernel compile).
See `docs/os-selfhost-roadmap.md` for full details and dependency chains.

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

### X-CPU2: NVMe Write + OsitoFS v2 Write
Full read/write storage support for persistence and file creation.
- **NVMe write**: IO write command (opcode 0x01), same queue infrastructure as read. `nvme_write(lba, count, buf)`, `nvme_write_bytes(byte_offset, buf, len)` with read-modify-write for unaligned access, `nvme_flush()` for cache flush.
- **OsitoFS v2 write**: `osfs2_create(name, size)` allocates contiguous blocks from `next_data_block`, `osfs2_write(file, offset, buf, len)` writes data, `osfs2_delete(name)` marks file invalid. Superblock + file table persisted to NVMe after each mutation.
- **Append-only allocator**: Space from deleted files is not reclaimed (simple bump allocator via `next_data_block`). Sufficient for logging and checkpoint files.

### X-CPU3: UDP Prompt Server
Network-accessible inference over UDP port 7777.
- **Protocol**: Send raw text → receive generated token IDs. Send `#128000 1234` → token ID prefill mode.
- **Prompt handler**: Replaces echo handler. Resets model state, prefills BOS or provided tokens, generates up to 32 tokens via `llama_forward` + argmax, sends token IDs back as decimal text.
- **Model lifecycle**: `llama_state_t` kept alive after boot inference (not freed), pointer stored in `prompt_llama` for handler access.
- **No tokenizer**: Client sends/receives raw token IDs. Text tokenization is client-side responsibility.

### X-OS1: IDT + Exceptions + APIC Timer
Own IDT replacing UEFI-provided one. Foundation for all OS services (paging, syscalls, scheduling).
- **IDT**: 256-entry table, 16 bytes each (4KB), loaded via `LIDT`. 64-bit interrupt gates, DPL=0. CS selector auto-detected from current GDT.
- **ISR stubs**: Assembly entry points (`isr_stubs.S`) — save all 15 GPRs, push vector + error code, call `isr_handler()` in C, restore, IRETQ. Vectors with CPU error code: 8,10-14,17,21,29,30.
- **Exception handlers**: Vectors 0-31 decoded with name + full register dump (RIP, RSP, RAX-R15, RFLAGS). #PF decodes CR2 + fault flags (P/W/U/RSVD/IF). #GP decodes selector index + table (IDT/GDT/LDT). Halts on exception (no recovery yet).
- **APIC timer**: xAPIC mode via MSR 0x1B. Periodic mode, vector 32, divide-by-16, initial count 100000 (~100 Hz). EOI on tick. `idt_get_ticks()` returns monotonic counter.
- **Integration**: Called from `kernel_entry()` after `mem_init()`, before PCI scan. Enables interrupts via `STI`.

### X-OS2: Paging (4-Level x86-64)
Own page tables replacing UEFI's. Identity maps all memory (virt == phys).
- **4-level hierarchy**: PML4 → PDPT → PD → PT. 512 entries per level, 4KB pages.
- **2MB large pages**: Used for aligned 2MB regions (bulk of RAM). Falls back to 4KB for partial ranges.
- **Identity map**: First 4GB always mapped (RAM + legacy MMIO + APIC + PCI config). Extended RAM above 4GB mapped if present.
- **MMIO support**: `paging_map_mmio(phys, size)` maps device MMIO as uncacheable (PWT+PCD flags).
- **CR3 switch**: `paging_init()` builds tables, then atomically switches CR3 with interrupts disabled.
- **API**: `paging_map_page()` for single 4KB mappings with INVLPG, `paging_get_kernel_cr3()` for process cloning, `paging_switch()` for context switch.
- **Integration**: Called after `idt_init()`, before PCI scan. Page tables allocated from physical page allocator.

### X-OS3: Heap Allocator
Kernel-space malloc/free over physical page allocator.
- **Algorithm**: First-fit free list with boundary-tag headers (32 bytes). Adjacent free blocks coalesced on free.
- **API**: `kmalloc(size)`, `kfree(ptr)`, `kcalloc(count, size)`, `krealloc(ptr, size)`.
- **Growth**: Initial 256KB from page allocator. Auto-grows 64KB at a time via `mem_alloc_pages()`.
- **Alignment**: All allocations 16-byte aligned. Minimum allocation 16 bytes.
- **Safety**: Magic number (0x4F53) corruption detection, double-free check.
- **Self-test**: alloc/free/coalesce cycle on init, verifies fragmentation handling.

### X-OS4: Syscall Interface
SYSCALL/SYSRET fast path for user→kernel transitions.
- **MSR setup**: EFER.SCE enable, LSTAR=entry point, STAR=kernel/user CS, FMASK=mask IF+DF+TF.
- **Entry**: `syscall_entry.S` — saves RCX(RIP)/R11(RFLAGS) + callee-saved regs, shuffles args to C ABI, calls `syscall_dispatch()`, SYSRETQ.
- **Syscalls**: write(1), read(0), exit(60), brk(12) implemented. open/close/arch_prctl are stubs returning -ENOSYS.
- **FD table**: 16 entries. fd 0=stdin(read→EOF), fd 1=stdout(write→serial+FB), fd 2=stderr(same as stdout).
- **Linux ABI**: RAX=nr, RDI/RSI/RDX/R10/R8/R9=args. Return in RAX. Compatible with static Linux binaries.

### X-OS5: ELF64 Loader
Load and execute ELF64 binaries from OsitoFS.
- **Validation**: Magic, class (64-bit), endianness (LE), machine (x86-64), type (EXEC or DYN).
- **Loading**: Iterates PT_LOAD segments, allocates pages, copies file data, zeros BSS. Adjusts entry point for physical load address.
- **Stack setup**: 64KB stack with argc/argv/envp layout (Linux-compatible: argc, argv ptrs, NULL, envp, NULL).
- **Execution**: `elf_jump()` sets RSP and jumps to entry. Currently ring-0 only.
- **API**: `elf_exec(filename, argc, argv)` — reads from OsitoFS, loads, and jumps. Does not return.

### X-OS6: Process Subsystem
Minimal process abstraction for exec/exit lifecycle.
- **Process table**: 16 slots, states: FREE/READY/RUNNING/ZOMBIE. PID auto-increment.
- **Per-process FD table**: 16 entries cloned from parent. stdin/stdout/stderr initialized to console.
- **Kernel process**: PID 1 created at init, set as `current_proc`.
- **proc_exec()**: Allocates process slot, sets current, calls `elf_exec()`. On failure, cleans up.
- **proc_exit()**: Marks process as ZOMBIE, stores exit code. Cleanup via `proc_waitpid()`.
- **proc_waitpid()**: Synchronous — checks for ZOMBIE, returns exit code, frees process.
- **proc_list()**: Dumps process table (PID, state, name) for debugging.
- **Memory tracking**: `mem_region_t` array per process for cleanup on exit.

**Tier 1 milestone**: OsitoK can now load and execute an ELF64 binary from OsitoFS that uses `write(1, "Hello\n", 6); exit(0);` syscalls.

### X-OS7: Terminal Line Editor
Line-buffered input with editing, bridging keyboard to shell.
- **Readline**: `term_readline(prompt, buf, size)` — blocking line input with echo. Returns length, -1 on EOF (Ctrl+D).
- **Editing**: Insert mode, backspace (with visual redraw), Ctrl+A (home), Ctrl+E (end), Ctrl+U (kill line), Ctrl+W (delete word backward).
- **Signals**: Ctrl+C returns empty line (shell interprets as cancel), Ctrl+D on empty line returns EOF.
- **History**: 8-entry circular buffer. Duplicate detection (won't add same command twice in a row).
- **Output**: Dual serial + framebuffer via `term_putchar()`/`term_puts()`.
- **Files**: `arch/x86/kernel/terminal.c`

### X-OS8: PS/2 Keyboard Driver
Scancode set 1 translation with modifier tracking and ring buffer.
- **Translation**: 128-entry normal + shifted tables. ASCII for printable keys, control codes for special keys.
- **Modifiers**: Left/right Shift, Ctrl, Alt, Caps Lock (toggle). Ctrl+C → ETX (0x03), Ctrl+D → EOT (0x04).
- **Ring buffer**: 64-byte circular buffer. `kb_getchar()` (blocking, HLT-based), `kb_trygetchar()` (non-blocking), `kb_has_input()`.
- **IRQ**: 8259 PIC IRQ 1 → IDT vector 0x71 via dedicated `isr_stub_33`. PIC EOI in ISR handler.
- **PIC remap**: 8259 PIC remapped to vectors 0x70-0x7F (master) / 0x78-0x7F (slave) to avoid collision with APIC timer at vector 32.
- **Files**: `arch/x86/kernel/keyboard.c`, ISR in `arch/x86/kernel/idt.c`

### X-OS9: Mini Shell
Interactive command shell with builtins and argument parsing.
- **Parsing**: Whitespace-delimited argv splitting, max 16 args.
- **Builtins**: `help`, `uname`, `ps`, `mem`, `uptime`, `echo`, `ls`, `cat`, `exec`, `cc`/`tcc`, `ping`, `resolve`, `tcptest`, `tlstest`, `curl`, `apikey`, `ask`, `clear`, `reboot`, `halt`.
- **exec**: Runs ELF binary from OsitoFS via `proc_exec()`. Process exit returns to shell.
- **cc/tcc**: Compile C with TCC. `cc file.c` produces file.elf, `cc -run file.c` compiles+executes.
- **cat**: Reads file from OsitoFS, displays as text (non-printable → '.'), max 4KB preview.
- **reboot**: Triple fault via zero-length IDT + INT3.
- **Network**: `net_poll()` called between commands (keeps UDP echo server responsive).
- **Files**: `arch/x86/kernel/shell.c`

### Tier 2 Key Fixes
- **kern_setjmp/longjmp** (`setjmp.S`): Saves/restores RBX,RBP,R12-R15,RSP,RIP. Used by proc_exec to save kernel context; proc_exit longjmps back. Cleanly bypasses syscall return path.
- **SYSCALL return race**: `popfq` has no interrupt shadow — APIC timer could fire between IF=1 and `jmp *%rcx`, corrupting return. Fix: `sti; jmp *%rcx` (STI shadows the next instruction).
- **STAR MSR**: SYSCALL CS base = `kernel_ss - 8` so SS = valid data segment (0x30), not TSS (0x40).

**Tier 2 milestone**: Interactive `osito>` shell with keyboard input, ELF execution, filesystem browsing. 20/20 QEMU test stability.

### X-OS10: File I/O Syscalls + GDT Relocation
Complete file I/O syscall set and critical GDT memory safety fix.
- **File syscalls**: open(2), close(3), read(0), lseek(8), fstat(5), brk(12), writev(20), ioctl(16). Linux-compatible numbers and semantics.
- **FD table**: 16 entries per process. Types: FD_TYPE_CONSOLE (stdin/stdout/stderr) and FD_TYPE_FILE (OsitoFS files). open() returns lowest free fd, close() releases it.
- **OsitoFS integration**: open() calls `osfs2_find()`/`osfs2_create()`, read/write dispatch to `osfs2_read()`/`osfs2_write()`. Tracks file offset per fd. O_CREAT, O_APPEND, O_TRUNC flags.
- **lseek**: SEEK_SET/SEEK_CUR/SEEK_END. fstat: returns st_size/st_mode/st_blksize for files, S_IFCHR for console.
- **brk**: Lazy 4MB heap allocation via kmalloc. brk(0) returns current break, brk(addr) adjusts within region. Zeroes newly exposed memory.
- **GDT relocation** (`idt.c:gdt_init()`): UEFI's GDT lives in boot services memory freed by mem_init(). iretq in ISR stubs reloads CS/SS from GDT on every interrupt return. When allocations overwrite the GDT, iretq jumps to garbage. Fix: copy GDT to static BSS buffer + LGDT before any page allocations.
- **ET_EXEC memory lifecycle**: `mem_reserve_range()` validates fixed-address ELF loads against page allocator bitmap. `proc_add_region()` registers ELF segments + stack with process for cleanup on exit. `syscall_reset_process()` frees file FDs and brk heap between processes.
- **Test**: `fileio.c` — 11 tests (open, read, lseek, fstat, close, read-after-close, ENOENT, brk alloc, brk read/write). All pass with sequential hello.elf → fileio.elf execution.
- **Files**: `syscall.c` (file syscalls), `memory.c` (mem_reserve_range), `elf.c` (lifecycle), `process.c` (proc_add_region), `idt.c` (gdt_init), `test/fileio.c`

**Tier 2 complete**: Full file I/O + stable sequential ELF execution. Ready for Tier 3 (TCC cross-compilation).

### X-OS11: TCC Cross-Compilation
TCC (Tiny C Compiler) 0.9.28rc compiles C programs for OsitoK from Linux host.
- **TCC source**: Built from git (`repo.or.cz/tinycc.git`), installed to `~/tcc-install/`.
- **Compilation flow**: `tcc -c -nostdlib -nostdinc app.c -o app.o` → `ld -nostdlib -static -no-pie -e _start -Ttext=0x401000 crt.o syscall.o app.o -o app.elf`
- **Output**: ET_EXEC ELF64, static, non-PIE at 0x400000. Runs on OsitoK via `proc_exec()`.
- **Verified**: hello_c.elf (TCC-compiled) runs printf, malloc, file I/O on OsitoK.
- **Build**: `make -C arch/x86/libc TCC=~/tcc-install/bin/tcc hello_c.elf`
- **Files**: `arch/x86/libc/Makefile`

### X-OS12: Minimal CRT (C Runtime)
Freestanding C runtime so TCC-compiled programs can use printf, malloc, and file I/O.
- **_start** (`crt.c`): ELF entry point. Reads argc/argv from stack, calls main(), calls _exit().
- **Syscall wrappers** (`syscall.S`): `__syscall1` through `__syscall4` — raw SYSCALL instruction with arg shuffle.
- **POSIX I/O**: write, read, open, close, lseek via syscall wrappers.
- **malloc/free**: brk-based bump allocator. malloc grows heap in 4KB increments via sys_brk. free is a no-op.
- **printf**: Minimal variadic printf supporting %s, %d, %ld, %u, %lu, %x, %lx, %p, %c, %%.
- **String ops**: strlen, memset, memcpy, strcmp.
- **Files**: `arch/x86/libc/crt.c`, `arch/x86/libc/syscall.S`, `arch/x86/libc/Makefile`

### X-OS13: TCC In-OS Compilation
TCC 0.9.28rc compiled as a static ELF64 binary running inside OsitoK. Can read C source from OsitoFS and produce .o object files.
- **TCC object**: GCC cross-compiles tcc.c with `-DONE_SOURCE=1 -DTCC_TARGET_X86_64=1 -DCONFIG_TCC_STATIC=1 -std=gnu11 -fno-builtin`. 556KB .o with 317 text symbols.
- **Extended libc** (`tcclib.c`): ~600 lines providing everything TCC needs beyond crt.c:
  - **FILE* I/O**: fopen/fclose/fread/fwrite/fseek/ftell/fflush/fgetc/fputc/fputs/fdopen/freopen. Static FILE array, unbuffered stdout/stderr, 1KB buffered for file I/O.
  - **Formatted output**: vsnprintf core with full format support (%d/%u/%x/%o/%s/%c/%p/%f, width, precision, length modifiers). fprintf/sprintf/snprintf/vfprintf all via common formatter.
  - **String/memory**: strcpy/strncmp/strchr/strrchr/strstr/strpbrk/memmove/memcmp/strdup/strerror + ctype functions.
  - **Number conversion**: strtol/strtoul/strtoull + `__isoc23_*` aliases (GCC 15 glibc redirect), strtod/strtof/strtold, ldexpl.
  - **Memory**: realloc (allocate + copy for bump allocator), calloc.
  - **qsort**: Insertion sort (sufficient for TCC's symbol tables).
  - **OS stubs**: getenv→NULL, getcwd→"/", realpath→identity, signal/sigaction→0, sem_*→no-op, mprotect→0, time→epoch 0.
  - **exit()**: Flushes all FILE streams before syscall exit.
  - **errno**: `__errno_location()` returns static int, `__assert_fail()` prints and exits.
- **_setjmp/longjmp** (`syscall.S`): Save/restore RBX,RBP,R12-R15,RSP,RIP. 64-byte jmp_buf.
- **_start fix** (`syscall.S`): Assembly _start reads argc/argv from RSP BEFORE any C prologue. TCC-generated C _start pushed RBP+SUB $0x20 before reading RSP → corrupted argc/argv.
- **Syscalls added**: access(21), unlink(87) for TCC file operations.
- **Build**: `ld -nostdlib -static -no-pie -e _start crt.o syscall.o tcclib.o tcc.o → tcc.elf` (406KB).
- **Verified**: `tcc -c -nostdlib -nostdinc tiny.c -o tiny.o` produces object file inside OsitoK.
- **Files**: `arch/x86/libc/tcclib.c`, `arch/x86/libc/syscall.S` (extended), `arch/x86/libc/Makefile` (extended)

**Tier 3 milestone**: TCC compiler runs inside OsitoK, reads C source from OsitoFS, and writes compiled object files back to disk.

### X-NET2: TCP Stack (Client-Only)
Minimal TCP implementation for outgoing connections. No listen/accept, no retransmission, polling-based.
- **3-way handshake**: SYN → SYN-ACK → ACK. Blocking `net_tcp_connect()` with 5s timeout.
- **TCP checksum**: Pseudo-header (src/dst IP, proto, length) + segment. IP words read as native 16-bit for endianness consistency.
- **State machine**: CLOSED → SYN_SENT → ESTABLISHED → FIN_WAIT_1 → FIN_WAIT_2 → TIME_WAIT → CLOSED. Also CLOSE_WAIT → LAST_ACK for remote-initiated close.
- **Send**: `net_tcp_send()` chunks data at MSS=1460 bytes, PSH+ACK flags.
- **Receive**: `net_tcp_recv()` (non-blocking) and `net_tcp_recv_timeout()` (blocking). Data buffered in 8KB rx_buf per connection, shifted on read.
- **Close**: `net_tcp_close()` sends FIN+ACK, waits for FIN response (3s timeout). TIME_WAIT skipped (no 2MSL needed in bare-metal).
- **Connection table**: 4 static `tcp_conn_t` slots. Per-connection: state, IPs, ports, sequence numbers, rx buffer.
- **ISN**: Simple counter (0x12345678 + 64000 per connection). Not cryptographically random.
- **Shell command**: `tcptest [ip] [port]` — connects, sends HTTP GET, displays response.
- **Bug fix**: `proc_exit` via longjmp bypasses SYSRET which would re-enable interrupts. Added `sti` after longjmp return in `proc_exec`. Without this, HLT hangs forever after any ELF execution.
- **Verified**: Full HTTP GET/response through QEMU SLIRP guestfwd. All checksums correct (tcpdump verified).
- **Files**: `arch/x86/kernel/net.c` (TCP implementation), `arch/x86/kernel/net.h` (types + API), `arch/x86/kernel/shell.c` (tcptest command), `arch/x86/kernel/process.c` (STI fix)

### X-NET3: DNS Resolver
Minimal DNS client over UDP. Resolves A records (IPv4 addresses) from hostnames.
- **Query format**: Standard DNS header (12B) + QNAME labels + QTYPE=A + QCLASS=IN. Transaction ID for matching.
- **Response parse**: Skips question section, iterates answer RRs, finds first TYPE=A (rdlength=4), extracts IPv4.
- **Name compression**: Handles DNS pointer labels (0xC0 prefix) in both question and answer sections.
- **ARP retry**: `net_udp_send` returns -1 if ARP not resolved. DNS send retries up to 5 times with poll between attempts.
- **Default server**: 10.0.2.3 (QEMU SLIRP DNS). Configurable via `net_dns_set_server()`.
- **Blocking**: Polls for 3s (300 ticks) with HLT between polls.
- **Shell command**: `resolve <hostname>` — displays resolved IP address.
- **Verified**: `api.anthropic.com` → `160.79.104.10` via QEMU SLIRP DNS.
- **Files**: `arch/x86/kernel/net.c` (dns_handler, net_dns_resolve), `arch/x86/kernel/net.h` (API), `arch/x86/kernel/shell.c` (resolve command)

### X-NET4: TLS 1.2 + Crypto Primitives
Complete TLS 1.2 client with all crypto implemented from scratch (no external libraries).
- **Cipher suite**: TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256 (0xC02F). Only suite offered/accepted.
- **Crypto primitives** (`crypto.c`, ~750 lines):
  - **SHA-256** (FIPS 180-4): Full implementation with init/update/final API.
  - **HMAC-SHA-256** (RFC 2104): Used for TLS PRF and Finished verify.
  - **AES-128** (FIPS 197): Key expansion + single-block encrypt. T-tables for performance.
  - **AES-128-GCM** (NIST SP 800-38D): GHASH + CTR mode. Encrypt and decrypt with authentication tag.
  - **X25519** (RFC 7748): Curve25519 scalar multiplication. 16-limb field elements (16-bit limbs in int64_t). Montgomery ladder, Fermat inverse. Proper final reduction via conditional subtraction of p.
- **Self-test** (`crypto_selftest()`): Runs at boot. SHA-256 "abc", HMAC RFC 4231, AES FIPS 197 Appendix B, GCM NIST Test Case 3, GCM round-trip + tamper, X25519 RFC 7748 Section 6.1.
- **TLS handshake** (`tls.c`, ~530 lines):
  - **ClientHello**: SNI extension, supported_groups (x25519), signature_algorithms (rsa_pkcs1_sha256).
  - **ServerHello parse**: Validates version=0x0303, cipher=0xC02F.
  - **Certificate**: Received but NOT verified (no CA trust store).
  - **ServerKeyExchange**: Extracts x25519 server pubkey, computes ECDHE shared secret.
  - **Key derivation**: PRF-SHA-256 for master_secret (48B) and key expansion (write keys + IVs).
  - **Finished**: verify_data via PRF over handshake transcript hash. Encrypted with AES-128-GCM.
  - **Application data**: GCM encrypt/decrypt with per-record nonce (implicit_iv || seq_num).
- **Gateway routing** (`net.c`): `arp_nexthop()` routes off-subnet IPs through default gateway (10.0.2.2 for QEMU SLIRP). Applied to ICMP, UDP, TCP send paths.
- **Shell command**: `tlstest` — DNS resolve + TCP connect + TLS handshake + HTTPS GET to example.com.
- **PRNG**: RDTSC-seeded xorshift64 for client_random and ECDHE private key.
- **Verified**: HTTPS GET to example.com returns HTTP/1.1 200 OK over encrypted TLS 1.2 channel.
- **Files**: `arch/x86/kernel/crypto.h`, `arch/x86/kernel/crypto.c`, `arch/x86/kernel/tls.h`, `arch/x86/kernel/tls.c`, `arch/x86/kernel/net.c` (gateway routing), `arch/x86/kernel/shell.c` (tlstest command)

### X-NET5: HTTP Client
HTTPS client over DNS + TCP + TLS. Supports GET/POST, chunked transfer-encoding, streaming body callback.
- **Session API**: `http_open(session, hostname)` — DNS resolve + TCP connect (port 443) + TLS 1.2 handshake in one call. `http_close(session)` tears down everything.
- **Request API**: `http_request(session, method, path, hostname, headers, body, body_len, resp)` — builds and sends HTTP request, reads and parses response status + headers. Handles `Connection: close`, `User-Agent`, `Content-Length`.
- **Response parsing**: Reads until `\r\n\r\n`, parses status code, up to 16 response headers (case-insensitive lookup), detects `Transfer-Encoding: chunked` and `Content-Length`.
- **Body reading**: `http_read_body(session, resp, callback, ctx)` — streaming body via callback. Handles both chunked (hex-size parsing, chunk-by-chunk delivery) and content-length modes. `http_read_body_full()` convenience wrapper reads into buffer.
- **Leftover handling**: Data received after header `\r\n\r\n` but before `http_read_body` call is stashed in static buffer (4KB) and delivered first.
- **Shell command**: `curl <hostname> [path]` — HTTPS GET, displays first 1000 chars of response body.
- **Verified**: `curl example.com /` returns 528 bytes HTML (`<!doctype html><html lang="en">...Example Domain...`).
- **Files**: `arch/x86/kernel/http.h`, `arch/x86/kernel/http.c`, `arch/x86/kernel/shell.c` (curl command)

### X-CL1: Claude API Client
Native C client for Anthropic Messages API with SSE streaming.
- **JSON builder**: Minimal hand-rolled JSON serialization (string escaping, integer, array). Builds `{"model":"...","max_tokens":N,"stream":true,"messages":[...]}`.
- **JSON parser**: `json_find_str()` — finds `"key":"value"` pairs in flat JSON. No full parser, sufficient for SSE event parsing.
- **SSE parser**: `sse_body_cb()` — processes `data: {...}` lines from streaming response. Extracts text from `content_block_delta` events. JSON-unescapes `\n`, `\r`, `\t`, `\"`, `\\`.
- **API flow**: `claude_chat()` — kmalloc session+response, http_open to api.anthropic.com, POST /v1/messages with JSON body, stream SSE response via http_read_body callback, extract and deliver text chunks.
- **Error handling**: HTTP non-200 responses logged with first 200 chars of error body.
- **API key**: `claude_set_api_key()` stores in static buffer. Shell command `apikey sk-ant-...` sets it.
- **Shell commands**: `apikey [key]` — set/show API key. `ask <prompt>` — single-turn chat with streaming output.
- **Convenience**: `claude_ask(prompt, buf, size)` — single-turn, response to buffer.
- **Files**: `arch/x86/kernel/claude.h`, `arch/x86/kernel/claude.c`, `arch/x86/kernel/shell.c` (apikey/ask commands)

### X-CL2: Claude REPL
Multi-turn interactive conversation with Claude from OsitoK shell.
- **Session state**: `claude_session_t` — 8-turn history (user + assistant), sliding window (oldest turn discarded when full). ~40KB per session via kmalloc.
- **Message builder**: Constructs alternating user/assistant message array from history. Current turn is last user message (no assistant yet). Past turns include both.
- **Response capture**: Dual callback — captures response into `pending_response` buffer for history AND forwards to user's display callback for streaming output.
- **Sliding window**: When turn_count reaches CLAUDE_SESSION_MAX_TURNS (8), shifts all turns left, discards turn 0. Keeps conversation flowing without unbounded memory.
- **JSON buffer scaling**: `claude_chat()` allocates `4096 + msg_count * 2048` bytes for request JSON, scaling with conversation length.
- **Shell command**: `claude` — enters REPL mode with `you>` prompt. Builtins: `quit` (exit), `clear` (reset history), Ctrl+D (exit). Reuses `term_readline()` for input with editing + history.
- **Integration**: `ask` command remains for single-turn. `claude` is the multi-turn REPL. Both use the same streaming callback for output.
- **Files**: `arch/x86/kernel/claude.h` (claude_session_t, session API), `arch/x86/kernel/claude.c` (session_send, sliding window), `arch/x86/kernel/shell.c` (cmd_claude REPL loop)

### X-CL3: Tool Use — File Read/Write/List
Claude can read, write, and list files on OsitoFS via Anthropic tool_use protocol. Fully integrated into the claude REPL.
- **Tools**: `file_read` (read file by name), `file_write` (create/overwrite file with content), `file_list` (list all files with sizes).
- **SSE parsing extensions**: Detects `content_block_start` (type `tool_use` → extract `id`, `name`), `content_block_delta` (type `input_json_delta` → accumulate partial JSON), `content_block_stop` (reset cur_tool), `message_delta` (detect `stop_reason: "tool_use"`).
- **Tool execution**: `tool_execute()` dispatches by name to `tool_file_read/write/list`. Uses OsitoFS API (`osfs2_find`, `osfs2_read`, `osfs2_create`, `osfs2_write`, `osfs2_file_at`).
- **Tool loop**: Up to 5 iterations. After each tool_use response: execute tools → build assistant content JSON (text + tool_use blocks) → build user tool_result JSON → re-request with full history.
- **Raw JSON content**: Messages with content starting with `[` are embedded as raw JSON arrays (for tool_use/tool_result blocks). Regular string content is quoted as before.
- **Request builder**: `build_request_json()` now accepts `with_tools` flag to append tools definitions. `claude_chat_ex()` internal function accepts tools flag and `claude_tool_state_t *`.
- **OsitoFS accessor**: Added `osfs2_file_at(index)` to iterate files by index for `file_list` tool.
- **Files**: `arch/x86/kernel/claude.h` (claude_tool_state_t, tool use types), `arch/x86/kernel/claude.c` (SSE parser, tool execution, tool loop), `arch/x86/fs/ositofs2.c` (osfs2_file_at)

### X-CL4: Tool Exec — Compile and Run Code
Claude can compile C code with TCC and execute ELF binaries, capturing stdout output.
- **Output capture**: `syscall_capture_start(buf, max)` / `syscall_capture_stop()` in `syscall.c`. `console_write()` copies to capture buffer alongside serial+FB output. Non-invasive — capture is NULL when not in use.
- **Tool: exec**: Run an existing ELF binary from OsitoFS. Capture stdout + report exit code.
- **Tool: run_code**: Write C source to `_cl_tmp.c`, compile with `tcc.elf` (`-nostdlib -nostdinc -static`), run `_cl_tmp.elf`, capture output, clean up temp files. Reports compilation errors if TCC fails.
- **Integration**: Tools added to `tools_json_def`, dispatched in `tool_execute()`. `proc_exec()` handles process lifecycle (setjmp/longjmp). Capture wraps the entire exec call.
- **Files**: `arch/x86/kernel/syscall.c` (capture API), `arch/x86/kernel/claude.c` (tool_exec, tool_run_code)

### X-CL5: Tool Search — Grep-like File Search
Claude can search for text patterns across files on OsitoFS.
- **Tool: search**: Takes `pattern` (required) and optional `path`. Substring match (case-sensitive). Returns `file:line: content` formatted matches.
- **Single file mode**: When `path` is given, searches only that file.
- **All files mode**: When no path, iterates all files via `osfs2_file_at()`. Skips files >256KB (binary/model data).
- **Line scanning**: Reads file into RAM, splits by `\n`, searches each line with `strfind()`. Truncates lines >200 chars in output.
- **Files**: `arch/x86/kernel/claude.c` (tool_search, search_file, strfind)

### X-TOK1: BPE Tokenizer
Byte-pair encoding tokenizer for Llama 3 models. Ported from xasko's C++ tiktoken (reason_agent/tools/tiktoken.cpp).
- **Vocabulary**: Extracted from GGUF metadata (`tokenizer.ggml.tokens` + `tokenizer.ggml.merges`). Zero-copy — string pointers into loaded GGUF file buffer.
- **Hash table**: FNV-1a hash, open addressing (linear probing), 2× vocab size for low collision. Maps byte sequences → token IDs.
- **Encode path**: (1) Word boundary splitter (alpha/digit/space/other classes, leading-space attachment, apostrophe contractions, 3-digit grouping — simplified tiktoken regex). (2) Per-word greedy forward matching to find longest known tokens. (3) Iterative BPE merge loop: find lowest-rank pair → merge → repeat.
- **Decode path**: Direct vocab array lookup. `tok_decode_one()` returns null-terminated string. `tok_global_decode()` wrapper for use from inference.c.
- **Integration**: `gguf_load_tokenizer()` extracts vocab from GGUF model in `main.c`, passes to `tok_init()`. `llama_generate()` in `inference.c` calls `tok_global_decode()` to print decoded text instead of raw token IDs.
- **Limits**: TOK_MAX_VOCAB=200000, TOK_MAX_TOKEN_LEN=128, TOK_MAX_MERGES=200000.
- **Memory**: ~25MB for Llama 3 128K vocab (vocab array + hash table + merge rules). All via kmalloc.
- **Files**: `arch/x86/kernel/tokenizer.h`, `arch/x86/kernel/tokenizer.c`, `arch/x86/fs/gguf.c` (gguf_load_tokenizer), `arch/x86/fs/gguf.h` (gguf_tokenizer_t)

### X-INF1: GPU Inference Dispatch
Full GPU dispatch for the Llama transformer forward pass. 6 of 7 operations dispatch to GPU with transparent CPU fallback.
- **matvec_q4_0 (streaming)**: Upload Q4_0 weights + float input to VRAM, dispatch `gemv_q4_0` kernel, download result. Per-dispatch allocation via save/restore (no VRAM leaks). Falls back to CPU for >16MB matrices (e.g., vocab projection 128K×2048).
- **rmsnorm**: Upload x + weight vectors to VRAM scratch buffers, dispatch `rmsnorm` kernel (eps=1e-5), download normalized output.
- **silu_mul (fused)**: Single GPU kernel replaces CPU `silu_inplace() + vec_mul()`. Dispatches `silu_mul` — computes `SiLU(gate[i]) * up[i]` in one pass. Saves one PCIe round trip.
- **rope (combined q+k)**: Uploads both Q and K vectors, dispatches `rope` kernel for in-place rotation, downloads both back. Replaces two separate CPU `rope()` calls.
- **vec_add (chunked)**: Pre-allocated VRAM scratch buffers (3 × max(dim, ffn_dim) floats). 256-element chunks with PRAMIN transfer per chunk.
- **VRAM allocator fix**: `gpu_tensor_reset()` was destroying pre-allocated scratch buffers. Replaced with `vram_save()/vram_restore()` pattern — saves bump allocator position before temp allocations, restores after download. CB0 allocations inside `gpu_dispatch_kernel()` are also reclaimed.
- **CPU-only ops**: Attention GQA loop (irregular per-head access pattern) and per-head softmax stay on CPU. ~4% of compute.
- **Dispatch per token** (Llama 3.2 1B, 16 layers): 112 matvec_q4_0 + 49 rmsnorm + 16 silu_mul + 16 rope + 32 vec_add = 225 GPU dispatches/token.
- **GMMU**: Identity map expanded 8→24MB (kernels 4MB + tensor buffers 16MB + headroom). VRAM buffer region expanded 4→16MB.
- **Files**: `arch/x86/drivers/gpu_inference.c` (dispatch wrappers + forward pass), `arch/x86/drivers/gpu_tensor.h` (VRAM size), `arch/x86/drivers/gmmu.c` (identity map size)

### X-INF2: VRAM-Resident Activations
Activations stay in VRAM between operations, eliminating per-op activation transfers. Two forward pass modes selected at init based on kernel availability.
- **VRAM-resident mode**: Requires all core kernels (matvec_q4_0, rmsnorm, silu_mul, rope, add_inplace). Activation vectors (x, xb, xb2, q, k, v, hb, hb2) allocated in VRAM at init (~116KB for Llama 3.2 1B). Persist across layers.
- **Data flow**: Embed token on CPU → upload x to VRAM once → run entire transformer chain in VRAM → download final x for logits. Per layer: download q/k/v for CPU attention (12KB), upload xb2 attention output (8KB). All other activations stay in VRAM.
- **Zero-transfer ops**: `silu_mul` (hb,hb2 already in VRAM), `rope` (q,k already in VRAM), `add_inplace` residuals (x,xb already in VRAM). ~200-300KB saved per layer.
- **VRAM-native dispatch helpers**: `gpu_matvec_vram()` (upload weights only, VRAM activations), `gpu_rmsnorm_vram()` (upload weight only), `gpu_silu_mul_vram()`, `gpu_rope_vram()`, `gpu_add_inplace_vram()` — all use save/restore for CB0 temp allocations.
- **Host-dispatch fallback (X-INF1)**: When any core kernel is missing, per-op upload/dispatch/download with CPU fallback. `gpu_llama_forward()` dispatches to VRAM or host path automatically.
- **Logits**: Vocab projection (128K×2048 = ~141MB Q4_0) too large for VRAM — always CPU. Final x downloaded from VRAM first.
- **add_inplace kernel**: Newly detected in init. Replaces vec_add for residual connections (a[i]+=b[i], one less VRAM buffer needed).
- **Files**: `arch/x86/drivers/gpu_inference.h` (gpu_vram_acts_t, vram_resident flag), `arch/x86/drivers/gpu_inference.c` (VRAM dispatch + forward_vram + forward_host)

### X-INF3: VRAM-Resident Weights
All model weights uploaded to VRAM once at boot, eliminating per-dispatch weight transfers via PRAMIN. Combined with X-INF2 VRAM-resident activations = zero PCIe transfers for all ops except attention KV cache.
- **Weight cache**: `weight_cache_t` with `MAX_WEIGHT_CACHE=256` entries. Linear lookup by `host_ptr` (system RAM address → VRAM address). Simple and fast for ~147 tensors.
- **Boot upload**: `gpu_upload_all_weights()` iterates all layers, uploads Q4_0 weight matrices + F32 norm vectors. Progress logged every 4 layers. ~523MB layer weights + ~141MB output = ~664MB for Llama 3.2 1B.
- **GMMU expansion**: Identity map expanded from 24MB to 768MB (VRAM+256MB to VRAM+1024MB). Multi-PD0 page tables: 2 PD1 entries × 1 PD0 page each, 384 SPT pages. ~1.5MB page tables from system RAM.
- **Tensor buffer expansion**: `GPU_TENSOR_VRAM_SIZE_MB` increased from 16 to 700. Bump allocator: permanent allocations (scratch 96KB + activations 116KB + weights ~664MB) at bottom, temp CB0 allocations via save/restore above.
- **Zero-transfer matvec**: `gpu_matvec_vram()` calls `wcache_lookup()` before allocating temp VRAM — if weight found in cache, dispatches directly (only CB0 temp alloc). Falls back to per-dispatch upload if not cached.
- **Zero-transfer rmsnorm**: `gpu_rmsnorm_vram()` same pattern — norm weights cached in VRAM.
- **GPU logits**: When output projection weights are in VRAM, logits matvec (vocab 128K×2048) runs on GPU instead of CPU. Result downloaded (512KB) for argmax.
- **Host-dispatch benefits**: `gpu_matvec_q4_0_dispatch()` (X-INF1 path) also checks weight cache — if weights cached, skips upload even in host-dispatch mode.
- **Partial cache**: If VRAM runs out mid-upload, already-uploaded weights are still usable. `weights_resident` flag only set on full success.
- **Mode auto-selection**: X-INF3 > X-INF2 > X-INF1. Init selects highest available tier based on kernels + VRAM capacity.
- **Estimated boot cost**: ~18s for 664MB via PRAMIN (4 bytes/write, ~100ns each). One-time cost, amortized over all tokens.
- **Per-token savings**: Eliminates ~34MB/token PRAMIN weight upload (112 matvecs × ~0.3MB avg). For 100 tokens: saves ~3.4GB of transfers.
- **Files**: `arch/x86/drivers/gpu_inference.h` (weight_cache_t, weights_resident), `arch/x86/drivers/gpu_inference.c` (wcache_lookup/add, gpu_upload_all_weights), `arch/x86/drivers/gmmu.c` (768MB identity map, multi-PD0), `arch/x86/drivers/gpu_tensor.h` (700MB tensor region)

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

### X34: Compute Class Bind + Kernel Dispatch
Binds compute class to GPFIFO channel, activates channel, pushes initial commands through pushbuffer.
- **Compute classes**: Turing 0xC5C0, Ampere 0xC6C0, Ada 0xC9C0 (per-gen selection via `compute_class_for_gen()`).
- **Pushbuffer format**: SEC_OP(31:29) | COUNT(28:16) | SUBCHANNEL(15:13) | METHOD_ADDR(11:0). Macros: `NV_METHOD()`, `NV_METHOD_NI()`, `NV_METHOD_IMMD()`.
- **Channel activation**: NVA06F_CTRL_CMD_BIND (0xa06f0104) binds to GR0 engine, NVA06F_CTRL_CMD_GPFIFO_SCHEDULE (0xa06f0103) enables on runlist — both via `gsp_rm_control()` on channel handle.
- **Initial pushbuffer sequence**: SET_OBJECT (class on subchannel 1) → INVALIDATE_SHADER_CACHES → WAIT_FOR_IDLE → semaphore release fence.
- **Semaphore sync**: Host allocates 4KB semaphore page. GPU writes payload via SEMAPHORE A/B/C/D release method. Host polls semaphore value with rdtsc timeout.
- **compute_state_t**: Tracks compute class, pushbuffer state, semaphore memory, bind/schedule/ready flags.
- **Integration**: Called from `gsp_boot()` after `gsp_channel_init()`. Without full GSP boot chain, semaphore times out (expected).

### X35: Copy Engine DMA
Host-to-device and device-to-host DMA transfers via the Copy Engine (CE).
- **CE classes**: Turing 0xC5B5, Ampere 0xC6B5, Ada 0xC7B5 (AMPERE_DMA_COPY_B). Per-gen via `ce_class_for_gen()`.
- **Subchannel 4**: CE shares the same GPFIFO channel as compute. SET_OBJECT binds CE class to subchannel 4.
- **Physical addressing**: SET_SRC_PHYS_MODE/SET_DST_PHYS_MODE select COHERENT_SYSMEM (1) or LOCAL_FB (0).
- **Copy sequence**: OFFSET_IN_UPPER/LOWER (src) → OFFSET_OUT_UPPER/LOWER (dst) → LINE_LENGTH_IN + LINE_COUNT → LAUNCH_DMA.
- **LAUNCH_DMA bitfields**: NON_PIPELINED + SRC_PITCH + DST_PITCH + SRC_PHYSICAL + DST_PHYSICAL + SEM_RELEASE_1WORD.
- **Chunking**: Copies larger than 16MB are split into multiple pushbuffer submissions.
- **Semaphore fence**: CE semaphore via SET_SEMAPHORE_A/B/PAYLOAD, monotonic fence_seq counter.
- **API**: `gsp_ce_copy_h2d(src_phys, dst_vram, size)`, `gsp_ce_copy_d2h(src_vram, dst_phys, size)`.
- **Integration**: Called from `gsp_boot()` after `gsp_compute_init()`.

### X36: Kernel Completion + Semaphore Sync
Complete compute dispatch pipeline: QMD construction, kernel launch, wait, result readback.
- **QMD (QMDV02_03)**: 256-byte descriptor with grid/block dims, program address, register count, shared memory, cache invalidation, RELEASE0 semaphore.
- **QMD fields**: DW4 (SM_GLOBAL_CACHING + SEM_ENABLE), DW5 (cache invalidation), DW11 (membar + API_VISIBLE_CALL_LIMIT), DW12-14 (grid dims), DW17 (shared_mem), DW18-19 (version + block dims), DW20 (REGISTER_COUNT_V), DW23-25 (RELEASE0 semaphore), DW48-49 (PROGRAM_ADDRESS).
- **Dispatch**: SEND_PCAS_A (QMD addr >> 8) + SEND_SIGNALING_PCAS_B (Turing: 0x02BC, invalidate+schedule) or SEND_SIGNALING_PCAS2_B (Ampere+: 0x02C0, action=3).
- **Kernel wait**: `gsp_compute_wait()` polls RELEASE0 semaphore address with rdtsc timeout, reports elapsed microseconds.
- **Result readback**: `gsp_compute_read_results()` uses CE D2H copy (X35) to transfer VRAM results to host.
- **Note**: Actual dispatch requires SASS shader (X37). QMD infrastructure is ready.

### X37: SASS Kernel Infrastructure
Pre-encoded SASS (Shader ASSembly) compute kernels for NVIDIA SM75+ GPUs.
- **Instruction format**: 128-bit per instruction (SM70+). Lower 64 bits = opcode + registers + modifiers. Upper 64 bits = control/scheduling codes (stall, yield, barrier, reuse).
- **No SPH needed**: Compute kernels dispatched via QMD don't use Shader Program Header — QMD carries all execution metadata (register count, shared memory, grid/block dims).
- **Pre-encoded kernels**: `nop` (EXIT only, 16B — dispatch smoke test), `s2r_exit` (S2R R2,SR_TID.X + EXIT, 32B — register read test), `nop4_exit` (4×NOP + EXIT, 80B — multi-instruction fetch test).
- **Verified encodings (SM75)**: EXIT=0x794d|0x000fea0003800000, NOP=0x7918|0x000fc00000000000, S2R R2,SR_TID.X=0x027919|0x000e220000002100.
- **VRAM upload**: Kernels placed at VRAM+256MB via PRAMIN window (same X18 pattern), 256-byte aligned, readback verify.
- **Smoke test**: `sass_smoke_test()` dispatches NOP kernel with QMD semaphore (RELEASE0), waits 500ms. Expected to timeout without full GSP boot chain.
- **Files**: `arch/x86/drivers/sass.h` (types, encoding defines, API), `arch/x86/drivers/sass.c` (kernel binaries, upload, smoke test).
- **Integration**: Called from `gsp_boot()` after compute/CE init. Works with or without GSP — PRAMIN upload always succeeds if GPU is present.

### X38: GMMU Page Tables + Kernel Loader
GPU MMU identity map for compute kernel dispatch. Compute engine always uses GPU virtual addresses — GMMU is mandatory.
- **Page table format**: GP100+ MMU v2. 5 levels: PDB(2b) → PD2(9b) → PD1(9b) → PD0(8b,dual) → SPT(9b). 49-bit VA space, 4KB small pages.
- **PTE encoding**: `data = (phys_addr >> 4) | flags`. VALID(bit 0), APERTURE(bits 2:1, 0=VRAM, 2=SYS_COHERENT), VOL(bit 3).
- **PD0 dual PDE**: 16-byte entries — `small_pde = table_addr | flags` (NOT shifted!), `big_pde = 0` (disabled).
- **Identity map**: GPU VA = VRAM physical address. Maps 4MB at VRAM+256MB (SASS kernel region). Requires ~24KB of page tables (1 PDB + 1 PD2 + 1 PD1 + 1 PD0 + 2 SPT pages).
- **Instance block**: PDB physical address written to channel RAMIN offset 0x200. TARGET=SYS_COHERENT (page tables in system RAM, GPU reads via PCIe).
- **Constant buffer**: `compute_dispatch_t` extended with `cbuf_addr`/`cbuf_size`. QMD DW20 bit 0 = CB0_VALID, DW32-33 = CB0 address + size.
- **Memory windows**: SET_SHADER_SHARED_MEMORY_WINDOW (0x077C/0x0780) = 0xFE000000, SET_SHADER_LOCAL_MEMORY_WINDOW (0x07B0) = 0xFF000000. Pushed during compute class initialization.
- **Files**: `arch/x86/drivers/gmmu.c` (page table builder, instance block config).
- **Integration**: Called from `gsp_boot()` before `sass_init()`. Independent of GSP boot — allocates page tables in system RAM.

### X39: GPU Tensor Ops
GPU-accelerated tensor operation dispatch layer with hand-encoded SM75 SASS kernels.
- **store_pattern kernel** (80B, 5 insns): MOV×3 + STG.E + EXIT. Writes 0xCAFEBABE to VRAM+257MB via GMMU identity map. Proves full compute+GMMU+STG pipeline.
- **vec_add_f32 kernel** (368B, 23 insns): S2R + IADD3(×4 offset) + LDG.E + FADD + STG.E. Self-modifying: MOV immediates patched with buffer addresses before VRAM upload.
- **SM75 instruction encodings** (verified): MOV imm(0x7802), STG.E(0x7386), LDG.E(0x7381), FADD(0x7221), S2R(0x7919), IADD3(0x7210). Control words from nvdisasm output.
- **GPU tensor dispatch** (`gpu_tensor.h/c`): VRAM bump allocator (4MB at +260MB), PRAMIN upload/download, dispatch wrappers, self-test.
- **PTX source**: Documented in `gpu_tensor.c` for all target kernels (vec_add, matvec_q4_0, rmsnorm, softmax, rope, silu). Ready for `ptxas --gpu-name sm_75` compilation.
- **Kernel patching**: `sass_patch_vec_add()` modifies MOV immediate fields at known offsets, re-uploads kernel to VRAM.
- **Files**: `arch/x86/drivers/gpu_tensor.h`, `arch/x86/drivers/gpu_tensor.c`, additions to `sass.h/sass.c`.
- **Integration**: `gpu_tensor_init()` called from `gsp_boot()` after `sass_init()`. Runs store_test + vec_add self-tests.

### X40: GPU-Accelerated Llama Inference
Hybrid GPU/CPU forward pass orchestration. Mirrors `llama_forward()` from `inference.c` with GPU dispatch hooks.
- **Dispatch table**: `gpu_kernel_table_t` — boolean per-kernel availability (vec_add, matvec_q4_0, rmsnorm, softmax, silu, rope, vec_mul). Auto-detected from SASS catalog.
- **VRAM scratch**: 3 buffers (A, B, Out) allocated from gpu_tensor VRAM region, sized for max(dim, ffn_dim) floats.
- **Chunked vec_add**: GPU `vec_add_f32` handles 256 elements/dispatch. Vectors larger than 256 are chunked into multiple dispatches with PRAMIN upload/download per chunk.
- **Forward pass**: `gpu_llama_forward()` — identical transformer logic to CPU, but dispatches available ops to GPU. Currently: vec_add on GPU (when ≤ VRAM and kernel available), all matvec/rmsnorm/softmax/silu/rope on CPU.
- **Generate loop**: `gpu_llama_generate()` — prefill + decode with per-token timing, GPU/CPU dispatch stats.
- **Benchmark**: `gpu_llama_benchmark()` — GPU vs CPU vec_add timing comparison with correctness verification. Reports PRAMIN transfer overhead. `gpu_llama_benchmark_standalone()` runs without a model.
- **Stats tracking**: GPU ops count, CPU ops count, GPU/CPU cycle totals, dispatch percentage.
- **Bottleneck analysis**: matvec_q4_0 = ~90% of compute. Current GPU vec_add saves ~1%. Real speedup comes when matvec_q4_0 SASS kernel is compiled.
- **Files**: `arch/x86/drivers/gpu_inference.h`, `arch/x86/drivers/gpu_inference.c`.
- **Integration**: `gpu_llama_init()` + `gpu_llama_generate()` called from `kernel/main.c` after GPU boot. CPU inference runs first as baseline, then GPU inference for comparison.

### X41: PTX Kernel Compilation Pipeline
Complete PTX translations of ntransformer's critical CUDA kernels + build tooling.
- **Kernels translated**: `gemv_q4_0.ptx` (Q4_0 GEMV — 90% of inference compute, shared memory tiling, warp reduction via `shfl.sync.bfly`), `rmsnorm.ptx` (warp + cross-warp reduction, `rsqrt.approx`), `softmax.ptx` (3-phase: max → exp → normalize, `ex2.approx`), `elementwise.ptx` (vec_add, vec_mul, silu_mul, add_inplace), `rope.ptx` (Llama-style RoPE, `cos/sin.approx`, `lg2/ex2` for pow).
- **Build pipeline**: `compile_kernels.sh` runs `ptxas --gpu-name sm_75` → cubin ELF → `cubin2array.py` extracts .text section → C byte array header. Optional `nvdisasm` for SASS listing.
- **cubin2array.py**: Pure Python ELF64 parser — reads section headers, finds `.text.<kernel>` executable section, generates `static const uint8_t sass_code_<name>[]` with per-instruction hex + opcode comments.
- **Target**: SM75 (Turing RTX 2070/2080). Configurable via `GPU_ARCH` env var.
- **Output**: `arch/x86/kernels/generated/*.h` — checked into git so bare-metal build doesn't need CUDA tools.
- **Integration path**: Generated headers included in `sass.c`, registered in `sass_init()`, uploaded to VRAM via existing PRAMIN infrastructure.
- **Purpose**: Eliminates hand-encoding SASS. ntransformer CUDA kernels are the reference — these PTX translations preserve the exact algorithms (warp reductions, shared memory tiling, Q4_0 dequant) that deliver 48.9 tok/s on 8B models.

### X42: Compiled Kernel Dispatch (CB0 Parameter Passing)
Dispatch wrappers that correctly pass kernel parameters via Constant Buffer 0 (CB0) to compiled SASS kernels.
- **CB0 ABI**: ptxas SM75 places `.param` arguments at `c[0x0][0x160]`. Lower offsets contain driver data: `c[0x0][0x00]`=blockDim.x, `c[0x0][0x04]`=blockDim.y, `c[0x0][0x08]`=blockDim.z. Total CB0 size = 512 bytes.
- **gpu_dispatch_kernel()**: Generic dispatch helper — looks up SASS kernel by name, allocates CB0 in VRAM (within GMMU identity-mapped region), fills blockDim at offset 0x00 and user params at offset 0x160, uploads via PRAMIN, builds `compute_dispatch_t` with CB0 address, dispatches via QMD + SEND_PCAS, waits on semaphore.
- **Per-kernel wrappers**: `gpu_vec_add_ptx`, `gpu_vec_mul_ptx`, `gpu_add_inplace_ptx`, `gpu_silu_mul_ptx` (256 threads/block, multi-block), `gpu_rmsnorm_ptx` (1 block, shared mem reduction), `gpu_softmax_ptx` (1 block/row, shared mem), `gpu_rope_ptx` (thread per dim pair), `gpu_gemv_q4_0_ptx` (1 block/row, 256 threads, warp reduction).
- **VRAM layout**: CB0 allocated from tensor buffer region (260MB+) within GMMU 8MB identity map (256-264MB). GPU VA = VRAM physical address.
- **QMD integration**: `compute_dispatch_t.cbuf_addr/cbuf_size` → QMD DW20 bit 0 (CB0_VALID) + DW32-33 (CB0 addr + size).

### X-SMP: Multi-Core AP Startup
SMP support — boot all Application Processors via INIT-SIPI-SIPI IPI sequence.
- **ACPI MADT parse**: Find RSDP from EFI System Table ConfigurationTable (ACPI 2.0/1.0 GUID). Parse RSDT→MADT, enumerate Type 0 (Processor Local APIC) entries. Up to 16 CPUs.
- **Trampoline** (`ap_trampoline.S`): 228-byte binary at physical 0x8000. Three-stage mode transition: 16-bit real (lgdt trampoline GDT, enable PE) → 32-bit PM (enable PAE, load CR3, enable LME+paging) → 64-bit LM (load stack, lgdt/lidt kernel, lretq CS reload, set segments, call entry).
- **Trampoline GDT**: 4 entries — null, 32-bit code (D=1,L=0), data, 64-bit code (L=1,D=0). Separate from kernel GDT.
- **Data block at 0x8100**: GDT(+0x00), GDTR(+0x20), CR3(+0x28), kernel GDTR(+0x30), kernel IDTR(+0x3C), entry(+0x48), stack(+0x50), cpu_idx(+0x58), ready(+0x5C).
- **Critical ordering**: In 64-bit LM, must load stack + kernel GDT + kernel IDT + reload CS BEFORE loading kernel data segments (selector 0x30 doesn't exist in trampoline GDT).
- **INIT-SIPI-SIPI**: LAPIC ICR write to target APIC ID. INIT assert → 200μs → INIT deassert → 10ms → SIPI (vector=0x08) → 200μs → SIPI → 200μs → poll ready flag (500ms timeout).
- **AP entry** (`smp_ap_entry`): Enable LAPIC SVR, start APIC timer (same config as BSP), mark online, atomic increment `ap_started_count`, HLT loop.
- **Shell command**: `cpus` — shows CPU table (APIC ID, online/offline, BSP flag).
- **delay_us/delay_ms**: Uses nop loops (not `pause` — `pause` hangs when APs execute concurrently on QEMU).
- **Verified**: QEMU `-smp 4` boots all 4 CPUs, `-smp 1` gracefully skips AP startup.
- **Files**: `arch/x86/kernel/smp.h` (types + API), `arch/x86/kernel/smp.c` (MADT parse, trampoline, IPI), `arch/x86/kernel/ap_trampoline.S` (source assembly), `arch/x86/boot/efi_main.c` (efi_acpi_rsdp export), `arch/x86/kernel/idt.c` (idt_get_apic_base export), `arch/x86/kernel/shell.c` (cpus command)

### X-PIPE: Pipes, Signals, Shell Redirection
IPC pipes, signal delivery, and shell I/O redirection operators.
- **pipe() syscall** (#22): Creates read/write FD pair sharing a `pipe_buf_t` (4KB circular buffer). `MAX_PIPES=8`. Read returns EAGAIN if empty (write end open) or 0 (EOF, write end closed). Write returns EAGAIN if full, EPIPE if read end closed.
- **dup2() syscall** (#33): Duplicates FD entry. Closes target if open. Used for I/O redirection.
- **FD_TYPE_PIPE**: New FD type alongside CONSOLE and FILE. Pipe buffer tracked via `fd_entry_t.file` pointer. `oflags` distinguishes read end (O_RDONLY) from write end (O_WRONLY). Close tracks `read_open`/`write_open` per pipe; buffer freed when both ends closed.
- **Signals**: 32-signal framework. `sig_handlers[NSIG]` array with SIG_DFL/SIG_IGN/function pointer. `sig_pending` bitmask. `sigaction()` syscall (#13) to install handlers. `kill()` syscall (#62, self-signal only). Default action for SIGINT/SIGTERM/SIGPIPE = terminate (exit 128+sig). SIGKILL always terminates. `syscall_check_signals()` for delivery.
- **Shell redirection**: `parse_redirects()` extracts `>`, `>>`, `<` operators from argv before dispatch. Output captured via `sh_redir_fn` hook in `sh_puts()`/`sh_puts_color()`/`sh_putdec()`. Captured bytes written to OsitoFS file via `osfs2_create()`/`osfs2_write()`. Append mode uses `osfs2_file_size()` offset.
- **CRT wrappers**: `pipe()`, `dup2()`, `kill()` added to `arch/x86/libc/crt.c` for userspace programs.
- **Files**: `arch/x86/kernel/syscall.c` (pipe/dup2/kill/sigaction syscalls, pipe_buf_t, signal state), `arch/x86/kernel/shell.c` (redirection parsing, output capture), `arch/x86/kernel/process.c` (proc_current_pid), `arch/x86/libc/crt.c` (userspace wrappers)

### X-DYN: Dynamic Linking
Runtime loading of ET_DYN ELF shared objects. Provides dlopen/dlsym/dlclose API for loadable kernel modules.
- **dl_open(filename)**: Loads .so from OsitoFS. Validates ELF64 ET_DYN, allocates contiguous pages via `mem_alloc_aligned`, copies PT_LOAD segments, parses PT_DYNAMIC for DT_SYMTAB/DT_STRTAB/DT_HASH/DT_RELA/DT_JMPREL, applies relocations, calls DT_INIT. Returns opaque handle. Refcount on duplicate open.
- **dl_sym(handle, name)**: Symbol lookup. ELF SysV hash (DT_HASH: O(1) via bucket/chain) with linear scan fallback. Returns address of global/weak symbols defined in module. Skips SHN_UNDEF.
- **dl_close(handle)**: Decrements refcount. On last close: calls DT_FINI, `mem_free_pages`, releases slot.
- **Relocations**: R_X86_64_RELATIVE (base adjustment, most common in PIC), R_X86_64_GLOB_DAT (GOT entries), R_X86_64_JUMP_SLOT (PLT entries), R_X86_64_64 (absolute 64-bit). Both `.rela.dyn` and `.rela.plt` processed.
- **Kernel symbol export**: `ksym_table[]` maps names to kernel function addresses (serial_puts, fb_puts, fb_puts_color, fb_putdec, serial_putdec, serial_puthex, kmalloc, kfree). Modules reference these via R_X86_64_JUMP_SLOT relocations; linker resolves at load time.
- **DT_GNU_HASH fallback**: When DT_HASH absent, `gnu_hash_nsyms()` computes symbol count by scanning GNU hash buckets + chains.
- **Module table**: 8 slots. Tracks: base, load_bias, size, pages, symtab, strtab, hashtab, init/fini, refcount.
- **Shell command**: `dl load <file.so>`, `dl sym <file.so> <name>`, `dl call <file.so> <name>` (call void fn), `dl close <file.so>`, `dl list` (show modules + exported symbols).
- **Test module**: `arch/x86/test/testmod.c` — exports mod_add, mod_hello, mod_factorial, mod_square. References kernel serial_puts/fb_puts/fb_puts_color. Build: `gcc -shared -fPIC -nostdlib -Wl,--hash-style=sysv -o testmod.so testmod.c`.
- **Files**: `arch/x86/kernel/dynlink.c` (dynamic linker), `arch/x86/test/testmod.c` (test module), `arch/x86/kernel/shell.c` (dl command), `arch/x86/kernel/main.c` (dl_init call)

### X-JS: QuickJS JavaScript Engine
QuickJS 2024-01-13 (Bellard) ported to OsitoK bare-metal. Full ES2020+ with BigInt/BigFloat/BigDecimal.
- **Source files**: quickjs.c (55K lines), libbf.c (bignum), cutils.c (buffers), libregexp.c (regex), libunicode.c (Unicode). Compiled with `-DEMSCRIPTEN` (disables atomics) and `-DCONFIG_BIGNUM`.
- **Shim headers** (`libc/qjs_headers/`): 18 freestanding headers providing stdlib.h, stdio.h, math.h, string.h, etc. Map to functions in crt.c + tcclib.c + math.c. No system libc dependency.
- **x87 FPU math library** (`libc/math.c`, ~440 lines): 35+ math.h functions using x87 hardware transcendentals (fsin, fcos, fpatan, fyl2x, f2xm1, fscale). IEEE 754 classification (isnan, isfinite, isinf, signbit). Rounding (floor/ceil/trunc/round via FPU control word). Float/long double wrappers via inline casts.
- **libc extensions** (`tcclib.c`): Added abort(), fesetround/fegetround (x87 FPU control word), localtime_r/gmtime_r/mktime/strftime/clock stubs, malloc_usable_size (returns 0), strtoimax/strtoumax, __udivti3/__udivmodti4 (128-bit integer division for BigInt), improved snprintf %e/%f/%g (scientific notation, sign flags, precision).
- **REPL wrapper** (`libc/qjs_main.c`): JS_NewRuntime2 with custom malloc/free/realloc allocator (8MB limit, 256KB stack). JS_NewContextRaw with all intrinsics (BaseObjects, Date, Eval, RegExp, JSON, Proxy, MapSet, TypedArrays, Promise, BigInt, BigFloat, BigDecimal). Global `console.log` and `print`. File evaluation mode (`js script.js`) and interactive REPL with `.exit` command.
- **Binary size**: 1002 KB (923 KB .text, 25 KB .data). Static ET_EXEC at 0x401000.
- **Shell command**: `js` (REPL) or `js script.js` (execute file from OsitoFS).
- **Verified features**: Arithmetic, Math (sqrt, sin, PI), strings (toUpperCase), JSON, arrow functions, RegExp, BigInt, closures, recursive functions.
- **Build**: `make -C arch/x86/libc qjs.elf QJS_SRC=/tmp/quickjs-2024-01-13`
- **Files**: `arch/x86/libc/math.c`, `arch/x86/libc/qjs_main.c`, `arch/x86/libc/qjs_headers/` (18 shim headers), `arch/x86/libc/tcclib.c` (extended), `arch/x86/libc/Makefile` (qjs.elf target), `arch/x86/kernel/shell.c` (js command)

### X-GIT: Git-Compatible Version Control
Standard git object model running on OsitoK bare-metal. SHA-1 addressing, zlib compression, byte-compatible with standard git.
- **SHA-1 hashing** (`crypto.c`): FIPS 180-1 implementation added alongside existing SHA-256. init/update/final API, one-shot wrapper. Used for all git object addressing.
- **zlib DEFLATE** (`zlib.c`, ~500 lines): Full inflate (stored, fixed, dynamic Huffman + LZ77 back-references) for reading objects. Deflate with fixed Huffman codes + LZ77 hash chain (32-entry chain, 32KB window) for writing objects. Adler-32 checksum. zlib framing (CMF+FLG header, trailer).
- **Object store**: Standard git format — each object stored as zlib-compressed `"type size\0data"` at `.git/objects/XX/YYY...YYYY` on OsitoFS. SHA-1 computed over uncompressed header+content. Dedup: skips write if object already exists.
- **Index/staging**: Text format at `.git/index` — `"mode sha1hex name\n"` per entry. Max 256 entries. Loaded/saved on each operation.
- **Refs**: Standard git ref files — `.git/refs/heads/<branch>` contains SHA-1 hex + newline. HEAD at `.git/HEAD` with `"ref: refs/heads/master\n"` format.
- **Tree objects**: Standard git tree format — entries of `"mode name\0sha1_raw"` concatenated. Octal mode encoding.
- **Commit objects**: Standard git commit format — tree/parent/author/committer headers + blank line + message. Fixed identity `OsitoK <osito@bare-metal>`.
- **Commands**: `git init` (create .git/HEAD), `git add <file>` (blob + index update), `git commit <msg>` (tree + commit + ref update), `git log` (walk parent chain, 50 max), `git status` (branch, staged, untracked), `git diff` (line-by-line old vs new), `git branch [name]` (list/create), `git checkout <branch>` (update HEAD + rebuild index).
- **Storage**: Each git object uses 1 OsitoFS file (1MB block minimum). Practical for repos with <100 objects (~100MB). 64-byte filename limit accommodates `.git/objects/XX/38-char-hash` (54 chars).
- **Files**: `arch/x86/kernel/crypto.c` (SHA-1), `arch/x86/kernel/zlib.h/c` (DEFLATE), `arch/x86/kernel/git.h/c` (git core), `arch/x86/kernel/shell.c` (git command)

### X-SCHED: Preemptive Scheduler
Timer-based round-robin context switching via APIC timer (100Hz). First preemptive multitasking in OsitoK.
- **Context switch mechanism**: ISR stub in `isr_stubs.S` checks `sched_switch_rsp` (BSS variable) after every timer tick. If non-zero, replaces RSP with the new value before popping GPRs + IRETQ — this switches to the next process's saved interrupt frame without a dedicated context_switch function.
- **Fake interrupt frame**: `sched_spawn()` allocates 16KB kernel stack, builds a 176-byte fake interrupt frame at the top (22 × uint64_t: 15 zeroed GPRs + vector/error + RIP/CS/RFLAGS/RSP/SS). CS=0x38, SS=0x30, RFLAGS=0x202 (IF=1). Return address (`sched_thread_exit`) placed below frame.
- **Round-robin scheduling**: `sched_tick()` called from ISR on APIC timer vector 32. Decrements quantum (5 ticks = 50ms). On expiry, scans process table for next READY process, saves current RSP, loads next's RSP into `sched_switch_rsp`.
- **SMP safety**: `sched_get_lapic_id()` check at top of `sched_tick()` — only BSP (LAPIC ID 0) runs the scheduler. APs receive timer interrupts but skip scheduling. Without this, multiple CPUs corrupt shared scheduler state (caused #GP on first implementation).
- **Auto-activation**: Scheduler stays dormant until first `sched_spawn()` call. Backwards-compatible with existing single-process ELF exec.
- **Thread lifecycle**: `sched_thread_exit()` trampoline marks process ZOMBIE when entry function returns. `sched_yield()` sets quantum=0 + HLT for voluntary preemption.
- **Shell command**: `sched` spawns 2 test threads printing interleaved A/B chars. `sched stats` shows switch count and active status.
- **Verified**: QEMU `-smp 4` — threads produce ABABAB... pattern (20 each), both terminate cleanly. No #GP or crashes.
- **Files**: `arch/x86/kernel/process.c` (sched_tick, sched_spawn, sched_yield, test threads), `arch/x86/kernel/isr_stubs.S` (RSP swap + sched_switch_rsp BSS), `arch/x86/kernel/idt.c` (timer→sched_tick call), `arch/x86/kernel/shell.c` (sched command)

### X-MMAP: mmap/munmap/mprotect
Virtual memory mapping syscalls for anonymous page allocation, unmapping, and protection changes.
- **sys_mmap (9)**: MAP_ANONYMOUS + MAP_PRIVATE only. Allocates contiguous physical pages via `mem_alloc_pages()`, returns identity-mapped address (virt == phys). Zeroes memory (POSIX guarantee). No file-backed mappings.
- **sys_munmap (11)**: Frees pages via `mem_free_pages()`, removes VMA tracking entry. Supports exact and containing VMA matches.
- **sys_mprotect (10)**: Changes page table flags via `paging_set_flags()` per 4KB page. Maps PROT_READ/WRITE/EXEC to PTE_PRESENT/WRITABLE/NX.
- **VMA tracking**: Static array of 64 `vma_t` entries (base, pages, prot, in_use). Cleaned up on `syscall_reset_process()` — all mmap regions freed on process exit.
- **Paging layer**: Added `paging_unmap_page()` (clear PTE + INVLPG) and `paging_set_flags()` (modify PTE flags + INVLPG) to `paging.c`. Both walk 4-level page tables, refuse on 2MB large pages.
- **CRT wrappers**: `mmap()`, `munmap()`, `mprotect()` in `crt.c` via `__syscall5`/`__syscall6` (new 5-arg and 6-arg SYSCALL stubs in `syscall.S`).
- **Verified**: 6/6 tests pass in QEMU — basic alloc, read/write, large alloc (64KB), zeroed guarantee, munmap, mprotect.
- **Files**: `arch/x86/kernel/syscall.c` (VMA table, sys_mmap/munmap/mprotect), `arch/x86/kernel/paging.c` (unmap_page, set_flags), `arch/x86/libc/crt.c` (mmap/munmap/mprotect wrappers), `arch/x86/libc/syscall.S` (__syscall5/__syscall6), `arch/x86/test/mmap_test.c`

### X-VFS: Virtual Filesystem Layer
VFS dispatch in sys_open routes paths to virtual devices, procfs, or OsitoFS.
- **Virtual devices**: `/dev/null` (read→EOF, write→discard), `/dev/zero` (read→zeros), `/dev/urandom` (xorshift64 PRNG seeded from RDTSC), `/dev/console` (serial+FB), `/dev/tty` (alias for console), `/dev/random` (alias for urandom).
- **Procfs**: `/proc/self/status` (Name, Pid, State), `/proc/self/maps` (VMA table in Linux maps format). Content generated on open, served from static 4KB buffer.
- **FD types**: `FD_TYPE_DEV` (device ID stored in offset field), `FD_TYPE_PROC` (read from generated buffer). Both integrated into sys_read/sys_write/sys_fstat.
- **New syscalls**: getcwd(79) returns "/", readlink(89) handles /proc/self/exe, getdents64(217) lists /dev entries and OsitoFS files.
- **Path routing**: sys_open checks `/dev/` and `/proc/` prefixes before falling through to OsitoFS. Leading "/" stripped for OsitoFS lookup compatibility.
- **fstat**: DEV files return S_IFCHR with correct major:minor (1:3 null, 1:5 zero, 1:9 urandom, 5:1 console). PROC files return S_IFREG|0444.
- **osfs2_file_name()**: New accessor added to ositofs2.c for getdents64.
- **Verified**: 7/7 tests pass in QEMU — null R/W, zero, urandom, proc/self/status, console, ENOENT.
- **Files**: `arch/x86/kernel/syscall.c` (VFS dispatch, dev/proc handlers, getcwd/readlink/getdents64), `arch/x86/kernel/process.c` (proc_current_name), `arch/x86/fs/ositofs2.c` (osfs2_file_name), `arch/x86/test/vfs_test.c`

### X-MUSL: musl libc Port
Cross-compiled musl 1.2.5 as static libc for OsitoK. Programs linked with musl run natively.
- **Build**: `git clone musl → ./configure --disable-shared → make → make install`. `musl-gcc -static -no-pie` produces ET_EXEC ELF64.
- **New syscalls** (~20 added to syscall.c):
  - **TLS**: arch_prctl(158) ARCH_SET_FS via WRMSR to MSR 0xC0000100 (IA32_FS_BASE).
  - **Thread IDs**: set_tid_address(218), getpid(39), gettid(186) — all return current PID.
  - **Signals**: rt_sigprocmask(14) stub, sigaltstack(131) stub, tgkill(234) reuses kill.
  - **Time**: clock_gettime(228) from APIC ticks (100Hz→ms), nanosleep(35) via HLT loop.
  - **Memory**: getrandom(318) from xorshift64 PRNG, futex(202) minimal WAIT/WAKE stubs.
  - **Files**: pread64(17), pwrite64(18), fcntl(72) F_GETFD/SETFD/GETFL/SETFL/DUPFD, newfstatat(262), dup(32), fsync(74).
  - **Process**: exit_group(231) alias to exit, sched_yield(24) via HLT, prlimit64(302) RLIMIT_STACK/NOFILE, set_robust_list(273) stub.
  - **Stubs**: clone(56), fork(57), execve(59) were stubs, now implemented in X-FORK.
- **Verified**: 9/9 tests pass in QEMU — printf, strlen/strcmp, malloc/free, mmap, getpid, clock_gettime, /dev/zero, snprintf with floats, argc/argv.
- **Files**: `arch/x86/kernel/syscall.c` (all new syscalls), `arch/x86/test/musl_test.c`
- **musl source**: `/tmp/musl-src/`, installed to `/tmp/musl-install/`

### X-FORK: fork/wait4/getppid Syscalls
Preemptive process creation via scheduler-based fork. Child gets own kernel stack with fake ISR interrupt frame; scheduler's IRETQ delivers child to userspace with RAX=0.
- **proc_fork()**: Reads parent's registers from SYSCALL stack frame (14 pushes by syscall_entry.S). Allocates 16KB kernel stack, builds 176-byte fake ISR frame (22 × uint64_t: 15 GPRs + vector/error + RIP/CS/RFLAGS/RSP/SS). Allocates 64KB user stack, copies 32KB from parent, relocates saved frame pointers.
- **Frame pointer relocation**: After memcpy of parent's user stack, scans copied region for any uint64_t value within parent's stack range [user_rsp .. user_rsp+32KB) and adjusts by parent→child delta. Without this, `pop %rbp` in function epilogues restores parent's frame pointer, causing child to write locals into parent's stack memory.
- **proc_exit() dual path**: Processes with kernel_stack (fork/sched_spawn) → set ZOMBIE + halt for scheduler. Processes from proc_exec (shell) → longjmp to exec_jmpbuf. Memory region cleanup deferred to proc_wait4 (process still running on user stack during exit).
- **proc_wait4()**: Non-blocking first scan, then blocking `sti;hlt;cli` loop. Reaps ZOMBIE children, frees kernel stack + memory regions, returns exit status in Linux format `(code & 0xFF) << 8`. **Critical**: `"memory"` clobber on the asm — without it, GCC `-O2` caches `proctab[i].state` in registers and parent never sees child ZOMBIE after context switch.
- **Scheduler fixes**: ZOMBIE processes force-switch immediately (quantum=0 bypass). `sched_tick` preserves ZOMBIE state (only RUNNING→READY on preemption). BSP-only guard prevents SMP corruption.
- **syscall_user_rsp**: Global in syscall_entry.S BSS, saved before any pushes. Fork reads parent's complete register state from this frame.
- **Memory allocator fix**: `mem_alloc_pages` and `mem_alloc_aligned` start at page 2048 (8MB) to avoid consuming the 0x400000-0x600000 range used by ET_EXEC ELF binaries. Without this, boot-time allocations (heap, page tables, crypto) consume the ELF load area and mem_reserve_range fails.
- **ELF loader fix**: `mem_reserve_range` failure during ET_EXEC loading is non-fatal — allows fork+execve of the same binary (parent already owns those pages).
- **Parent state save/restore** (identity-mapped fork+execve of same binary):
  - `elf.c`: Before memset overwrites parent's ELF data, saves all PF_W segments (up to 4) via kmalloc. `elf_fork_restore()` restores them after child is reaped.
  - `syscall.c`: `syscall_save_brk()` saves brk pointers + FS_BASE MSR + fd_table + sig_handlers + vma_table. `syscall_restore_brk()` restores all after child exit. Prevents: parent free() crash (wrong brk), TLS corruption (wrong FS_BASE), fd leak/loss, signal handler loss.
  - `process.c`: Save called in `proc_fork()`, restore called in both `proc_wait4()` reap paths.
  - `syscall_reset_process()` guards: skips `kfree(brk_base)` and mmap region freeing when `saved_parent.valid` (child's execve must not free parent's resources).
- **proc_execve**: Redirects `/proc/self/exe` and `/proc/<pid>/exe` to current process name (busybox re-exec pattern). Forked children skip parent region freeing (identity-mapped OS, shared address space).
- **Keyboard stdin**: `console_read` reads from PS/2 keyboard ring buffer. No kernel echo (apps handle their own terminal echo).
- **Terminal ioctl**: TCGETS returns minimal termios for isatty() detection. TIOCGWINSZ returns 80×25. TCSETS/TCSETSW/TCSETSF accepted and ignored.
- **poll syscall**: Full implementation with keyboard POLLIN detection, file/pipe always-ready, timeout via `sti;hlt;cli` loop. ppoll and select stubs dispatch to poll.
- **Basename lookup**: sys_open and sys_access try basename extraction for flat filesystem (e.g., `/bin/busybox` → `busybox`). Virtual paths `/dev/` and `/proc/` recognized by sys_access.
- **sys_readlink**: Returns `"/name"` format for `/proc/self/exe` (busybox applet re-exec needs leading slash).
- **New syscalls**: poll(7), vfork(58), dup(32), ppoll(271), select(23) stubs.
- **getppid**: Per-process `ppid` field set in proc_alloc from current_proc->pid. proc_current_ppid() exported.
- **Verified**: 3/3 QEMU tests — T1 fork+wait (exit 42), T2 fork+exit (exit 7), T3 getpid. Busybox ash interactive shell works with echo, uname, cat applets.
- **Files**: `arch/x86/kernel/process.c` (proc_fork, proc_wait4, proc_execve), `arch/x86/kernel/syscall.c` (ioctl, poll, basename, +stubs), `arch/x86/kernel/elf.c` (reserve fallback), `arch/x86/kernel/memory.c` (8MB boundary), `arch/x86/test/fork_test.c`

### AArch64/SM8350 Port (arch/arm/)
Reference bare-metal code for ASUS ROG Phone 5 (Snapdragon 888).
All code verified on real hardware via sm8350-boot bootloader (2026-03-03/04).

```
arch/arm/include/sm8350.h      SM8350 MMIO register definitions (verified)
arch/arm/include/aarch64.h     System register helpers, GICv3 ICC, timer, context frame
arch/arm/boot/start.S          EL1 entry, ARM64 image header, exception vectors
arch/arm/boot/linker.ld        Linker script (WARNING: load addr != link addr)
arch/arm/drivers/geni_uart.c   Qualcomm GENI UART (TX timeout mandatory)
arch/arm/drivers/framebuffer.c Splash FB console (ARGB8888, 1080x2448, 0xE5000000)
arch/arm/drivers/timer.c       ARM Generic Timer (19.2 MHz, scheduler tick)
arch/arm/drivers/spmi.c        SPMI PMIC buttons (WIP, data abort on observer probe)
```

**Critical rules for AArch64 SM8350**:
- ABL loads at ~0xA0080000, NOT 0x80080000. Never use stored pointers (`char *p = "str"`), always use arrays (`char p[] = "str"`)
- UART TX MUST have timeout (~100k iterations) or system hangs
- FB output BEFORE UART output in dual-output wrappers
- PSCI reboot: `ldr x0, =0x84000009` then `smc #0` (not mov — immediate too large)

See `docs/porting-rog5-sm8350.md` for full hardware reference and verified findings.

## Language
The user speaks Spanish. Communicate in Spanish when appropriate.
