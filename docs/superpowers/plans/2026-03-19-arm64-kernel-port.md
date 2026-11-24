# AArch64 Kernel Port — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port the OsitoK kernel to AArch64 with dual-platform support (QEMU virt + ROG Phone 5 SM8350), achieving feature parity with the x86 kernel's core subsystems: serial, framebuffer, GICv3 interrupts, timer, heap, preemptive scheduler, and interactive shell.

**Architecture:** HAL (Hardware Abstraction Layer) via compile-time platform selection. Common kernel code in `arch/arm/kernel/`, platform-specific drivers in `arch/arm/platform/{virt,sm8350}/`. Makefile builds with `PLATFORM=virt` (default) or `PLATFORM=sm8350`. No function pointers — just different source files compiled per target.

**Tech Stack:** `aarch64-linux-gnu-gcc` cross-compiler, QEMU `-M virt` with edk2 UEFI for development, `fastboot boot` for ROG Phone 5 milestone testing.

---

## File Structure

```
arch/arm/
  include/
    aarch64.h           ← EXISTS — system regs, context frame, GIC ICC, irq save/restore
    hal.h               ← NEW — common function declarations (uart_putc, fb_init, etc.)
    types.h             ← NEW — uint8/16/32/64_t + memset/memcpy/strlen
  platform/
    virt/
      virt.h            ← NEW — QEMU virt MMIO addresses (PL011, GIC, RAM)
      uart.c            ← NEW — PL011 UART driver
      platform.c        ← NEW — platform_init(), mem_alloc_pages() stub
      framebuffer.c     ← NEW — ramfb or simple serial-only (Phase 1)
    sm8350/
      sm8350.h          ← MOVE from include/sm8350.h
      uart.c            ← MOVE+RENAME from drivers/geni_uart.c
      platform.c        ← NEW — platform_init(), memory detection via DTB
      framebuffer.c     ← MOVE from drivers/framebuffer.c
      spmi.c            ← MOVE from drivers/spmi.c
  boot/
    start.S             ← EXISTS — position-independent entry, works for both
    linker_virt.ld      ← NEW — QEMU virt (base 0x40080000, known load addr)
    linker_sm8350.ld    ← MOVE+RENAME from boot/linker.ld
  kernel/
    main.c              ← NEW — kernel_main() init sequence (mirrors x86)
    serial.c            ← NEW — serial_puts/puthex/putdec wrappers over hal
    framebuffer.c       ← NEW — common fb text console with font8x16 (from x86)
    gic.c               ← NEW — GICv3 distributor + redistributor init
    timer.c             ← MOVE+ENHANCE from drivers/timer.c (add IRQ-based tick)
    heap.c              ← NEW — port from x86 (identical logic, 64-bit pointers)
    context_switch.S    ← NEW — full AArch64 context save/restore + task entry
    sched.c             ← NEW — preemptive scheduler (port from ESP8266 design)
    task.h              ← NEW — TCB struct, states, MAX_TASKS
    shell.c             ← NEW — interactive shell over UART
    term.c              ← NEW — line editor with history (readline-style)
  scripts/
    qemu-test.sh        ← NEW — QEMU -M virt launch script
  Makefile              ← NEW — dual-platform build system
```

---

## Task 1: Build System + Minimal Boot (UART Hello World)

**Files:**
- Create: `arch/arm/Makefile`
- Create: `arch/arm/include/types.h`
- Create: `arch/arm/include/hal.h`
- Create: `arch/arm/platform/virt/virt.h`
- Create: `arch/arm/platform/virt/uart.c`
- Create: `arch/arm/platform/virt/platform.c`
- Move: `arch/arm/boot/linker.ld` → `arch/arm/boot/linker_sm8350.ld`
- Create: `arch/arm/boot/linker_virt.ld`
- Create: `arch/arm/kernel/main.c`
- Create: `arch/arm/kernel/serial.c`
- Create: `arch/arm/scripts/qemu-test.sh`

**Goal:** Boot on QEMU virt, print "OsitoK ARM64" over PL011 UART to serial log.

- [ ] **Step 1: Create `arch/arm/include/types.h`**

Minimal type definitions + inline helpers (same pattern as x86):

```c
#ifndef OSITO_ARM_TYPES_H
#define OSITO_ARM_TYPES_H

#include <stdint.h>
#include <stddef.h>

/* memset / memcpy / strlen — freestanding, no libc */
static inline void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    while (n--) *p++ = (uint8_t)c;
    return s;
}

static inline void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

static inline size_t strlen(const char *s) {
    size_t n = 0;
    while (*s++) n++;
    return n;
}

static inline int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *(unsigned char *)a - *(unsigned char *)b;
}

static inline int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? *(unsigned char *)a - *(unsigned char *)b : 0;
}

#endif
```

- [ ] **Step 2: Create `arch/arm/include/hal.h`**

Common function declarations that both platforms implement:

```c
#ifndef OSITO_ARM_HAL_H
#define OSITO_ARM_HAL_H

#include "types.h"

/* ── Platform init ─────────────────────────────────────────── */
void platform_init(void *dtb);
void platform_reboot(void);

/* ── UART (platform-specific implementation) ───────────────── */
void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
int  uart_rx_ready(void);
char uart_getc(void);
int  uart_trygetc(void);

/* ── Serial output (common wrappers in kernel/serial.c) ───── */
void serial_init(void);
void serial_putc(char c);
void serial_putchar(char c);
void serial_puts(const char *s);
void serial_puthex(uint64_t val, int digits);
void serial_putdec(uint64_t val);
int  serial_getc(void);

/* ── Framebuffer (common in kernel/framebuffer.c) ─────────── */
void fb_init_platform(uint32_t *base, uint32_t width, uint32_t height, uint32_t pitch);
void fb_clear(void);
void fb_putc(char c);
void fb_puts(const char *s);
void fb_puts_color(const char *s, uint32_t color);
void fb_putdec(uint64_t val);

/* ── Memory (platform-specific) ────────────────────────────── */
void *mem_alloc_pages(uint64_t count);
void  mem_free_pages(void *addr, uint64_t count);

/* ── Heap (common in kernel/heap.c) ────────────────────────── */
void  heap_init(void);
void *kmalloc(uint64_t size);
void  kfree(void *ptr);
void *kcalloc(uint64_t count, uint64_t size);
void *krealloc(void *ptr, uint64_t new_size);
uint64_t heap_get_used(void);
uint64_t heap_get_total(void);

/* ── GIC (common in kernel/gic.c) ──────────────────────────── */
void gic_init(void);
uint32_t gic_ack_irq(void);
void gic_end_irq(uint32_t irqnr);

/* ── Timer (common in kernel/timer.c) ──────────────────────── */
void     timer_init(uint32_t hz);
uint64_t timer_get_ticks(void);
uint64_t timer_ms(void);
void     udelay(uint32_t us);
void     mdelay(uint32_t ms);

/* ── Scheduler (common in kernel/sched.c) ──────────────────── */
void sched_init(void);
int  task_create(void (*func)(void *), void *arg, uint8_t priority, const char *name);
void task_yield(void);
void task_delay_ms(uint32_t ms);
void sched_start(void);

/* ── Shell (common in kernel/shell.c) ──────────────────────── */
void shell_run(void);

/* ── Platform-provided GIC base addresses ──────────────────── */
uintptr_t platform_gicd_base(void);
uintptr_t platform_gicr_base(void);

#endif
```

- [ ] **Step 3: Create `arch/arm/platform/virt/virt.h`**

QEMU virt machine MMIO addresses:

```c
#ifndef OSITO_VIRT_H
#define OSITO_VIRT_H

#include <stdint.h>

/* ── MMIO helpers ──────────────────────────────────────────── */
static inline uint32_t mmio_read32(uintptr_t addr) {
    return *(volatile uint32_t *)addr;
}
static inline void mmio_write32(uintptr_t addr, uint32_t val) {
    *(volatile uint32_t *)addr = val;
}

#define dsb()   __asm__ volatile("dsb sy" ::: "memory")
#define dmb()   __asm__ volatile("dmb sy" ::: "memory")
#define isb()   __asm__ volatile("isb"    ::: "memory")

/* ── PL011 UART (QEMU virt) ────────────────────────────────── */
#define PL011_BASE          0x09000000UL

#define PL011_DR            0x000   /* Data Register */
#define PL011_FR            0x018   /* Flag Register */
#define PL011_IBRD          0x024   /* Integer Baud Rate */
#define PL011_FBRD          0x028   /* Fractional Baud Rate */
#define PL011_LCR_H         0x02C   /* Line Control */
#define PL011_CR            0x030   /* Control Register */
#define PL011_IMSC          0x038   /* Interrupt Mask */
#define PL011_ICR           0x044   /* Interrupt Clear */

#define PL011_FR_TXFF       (1 << 5)    /* TX FIFO full */
#define PL011_FR_RXFE       (1 << 4)    /* RX FIFO empty */

/* ── GICv3 (QEMU virt) ────────────────────────────────────── */
#define GICD_BASE           0x08000000UL
#define GICR_BASE           0x080A0000UL
#define GICR_STRIDE         0x00020000UL

/* ── RAM layout (QEMU virt, -m 512M) ──────────────────────── */
#define RAM_BASE            0x40000000UL
#define RAM_SIZE            0x20000000UL    /* 512 MB */

/* ── Kernel load address (QEMU virt UEFI or direct) ───────── */
#define KERNEL_LOAD_ADDR    0x40080000UL

/* ── Timer (same as SM8350 — ARM generic timer) ────────────── */
/* QEMU virt uses 62.5 MHz by default (CNTFRQ) but we read it dynamically */

/* ── RTC (PL031) ───────────────────────────────────────────── */
#define PL031_BASE          0x09010000UL

#endif
```

- [ ] **Step 4: Create `arch/arm/platform/virt/uart.c`**

