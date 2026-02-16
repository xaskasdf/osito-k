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
src/mem/pool_alloc.cpp      Fixed-block allocator (32B × 256 = 8KB)
src/drivers/uart.cpp        UART0: polled TX, interrupt RX, ring buffer, mutex
src/shell/shell.cpp         Interactive shell: ps, mem, ticks, help, reboot
src/main.cpp                kernel_main: init → create tasks → timer → sched_start
```

## Language
The user speaks Spanish. Communicate in Spanish when appropriate.
