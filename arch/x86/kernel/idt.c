/*
 * OsitoK x86-64 — IDT + Exception Handlers + APIC Timer
 *
 * X-OS1: Sets up our own Interrupt Descriptor Table, replacing the
 * UEFI-provided one. Handles CPU exceptions (#PF, #GP, #UD, etc.)
 * and provides a periodic APIC timer tick for future scheduling.
 *
 * All 256 IDT entries are populated. Vectors 0-31 are CPU exceptions,
 * vector 32 is APIC timer, vectors 33-255 are stubs that just IRET.
 */

#include "../include/types.h"

/* ── External functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);
extern void fb_puts_color(const char *s, uint32_t color);
extern void fb_putdec(uint64_t val);
extern void fb_puthex(uint64_t val, int digits);

/* ── IDT structures (x86-64 long mode) ──────────────────────── */

typedef struct __attribute__((packed)) {
    uint16_t offset_low;     /* bits 0-15 of handler address */
    uint16_t selector;       /* code segment selector (GDT) */
    uint8_t  ist;            /* bits 0-2: IST index, rest zero */
    uint8_t  type_attr;      /* type + DPL + present */
    uint16_t offset_mid;     /* bits 16-31 */
    uint32_t offset_high;    /* bits 32-63 */
    uint32_t reserved;
} idt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} idt_ptr_t;

_Static_assert(sizeof(idt_entry_t) == 16, "IDT entry must be 16 bytes");

/* ── IDT table (256 entries, 16 bytes each = 4KB) ───────────── */

static idt_entry_t idt[256] __attribute__((aligned(16)));
static idt_ptr_t   idtr;

/* ── Interrupt frame pushed by CPU + our stub ────────────────── */

typedef struct __attribute__((packed)) {
    /* Pushed by our stub (isr_common) */
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;  /* CPU pushes for some vectors, stub pushes 0 for others */
    /* Pushed by CPU on interrupt */
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} interrupt_frame_t;

/* ── ISR stub declarations (defined in isr_stubs.S) ──────────── */

/* Each vector has an entry point: isr_stub_0 through isr_stub_255 */
extern void isr_stub_0(void);
extern void isr_stub_1(void);
extern void isr_stub_2(void);
extern void isr_stub_3(void);
extern void isr_stub_4(void);
extern void isr_stub_5(void);
extern void isr_stub_6(void);
extern void isr_stub_7(void);
extern void isr_stub_8(void);
extern void isr_stub_9(void);
extern void isr_stub_10(void);
extern void isr_stub_11(void);
extern void isr_stub_12(void);
extern void isr_stub_13(void);
extern void isr_stub_14(void);
extern void isr_stub_15(void);
extern void isr_stub_16(void);
extern void isr_stub_17(void);
extern void isr_stub_18(void);
extern void isr_stub_19(void);
extern void isr_stub_20(void);
extern void isr_stub_21(void);
extern void isr_stub_22(void);
extern void isr_stub_23(void);
extern void isr_stub_24(void);
extern void isr_stub_25(void);
extern void isr_stub_26(void);
extern void isr_stub_27(void);
extern void isr_stub_28(void);
extern void isr_stub_29(void);
extern void isr_stub_30(void);
extern void isr_stub_31(void);
extern void isr_stub_32(void);   /* APIC timer */
extern void isr_stub_33(void);   /* Keyboard IRQ */
extern void isr_stub_default(void);  /* vectors 34-255 */

/* Keyboard handler */
extern void keyboard_irq(void);

/* ── Exception names ─────────────────────────────────────────── */

static const char *exception_names[] = {
    "#DE Divide Error",
    "#DB Debug",
    "NMI",
    "#BP Breakpoint",
    "#OF Overflow",
    "#BR Bound Range",
    "#UD Invalid Opcode",
    "#NM No Math Coprocessor",
    "#DF Double Fault",
    "Coprocessor Segment Overrun",
    "#TS Invalid TSS",
    "#NP Segment Not Present",
    "#SS Stack-Segment Fault",
    "#GP General Protection",
    "#PF Page Fault",
    "(reserved 15)",
    "#MF x87 FPU Error",
    "#AC Alignment Check",
    "#MC Machine Check",
    "#XM SIMD Exception",
    "#VE Virtualization",
    "#CP Control Protection",
    "(reserved 22)", "(reserved 23)", "(reserved 24)", "(reserved 25)",
    "(reserved 26)", "(reserved 27)", "(reserved 28)",
    "#VC VMM Communication",
    "#SX Security Exception",
    "(reserved 31)"
};