PL011 UART driver:

```c
/*
 * PL011 UART driver for QEMU virt machine
 *
 * QEMU pre-configures PL011 at 0x09000000. We just need to
 * read/write the data register. No baud rate setup needed
 * (QEMU ignores it for virtual UARTs).
 */

#include "virt.h"

#define UART_BASE   PL011_BASE
#define UART_REG(off)   (UART_BASE + (off))

void uart_init(void) {
    /* QEMU PL011 is already configured, but ensure TX/RX enabled */
    mmio_write32(UART_REG(PL011_CR), (1 << 0) | (1 << 8) | (1 << 9)); /* UARTEN | TXE | RXE */
}

void uart_putc(char c) {
    /* Wait until TX FIFO not full */
    while (mmio_read32(UART_REG(PL011_FR)) & PL011_FR_TXFF)
        ;
    mmio_write32(UART_REG(PL011_DR), (uint32_t)c);
}

void uart_puts(const char *s) {
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s++);
    }
}

int uart_rx_ready(void) {
    return !(mmio_read32(UART_REG(PL011_FR)) & PL011_FR_RXFE);
}

char uart_getc(void) {
    while (!uart_rx_ready())
        ;
    return (char)(mmio_read32(UART_REG(PL011_DR)) & 0xFF);
}

int uart_trygetc(void) {
    if (!uart_rx_ready())
        return -1;
    return (int)(mmio_read32(UART_REG(PL011_DR)) & 0xFF);
}
```

- [ ] **Step 5: Create `arch/arm/platform/virt/platform.c`**

Minimal platform init + static page allocator:

```c
/*
 * QEMU virt platform initialization
 *
 * Simple bump allocator for pages. Full bitmap allocator comes later
 * when we port the x86 memory manager.
 */

#include "../../include/hal.h"
#include "virt.h"

/* Simple bump allocator: start after kernel (at 64MB mark) */
static uint8_t *page_bump = (uint8_t *)0x44000000UL;  /* 64MB into RAM */
static uint8_t *page_end  = (uint8_t *)(RAM_BASE + RAM_SIZE);
static uint64_t pages_allocated;

void platform_init(void *dtb) {
    (void)dtb;
    uart_init();
    pages_allocated = 0;
}

void platform_reboot(void) {
    /* PSCI SYSTEM_RESET via SMC */
    register uint64_t x0 __asm__("x0") = 0x84000009ULL;
    __asm__ volatile("smc #0" :: "r"(x0));
    for (;;) __asm__ volatile("wfe");
}

void *mem_alloc_pages(uint64_t count) {
    uint64_t size = count * 4096;
    uint8_t *result = page_bump;

    if (result + size > page_end)
        return (void *)0;

    page_bump += size;
    pages_allocated += count;

    /* Zero the pages */
    for (uint64_t i = 0; i < size; i++)
        result[i] = 0;

    return result;
}

void mem_free_pages(void *addr, uint64_t count) {
    /* Bump allocator doesn't free — placeholder */
    (void)addr;
    (void)count;
}

uintptr_t platform_gicd_base(void) { return GICD_BASE; }
uintptr_t platform_gicr_base(void) { return GICR_BASE; }
```

- [ ] **Step 6: Create `arch/arm/kernel/serial.c`**

Common serial output wrappers (identical to x86 pattern):

```c
/*
 * OsitoK AArch64 — Serial Output Wrappers
 *
 * Wraps platform-specific uart_putc/uart_getc with formatting helpers.
 * Same interface as arch/x86/kernel/serial.c.
 */

#include "../include/hal.h"

void serial_init(void) {
    uart_init();
}

void serial_putc(char c) {
    uart_putc(c);
}

void serial_putchar(char c) {
    if (c == '\n') uart_putc('\r');
    uart_putc(c);
}

void serial_puts(const char *s) {
    while (*s) {
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s++);
    }
}

void serial_puthex(uint64_t val, int digits) {
    static const char hex[] = "0123456789ABCDEF";
    serial_puts("0x");
    for (int i = (digits - 1) * 4; i >= 0; i -= 4)
        serial_putc(hex[(val >> i) & 0xF]);
}

void serial_putdec(uint64_t val) {
    char buf[20];
    int i = 0;
    if (val == 0) { serial_putc('0'); return; }
    while (val > 0) {
        buf[i++] = '0' + (val % 10);
        val /= 10;
    }
    while (--i >= 0) serial_putc(buf[i]);
}

int serial_getc(void) {
    return uart_trygetc();
}
```

- [ ] **Step 7: Create `arch/arm/kernel/main.c`**

Minimal kernel_main — just serial banner for now:

```c
/*
 * OsitoK AArch64 — Kernel Main
 *
 * Entry point from start.S. Mirrors the x86 kernel_entry() init sequence.
 * Platform-specific code is in platform/{virt,sm8350}/
 */

#include "../include/hal.h"
#include "../include/aarch64.h"

static void print_banner(void) {
    serial_puts("\n");
    serial_puts("  =============================================\n");
    serial_puts("  ██████╗ ███████╗██╗████████╗ ██████╗ ██╗  ██╗\n");
    serial_puts("  ██╔═══╝ ██╔════╝██║╚══██╔══╝██╔═══██╗██║ ██╔╝\n");
    serial_puts("  ██║     ███████╗██║   ██║   ██║   ██║█████╔╝ \n");
    serial_puts("  ██║     ╚════██║██║   ██║   ██║   ██║██╔═██╗ \n");
    serial_puts("  ██████╗ ███████║██║   ██║   ╚██████╔╝██║  ██╗\n");
    serial_puts("  ╚═════╝ ╚══════╝╚═╝   ╚═╝    ╚═════╝ ╚═╝  ╚═╝\n");
    serial_puts("  OsitoK AArch64 — Bare Metal Kernel\n");
    serial_puts("  =============================================\n\n");
}

static void print_cpu_info(void) {
    uint64_t midr = read_midr_el1();
    uint64_t mpidr = read_mpidr_el1();
    uint32_t el = read_currentel();

    serial_puts("[CPU] MIDR_EL1: ");
    serial_puthex(midr, 8);
    serial_puts("  MPIDR_EL1: ");
    serial_puthex(mpidr, 16);
    serial_puts("  EL: ");
    serial_putdec(el);
    serial_puts("\n");

    uint32_t part = (midr >> 4) & 0xFFF;
    serial_puts("[CPU] Core: ");
    if (part == 0xD05) serial_puts("Cortex-A55");
    else if (part == 0xD41) serial_puts("Cortex-A78");
    else if (part == 0xD44) serial_puts("Cortex-X1");
    else { serial_puts("Unknown ("); serial_puthex(part, 3); serial_puts(")"); }
    serial_puts("\n");
}

/* Default exception handlers (weak — overridden when GIC is set up) */
void __attribute__((weak)) exception_handler(uint64_t esr, uint64_t elr, uint64_t far) {
    serial_puts("\n!!! EXCEPTION !!!\n");
    serial_puts("  ESR_EL1: "); serial_puthex(esr, 8); serial_puts("\n");
    serial_puts("  ELR_EL1: "); serial_puthex(elr, 16); serial_puts("\n");
    serial_puts("  FAR_EL1: "); serial_puthex(far, 16); serial_puts("\n");
    for (;;) __asm__ volatile("wfe");
}

void __attribute__((weak)) irq_handler(void) {
    serial_puts("[IRQ] Unhandled IRQ\n");
}

void __attribute__((weak)) serror_handler(uint64_t esr) {
    serial_puts("\n!!! SError !!!\n");
    serial_puts("  ESR_EL1: "); serial_puthex(esr, 8); serial_puts("\n");
    for (;;) __asm__ volatile("wfe");
}

void kernel_main(void *dtb) {
    /* Step 0: Platform + serial init */
    platform_init(dtb);
    serial_puts("[KERN] Serial OK\n");

    /* Banner */
    print_banner();
    print_cpu_info();

    /* Step 1: Timer info */
    uint32_t freq = read_cntfrq_el0();
    serial_puts("[TMR] Timer freq: ");
    serial_putdec(freq);
    serial_puts(" Hz\n");

    serial_puts("[KERN] Boot complete. Halting.\n");

    for (;;) __asm__ volatile("wfe");
}
```

- [ ] **Step 8: Move `arch/arm/boot/linker.ld` → `arch/arm/boot/linker_sm8350.ld`**

```bash
mv arch/arm/boot/linker.ld arch/arm/boot/linker_sm8350.ld
```

- [ ] **Step 9: Create `arch/arm/boot/linker_virt.ld`**

```ld
/*
 * Linker script for QEMU virt (-M virt) — kernel loaded as raw binary
 *
 * QEMU virt loads kernel at 0x40080000 by default.
 * Unlike SM8350, load address matches link address here.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x40080000;

    .text.boot : {
        *(.text.boot)
    }

    . = ALIGN(4096);
    __text_start = .;
    .text : {
        *(.text .text.*)
    }
    __text_end = .;

    . = ALIGN(4096);
    __rodata_start = .;
    .rodata : {
        *(.rodata .rodata.*)
    }
    __rodata_end = .;

    . = ALIGN(4096);
    .data : {
        *(.data .data.*)
    }

    . = ALIGN(16);
    __bss_start = .;
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }
    . = ALIGN(16);
    __bss_end = .;

    _image_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
```

- [ ] **Step 10: Create `arch/arm/Makefile`**

