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
    install_dos_idt_entry(0x08, dos_int08_stub);   /* Timer (IRQ 0) */
    install_dos_idt_entry(0x10, dos_int10_stub);   /* BIOS Video */
    install_dos_idt_entry(0x16, dos_int16_stub);   /* BIOS Keyboard */
    install_dos_idt_entry(0x20, dos_int20_stub);   /* DOS Terminate */
    install_dos_idt_entry(0x21, dos_int21_stub);   /* DOS API */
    install_dos_idt_entry(0x2F, dos_int2f_stub);   /* DOS Multiplex */
    install_dos_idt_entry(0x31, dos_int31_stub);   /* DPMI */
    install_dos_idt_entry(0x33, dos_int33_stub);   /* Mouse */

    serial_puts("[DOS] IDT vectors installed: 08h,10h,16h,20h,21h,2Fh,31h,33h (IST2)\n");

    dos_initialized = 1;

    serial_puts("[DOS] Ready. 16-bit via interpreter, 32-bit via native compat mode.\n");
    fb_puts(" DOS compat layer ready\n");
}

int dos_is_initialized(void)
{
    return dos_initialized;
}