/* ── APIC registers ──────────────────────────────────────────── */

#define APIC_BASE_MSR          0x1B
#define APIC_BASE_ENABLE       (1ULL << 11)

/* APIC MMIO (xAPIC mode) */
static volatile uint32_t *apic_base;

#define APIC_ID          0x020
#define APIC_VERSION     0x030
#define APIC_TPR         0x080
#define APIC_EOI         0x0B0
#define APIC_SVR         0x0F0
#define APIC_ESR         0x280
#define APIC_ICR_LOW     0x300
#define APIC_ICR_HIGH    0x310
#define APIC_LVT_TIMER   0x320
#define APIC_TIMER_INIT  0x380
#define APIC_TIMER_CURR  0x390
#define APIC_TIMER_DIV   0x3E0

#define APIC_SVR_ENABLE  0x100
#define APIC_LVT_MASKED  0x10000
#define APIC_TIMER_PERIODIC  0x20000

static inline void apic_write(uint32_t reg, uint32_t val) {
    apic_base[reg / 4] = val;
}

static inline uint32_t apic_read(uint32_t reg) {
    return apic_base[reg / 4];
}

/* ── MSR helpers ─────────────────────────────────────────────── */

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile ("wrmsr" : : "c"(msr),
                      "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

/* ── State ───────────────────────────────────────────────────── */

static volatile uint64_t tick_count;
static bool apic_enabled;

uint64_t idt_get_ticks(void) { return tick_count; }

/* ── Set one IDT entry ───────────────────────────────────────── */

static void idt_set_entry(int vector, void (*handler)(void), uint8_t ist)
{
    uint64_t addr = (uint64_t)handler;
    idt[vector].offset_low  = (uint16_t)(addr & 0xFFFF);
    idt[vector].offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF);
    idt[vector].offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    idt[vector].selector    = 0x38;  /* kernel code segment — typical UEFI GDT CS */
    idt[vector].ist         = ist;
    idt[vector].type_attr   = 0x8E;  /* Present, DPL=0, 64-bit interrupt gate */
    idt[vector].reserved    = 0;
}

/* ── Read current CS for IDT selector ────────────────────────── */

static uint16_t get_cs(void)
{
    uint16_t cs;
    __asm__ volatile ("mov %%cs, %0" : "=r"(cs));
    return cs;
}

/* ── Exception handler (called from isr_common in ASM) ─────── */