```makefile
# OsitoK AArch64 — Build System
#
# Usage:
#   make                    # Build for QEMU virt (default)
#   make PLATFORM=sm8350    # Build for ROG Phone 5
#   make run                # Build + boot QEMU

PLATFORM ?= virt

# Cross-compiler
CROSS   := aarch64-linux-gnu-
CC      := $(CROSS)gcc
AS      := $(CROSS)gcc
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy

# Directories
BUILD   := build
PLAT_DIR := platform/$(PLATFORM)

# Compiler flags
CFLAGS  := -ffreestanding -nostdlib -nostartfiles \
           -march=armv8.2-a -mtune=cortex-a78 -O2 \
           -mgeneral-regs-only -Wall -Wextra \
           -Iinclude -I$(PLAT_DIR)
ASFLAGS := $(CFLAGS)
LDFLAGS := -nostdlib

# Linker script
ifeq ($(PLATFORM),virt)
  LDSCRIPT := boot/linker_virt.ld
else ifeq ($(PLATFORM),sm8350)
  LDSCRIPT := boot/linker_sm8350.ld
endif

# Sources — boot
BOOT_ASM := boot/start.S

# Sources — kernel (common)
KERN_SRC := kernel/main.c \
            kernel/serial.c

# Sources — platform-specific
PLAT_SRC := $(PLAT_DIR)/uart.c \
            $(PLAT_DIR)/platform.c

# All sources
ALL_SRC := $(BOOT_ASM) $(KERN_SRC) $(PLAT_SRC)
ALL_OBJ := $(patsubst %.S,$(BUILD)/%.o,$(patsubst %.c,$(BUILD)/%.o,$(ALL_SRC)))

# Targets
KERNEL_ELF := $(BUILD)/kernel.elf
KERNEL_BIN := $(BUILD)/Image

.PHONY: all clean run

all: $(KERNEL_BIN)

$(KERNEL_ELF): $(ALL_OBJ) $(LDSCRIPT)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -T $(LDSCRIPT) -o $@ $(ALL_OBJ)
	@echo "  LD    $@ ($(shell stat -c%s $@ 2>/dev/null || echo '?') bytes)"

$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@
	@echo "  BIN   $@ ($(shell stat -c%s $@ 2>/dev/null || echo '?') bytes)"

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -c $< -o $@

run: all
	./scripts/qemu-test.sh

clean:
	rm -rf $(BUILD)
```

- [ ] **Step 11: Create `arch/arm/scripts/qemu-test.sh`**

```bash
#!/bin/bash
#
# OsitoK AArch64 — QEMU Test Script
#
# Boots kernel on QEMU virt machine with GICv3 + PL011 serial.
#
# Usage:
#   ./qemu-test.sh              # Build + boot
#   ./qemu-test.sh --no-build   # Boot only
#
# Exit QEMU: Ctrl-A X

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ARM_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$ARM_DIR/build"
KERNEL_BIN="$BUILD_DIR/Image"
SERIAL_LOG="$BUILD_DIR/serial.log"

RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
NC='\033[0m'

info()  { echo -e "${GREEN}[+]${NC} $*"; }
error() { echo -e "${RED}[!]${NC} $*"; exit 1; }

# ── Check prerequisites ──────────────────────────────────
command -v qemu-system-aarch64 >/dev/null || error "qemu-system-aarch64 not installed. Run: sudo apt install qemu-system-arm"

# ── Build if needed ──────────────────────────────────────
if [ "$1" != "--no-build" ]; then
    info "Building AArch64 kernel..."
    make -C "$ARM_DIR" PLATFORM=virt -j$(nproc) 2>&1 | tail -5
fi

[ -f "$KERNEL_BIN" ] || error "Image not found at $KERNEL_BIN"

# ── Launch QEMU ──────────────────────────────────────────
rm -f "$SERIAL_LOG"

info "Launching QEMU (aarch64 virt)..."
info "  CPU:    Cortex-A72 (single core)"
info "  RAM:    512 MB"
info "  GIC:    v3"
info "  Serial: PL011 → $SERIAL_LOG"
info ""
echo -e "${CYAN}  Watch: tail -f $SERIAL_LOG${NC}"
echo -e "${CYAN}  Exit:  Ctrl-A X${NC}"
info ""

qemu-system-aarch64 \
    -M virt,gic-version=3 \
    -cpu cortex-a72 \
    -m 512M \
    -nographic \
    -kernel "$KERNEL_BIN" \
    -serial file:"$SERIAL_LOG" \
    -serial mon:stdio \
    -no-reboot
```

- [ ] **Step 12: Verify cross-compiler is installed**

```bash
aarch64-linux-gnu-gcc --version || sudo apt install gcc-aarch64-linux-gnu
```

- [ ] **Step 13: Build and boot**

```bash
cd arch/arm && make PLATFORM=virt && ./scripts/qemu-test.sh --no-build
```

Expected serial.log:
```
[KERN] Serial OK
  ...banner...
[CPU] MIDR_EL1: 0x...  MPIDR_EL1: 0x...  EL: 1
[CPU] Core: ...
[TMR] Timer freq: ... Hz
[KERN] Boot complete. Halting.
```

- [ ] **Step 14: Commit**

```bash
git add arch/arm/
git commit -m "arm64: add Makefile, PL011 UART, kernel_main for QEMU virt boot"
```

---

## Task 2: SM8350 Platform Backend

**Files:**
- Move: `arch/arm/include/sm8350.h` → `arch/arm/platform/sm8350/sm8350.h`
- Move: `arch/arm/drivers/geni_uart.c` → `arch/arm/platform/sm8350/uart.c`
- Move: `arch/arm/drivers/framebuffer.c` → `arch/arm/platform/sm8350/framebuffer.c`
- Move: `arch/arm/drivers/spmi.c` → `arch/arm/platform/sm8350/spmi.c`
- Create: `arch/arm/platform/sm8350/platform.c`

**Goal:** Restructure SM8350 files into platform/ layout so they compile with `PLATFORM=sm8350`.

- [ ] **Step 1: Create platform/sm8350/ directory and move files**

```bash
mkdir -p arch/arm/platform/sm8350
mv arch/arm/include/sm8350.h arch/arm/platform/sm8350/
mv arch/arm/drivers/geni_uart.c arch/arm/platform/sm8350/uart.c
mv arch/arm/drivers/framebuffer.c arch/arm/platform/sm8350/framebuffer.c
mv arch/arm/drivers/spmi.c arch/arm/platform/sm8350/spmi.c
```

- [ ] **Step 2: Fix include paths in moved files**

All moved files use `#include "sm8350.h"` — this still works since they're in the same directory now. No changes needed to uart.c, framebuffer.c, spmi.c.

- [ ] **Step 3: Create `arch/arm/platform/sm8350/platform.c`**

```c
/*
 * SM8350 (ROG Phone 5) platform initialization
 *
 * ABL pre-configures UART, display, clocks. We just need to start using them.
 * Memory: bump allocator starting at 0xA8000000 (well above kernel load).
 */

#include "../../include/hal.h"
#include "sm8350.h"

static uint8_t *page_bump = (uint8_t *)0xA8000000UL;
static uint8_t *page_end  = (uint8_t *)0xC0000000UL;  /* Conservative 512MB region */

void platform_init(void *dtb) {
    (void)dtb;
    /* GENI UART already configured by ABL — no uart_init() needed,
     * but call it anyway for consistency */
    uart_init();
}

void platform_reboot(void) {
    /* PSCI SYSTEM_RESET via SMC (must use ldr, immediate too large for mov) */
    register uint64_t x0 __asm__("x0") = 0x84000009ULL;
    __asm__ volatile("smc #0" :: "r"(x0));
    for (;;) __asm__ volatile("wfe");
}

void *mem_alloc_pages(uint64_t count) {
    uint64_t size = count * 4096;
    uint8_t *result = page_bump;
    if (result + size > page_end)
        return (void *)0;
    page_bump += size;
    for (uint64_t i = 0; i < size; i++)
        result[i] = 0;
    return result;
}

void mem_free_pages(void *addr, uint64_t count) {
    (void)addr;
    (void)count;
}

uintptr_t platform_gicd_base(void) { return 0x17A00000UL; }
uintptr_t platform_gicr_base(void) { return 0x17A60000UL; }
```

- [ ] **Step 4: Add `uart_init()` to SM8350 uart.c**

The existing geni_uart.c doesn't have `uart_init()` — ABL pre-configures it. Add a no-op:

Add at the top of `platform/sm8350/uart.c`, after the includes:
```c
void uart_init(void) {
    /* ABL pre-configures GENI UART — nothing to do */
}
```

- [ ] **Step 5: Update Makefile SM8350 source list**

The Makefile already handles `PLAT_SRC := $(PLAT_DIR)/uart.c $(PLAT_DIR)/platform.c`. For SM8350, we also need framebuffer.c. Update the `PLAT_SRC` in Makefile:

```makefile
# Sources — platform-specific
PLAT_SRC := $(PLAT_DIR)/uart.c \
            $(PLAT_DIR)/platform.c
ifeq ($(PLATFORM),sm8350)
  PLAT_SRC += $(PLAT_DIR)/framebuffer.c
endif
```

- [ ] **Step 6: Verify SM8350 build compiles**

```bash
cd arch/arm && make PLATFORM=sm8350
```

Expected: Compiles without errors, produces `build/Image`.

- [ ] **Step 7: Commit**

```bash
git add arch/arm/
git commit -m "arm64: restructure SM8350 files into platform/ layout"
```

---

## Task 3: GICv3 Interrupt Controller

**Files:**
- Create: `arch/arm/kernel/gic.c`

**Goal:** Initialize GICv3 distributor + redistributor, enable timer PPI 30, provide IRQ ack/EOI.

- [ ] **Step 1: Create `arch/arm/kernel/gic.c`**

