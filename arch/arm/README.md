# Osito-K — AArch64 Port (Qualcomm SM8350)

Target: ASUS ROG Phone 5 (I005DA), Qualcomm Snapdragon 888 (SM8350 "Lahaina")
Status: Bootloader proven on real hardware (sm8350-boot v0.4), kernel port pending.

## What's Here

```
arch/arm/
  include/
    sm8350.h          Hardware register definitions (tested on real device)
    aarch64.h         AArch64 system register helpers (EL1, MMU, cache, barriers)
  boot/
    start.S           Entry point — save DTB, set stack, clear BSS, branch to C
    linker.ld         Linker script (NOTE: load address != link address, see below)
  kernel/
    (empty)           Port scheduler, shell, etc. from ESP8266/x86 here
  drivers/
    geni_uart.c       GENI UART TX/RX (tested, works, needs timeout on TX)
    framebuffer.c     Splash FB console (tested, ARGB8888, 1080x2448)
    timer.c           ARM Generic Timer (tested, 19.2 MHz)
    spmi.c            SPMI PMIC access (WIP — observer probe causes data abort)
```

## Critical Findings from sm8350-boot (real hardware, 2026-03-03)

### 1. ABL Load Address Mismatch
ABL loads kernel at **~0xA0080000**, NOT 0x80080000 as boot.img header suggests.
DTB observed at ~0xAF4FE000.

**This means**: any stored pointer in `.data`/`.rodata` that contains a link-time
address (0x8008xxxx) will be WRONG at runtime (actual address is 0xA008xxxx).

**Fix**: GCC aarch64 -O2 uses ADRP (PC-relative) for direct symbol references.
This is safe at any load address. BUT stored pointers break:

```c
// BROKEN — pointer stores link-time address in .data:
static const char *msg = "hello";

// SAFE — array contents accessed via PC-relative ADRP:
static const char msg[] = "hello";

// BROKEN — array of pointers stores link-time addresses:
static const char *labels[] = { "one", "two" };

// SAFE — 2D array, no stored pointers:
static const char labels[][8] = { "one", "two" };
```

Until we set up MMU with proper relocation, this rule is **mandatory**.

### 2. UART TX Hangs
ABL may not leave UART TX fully functional. uart_putc() with infinite polling
WILL hang the system. Always use a timeout loop (~100k iterations).

### 3. Framebuffer
- Address: 0xE5000000 (splash_region from DTB, 35MB)
- Format: ARGB8888 (confirmed with color bar test)
- Resolution: 1080x2448 (Samsung AMS678 ER2 OLED, DSI command mode)
- Stride: 1080 pixels (linear, no padding)
- ABL leaves splash active — no MDSS init needed, just write pixels

### 4. CPU State at Entry
- CPU: Cortex-A55 (little core), EL1
- Timer: 19.2 MHz (CNTFRQ_EL0 = 0x124F800)
- Interrupts: disabled (DAIF masked)
- x0 = DTB physical address
- Caches: ON, MMU: may be ON (ABL identity mapping)
- Stack: must set our own

### 5. SPMI / Buttons
PON (power+vol_down) at periph 0x13, Vol Up at PMIC GPIO6 (periph 0x8D).
Direct SPMI observer probe causes data abort — channel mapping unknown.
Need to extract arbiter config from running bootloader or try known values.

## Build

Same cross-compiler as sm8350-boot:
```bash
aarch64-linux-gnu-gcc -ffreestanding -nostdlib -nostartfiles \
    -march=armv8.2-a -mtune=cortex-a78 -O2 -mgeneral-regs-only \
    -I arch/arm/include
```

Boot image packaging:
```bash
mkbootimg --kernel Image --ramdisk /dev/null --dtb stock.dtb \
    --base 0x00000000 --kernel_offset 0x00080000 \
    --header_version 2 --pagesize 4096 \
    -o boot.img
# Test without flashing:
fastboot boot boot.img
```

## Porting Priority

1. **start.S + linker.ld** — EL1 entry, stack at 0x85000000, BSS clear
2. **GENI UART** — serial console (ABL pre-configured, just write FIFO)
3. **Exception vectors** — VBAR_EL1, sync/irq/fiq/serror handlers
4. **ARM Generic Timer** — scheduler tick via CNTP_TVAL_EL0
5. **GICv3** — enable timer PPI, interrupt dispatch
6. **Context switch** — AArch64 context frame (272B min, 784B with NEON)
7. **Scheduler** — port from ESP8266 (adapt context frame + timer)
8. **Shell** — port over UART
9. **Framebuffer** — splash FB console at 0xE5000000
10. **UFS** — storage access for OsitoFS