void isr_handler(interrupt_frame_t *frame)
{
    uint64_t vec = frame->vector;

    /* APIC timer tick */
    if (vec == 32) {
        tick_count++;
        if (apic_enabled)
            apic_write(APIC_EOI, 0);
        return;
    }

    /* Keyboard IRQ (IDT 0x71 uses isr_stub_33 which pushes vec=33) */
    if (vec == 33) {
        keyboard_irq();
        outb(0x20, 0x20);  /* PIC EOI to master */
        return;
    }

    /* Other PIC IRQs (just EOI and ignore) */
    if (vec >= 0x70 && vec < 0x80) {
        if (vec >= 0x78) outb(0xA0, 0x20);  /* Slave EOI */
        outb(0x20, 0x20);  /* Master EOI */
        return;
    }

    /* CPU exception (vectors 0-31) */
    if (vec < 32) {
        serial_puts("\n!!! EXCEPTION: ");
        serial_puts(exception_names[vec]);
        serial_puts(" (vector ");
        serial_putdec(vec);
        serial_puts(")\n");

        serial_puts("  RIP = 0x");
        serial_puthex(frame->rip, 16);
        serial_puts("  CS  = 0x");
        serial_puthex(frame->cs, 4);
        serial_puts("\n");

        serial_puts("  ERR = 0x");
        serial_puthex(frame->error_code, 16);
        serial_puts("  RSP = 0x");
        serial_puthex(frame->rsp, 16);
        serial_puts("\n");

        serial_puts("  RAX = 0x");
        serial_puthex(frame->rax, 16);
        serial_puts("  RBX = 0x");
        serial_puthex(frame->rbx, 16);
        serial_puts("\n");

        serial_puts("  RCX = 0x");
        serial_puthex(frame->rcx, 16);
        serial_puts("  RDX = 0x");
        serial_puthex(frame->rdx, 16);
        serial_puts("\n");

        serial_puts("  RSI = 0x");
        serial_puthex(frame->rsi, 16);
        serial_puts("  RDI = 0x");
        serial_puthex(frame->rdi, 16);
        serial_puts("\n");

        serial_puts("  RBP = 0x");
        serial_puthex(frame->rbp, 16);
        serial_puts("  RFLAGS = 0x");
        serial_puthex(frame->rflags, 16);
        serial_puts("\n");

        /* Page fault: decode CR2 */
        if (vec == 14) {
            uint64_t cr2;
            __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
            serial_puts("  CR2 = 0x");
            serial_puthex(cr2, 16);
            serial_puts(" (faulting address)\n");

            serial_puts("  PF flags: ");
            if (frame->error_code & 1) serial_puts("PRESENT ");
            if (frame->error_code & 2) serial_puts("WRITE ");
            else serial_puts("READ ");
            if (frame->error_code & 4) serial_puts("USER ");
            else serial_puts("SUPERVISOR ");
            if (frame->error_code & 8) serial_puts("RESERVED-BIT ");
            if (frame->error_code & 16) serial_puts("INSTRUCTION-FETCH ");
            serial_puts("\n");
        }

        /* GP fault: decode error code */
        if (vec == 13 && frame->error_code != 0) {
            serial_puts("  GP selector: 0x");
            serial_puthex(frame->error_code & 0xFFFF, 4);
            if (frame->error_code & 1) serial_puts(" (EXT)");
            if (frame->error_code & 2) serial_puts(" (IDT)");
            else if (frame->error_code & 4) serial_puts(" (LDT)");
            else serial_puts(" (GDT)");
            serial_puts("\n");
        }

        /* Framebuffer output */
        fb_puts_color("\n !!! ", 0x00FF0000);
        fb_puts_color(exception_names[vec], 0x00FF0000);
        fb_puts(" at 0x");
        fb_puthex(frame->rip, 16);
        fb_puts("\n");

        /* Halt on exception — no recovery yet */
        serial_puts("  SYSTEM HALTED\n");
        fb_puts_color(" SYSTEM HALTED\n", 0x00FF0000);
        __asm__ volatile ("cli");
        for (;;) __asm__ volatile ("hlt");
    }

    /* Spurious / unhandled vector — just EOI */
    if (apic_enabled)
        apic_write(APIC_EOI, 0);
}

/* ── 8259 PIC init (remap IRQ 0-15 → vectors 32-47) ──────────── */

static void pic_init(void)
{
    /* Mask all IRQs first to prevent spurious interrupts during remap */
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

    /* Small I/O delay */
    inb(0x80); inb(0x80);

    /* ICW1: begin init sequence, cascade, ICW4 needed */
    outb(0x20, 0x11);  /* Master PIC command */
    inb(0x80);
    outb(0xA0, 0x11);  /* Slave PIC command */
    inb(0x80);

    /* ICW2: remap vectors (above APIC timer at 32) */
    outb(0x21, 0x70);  /* Master: IRQ 0-7 → vectors 0x70-0x77 */
    inb(0x80);
    outb(0xA1, 0x78);  /* Slave:  IRQ 8-15 → vectors 0x78-0x7F */
    inb(0x80);

    /* ICW3: cascade wiring */
    outb(0x21, 0x04);  /* Master: slave on IRQ 2 */
    inb(0x80);
    outb(0xA1, 0x02);  /* Slave:  cascade identity 2 */
    inb(0x80);

    /* ICW4: 8086 mode */
    outb(0x21, 0x01);
    inb(0x80);
    outb(0xA1, 0x01);
    inb(0x80);

    /* Mask all IRQs initially (keyboard will unmask IRQ 1 in kb_init) */
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

    /* Send EOI to clear any pending */
    outb(0x20, 0x20);
    outb(0xA0, 0x20);

    serial_puts("[IDT] 8259 PIC remapped: IRQ 0-7 → vec 0x70, IRQ 8-15 → vec 0x78\n");
}

/* ── APIC init ───────────────────────────────────────────────── */