```c
/*
 * OsitoK AArch64 — GICv3 Interrupt Controller Driver
 *
 * Initializes the GIC distributor (GICD) and redistributor (GICR),
 * enables the ARM generic timer PPI (INTID 30), and provides
 * IRQ acknowledge/end-of-interrupt functions.
 *
 * Uses ICC system registers (not memory-mapped CPU interface).
 * Base addresses come from platform_gicd_base() / platform_gicr_base().
 */

#include "../include/hal.h"
#include "../include/aarch64.h"

/* MMIO helper — platform provides the base addresses */
static inline uint32_t gicd_read(uint32_t off) {
    return *(volatile uint32_t *)(platform_gicd_base() + off);
}
static inline void gicd_write(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(platform_gicd_base() + off) = val;
}
static inline uint32_t gicr_read(uint32_t off) {
    return *(volatile uint32_t *)(platform_gicr_base() + off);
}
static inline void gicr_write(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(platform_gicr_base() + off) = val;
}

/* GICD register offsets */
#define GICD_CTLR           0x000
#define GICD_TYPER          0x004
#define GICD_ISENABLER(n)   (0x100 + (n)*4)
#define GICD_ICENABLER(n)   (0x180 + (n)*4)
#define GICD_IPRIORITYR(n)  (0x400 + (n)*4)
#define GICD_IGROUPR(n)     (0x080 + (n)*4)
#define GICD_IGRPMODR(n)    (0xD00 + (n)*4)

/* GICR register offsets (per-CPU redistributor) */
#define GICR_WAKER          0x014
#define GICR_SGI_BASE       0x10000
#define GICR_ISENABLER0     (GICR_SGI_BASE + 0x100)
#define GICR_ICENABLER0     (GICR_SGI_BASE + 0x180)
#define GICR_IPRIORITYR(n)  (GICR_SGI_BASE + 0x400 + (n)*4)
#define GICR_IGROUPR0       (GICR_SGI_BASE + 0x080)
#define GICR_IGRPMODR0      (GICR_SGI_BASE + 0xD00)
#define GICR_ISPENDR0       (GICR_SGI_BASE + 0x200)

/* Timer PPI: non-secure physical timer = INTID 30 */
#define TIMER_PPI           30

/* Software-generated interrupt for yield = INTID 0 (SGI 0) */
#define YIELD_SGI           0

void gic_init(void) {
    serial_puts("[GIC] Initializing GICv3...\n");

    /* ── Distributor init ─────────────────────────────────── */

    /* Disable distributor */
    gicd_write(GICD_CTLR, 0);
    __asm__ volatile("isb");

    /* Read max IRQ count */
    uint32_t typer = gicd_read(GICD_TYPER);
    uint32_t max_irq = ((typer & 0x1F) + 1) * 32;
    serial_puts("[GIC] Max IRQs: ");
    serial_putdec(max_irq);
    serial_puts("\n");

    /* Disable all SPIs, set priority to 0xA0, group 1 NS */
    for (uint32_t i = 1; i < max_irq / 32; i++) {
        gicd_write(GICD_ICENABLER(i), 0xFFFFFFFF);
        gicd_write(GICD_IGROUPR(i), 0xFFFFFFFF);     /* Group 1 */
        gicd_write(GICD_IGRPMODR(i), 0x00000000);    /* NS */
    }
    for (uint32_t i = 8; i < max_irq / 4; i++)
        gicd_write(GICD_IPRIORITYR(i), 0xA0A0A0A0);

    /* Enable distributor: ARE_NS + EnableGrp1NS */
    gicd_write(GICD_CTLR, (1 << 4) | (1 << 1));  /* ARE_NS | EnableGrp1NS */
    __asm__ volatile("isb");

    /* ── Redistributor init (CPU 0) ──────────────────────── */

    /* Wake redistributor */
    uint32_t waker = gicr_read(GICR_WAKER);
    waker &= ~(1 << 1);  /* Clear ProcessorSleep */
    gicr_write(GICR_WAKER, waker);
    /* Wait for ChildrenAsleep to clear */
    while (gicr_read(GICR_WAKER) & (1 << 2))
        ;

    /* Configure SGIs + PPIs: group 1 NS, priority 0xA0 */
    gicr_write(GICR_IGROUPR0, 0xFFFFFFFF);
    gicr_write(GICR_IGRPMODR0, 0x00000000);
    for (uint32_t i = 0; i < 8; i++)
        gicr_write(GICR_IPRIORITYR(i), 0xA0A0A0A0);

    /* Enable timer PPI (INTID 30) */
    gicr_write(GICR_ISENABLER0, (1 << TIMER_PPI));

    /* Enable SGI 0 for software yield */
    gicr_write(GICR_ISENABLER0, gicr_read(GICR_ISENABLER0) | (1 << YIELD_SGI));

    __asm__ volatile("isb");

    /* ── CPU interface (ICC system registers) ────────────── */

    /* Enable system register interface */
    write_icc_sre_el1(read_icc_sre_el1() | 1);
    __asm__ volatile("isb");

    /* Set priority mask (allow all priorities) */
    write_icc_pmr_el1(0xFF);

    /* Enable Group 1 interrupts */
    write_icc_igrpen1_el1(1);
    __asm__ volatile("isb");

    serial_puts("[GIC] GICv3 initialized (timer PPI ");
    serial_putdec(TIMER_PPI);
    serial_puts(" enabled)\n");
}

uint32_t gic_ack_irq(void) {
    return (uint32_t)read_icc_iar1_el1();
}

void gic_end_irq(uint32_t irqnr) {
    write_icc_eoir1_el1(irqnr);
}

/* Send SGI 0 to self (for task_yield) */
void gic_send_sgi_self(uint32_t sgi_id) {
    /* ICC_SGI1R_EL1: Aff=0, IRM=0, TargetList=1 (self) */
    uint64_t val = (uint64_t)(sgi_id & 0xF) << 24 | 1;
    __asm__ volatile("msr S3_0_C12_C11_5, %0" :: "r"(val));
    __asm__ volatile("isb");
}
```

- [ ] **Step 2: Add gic.c to Makefile KERN_SRC**

In `arch/arm/Makefile`, update:
```makefile
KERN_SRC := kernel/main.c \
            kernel/serial.c \
            kernel/gic.c
```

- [ ] **Step 3: Add GIC init to kernel_main**

In `kernel/main.c`, after the timer freq print, add:
```c
    /* Step 2: GIC */
    gic_init();
```

- [ ] **Step 4: Build and verify GIC init in serial log**

```bash
make PLATFORM=virt clean && make PLATFORM=virt && ./scripts/qemu-test.sh --no-build
```

Expected:
```
[GIC] Initializing GICv3...
[GIC] Max IRQs: ...
[GIC] GICv3 initialized (timer PPI 30 enabled)
```

- [ ] **Step 5: Commit**

```bash
git add arch/arm/kernel/gic.c arch/arm/Makefile arch/arm/kernel/main.c
git commit -m "arm64: add GICv3 driver with timer PPI and yield SGI support"
```

---

## Task 4: Timer IRQ + Tick Counter

**Files:**
- Modify: `arch/arm/kernel/timer.c` (enhance existing drivers/timer.c → move to kernel/)
- Modify: `arch/arm/kernel/main.c`
- Modify: `arch/arm/boot/start.S` (update IRQ handler to call C)

**Goal:** ARM generic timer fires at 100Hz, IRQ handler increments tick counter, serial prints ticks.

- [ ] **Step 1: Move and enhance timer to `arch/arm/kernel/timer.c`**

Replace the existing drivers/timer.c with a kernel-integrated version:

```c
/*
 * OsitoK AArch64 — ARM Generic Timer (scheduler tick)
 *
 * Uses the non-secure physical timer (CNTP). Fires at configurable Hz
 * via PPI 30 through GICv3. This is the preemption source.
 */

#include "../include/hal.h"
#include "../include/aarch64.h"

static uint32_t timer_freq;
static uint32_t tick_interval;  /* ticks per scheduler tick */
static volatile uint64_t tick_count;

#define TIMER_PPI   30

uint64_t timer_get_ticks(void) {
    return read_cntpct_el0();
}

uint32_t timer_get_freq(void) {
    return timer_freq;
}

uint64_t timer_ms(void) {
    return read_cntpct_el0() / (timer_freq / 1000);
}

uint64_t timer_get_tick_count(void) {
    return tick_count;
}

void udelay(uint32_t us) {
    uint64_t start = read_cntpct_el0();
    uint64_t target = (uint64_t)us * (timer_freq / 1000000);
    while ((read_cntpct_el0() - start) < target)
        ;
}

void mdelay(uint32_t ms) {
    uint64_t start = read_cntpct_el0();
    uint64_t target = (uint64_t)ms * (timer_freq / 1000);
    while ((read_cntpct_el0() - start) < target)
        ;
}

void timer_init(uint32_t hz) {
    timer_freq = read_cntfrq_el0();
    tick_interval = timer_freq / hz;
    tick_count = 0;

    serial_puts("[TMR] Freq: ");
    serial_putdec(timer_freq);
    serial_puts(" Hz, tick interval: ");
    serial_putdec(tick_interval);
    serial_puts(" (");
    serial_putdec(hz);
    serial_puts(" Hz scheduler)\n");

    /* Set first timer tick */
    write_cntp_tval_el0(tick_interval);
    write_cntp_ctl_el0(1);  /* ENABLE=1, IMASK=0 */
}

/* Called from IRQ handler when timer PPI fires */
void timer_tick_handler(void) {
    tick_count++;
    /* Reload timer for next tick */
    write_cntp_tval_el0(tick_interval);
}
```

- [ ] **Step 2: Update IRQ handler in start.S**

Replace the minimal `_exception_irq` in `boot/start.S` with a proper handler that saves caller-saved regs and calls C:

