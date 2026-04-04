/*
 * OsitoK — DOS Compatibility Layer Initialization
 *
 * Sets up IDT entries for native 32-bit DOS INT handling (INT 21h, 10h, etc.)
 * and marks the DOS subsystem as ready. Called from main.c after win32_init().
 *
 * For 32-bit protected mode code (DOS4GW/DOOM), INTs trap via IDT to
 * dos_int_stub.S → dos_int_native_dispatch(), running at native CPU speed.
 * For 16-bit real mode code (COM/MZ), the 8086 interpreter handles INTs.
 */

#include "dos_types.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void fb_puts(const char *s);

/* IDT entry structure (from idt.c) */
typedef struct __attribute__((packed)) {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} dos_idt_entry_t;

/* Kernel IDT array (256 entries, defined in idt.c) */
extern dos_idt_entry_t idt[] __attribute__((weak));

/* Assembly INT stubs (dos_int_stub.S) */
extern void dos_int08_stub(void);
extern void dos_int10_stub(void);
extern void dos_int16_stub(void);
extern void dos_int20_stub(void);
extern void dos_int21_stub(void);
extern void dos_int2f_stub(void);
extern void dos_int31_stub(void);
extern void dos_int33_stub(void);

/* ── Install a single DOS IDT entry ─────────────────────────────── */

static void install_dos_idt_entry(uint8_t vector, void (*handler)(void))
{
    if (!idt) return;

    uint16_t cs;
    __asm__ volatile ("mov %%cs, %0" : "=r"(cs));

    uint64_t addr = (uint64_t)handler;

    idt[vector].offset_low  = (uint16_t)(addr & 0xFFFF);
    idt[vector].offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF);
    idt[vector].offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    idt[vector].selector    = cs;
    idt[vector].ist         = 2;     /* IST2: DOS dedicated stack */
    idt[vector].type_attr   = 0x8F;  /* present, DPL=0, 64-bit trap gate */
    idt[vector].reserved    = 0;
}

/* ── DOS subsystem initialization ───────────────────────────────── */

static int dos_initialized = 0;

void dos_init(void)
{
    if (dos_initialized) return;

    serial_puts("\n[DOS] Initializing DOS compatibility layer...\n");

    /* Install IDT entries for native 32-bit INT handling.
     * When DOS4GW switches to PM and DOOM runs natively,
     * these INTs trap to our assembly stubs → C handlers. */
    /* IMPORTANT:
     * Vectors 0x00-0x1F = CPU exceptions — NEVER overwrite
     * Vector 0x20 (32) = APIC timer — NEVER overwrite
     * Vectors 0x21-0x2F = kernel IRQs may use some — check first
     *
     * Safe DOS vectors (not used by kernel hardware):
     * 0x21 (33) — not used by kernel (PIC remapped above 0x20)
     * But to be safe, only install vectors >= 0x21 that the kernel
     * doesn't use for hardware IRQs. The kernel uses 0x20 for APIC timer
     * and 0x21-0x2F for PIC IRQs (keyboard at 0x21, etc.)
     *
     * Strategy: DON'T install any IDT entries at boot time.
     * Instead, install them ONLY when entering native DOS 32-bit mode
     * (dos_transfer_to_native), and restore originals on exit.
     * This avoids ALL conflicts with the kernel. */

    serial_puts("[DOS] IDT entries deferred until native PM switch\n");

    dos_initialized = 1;

    serial_puts("[DOS] Ready. 16-bit via interpreter, 32-bit via native compat mode.\n");
    fb_puts(" DOS compat layer ready\n");
}

int dos_is_initialized(void)
{
    return dos_initialized;
}
