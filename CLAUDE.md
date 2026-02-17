# OsitoK - Developer Notes

## What is this
Bare-metal preemptive kernel for ESP8266 (Wemos D1, Xtensa LX106 @ 80MHz).
No Espressif SDK — runs directly on hardware using `nosdk8266` approach.

## Build & Flash

```bash
export PATH="/c/Users/xasko/osito-k/tools/xtensa-lx106-elf/bin:$PATH"
make                    # ELF links fine, BIN step fails (python vs py)
# Manual image + flash:
py -m esptool --chip esp8266 elf2image --flash_mode dout --flash_size 4MB --flash_freq 40m --version 1 -o build/osito build/osito.elf
py -m esptool --chip esp8266 --port COM4 --baud 460800 write_flash --flash_mode dout --flash_size 4MB --flash_freq 40m 0x00000 build/osito0x00000.bin
```

**Serial monitor**: 74880 baud (ROM bootloader uses ~52MHz APB, not 80MHz).
```bash
py -c "import serial,time; s=serial.Serial('COM4',74880,timeout=0.5); s.dtr=False; s.rts=False; [print(s.read(256).decode('ascii',errors='replace'),end='') for _ in range(100)]"
```

**Restore original firmware**: `py -m esptool --port COM4 write_flash 0x0 backup/wemos_d1_full_backup.bin`

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
- Makefile BIN step uses `python` but system has `py`. Run esptool manually.
- UART baud shows 74880 because ROM bootloader sets ~52MHz APB clock before our PLL init.
- Flash mode MUST be DOUT (QIO causes boot failure on Wemos D1).
- Image format MUST be version 1 (v2 doesn't boot on ESP8266).

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
src/shell/shell.cpp         Interactive shell (ps, mem, heap, fs, gpio, forth, run, etc.)
src/main.cpp                kernel_main: init → create tasks → timer → sched_start
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
- **Syscalls**: emit(0), print(1), tell(2), fb-clear(128), fb-pixel(129), fb-line(130), fb-flush(131), fb-text(132), yield(133), ticks(134), delay(135)
- **setjmp/longjmp**: custom Xtensa CALL0 asm, saves a0/a1/a12-a15 (24 bytes)
- **Shell**: `forth` (REPL, Ctrl+C exit), `run <file.zf>` (execute from OsitoFS)

## Resource Budget (as of F10 + zForth)
```
IRAM .text:  24,602 / 32,768 bytes (~8.2 KB free)
DRAM:        sin_table 1KB + pool 8KB + heap 8KB + FS buffers + zf_ctx ~2.3KB
Flash:       OsitoFS on SPI flash (4MB total)
Tasks:       idle, input, shell (3 of 8 slots used)
```

## Roadmap — Remaining Features

Goal: port BBC Micro Elite (wireframe 3D) + spreadsheet app.

| Feature | Description | Status |
|---------|-------------|--------|
| F1-F5   | Kernel, scheduler, drivers, FS, heap, font, framebuffer | Done |
| F6      | Fixed-point 16.16 math library (sin/cos/div/sqrt) | Done |
| F7      | 3D vectors, matrices, perspective projection | Done |
| F8      | Wireframe renderer — vertex/edge → rotate → project → draw | Done |
| F9      | Ship models — Cobra, Sidewinder, Coriolis, Viper, Asp, Shuttle | Done |
| F10     | Game loop + HUD — flight, starfield, radar, joystick | Done |
| **zF**  | **zForth** — replaced BASIC+VM, saved ~4KB IRAM | Done |
| **F11** | **Spreadsheet engine** — cell grid, formula parser, cursor UI | Next |

### F11: Spreadsheet Engine
Cell grid (e.g. 8×16), each cell holds number or formula string.
Formula parser: `=A1+B2*3`, cell references, basic operators (+−×÷).
Evaluation with dependency tracking (topological sort or mark-dirty).
UI: grid rendered on framebuffer, cursor navigation with joystick, cell editing via UART.

## Language
The user speaks Spanish. Communicate in Spanish when appropriate.