```asm
_exception_irq:
    /* Save caller-saved registers */
    stp     x0, x1, [sp, #-16]!
    stp     x2, x3, [sp, #-16]!
    stp     x4, x5, [sp, #-16]!
    stp     x6, x7, [sp, #-16]!
    stp     x8, x9, [sp, #-16]!
    stp     x10, x11, [sp, #-16]!
    stp     x12, x13, [sp, #-16]!
    stp     x14, x15, [sp, #-16]!
    stp     x16, x17, [sp, #-16]!
    stp     x18, x29, [sp, #-16]!
    stp     x30, xzr, [sp, #-16]!

    bl      irq_handler

    ldp     x30, xzr, [sp], #16
    ldp     x18, x29, [sp], #16
    ldp     x16, x17, [sp], #16
    ldp     x14, x15, [sp], #16
    ldp     x12, x13, [sp], #16
    ldp     x10, x11, [sp], #16
    ldp     x8, x9, [sp], #16
    ldp     x6, x7, [sp], #16
    ldp     x4, x5, [sp], #16
    ldp     x2, x3, [sp], #16
    ldp     x0, x1, [sp], #16
    eret
```

- [ ] **Step 3: Implement `irq_handler()` in main.c**

Replace the weak stub:

```c
void irq_handler(void) {
    uint32_t irqnr = gic_ack_irq();

    if (irqnr == 30) {
        /* Timer PPI */
        timer_tick_handler();
    } else if (irqnr == 1023) {
        /* Spurious interrupt — ignore */
    } else {
        serial_puts("[IRQ] Unhandled INTID: ");
        serial_putdec(irqnr);
        serial_puts("\n");
    }

    if (irqnr != 1023)
        gic_end_irq(irqnr);
}
```

- [ ] **Step 4: Enable IRQs in kernel_main and add tick test loop**

After `gic_init()`:
```c
    /* Step 3: Timer (100 Hz) */
    timer_init(100);

    /* Enable IRQs */
    irq_enable();
    serial_puts("[KERN] IRQs enabled\n");

    /* Test: print tick count every second */
    serial_puts("[KERN] Tick counter test (5 seconds)...\n");
    for (int i = 0; i < 5; i++) {
        mdelay(1000);
        serial_puts("  tick_count = ");
        serial_putdec(timer_get_tick_count());
        serial_puts("\n");
    }
```

- [ ] **Step 5: Move timer.c from drivers/ to kernel/ and update Makefile**

```bash
rm arch/arm/drivers/timer.c
```

Add to Makefile:
```makefile
KERN_SRC := kernel/main.c \
            kernel/serial.c \
            kernel/gic.c \
            kernel/timer.c
```

- [ ] **Step 6: Build and verify timer ticks**

```bash
make PLATFORM=virt clean && make PLATFORM=virt && ./scripts/qemu-test.sh --no-build
```

Expected serial log:
```
[TMR] Freq: ... Hz, tick interval: ... (100 Hz scheduler)
[KERN] IRQs enabled
[KERN] Tick counter test (5 seconds)...
  tick_count = ~100
  tick_count = ~200
  tick_count = ~300
  tick_count = ~400
  tick_count = ~500
```

- [ ] **Step 7: Commit**

```bash
git add arch/arm/
git commit -m "arm64: timer IRQ via GICv3, 100Hz tick counter verified"
```

---

## Task 5: Heap Allocator

**Files:**
- Create: `arch/arm/kernel/heap.c`

**Goal:** Port the x86 heap allocator (first-fit, boundary-tag, coalescing). Identical logic, just different pointer sizes (already 64-bit).

- [ ] **Step 1: Create `arch/arm/kernel/heap.c`**

Direct port from x86 — the code is already portable. Key changes:
- Include paths: `../include/hal.h` instead of `../include/types.h`
- Remove `#include "../include/sys_caps.h"` — use fixed initial sizes
- Use `serial_puts`/`serial_puthex`/`serial_putdec` from HAL

```c
/*
 * OsitoK AArch64 — Heap Allocator (kmalloc/kfree)
 *
 * Direct port of arch/x86/kernel/heap.c.
 * First-fit free list with block coalescing.
 */

#include "../include/hal.h"

#define PAGE_SIZE       4096
#define HEAP_INIT_PAGES 64      /* 256 KB initial */
#define HEAP_GROW_PAGES 16      /* 64 KB growth */
#define MIN_ALLOC       16
#define ALIGNMENT       16

#define BLOCK_MAGIC     0x4F53  /* "OS" */
#define BLOCK_FREE      0
#define BLOCK_USED      1

typedef struct block_hdr {
    uint16_t            magic;
    uint16_t            flags;
    uint32_t            _pad;
    uint64_t            size;
    struct block_hdr   *next;
    struct block_hdr   *prev;
} block_hdr_t;

_Static_assert(sizeof(block_hdr_t) == 32, "block header must be 32 bytes");

static block_hdr_t *free_list;
static uint8_t     *heap_start;
static uint8_t     *heap_end;
static uint64_t     heap_size;
static uint64_t     heap_used;
static uint64_t     alloc_count;

static inline uint64_t align_up(uint64_t val, uint64_t align) {
    return (val + align - 1) & ~(align - 1);
}

static void free_list_insert(block_hdr_t *block) {
    block->flags = BLOCK_FREE;
    block_hdr_t *prev = (void *)0, *curr = free_list;
    while (curr && curr < block) { prev = curr; curr = curr->next; }
    block->next = curr;
    block->prev = prev;
    if (prev) prev->next = block; else free_list = block;
    if (curr) curr->prev = block;
}

static void free_list_remove(block_hdr_t *block) {
    if (block->prev) block->prev->next = block->next;
    else free_list = block->next;
    if (block->next) block->next->prev = block;
    block->next = (void *)0;
    block->prev = (void *)0;
}

static void coalesce(block_hdr_t *block) {
    if (block->next) {
        uint8_t *end = (uint8_t *)block + sizeof(block_hdr_t) + block->size;
        if (end == (uint8_t *)block->next) {
            block_hdr_t *next = block->next;
            block->size += sizeof(block_hdr_t) + next->size;
            block->next = next->next;
            if (next->next) next->next->prev = block;
        }
    }
    if (block->prev) {
        uint8_t *pend = (uint8_t *)block->prev + sizeof(block_hdr_t) + block->prev->size;
        if (pend == (uint8_t *)block) {
            block_hdr_t *prev = block->prev;
            prev->size += sizeof(block_hdr_t) + block->size;
            prev->next = block->next;
            if (block->next) block->next->prev = prev;
        }
    }
}

static int heap_grow(uint64_t min_bytes) {
    uint64_t pages = (min_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages < HEAP_GROW_PAGES) pages = HEAP_GROW_PAGES;
    void *new_pages = mem_alloc_pages(pages);
    if (!new_pages) { serial_puts("[HEAP] Failed to grow\n"); return -1; }
    uint64_t new_size = pages * PAGE_SIZE;
    block_hdr_t *nb = (block_hdr_t *)new_pages;
    nb->magic = BLOCK_MAGIC;
    nb->flags = BLOCK_FREE;
    nb->size = new_size - sizeof(block_hdr_t);
    nb->next = (void *)0;
    nb->prev = (void *)0;
    free_list_insert(nb);
    coalesce(nb);
    heap_size += new_size;
    uint8_t *ne = (uint8_t *)new_pages + new_size;
    if (ne > heap_end) heap_end = ne;
    return 0;
}

static void block_split(block_hdr_t *block, uint64_t needed) {
    uint64_t remaining = block->size - needed;
    if (remaining < sizeof(block_hdr_t) + MIN_ALLOC) return;
    block_hdr_t *nb = (block_hdr_t *)((uint8_t *)block + sizeof(block_hdr_t) + needed);
    nb->magic = BLOCK_MAGIC;
    nb->flags = BLOCK_FREE;
    nb->size = remaining - sizeof(block_hdr_t);
    block->size = needed;
    nb->next = block->next;
    nb->prev = block->prev;
    if (block->next) block->next->prev = nb;
    if (block->prev) block->prev->next = nb;
    if (free_list == block) free_list = nb;
}

void *kmalloc(uint64_t size) {
    if (size == 0) return (void *)0;
    size = align_up(size, ALIGNMENT);
    if (size < MIN_ALLOC) size = MIN_ALLOC;
    block_hdr_t *block = free_list;
    while (block) {
        if (block->size >= size) {
            block_split(block, size);
            free_list_remove(block);
            block->flags = BLOCK_USED;
            heap_used += block->size;
            alloc_count++;
            return (void *)((uint8_t *)block + sizeof(block_hdr_t));
        }
        block = block->next;
    }
    if (heap_grow(sizeof(block_hdr_t) + size) < 0) return (void *)0;
    return kmalloc(size);
}

void kfree(void *ptr) {
    if (!ptr) return;
    block_hdr_t *block = (block_hdr_t *)((uint8_t *)ptr - sizeof(block_hdr_t));
    if (block->magic != BLOCK_MAGIC) {
        serial_puts("[HEAP] CORRUPTION at "); serial_puthex((uint64_t)block, 16); serial_puts("\n");
        return;
    }
    if (block->flags != BLOCK_USED) {
        serial_puts("[HEAP] DOUBLE FREE at "); serial_puthex((uint64_t)ptr, 16); serial_puts("\n");
        return;
    }
    heap_used -= block->size;
    alloc_count--;
    free_list_insert(block);
    coalesce(block);
}

void *kcalloc(uint64_t count, uint64_t size) {
    uint64_t total = count * size;
    void *ptr = kmalloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void *krealloc(void *ptr, uint64_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) { kfree(ptr); return (void *)0; }
    block_hdr_t *block = (block_hdr_t *)((uint8_t *)ptr - sizeof(block_hdr_t));
    if (block->magic != BLOCK_MAGIC) return (void *)0;
    if (block->size >= new_size) return ptr;
    void *np = kmalloc(new_size);
    if (!np) return (void *)0;
    memcpy(np, ptr, block->size);
    kfree(ptr);
    return np;
}

uint64_t heap_get_used(void)  { return heap_used; }
uint64_t heap_get_total(void) { return heap_size; }
uint64_t heap_get_count(void) { return alloc_count; }

void heap_init(void) {
    serial_puts("[HEAP] Initializing...\n");
    uint64_t init_size = HEAP_INIT_PAGES * PAGE_SIZE;
    heap_start = (uint8_t *)mem_alloc_pages(HEAP_INIT_PAGES);
    if (!heap_start) { serial_puts("[HEAP] FATAL: alloc failed\n"); return; }
    heap_end = heap_start + init_size;
    heap_size = init_size;
    heap_used = 0;
    alloc_count = 0;
    block_hdr_t *initial = (block_hdr_t *)heap_start;
    initial->magic = BLOCK_MAGIC;
    initial->flags = BLOCK_FREE;
    initial->size = init_size - sizeof(block_hdr_t);
    initial->next = (void *)0;
    initial->prev = (void *)0;
    free_list = initial;
    serial_puts("[HEAP] At "); serial_puthex((uint64_t)heap_start, 16);
    serial_puts(", "); serial_putdec(init_size / 1024); serial_puts(" KB\n");
    /* Self-test */
    void *a = kmalloc(128), *b = kmalloc(256), *c = kmalloc(64);
    if (a && b && c) {
        memset(a, 0xAA, 128); memset(b, 0xBB, 256); memset(c, 0xCC, 64);
        kfree(b);
        void *d = kmalloc(128);
        if (d) { kfree(a); kfree(c); kfree(d); serial_puts("[HEAP] Self-test passed\n"); }
        else serial_puts("[HEAP] Self-test FAILED\n");
    } else serial_puts("[HEAP] Self-test FAILED: alloc\n");
}
```