static void apic_init(void)
{
    /* Read APIC base address from MSR */
    uint64_t apic_msr = rdmsr(APIC_BASE_MSR);
    uint64_t apic_phys = apic_msr & 0xFFFFFFFFF000ULL;

    /* Ensure APIC is enabled in MSR */
    if (!(apic_msr & APIC_BASE_ENABLE)) {
        wrmsr(APIC_BASE_MSR, apic_msr | APIC_BASE_ENABLE);
    }

    apic_base = (volatile uint32_t *)apic_phys;

    serial_puts("[IDT] APIC base: 0x");
    serial_puthex(apic_phys, 16);
    serial_puts("\n");

    /* Enable APIC via SVR (spurious vector = 0xFF) */
    apic_write(APIC_SVR, APIC_SVR_ENABLE | 0xFF);

    /* Clear ESR */
    apic_write(APIC_ESR, 0);

    /* Setup timer: periodic, vector 32, divide by 16 */
    apic_write(APIC_TIMER_DIV, 0x03);  /* divide by 16 */
    apic_write(APIC_LVT_TIMER, APIC_TIMER_PERIODIC | 32);

    /* Set initial count — calibrate roughly:
     * APIC timer frequency = bus_freq / divider
     * We want ~100 Hz. On most systems bus freq ~100-200 MHz.
     * With div=16: timer_freq ~6-12 MHz. For 100 Hz: count ~60000-120000.
     * Start with 100000 — will be refined with PIT calibration later. */
    apic_write(APIC_TIMER_INIT, 100000);

    apic_enabled = true;

    serial_puts("[IDT] APIC timer: periodic, vector 32, ~100 Hz\n");
}

/* ── IDT init (public API) ───────────────────────────────────── */

void idt_init(void)
{
    serial_puts("[IDT] Setting up interrupt descriptor table...\n");

    /* Disable interrupts during IDT setup */
    __asm__ volatile ("cli");

    /* Detect current CS selector from GDT */
    uint16_t cs = get_cs();
    serial_puts("[IDT] Current CS: 0x");
    serial_puthex(cs, 4);
    serial_puts("\n");

    /* Clear IDT */
    memset(idt, 0, sizeof(idt));

    /* ISR stub table — vectors 0-32 have individual stubs */
    void (*stubs[])(void) = {
        isr_stub_0,  isr_stub_1,  isr_stub_2,  isr_stub_3,
        isr_stub_4,  isr_stub_5,  isr_stub_6,  isr_stub_7,
        isr_stub_8,  isr_stub_9,  isr_stub_10, isr_stub_11,
        isr_stub_12, isr_stub_13, isr_stub_14, isr_stub_15,
        isr_stub_16, isr_stub_17, isr_stub_18, isr_stub_19,
        isr_stub_20, isr_stub_21, isr_stub_22, isr_stub_23,
        isr_stub_24, isr_stub_25, isr_stub_26, isr_stub_27,
        isr_stub_28, isr_stub_29, isr_stub_30, isr_stub_31,
        isr_stub_32,
        isr_stub_33
    };

    /* Set exception + timer + keyboard entries with actual CS */
    for (int i = 0; i <= 33; i++) {
        idt_set_entry(i, stubs[i], 0);
        idt[i].selector = cs;
    }

    /* Vectors 34-255: default stub (just IRET) */
    for (int i = 34; i < 256; i++) {
        idt_set_entry(i, isr_stub_default, 0);
        idt[i].selector = cs;
    }

    /* Override: vector 0x71 = keyboard IRQ (uses isr_stub_33) */
    idt_set_entry(0x71, isr_stub_33, 0);
    idt[0x71].selector = cs;

    /* Load IDT */
    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint64_t)&idt[0];

    __asm__ volatile ("lidt %0" : : "m"(idtr));

    serial_puts("[IDT] Loaded IDT (256 entries) at 0x");
    serial_puthex(idtr.base, 16);
    serial_puts("\n");

    /* Initialize 8259 PIC (remap IRQs to vectors 32+) */
    pic_init();

    /* Initialize APIC and enable timer */
    apic_init();

    /* Enable interrupts */
    __asm__ volatile ("sti");

    serial_puts("[IDT] Interrupts enabled\n");
    fb_puts(" IDT + APIC timer ready\n");
}