- [ ] **Step 2: Add heap.c to Makefile and kernel_main**

Makefile:
```makefile
KERN_SRC := kernel/main.c \
            kernel/serial.c \
            kernel/gic.c \
            kernel/timer.c \
            kernel/heap.c
```

kernel_main:
```c
    /* Step 4: Heap */
    heap_init();
```

- [ ] **Step 3: Build, verify self-test passes**

Expected: `[HEAP] Self-test passed`

- [ ] **Step 4: Commit**

```bash
git add arch/arm/kernel/heap.c arch/arm/Makefile arch/arm/kernel/main.c
git commit -m "arm64: port heap allocator from x86 with self-test"
```

---

## Task 6: Preemptive Scheduler

**Files:**
- Create: `arch/arm/kernel/task.h`
- Create: `arch/arm/kernel/context_switch.S`
- Create: `arch/arm/kernel/sched.c`
- Modify: `arch/arm/kernel/main.c`
- Modify: `arch/arm/boot/start.S`

**Goal:** Preemptive scheduler with context switch on timer IRQ. Port from ESP8266 design but adapted for AArch64 register set and GICv3.

- [ ] **Step 1: Create `arch/arm/kernel/task.h`**

```c
#ifndef OSITO_ARM_TASK_H
#define OSITO_ARM_TASK_H

#include "../include/types.h"

#define MAX_TASKS       16
#define TASK_STACK_SIZE 8192    /* 8 KB per task */

/* Task states (same as ESP8266) */
typedef enum {
    TASK_STATE_FREE    = 0,
    TASK_STATE_READY   = 1,
    TASK_STATE_RUNNING = 2,
    TASK_STATE_BLOCKED = 3,
    TASK_STATE_DEAD    = 4,
} task_state_t;

/*
 * AArch64 context frame — saved on task's stack by IRQ handler.
 * 272 bytes = 34 x 8 (x0-x30, SP_EL0, ELR_EL1, SPSR_EL1)
 *
 * This struct must match the layout in context_switch.S exactly.
 */
typedef struct {
    uint64_t x[31];     /* x0-x30 (x30 = LR) */
    uint64_t sp;        /* SP_EL0 (if we run tasks at EL0) or task SP */
    uint64_t elr;       /* ELR_EL1 — return address */
    uint64_t spsr;      /* SPSR_EL1 — saved PSTATE */
} aarch64_ctx_t;

_Static_assert(sizeof(aarch64_ctx_t) == 272, "context frame must be 272 bytes");

/* Task Control Block (sp MUST be at offset 0 — used by asm) */
typedef struct {
    uint64_t        sp;         /* Offset 0: saved stack pointer */
    task_state_t    state;
    uint8_t         id;
    uint8_t         priority;
    uint8_t         _pad;
    uint32_t        ticks_run;
    uint64_t        wake_tick;
    uint8_t        *stack_base;
    uint32_t        stack_size;
    const char     *name;       /* SAFE: only use arrays, not pointers, on SM8350 */
} task_tcb_t;

/* Global scheduler state (accessed by asm) */
extern task_tcb_t  task_pool[MAX_TASKS];
extern task_tcb_t *current_task;
extern volatile int need_schedule;

/* Scheduler functions */
void schedule(void);
task_tcb_t *sched_get_task_pool(void);

#endif
```

- [ ] **Step 2: Create `arch/arm/kernel/context_switch.S`**

```asm
/*
 * context_switch.S — AArch64 full context save/restore for preemptive scheduler
 *
 * Called from IRQ vector. Saves all GP regs + ELR + SPSR to current task's
 * stack, calls C IRQ handler (which may call schedule()), restores next
 * task's context, and erets.
 *
 * Context frame layout (272 bytes, must match task.h aarch64_ctx_t):
 *   [sp, #0]   = x0
 *   [sp, #8]   = x1
 *   ...
 *   [sp, #240] = x30
 *   [sp, #248] = sp_saved (pre-exception SP)
 *   [sp, #256] = ELR_EL1
 *   [sp, #264] = SPSR_EL1
 */

.section .text

/* ── IRQ entry with full context save ──────────────────────── */
.globl irq_vector_entry
irq_vector_entry:
    /* We arrive on the kernel SP (SP_EL1).
     * First, allocate context frame */
    sub     sp, sp, #272

    /* Save x0-x30 */
    stp     x0,  x1,  [sp, #0]
    stp     x2,  x3,  [sp, #16]
    stp     x4,  x5,  [sp, #32]
    stp     x6,  x7,  [sp, #48]
    stp     x8,  x9,  [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30,      [sp, #240]

    /* Save SP (pre-exception = sp + 272) */
    add     x0, sp, #272
    str     x0, [sp, #248]

    /* Save ELR_EL1 and SPSR_EL1 */
    mrs     x0, ELR_EL1
    mrs     x1, SPSR_EL1
    stp     x0, x1, [sp, #256]

    /* Save current SP to current_task->sp (offset 0) */
    adrp    x0, current_task
    add     x0, x0, :lo12:current_task
    ldr     x1, [x0]           /* x1 = current_task pointer */
    mov     x2, sp
    str     x2, [x1]           /* current_task->sp = sp */

    /* Call C IRQ handler (may call schedule() and change current_task) */
    bl      irq_handler

    /* ── Restore (possibly different task) ────────────────── */

    /* Load new current_task->sp */
    adrp    x0, current_task
    add     x0, x0, :lo12:current_task
    ldr     x1, [x0]           /* x1 = (possibly new) current_task */
    ldr     x2, [x1]           /* x2 = current_task->sp */
    mov     sp, x2

    /* Restore ELR_EL1 and SPSR_EL1 */
    ldp     x0, x1, [sp, #256]
    msr     ELR_EL1, x0
    msr     SPSR_EL1, x1

    /* Restore x0-x30 */
    ldp     x0,  x1,  [sp, #0]
    ldp     x2,  x3,  [sp, #16]
    ldp     x4,  x5,  [sp, #32]
    ldp     x6,  x7,  [sp, #48]
    ldp     x8,  x9,  [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldr     x30,      [sp, #240]

    /* Deallocate frame and return from exception */
    add     sp, sp, #272
    eret

/* ── Task entry trampoline ─────────────────────────────────── */
/* New tasks start here via eret. x19=func, x20=arg (callee-saved) */
.globl task_entry_trampoline
task_entry_trampoline:
    mov     x0, x20             /* arg */
    blr     x19                 /* call func(arg) */

    /* Task returned — mark as dead and yield */
    bl      task_exit_handler
1:  wfe
    b       1b
```

- [ ] **Step 3: Update start.S to use irq_vector_entry**

Replace the `_exception_irq` handler in start.S with a jump to the context_switch.S entry:

```asm
_exception_irq:
    b       irq_vector_entry
```

- [ ] **Step 4: Create `arch/arm/kernel/sched.c`**

```c
/*
 * OsitoK AArch64 — Preemptive Scheduler
 *
 * Port of ESP8266 scheduler design. Priority-based round-robin.
 * Timer IRQ calls schedule() which may swap current_task pointer.
 * Context switch happens in context_switch.S by loading new task's SP.
 */

#include "../include/hal.h"
#include "../include/aarch64.h"
#include "task.h"

/* Global state (accessed by context_switch.S) */
task_tcb_t  task_pool[MAX_TASKS];
task_tcb_t *current_task;
volatile int need_schedule;

static int last_scheduled;
static uint8_t stack_pool[MAX_TASKS][TASK_STACK_SIZE]
    __attribute__((aligned(16)));

/* External from context_switch.S */
extern void task_entry_trampoline(void);

task_tcb_t *sched_get_task_pool(void) { return task_pool; }

static void idle_task(void *arg) {
    (void)arg;
    for (;;) __asm__ volatile("wfe");
}

void sched_init(void) {
    serial_puts("[SCHED] Initializing...\n");

    for (int i = 0; i < MAX_TASKS; i++) {
        task_pool[i].state = TASK_STATE_FREE;
        task_pool[i].id = i;
    }

    last_scheduled = 0;
    need_schedule = 0;

    /* Create idle task (ID 0, lowest priority) */
    task_create(idle_task, (void *)0, 0, "idle");

    serial_puts("[SCHED] Scheduler ready\n");
}

int task_create(void (*func)(void *), void *arg, uint8_t priority, const char *name) {
    uint64_t flags = irq_save();

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_pool[i].state == TASK_STATE_FREE) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        irq_restore(flags);
        serial_puts("[SCHED] No free task slots!\n");
        return -1;
    }

    task_tcb_t *t = &task_pool[slot];
    t->state = TASK_STATE_READY;
    t->priority = priority;
    t->name = name;
    t->ticks_run = 0;
    t->wake_tick = 0;
    t->stack_base = stack_pool[slot];
    t->stack_size = TASK_STACK_SIZE;

    /* Set up initial context frame on top of stack */
    uint64_t sp_top = (uint64_t)(t->stack_base + t->stack_size);
    sp_top &= ~0xFULL;         /* 16-byte align */
    sp_top -= sizeof(aarch64_ctx_t);  /* allocate context frame */

    aarch64_ctx_t *ctx = (aarch64_ctx_t *)sp_top;
    memset(ctx, 0, sizeof(*ctx));

    /* Task entry: trampoline reads x19=func, x20=arg (callee-saved) */
    ctx->x[19] = (uint64_t)func;
    ctx->x[20] = (uint64_t)arg;
    ctx->elr   = (uint64_t)task_entry_trampoline;
    ctx->spsr  = 0x305;  /* EL1h, IRQ unmasked (DAIF.I=0) */
    ctx->sp    = sp_top + sizeof(aarch64_ctx_t);  /* pre-exception SP */

    t->sp = sp_top;

    serial_puts("[SCHED] Created task '");
    serial_puts(name);
    serial_puts("' (id=");
    serial_putdec(slot);
    serial_puts(", pri=");
    serial_putdec(priority);
    serial_puts(")\n");

    irq_restore(flags);
    return slot;
}

void schedule(void) {
    /* Find highest priority ready task (round-robin at same priority) */
    int best = -1;
    uint8_t best_pri = 0;

    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_pool[i].state == TASK_STATE_READY &&
            task_pool[i].priority >= best_pri) {
            best_pri = task_pool[i].priority;
        }
    }

    /* Round-robin among tasks at best_pri */
    int start = (last_scheduled + 1) % MAX_TASKS;
    for (int n = 0; n < MAX_TASKS; n++) {
        int i = (start + n) % MAX_TASKS;
        if (task_pool[i].state == TASK_STATE_READY &&
            task_pool[i].priority == best_pri) {
            /* Skip idle (0) if anything else is ready */
            if (i == 0 && best_pri == 0) {
                int others = 0;
                for (int j = 1; j < MAX_TASKS; j++)
                    if (task_pool[j].state == TASK_STATE_READY) { others = 1; break; }
                if (others) continue;
            }
            best = i;
            break;
        }
    }

    if (best < 0) best = 0;  /* Fallback to idle */

    /* Switch */
    if (current_task && current_task->state == TASK_STATE_RUNNING)
        current_task->state = TASK_STATE_READY;

    current_task = &task_pool[best];
    current_task->state = TASK_STATE_RUNNING;
    last_scheduled = best;
}

void task_yield(void) {
    /* Trigger SGI 0 to self — enters IRQ handler → schedule() */
    gic_send_sgi_self(0);
}

void task_delay_ms(uint32_t ms) {
    uint64_t flags = irq_save();
    uint64_t ticks = (uint64_t)ms / 10;  /* 100 Hz tick = 10ms per tick */
    if (ticks == 0) ticks = 1;
    current_task->wake_tick = timer_get_tick_count() + ticks;
    current_task->state = TASK_STATE_BLOCKED;
    irq_restore(flags);
    task_yield();
}

void task_exit_handler(void) {
    uint64_t flags = irq_save();
    current_task->state = TASK_STATE_DEAD;
    irq_restore(flags);
    task_yield();
    for (;;) __asm__ volatile("wfe");
}

void sched_start(void) {
    serial_puts("[SCHED] Starting scheduler\n");

    /* Pick first ready task */
    schedule();

    /* Load its context and eret — never returns */
    uint64_t sp = current_task->sp;
    __asm__ volatile(
        "mov sp, %0\n"
        /* Restore ELR_EL1 and SPSR_EL1 */
        "ldp x0, x1, [sp, #256]\n"
        "msr ELR_EL1, x0\n"
        "msr SPSR_EL1, x1\n"
        /* Restore x0-x30 */
        "ldp x0,  x1,  [sp, #0]\n"
        "ldp x2,  x3,  [sp, #16]\n"
        "ldp x4,  x5,  [sp, #32]\n"
        "ldp x6,  x7,  [sp, #48]\n"
        "ldp x8,  x9,  [sp, #64]\n"
        "ldp x10, x11, [sp, #80]\n"
        "ldp x12, x13, [sp, #96]\n"
        "ldp x14, x15, [sp, #112]\n"
        "ldp x16, x17, [sp, #128]\n"
        "ldp x18, x19, [sp, #144]\n"
        "ldp x20, x21, [sp, #160]\n"
        "ldp x22, x23, [sp, #176]\n"
        "ldp x24, x25, [sp, #192]\n"
        "ldp x26, x27, [sp, #208]\n"
        "ldp x28, x29, [sp, #224]\n"
        "ldr x30,      [sp, #240]\n"
        "add sp, sp, #272\n"
        "eret\n"
        :: "r"(sp)
    );
    __builtin_unreachable();
}
```

- [ ] **Step 5: Update irq_handler in main.c to handle scheduler ticks + wake blocked tasks**

```c
/* extern declarations */
extern void timer_tick_handler(void);
extern void schedule(void);
extern task_tcb_t task_pool[];
extern volatile int need_schedule;

void irq_handler(void) {
    uint32_t irqnr = gic_ack_irq();

    if (irqnr == 30) {
        /* Timer PPI */
        timer_tick_handler();

        if (current_task)
            current_task->ticks_run++;

        /* Wake sleeping tasks */
        uint64_t now = timer_get_tick_count();
        for (int i = 0; i < MAX_TASKS; i++) {
            if (task_pool[i].state == TASK_STATE_BLOCKED &&
                task_pool[i].wake_tick != 0 &&
                (int64_t)(now - task_pool[i].wake_tick) >= 0) {
                task_pool[i].wake_tick = 0;
                task_pool[i].state = TASK_STATE_READY;
            }
        }

        need_schedule = 1;
    } else if (irqnr == 0) {
        /* SGI 0 — software yield */
        need_schedule = 1;
    } else if (irqnr != 1023) {
        serial_puts("[IRQ] Unhandled INTID: ");
        serial_putdec(irqnr);
        serial_puts("\n");
    }

    if (need_schedule && current_task) {
        need_schedule = 0;
        schedule();
    }

    if (irqnr != 1023)
        gic_end_irq(irqnr);
}
```

- [ ] **Step 6: Update kernel_main to use scheduler with test tasks**

```c
static void test_task_a(void *arg) {
    (void)arg;
    for (int i = 0; i < 10; i++) {
        serial_puts("[A] tick ");
        serial_putdec(timer_get_tick_count());
        serial_puts("\n");
        task_delay_ms(500);
    }
    serial_puts("[A] done\n");
}

static void test_task_b(void *arg) {
    (void)arg;
    for (int i = 0; i < 10; i++) {
        serial_puts("[B] tick ");
        serial_putdec(timer_get_tick_count());
        serial_puts("\n");
        task_delay_ms(700);
    }
    serial_puts("[B] done\n");
}

void kernel_main(void *dtb) {
    /* Step 0 */ platform_init(dtb);
    serial_puts("[KERN] Serial OK\n");
    print_banner();
    print_cpu_info();

    /* Step 2 */ gic_init();
    /* Step 3 */ timer_init(100);
    /* Step 4 */ heap_init();

    /* Step 5: Scheduler */
    sched_init();
    task_create(test_task_a, (void *)0, 1, "taskA");
    task_create(test_task_b, (void *)0, 1, "taskB");

    /* Enable IRQs and start scheduler (never returns) */
    irq_enable();
    sched_start();
}
```

- [ ] **Step 7: Update Makefile**

```makefile
KERN_SRC := kernel/main.c \
            kernel/serial.c \
            kernel/gic.c \
            kernel/timer.c \
            kernel/heap.c \
            kernel/sched.c

ASM_SRC  := kernel/context_switch.S

ALL_SRC := $(BOOT_ASM) $(KERN_SRC) $(PLAT_SRC) $(ASM_SRC)
```

- [ ] **Step 8: Build and verify preemptive multitasking**

Expected serial output:
```
[SCHED] Starting scheduler
[A] tick 1
[B] tick 1
[A] tick 51
[B] tick 71
...interleaved A and B ticks...
```

- [ ] **Step 9: Commit**

```bash
git add arch/arm/
git commit -m "arm64: preemptive scheduler with context switch and timer-driven multitasking"
```

---

## Task 7: Framebuffer Console

**Files:**
- Create: `arch/arm/kernel/framebuffer.c`

**Goal:** Port x86 framebuffer text console with font8x16. QEMU uses ramfb or serial-only initially; SM8350 uses splash FB.

- [ ] **Step 1: Create `arch/arm/kernel/framebuffer.c`**

Port the x86 framebuffer.c with the full font8x16 array. The font data and text rendering logic are identical. Platform provides the base address via `platform_init()` → `fb_init_platform()`.

This file is essentially a copy of `arch/x86/kernel/framebuffer.c` lines 1-134 (font data) plus the text console logic, adapted to use `hal.h` includes.

- [ ] **Step 2: Add fb_init_platform call for QEMU virt**

In `platform/virt/platform.c`, add ramfb init or leave as no-op (serial-only for Phase 1):
```c
void fb_init_platform(uint32_t *base, uint32_t w, uint32_t h, uint32_t pitch) {
    /* QEMU virt: no framebuffer in Phase 1. Serial-only output. */
    (void)base; (void)w; (void)h; (void)pitch;
}
```

- [ ] **Step 3: SM8350 framebuffer integration**

Already exists in `platform/sm8350/framebuffer.c`. Call from platform_init:
```c
fb_init_platform((uint32_t *)SPLASH_FB_BASE, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_WIDTH);
```

- [ ] **Step 4: Commit**

```bash
git add arch/arm/kernel/framebuffer.c
git commit -m "arm64: port framebuffer text console with 8x16 font from x86"
```

---

## Task 8: Terminal + Shell

**Files:**
- Create: `arch/arm/kernel/term.c`
- Create: `arch/arm/kernel/shell.c`

**Goal:** Interactive shell over UART with line editing and basic builtins (help, ps, mem, reboot, clear, uname).

- [ ] **Step 1: Create `arch/arm/kernel/term.c`**

Simple line editor with history:

```c
/*
 * Terminal line editor — readline-style over UART
 */
#include "../include/hal.h"

#define TERM_BUFSIZE    256
#define TERM_HISTORY    8

static char history[TERM_HISTORY][TERM_BUFSIZE];
static int hist_count, hist_pos;

void term_init(void) {
    hist_count = 0;
    hist_pos = 0;
}

int term_readline(const char *prompt, char *buf, int bufsize) {
    serial_puts(prompt);
    int pos = 0;
    buf[0] = 0;

    for (;;) {
        char c = uart_getc();

        if (c == '\r' || c == '\n') {
            serial_puts("\n");
            buf[pos] = 0;
            /* Add to history */
            if (pos > 0 && hist_count < TERM_HISTORY) {
                for (int i = 0; i < pos + 1; i++)
                    history[hist_count][i] = buf[i];
                hist_count++;
            }
            hist_pos = hist_count;
            return pos;
        } else if (c == 0x7F || c == '\b') {
            if (pos > 0) {
                pos--;
                serial_puts("\b \b");
            }
        } else if (c == 0x03) {  /* Ctrl-C */
            serial_puts("^C\n");
            buf[0] = 0;
            return 0;
        } else if (c >= 0x20 && c < 0x7F && pos < bufsize - 1) {
            buf[pos++] = c;
            serial_putc(c);
        }
    }
}
```

- [ ] **Step 2: Create `arch/arm/kernel/shell.c`**

```c
/*
 * OsitoK AArch64 — Interactive Shell
 *
 * Mirrors x86 shell with basic builtins.
 */
#include "../include/hal.h"
#include "task.h"

extern int term_readline(const char *prompt, char *buf, int bufsize);

#define MAX_ARGS 16

static int parse_args(char *line, char **argv) {
    int argc = 0;
    while (*line && argc < MAX_ARGS) {
        while (*line == ' ') line++;
        if (!*line) break;
        argv[argc++] = line;
        while (*line && *line != ' ') line++;
        if (*line) *line++ = 0;
    }
    return argc;
}

static void cmd_help(void) {
    serial_puts("Commands: help, ps, mem, reboot, clear, uname, uptime\n");
}

static void cmd_ps(void) {
    serial_puts("ID  State    Pri  Ticks  Name\n");
    serial_puts("--  -------  ---  -----  ----\n");
    const char *states[] = {"free", "ready", "RUN", "blocked", "dead"};
    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_pool[i].state == TASK_STATE_FREE) continue;
        serial_putdec(i);
        serial_puts(i < 10 ? "   " : "  ");
        serial_puts(states[task_pool[i].state]);
        /* Pad */
        int slen = strlen(states[task_pool[i].state]);
        for (int p = slen; p < 9; p++) serial_putc(' ');
        serial_putdec(task_pool[i].priority);
        serial_puts("    ");
        serial_putdec(task_pool[i].ticks_run);
        serial_puts("   ");
        serial_puts(task_pool[i].name ? task_pool[i].name : "?");
        serial_puts("\n");
    }
}

static void cmd_mem(void) {
    serial_puts("Heap used: ");
    serial_putdec(heap_get_used() / 1024);
    serial_puts(" KB / ");
    serial_putdec(heap_get_total() / 1024);
    serial_puts(" KB\n");
}

static void cmd_uname(void) {
    serial_puts("OsitoK AArch64 v0.1 (bare-metal)\n");
}

static void cmd_uptime(void) {
    uint64_t ticks = timer_get_tick_count();
    uint64_t secs = ticks / 100;
    serial_puts("Uptime: ");
    serial_putdec(secs);
    serial_puts("s (");
    serial_putdec(ticks);
    serial_puts(" ticks)\n");
}

void shell_task(void *arg) {
    (void)arg;
    char buf[256];
    char *argv[MAX_ARGS];

    serial_puts("\nOsitoK shell ready. Type 'help' for commands.\n");

    for (;;) {
        int len = term_readline("shell> ", buf, sizeof(buf));
        if (len == 0) continue;

        int argc = parse_args(buf, argv);
        if (argc == 0) continue;

        if      (strcmp(argv[0], "help") == 0)   cmd_help();
        else if (strcmp(argv[0], "ps") == 0)     cmd_ps();
        else if (strcmp(argv[0], "mem") == 0)    cmd_mem();
        else if (strcmp(argv[0], "uname") == 0)  cmd_uname();
        else if (strcmp(argv[0], "uptime") == 0) cmd_uptime();
        else if (strcmp(argv[0], "reboot") == 0) platform_reboot();
        else if (strcmp(argv[0], "clear") == 0)  serial_puts("\033[2J\033[H");
        else {
            serial_puts("Unknown command: ");
            serial_puts(argv[0]);
            serial_puts("\n");
        }
    }
}

void shell_run(void) {
    shell_task((void *)0);
}
```

- [ ] **Step 3: Update kernel_main — replace test tasks with shell**

```c
void kernel_main(void *dtb) {
    platform_init(dtb);
    serial_puts("[KERN] Serial OK\n");
    print_banner();
    print_cpu_info();

    gic_init();
    timer_init(100);
    heap_init();

    sched_init();
    term_init();
    task_create(shell_task, (void *)0, 1, "shell");

    serial_puts("[KERN] Boot complete.\n");
    irq_enable();
    sched_start();
}
```

- [ ] **Step 4: Update Makefile**

```makefile
KERN_SRC := kernel/main.c \
            kernel/serial.c \
            kernel/gic.c \
            kernel/timer.c \
            kernel/heap.c \
            kernel/sched.c \
            kernel/term.c \
            kernel/shell.c
```

- [ ] **Step 5: Build and test interactive shell**

Boot QEMU, type commands via stdio serial:
```
shell> help
Commands: help, ps, mem, reboot, clear, uname, uptime
shell> ps
ID  State    Pri  Ticks  Name
--  -------  ---  -----  ----
0   ready    0    1234   idle
1   RUN      1    56     shell
shell> mem
Heap used: 0 KB / 256 KB
shell> uname
OsitoK AArch64 v0.1 (bare-metal)
```

- [ ] **Step 6: Commit**

```bash
git add arch/arm/kernel/term.c arch/arm/kernel/shell.c arch/arm/Makefile arch/arm/kernel/main.c
git commit -m "arm64: interactive shell with ps, mem, uname, uptime builtins"
```

---

## Task 9: QEMU Test Script Polish + SM8350 Milestone Test

**Goal:** Verify the full stack works on QEMU, then build boot.img and test on ROG Phone 5.

- [ ] **Step 1: Update qemu-test.sh for interactive serial**

Change `-serial file:...` to `-serial mon:stdio` for interactive shell:

```bash
qemu-system-aarch64 \
    -M virt,gic-version=3 \
    -cpu cortex-a72 \
    -m 512M \
    -nographic \
    -kernel "$KERNEL_BIN" \
    -no-reboot
```

(`-nographic` routes serial to stdio)

- [ ] **Step 2: Create SM8350 boot.img build script**

Create `arch/arm/scripts/mkbootimg.sh`:
```bash
#!/bin/bash
# Build boot.img for ROG Phone 5 (SM8350)
set -e
ARM_DIR="$(cd "$(dirname "$0")/.." && pwd)"
make -C "$ARM_DIR" PLATFORM=sm8350 -j$(nproc)
IMAGE="$ARM_DIR/build/Image"
# Requires stock.dtb from device (extracted via adb)
DTB="$ARM_DIR/build/stock.dtb"
[ -f "$DTB" ] || { echo "Need stock.dtb — extract from device"; exit 1; }
mkbootimg --kernel "$IMAGE" --ramdisk /dev/null --dtb "$DTB" \
    --base 0x00000000 --kernel_offset 0x00080000 \
    --header_version 2 --pagesize 4096 \
    -o "$ARM_DIR/build/boot.img"
echo "boot.img ready: $ARM_DIR/build/boot.img"
echo "Flash: fastboot boot $ARM_DIR/build/boot.img"
```

- [ ] **Step 3: Test on ROG Phone 5**

```bash
fastboot boot arch/arm/build/boot.img
# Watch serial output via USB-UART adapter or framebuffer
```

- [ ] **Step 4: Commit**

```bash
git add arch/arm/scripts/
git commit -m "arm64: add QEMU and SM8350 boot scripts"
```

---

## Summary — Milestone Checklist

| Task | Subsystem | Deliverable |
|------|-----------|-------------|
| 1 | Build + UART | "OsitoK ARM64" on QEMU serial |
| 2 | SM8350 backend | Platform files restructured, compiles |
| 3 | GICv3 | Interrupt controller initialized |
| 4 | Timer IRQ | 100Hz tick counter verified |
| 5 | Heap | kmalloc/kfree with self-test |
| 6 | Scheduler | Preemptive multitasking (two tasks interleaving) |
| 7 | Framebuffer | Text console with 8x16 font |
| 8 | Shell | Interactive shell with builtins |
| 9 | Integration | QEMU + ROG Phone 5 milestone boot |

After Task 9, the ARM64 kernel has feature parity with x86 core subsystems. Next phase would port: NVMe, OsitoFS, network stack, syscalls, process subsystem.
