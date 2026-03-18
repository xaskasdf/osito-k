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

/* X-SCHED: scheduler tick (process.c) */
extern void sched_tick(void *frame);

/* Paging (paging.c) */
extern int  paging_set_flags(uint64_t virt, uint64_t flags);
#define PTE_PRESENT  (1ULL << 0)
#define PTE_WRITABLE (1ULL << 1)
#define PTE_GLOBAL   (1ULL << 8)

/* NULL page write-through: page 0 is read-only+NX. When compat32 code
 * writes to NULL (e.g., FString copies), we temporarily make it writable,
 * set TF (single-step), let the write execute, then in #DB re-protect
 * and re-zero the page. This prevents corruption that turns NULL reads
 * from 0 into garbage values like 1. */
volatile int g_null_page_dirty = 0;
#define PTE_NX (1ULL << 63)

/* Process management (process.c) */
extern void proc_exit(int32_t code);
extern int  proc_exception_kill(int32_t code);
extern uint32_t proc_current_pid(void);

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

idt_entry_t idt[256] __attribute__((aligned(16)));
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

/* ── GDT relocation ─────────────────────────────────────────── */
/*
 * UEFI's GDT lives in boot services memory, which gets freed by
 * mem_init(). Once the page allocator reuses those pages, the GDT
 * entries are overwritten.  SYSCALL doesn't consult the GDT (uses
 * STAR MSR), so syscalls keep working — but iretq in ISR stubs
 * DOES reload CS/SS from the GDT on every interrupt return.
 * A corrupted GDT entry makes iretq jump to garbage.
 *
 * Fix: copy the GDT to a static BSS buffer and LGDT it before
 * any allocations can corrupt the original.
 */

#define GDT_MAX_ENTRIES 32
uint64_t kernel_gdt[GDT_MAX_ENTRIES] __attribute__((aligned(16)));

struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} kernel_gdtr;

static void gdt_init(void)
{
    /* Read current GDTR (UEFI's GDT) */
    struct __attribute__((packed)) {
        uint16_t limit;
        uint64_t base;
    } old_gdtr;

    __asm__ volatile ("sgdt %0" : "=m"(old_gdtr));

    serial_puts("[GDT] UEFI GDT at 0x");
    serial_puthex(old_gdtr.base, 16);
    serial_puts(", limit=");
    serial_putdec(old_gdtr.limit + 1);
    serial_puts(" bytes\n");

    uint64_t gdt_bytes = (uint64_t)(old_gdtr.limit) + 1;
    int entries = (int)(gdt_bytes / 8);
    if (entries > GDT_MAX_ENTRIES) entries = GDT_MAX_ENTRIES;

    /* Copy existing GDT to our static buffer */
    memcpy(kernel_gdt, (void *)old_gdtr.base, (uint64_t)entries * 8);

    /* Ensure critical entries are valid 64-bit segments.
     * SYSCALL uses CS=0x28 (index 5), SS=0x30 (index 6).
     * IDT gates use CS=0x38 (index 7). */
    if (entries < 8) entries = 8;  /* Extend if UEFI GDT was smaller */
    kernel_gdt[5] = 0x00AF9A000000FFFFULL; /* 0x28: 64-bit code (P=1,DPL=0,S=1,type=0xA,L=1,G=1) */
    kernel_gdt[6] = 0x00CF92000000FFFFULL; /* 0x30: 64-bit data (P=1,DPL=0,S=1,type=0x2,G=1) */
    kernel_gdt[7] = 0x00AF9A000000FFFFULL; /* 0x38: 64-bit code (same as 0x28) */

    /* Load our GDT */
    kernel_gdtr.limit = (uint16_t)((uint64_t)entries * 8 - 1);
    kernel_gdtr.base  = (uint64_t)kernel_gdt;

    __asm__ volatile ("lgdt %0" : : "m"(kernel_gdtr));

    serial_puts("[GDT] Relocated to static buffer at 0x");
    serial_puthex((uint64_t)kernel_gdt, 16);
    serial_puts(" (");
    serial_putdec((uint64_t)entries);
    serial_puts(" entries)\n");
}

/* ── TSS (Task State Segment) for IST ────────────────────────── */

/*
 * x86-64 TSS: 104 bytes minimum. We only need IST entries for
 * dedicated interrupt stacks. IST1 is used by INT 0x2E (compat32).
 */
struct __attribute__((packed)) tss64 {
    uint32_t reserved0;
    uint64_t rsp0;      /* Ring 0 stack (unused — we're already ring 0) */
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;      /* IST1: INT 0x2E (compat32 dispatch) */
    uint64_t ist2;      /* IST2: available for future use */
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
};

struct tss64 kernel_tss __attribute__((aligned(16)));
/* Exported for int2e_stub.S to update IST1 for re-entrant interrupts */
uint64_t *tss_ist1_ptr;  /* = &kernel_tss.ist1, set in tss_init() */

/* IST1 stack for INT 0x2E — 64KB (needs room for re-entrant callbacks) */
#define IST1_STACK_SIZE 65536
static uint8_t ist1_stack[IST1_STACK_SIZE] __attribute__((aligned(16)));

/* IST2 stack for #DB — 8KB (separate from INT 0x2E to avoid conflicts) */
#define IST2_STACK_SIZE 8192
static uint8_t ist2_stack[IST2_STACK_SIZE] __attribute__((aligned(16)));

/*
 * Install TSS: write descriptor to GDT index 10-11 (selector 0x50),
 * configure IST1, and load TR.
 *
 * TSS descriptor in 64-bit mode occupies 16 bytes (2 GDT entries):
 *   Entry N:   [limit 15:0] [base 15:0] [base 23:16] [type=0x9,P=1] [limit 19:16] [base 31:24]
 *   Entry N+1: [base 63:32] [reserved]
 */
static void tss_init(void)
{
    /* Zero TSS, set IST1 to top of dedicated stack */
    memset(&kernel_tss, 0, sizeof(kernel_tss));
    kernel_tss.ist1 = (uint64_t)(ist1_stack + IST1_STACK_SIZE);
    kernel_tss.ist2 = (uint64_t)(ist2_stack + IST2_STACK_SIZE);
    kernel_tss.iopb_offset = sizeof(struct tss64);
    tss_ist1_ptr = &kernel_tss.ist1;

    /* Build TSS descriptor at GDT index 10 (selector 0x50) */
    uint64_t base = (uint64_t)&kernel_tss;
    uint32_t limit = sizeof(struct tss64) - 1;

    /*
     * GDT entry (low qword):
     *   bits  0-15: limit[15:0]
     *   bits 16-31: base[15:0]
     *   bits 32-39: base[23:16]
     *   bits 40-43: type (0x9 = 64-bit TSS available)
     *   bit  44:    S=0 (system segment)
     *   bits 45-46: DPL=0
     *   bit  47:    P=1 (present)
     *   bits 48-51: limit[19:16]
     *   bits 52-55: flags (G=0, AVL=0)
     *   bits 56-63: base[31:24]
     */
    uint64_t lo = 0;
    lo |= (uint64_t)(limit & 0xFFFF);                    /* limit[15:0] */
    lo |= (uint64_t)(base & 0xFFFF) << 16;               /* base[15:0] */
    lo |= (uint64_t)((base >> 16) & 0xFF) << 32;         /* base[23:16] */
    lo |= (uint64_t)0x89ULL << 40;                        /* type=0x9, P=1 */
    lo |= (uint64_t)((limit >> 16) & 0xF) << 48;         /* limit[19:16] */
    lo |= (uint64_t)((base >> 24) & 0xFF) << 56;         /* base[31:24] */

    /* High qword: base[63:32] */
    uint64_t hi = (base >> 32) & 0xFFFFFFFF;

    kernel_gdt[10] = lo;
    kernel_gdt[11] = hi;

    /* Update GDT limit to include TSS descriptor (index 10-11 = 12 entries min) */
    int current_entries = (kernel_gdtr.limit + 1) / 8;
    if (current_entries < 12) {
        kernel_gdtr.limit = 12 * 8 - 1;
        __asm__ volatile ("lgdt %0" : : "m"(kernel_gdtr));
    }

    /* Load Task Register */
    uint16_t tss_sel = 10 * 8;  /* 0x50 */
    __asm__ volatile ("ltr %0" : : "r"(tss_sel));

    serial_puts("[TSS] Installed at GDT 0x50, IST1=0x");
    serial_puthex(kernel_tss.ist1, 16);
    serial_puts("\n");
}

/* ── State ───────────────────────────────────────────────────── */

static volatile uint64_t tick_count;
static bool apic_enabled;

uint64_t idt_get_ticks(void) { return tick_count; }

/*
 * Arm a hardware write watchpoint on a 4-byte address.
 * Uses debug register DR0. #DB exception fires on every write.
 */
void idt_watch_write4(void *addr)
{
    uint64_t a = (uint64_t)addr;
    __asm__ volatile ("mov %0, %%dr0" : : "r"(a));
    __asm__ volatile ("mov %0, %%dr6" : : "r"((uint64_t)0));
    /* DR7: L0=1, RW0=01 (write), LEN0=11 (4 bytes) */
    uint64_t dr7 = (1ULL << 0) | (1ULL << 16) | (3ULL << 18);
    __asm__ volatile ("mov %0, %%dr7" : : "r"(dr7));
    serial_puts("[IDT] Watchpoint on 0x");
    serial_puthex(a, 16);
    serial_puts("\n");
}

void idt_break_exec(uint64_t addr)
{
    __asm__ volatile ("mov %0, %%dr0" : : "r"(addr));
    __asm__ volatile ("mov %0, %%dr6" : : "r"((uint64_t)0));
    /* DR7: L0=1, RW0=00 (exec), LEN0=00 (1 byte) */
    __asm__ volatile ("mov %0, %%dr7" : : "r"((uint64_t)(1ULL << 0)));
    serial_puts("[IDT] Exec BP at 0x");
    serial_puthex(addr, 8);
    serial_puts("\n");
}

volatile uint32_t *idt_get_apic_base(void) { return apic_base; }

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

/* Software breakpoint support */
uint8_t  g_swbreak_saved = 0;
uint32_t g_swbreak_addr = 0;

void isr_handler(interrupt_frame_t *frame)
{
    uint64_t vec = frame->vector;

    /* Log ALL compat32 exceptions (not timer/keyboard) for debugging */
    if (vec < 32 && (frame->cs & 0xFFFF) == 0x40 && vec != 1 && vec != 3) {
        static int exc_count = 0;
        exc_count++;
        if (exc_count <= 20) {
            serial_puts("[EXC32] vec=");
            serial_putdec(vec);
            serial_puts(" RIP=0x");
            serial_puthex(frame->rip, 8);
            serial_puts(" err=0x");
            serial_puthex(frame->error_code, 4);
            if (vec == 14) {
                uint64_t cr2;
                __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
                serial_puts(" CR2=0x");
                serial_puthex(cr2, 8);
            }
            serial_puts(" #");
            serial_putdec(exc_count);
            serial_puts("\n");
        }
    }

    /* #BP (INT3) — software breakpoint for tracing PE32 execution */
    if (vec == 3 && g_swbreak_addr && (uint32_t)frame->rip == g_swbreak_addr + 1) {
        serial_puts("[SWBREAK] Hit at 0x");
        serial_puthex(g_swbreak_addr, 8);
        /* Dump context based on breakpoint location */
        if (g_swbreak_addr == 0x10902A40) {
            /* FMallocWindows::Free — log ptr and caller */
            uint32_t *sp = (uint32_t *)(uintptr_t)(frame->rsp & 0xFFFFFFFF);
            serial_puts("\n[FREE] ptr=0x");
            serial_puthex(sp[1], 8);  /* [ESP+4] after PUSH EBP = Data */
            serial_puts(" ret=0x");
            serial_puthex(sp[0], 8);  /* return address */
            serial_puts(" ECX=0x");
            serial_puthex((uint32_t)frame->rcx, 8);
            serial_puts("\n");
        } else if (g_swbreak_addr == 0x10902750) {
            /* FMallocWindows::Realloc entry. INT3 is permanent — don't restore.
             * Log args, restore byte, skip forward, then repatch. */
            static int realloc_count = 0;
            realloc_count++;
            /* thiscall: ECX=this, [ESP+0]=retaddr, [ESP+4]=Data, [ESP+8]=Size, [ESP+12]=Name */
            uint32_t *sp = (uint32_t *)(uintptr_t)(frame->rsp & 0xFFFFFFFF);
            uint32_t data = sp[1], sz = sp[2];
            if (realloc_count <= 5 || sz == 0 || (realloc_count % 500) == 0) {
                serial_puts("[RA#");
                serial_putdec(realloc_count);
                serial_puts("] D=0x");
                serial_puthex(data, 8);
                serial_puts(" S=0x");
                serial_puthex(sz, 8);
                serial_puts(" ret=0x");
                serial_puthex(sp[0], 8);
                serial_puts("\n");
            }
            /* Restore byte, execute, then we lose the BP (one-shot per-call chain).
             * To make it permanent, we'd need TF which doesn't work.
             * Instead: restore, set addr=0 so iretq goes to real function. */
            *(uint8_t *)(uintptr_t)g_swbreak_addr = g_swbreak_saved;
            frame->rip = g_swbreak_addr;
            /* DON'T clear g_swbreak_addr — the #BP won't fire again since
             * the byte is restored. But after Realloc returns, WinMain's next
             * Realloc call won't hit either. This is effectively one-shot. */
            g_swbreak_addr = 0;  /* prevent future match */
        } else if (g_swbreak_addr == 0x10909E92) {
            /* WinMain catch(...) handler — dump GErrorHist */
            serial_puts("\n[CATCH] WinMain catch(...) handler hit!\n");
            /* GErrorHist is at IAT 0x10958C60 → points to WCHAR[] buffer */
            volatile uint32_t *iat = (volatile uint32_t *)(uintptr_t)0x10958C60;
            uint32_t hist_ptr = *iat;
            if (hist_ptr > 0x10000 && hist_ptr < 0x7FFFFFFF) {
                const uint16_t *ws = (const uint16_t *)(uintptr_t)hist_ptr;
                serial_puts("[CATCH] GErrorHist: \"");
                for (int k = 0; k < 500 && ws[k]; k++) {
                    char ch = (char)(ws[k] & 0x7F);
                    serial_puts((const char[]){ch, 0});
                }
                serial_puts("\"\n");
            } else {
                serial_puts("[CATCH] GErrorHist ptr=0x");
                serial_puthex(hist_ptr, 8);
                serial_puts(" (invalid)\n");
            }
        } else if (g_swbreak_addr == 0x10902A6C) {
            /* FMallocWindows::Free — log ptr being freed */
            uint32_t *sp = (uint32_t *)(uintptr_t)(frame->rsp & 0xFFFFFFFF);
            /* thiscall: ECX=this, [ESP+4]=ptr (after the PUSH EBP; MOV EBP,ESP; ...; prologue
             * but we're at 0x10902A6C which is AFTER prologue, so [EBP+8]=ptr) */
            static int free_log = 0;
            free_log++;
            if (free_log <= 5 || free_log == 100 || free_log == 1000) {
                uint32_t ebp_val = (uint32_t)frame->rbp;
                uint32_t ptr = *(uint32_t *)(uintptr_t)(ebp_val + 8);
                serial_puts(" Free(0x");
                serial_puthex(ptr, 8);
                serial_puts(") #");
                serial_putdec(free_log);
            }
            serial_puts("\n");
        } else if (g_swbreak_addr == 0x10915038) {
            /* __except handler: dump EBP chain to find what threw */
            uint32_t ebp = (uint32_t)frame->rbp;
            serial_puts("\n[EXCEPT] __except handler fired!");
            serial_puts(" EBP=0x");
            serial_puthex(ebp, 8);
            serial_puts(" ESP=0x");
            serial_puthex(frame->rsp, 8);
            /* Read [ebp-0x30] which has the exception info */
            if (ebp > 0x10000) {
                uint32_t *ebpp = (uint32_t *)(uintptr_t)ebp;
                serial_puts("\n  [ebp-0x30]=0x");
                serial_puthex(*(uint32_t *)(uintptr_t)(ebp - 0x30), 8);
                serial_puts(" [ebp-0x2C]=0x");
                serial_puthex(*(uint32_t *)(uintptr_t)(ebp - 0x2C), 8);
                serial_puts("\n  caller=[ebp+4]=0x");
                serial_puthex(ebpp[1], 8);
            }
            serial_puts("\n");
        } else if (g_swbreak_addr == 0x10102CA2) {
            uint32_t *sp = (uint32_t *)(uintptr_t)(frame->rsp & 0xFFFFFFFF);
            uint32_t ret = sp[0], expr = sp[1], file = sp[2], line = sp[3];
            serial_puts("\n[ASSERT] ");
            if (expr) { const char *s = (const char *)(uintptr_t)expr; serial_puts(s); }
            serial_puts(" @ ");
            if (file) { const char *s = (const char *)(uintptr_t)file; serial_puts(s); }
            serial_puts(":");
            serial_putdec(line);
            serial_puts("\n");
        } else {
            serial_puts(" ESP=0x");
            serial_puthex(frame->rsp, 8);
            serial_puts("\n");
        }
        /* One-shot: restore and continue */
        *(uint8_t *)(uintptr_t)g_swbreak_addr = g_swbreak_saved;
        frame->rip = g_swbreak_addr;
        g_swbreak_addr = 0;
        return;
    }

    /* Demand paging — handle #PF for high addresses FIRST, before any output.
     * This must be the earliest possible check to avoid stack corruption. */
    if (vec == 14 && !(frame->error_code & 1)) {
        uint64_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        if (cr2 >= 0x100000000ULL) {
            extern int demand_page_fault(uint64_t addr, uint64_t error_code);
            if (demand_page_fault(cr2, frame->error_code) == 0)
                return;
        }
    }

    /* APIC timer tick */
    if (vec == 32) {
        tick_count++;

        /* Watchdog: log PE32 execution every ~5s */
        {
            static int compat32_ticks = 0;
            uint16_t cs = (uint16_t)(frame->cs & 0xFFFF);

            if (cs == 0x40) {
                compat32_ticks++;
                if ((compat32_ticks % 100) == 1) {  /* every ~1s */
                    serial_puts("[TIMER] PE32 RIP=0x");
                    serial_puthex(frame->rip, 8);
                    serial_puts(" ECX=0x");
                    serial_puthex(frame->rcx, 8);
                    serial_puts(" EBP=0x");
                    serial_puthex(frame->rbp, 8);
                    serial_puts(" t=");
                    serial_putdec(compat32_ticks);
                    if (compat32_ticks <= 301) {
                        /* Read GErrorHist to capture engine error message */
                        volatile uint32_t *iat_hist = (volatile uint32_t *)(uintptr_t)0x10958C60;
                        uint32_t hist_addr = *iat_hist;
                        if (hist_addr > 0x10000 && hist_addr < 0x7FFFFFFF) {
                            const uint16_t *ws = (const uint16_t *)(uintptr_t)hist_addr;
                            if (ws[0] != 0) {
                                serial_puts("\n  GErrorHist=\"");
                                for (int k = 0; k < 200 && ws[k]; k++)
                                    serial_puts((const char[]){(char)(ws[k] & 0x7F), 0});
                                serial_puts("\"");
                            }
                        }
                        /* Deep stack walk for stuck analysis */
                        if (compat32_ticks >= 201 && compat32_ticks <= 301) {
                            uint32_t ebp = (uint32_t)frame->rbp;
                            serial_puts("\n  STACK:");
                            for (int depth = 0; depth < 8 && ebp > 0x10000 && ebp < 0x7FFFFFFF; depth++) {
                                uint32_t *fp = (uint32_t *)(uintptr_t)ebp;
                                serial_puts(" 0x");
                                serial_puthex(fp[1], 8);  /* return address */
                                ebp = fp[0];  /* next frame */
                            }
                        }
                        /* Check FArray::Empty IAT + JMP thunk bytes */
                        volatile uint32_t *iat_empty = (volatile uint32_t *)(uintptr_t)0x10958B98;
                        serial_puts(" Empty=0x");
                        serial_puthex(*iat_empty, 8);
                        /* Read first 5 bytes of the JMP thunk at 0x10102865 */
                        if (compat32_ticks <= 201) {
                            volatile uint8_t *thunk = (volatile uint8_t *)(uintptr_t)0x10102865;
                            serial_puts(" thunk:");
                            for (int tb = 0; tb < 5; tb++)
                                serial_puthex(thunk[tb], 2);
                            /* Read EXE code at 0x109090B0 (the call instruction) */
                            volatile uint8_t *callsite = (volatile uint8_t *)(uintptr_t)0x109090B0;
                            serial_puts(" call@90B0:");
                            for (int tb = 0; tb < 12; tb++)
                                serial_puthex(callsite[tb], 2);
                        }
                        /* Stack walk */
                        uint32_t ebp = (uint32_t)frame->rbp;
                        uint32_t *stk = (uint32_t *)(uintptr_t)ebp;
                        serial_puts(" [EBP+4]=0x");
                        serial_puthex(stk[1], 8);
                        if (stk[0] > 0x10000 && stk[0] < 0x7FFFFFFF) {
                            uint32_t *prev = (uint32_t *)(uintptr_t)stk[0];
                            serial_puts(" caller=0x");
                            serial_puthex(prev[1], 8);
                            if (prev[0] > 0x10000 && prev[0] < 0x7FFFFFFF) {
                                uint32_t *prev2 = (uint32_t *)(uintptr_t)prev[0];
                                serial_puts(" caller2=0x");
                                serial_puthex(prev2[1], 8);
                            }
                        }
                    }
                    serial_puts("\n");
                }
            }
        }

        /* X-SCHED: preemptive scheduler — check quantum, switch if expired.
         * frame points to saved GPRs on the current process's stack. */
        sched_tick(frame);
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

    /* #DB Debug exception — hardware watchpoint handler */
    if (vec == 1) {
        static int db_hit_count = 0;
        uint64_t dr6;
        __asm__ volatile ("mov %%dr6, %0" : "=r"(dr6));

        if (dr6 & 0x1) {  /* B0: breakpoint 0 hit */
            /* Read the watched address value (g_teb32.ExceptionList) */
            uint64_t dr0;
            __asm__ volatile ("mov %%dr0, %0" : "=r"(dr0));
            uint32_t val = *(volatile uint32_t *)dr0;

            /* Only log when value is suspiciously low (< 0x1000, not 0xFFFFFFFF) */
            if (val < 0x1000 && val != 0xFFFFFFFF) {
                serial_puts("[WP] FS:[0]=0x");
                serial_puthex(val, 8);
                serial_puts(" RIP=0x");
                serial_puthex(frame->rip, 8);
                serial_puts(" ESP=0x");
                serial_puthex(frame->rsp & 0xFFFFFFFF, 8);

                /* Dump instruction bytes at RIP for first 5 hits */
                if (db_hit_count < 5) {
                    serial_puts(" insn:");
                    uint8_t *ip = (uint8_t *)(uint64_t)frame->rip;
                    /* Back up 6 bytes to catch prefix+opcode before the write */
                    for (int i = -6; i < 8; i++) {
                        if (i == 0) serial_puts(" [");
                        serial_puthex(ip[i], 2);
                        if (i == 0) serial_puts("]");
                        else serial_puts(" ");
                    }

                    /* Also show the stack around ESP to see saved SEH frame */
                    serial_puts("\n  stack:");
                    uint32_t *sp = (uint32_t *)(uint64_t)(frame->rsp & 0xFFFFFFFF);
                    for (int i = 0; i < 8; i++) {
                        serial_puts(" ");
                        serial_puthex(sp[i], 8);
                    }
                }
                db_hit_count++;
                serial_puts("\n");
            }

            /* Clear DR6 status bits */
            __asm__ volatile ("mov %0, %%dr6" : : "r"((uint64_t)0));
            return;  /* non-fatal: resume execution */
        }

        /* TF single-step after NULL page write: re-protect + re-zero page 0 */
        if (g_null_page_dirty) {
            g_null_page_dirty = 0;
            frame->rflags &= ~(1ULL << 8);  /* clear TF */
            /* Re-zero the page and re-protect as read-only+NX */
            memset((void *)0, 0, 4096);
            paging_set_flags(0, PTE_PRESENT | PTE_GLOBAL | PTE_NX);
            __asm__ volatile ("invlpg (%0)" :: "r"((uint64_t)0) : "memory");
            __asm__ volatile ("mov %0, %%dr6" : : "r"((uint64_t)0));
            return;
        }

        /* Other debug reasons (single-step etc.) — clear and resume */
        __asm__ volatile ("mov %0, %%dr6" : : "r"((uint64_t)0));
        return;
    }

    /* #PF on NULL page: page 0 is read-only+NX.
     * - WRITE fault: temporarily make writable, set TF, let it write, re-protect in #DB
     * - INSTRUCTION-FETCH: NULL function pointer call — log and crash */
    if (vec == 14) {
        uint64_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

        /* WRITE fault on page 0: allow it via TF single-step.
         * For compat32 (CS=0x40): skip TF — #DB delivery from compat mode
         * causes #GP(0x0A) because the 64-bit exception frame can't be
         * pushed on the 32-bit stack without IST. Just leave page writable. */
        if (cr2 < 0x1000 && (frame->error_code & 2) && !(frame->error_code & 16)) {
            paging_set_flags(0, PTE_PRESENT | PTE_WRITABLE | PTE_GLOBAL | PTE_NX);
            __asm__ volatile ("invlpg (%0)" :: "r"((uint64_t)0) : "memory");
            if ((frame->cs & 0xFFFF) != 0x40) {
                /* 64-bit mode: use TF single-step to re-protect after write */
                frame->rflags |= (1ULL << 8);  /* TF bit */
            }
            g_null_page_dirty = 1;
            return;
        }

        if (cr2 < 0x1000 && (frame->error_code & 16)) {  /* INSTRUCTION-FETCH on page 0 */
            /* NULL function pointer call in compat32: simulate RET 0.
             * Pop the return address from the 32-bit stack and set RIP to it.
             * Set EAX=0 (return value 0). This makes NULL calls safe. */
            static int null_call_count = 0;
            null_call_count++;
            if (null_call_count <= 10) {
                serial_puts("[NULL-CALL] addr=0x");
                serial_puthex(cr2, 4);
                serial_puts(" ESP=0x");
                serial_puthex(frame->rsp, 8);
                if (frame->cs == 0x40 || frame->cs == 0x23) {
                    uint32_t *sp32 = (uint32_t *)(frame->rsp & 0xFFFFFFFF);
                    serial_puts(" stack:");
                    for (int si = 0; si < 16; si++) {
                        if (si % 4 == 0) {
                            serial_puts("\n  [+"); serial_putdec(si*4);
                            serial_puts("] ");
                        }
                        serial_puthex(sp32[si], 8);
                        serial_puts(" ");
                    }
                    serial_puts("\n  EAX=0x"); serial_puthex(frame->rax, 8);
                    serial_puts(" EBX=0x"); serial_puthex(frame->rbx, 8);
                    serial_puts(" ECX=0x"); serial_puthex(frame->rcx, 8);
                    serial_puts(" EDX=0x"); serial_puthex(frame->rdx, 8);
                    serial_puts("\n  ESI=0x"); serial_puthex(frame->rsi, 8);
                    serial_puts(" EDI=0x"); serial_puthex(frame->rdi, 8);
                    serial_puts(" EBP=0x"); serial_puthex(frame->rbp, 8);
                }
                serial_puts(" #");
                serial_putdec(null_call_count);
                serial_puts("\n");
            }
            if (frame->cs == 0x40 || frame->cs == 0x23) {
                /* compat32: pop return address, set EAX=0 */
                uint32_t *sp32 = (uint32_t *)(frame->rsp & 0xFFFFFFFF);
                frame->rip = sp32[0];  /* return address */
                frame->rsp += 4;       /* pop */
                frame->rax = 0;        /* return 0 */
                return;
            }
        }
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

        /* Dump bytes at RIP (useful for crashes on stack/corrupted code) */
        if (frame->rip < 0x100000000ULL) {
            uint8_t *code = (uint8_t *)(frame->rip & 0xFFFFFFFF);
            serial_puts("  Code @ RIP: ");
            for (int bi = 0; bi < 16; bi++) {
                serial_puthex(code[bi], 2);
                serial_puts(" ");
            }
            serial_puts("\n");
        }

        /* Page fault: demand paging for mmap'd regions */
        if (vec == 14) {
            uint64_t cr2;
            __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

            /* Try demand paging: allocate page on fault within VMA */
            {
                extern int demand_page_fault(uint64_t addr, uint64_t error_code);
                if (demand_page_fault(cr2, frame->error_code) == 0)
                    return;  /* Page mapped — resume execution */
            }

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

        /* Stack dump: show 16 dwords from RSP for crash diagnosis */
        {
            uint32_t *sp = (uint32_t *)(uint64_t)frame->rsp;
            serial_puts("  Stack dump (RSP):\n");
            for (int i = 0; i < 16; i++) {
                serial_puts("    [RSP+");
                serial_puthex((uint64_t)(i * 4), 2);
                serial_puts("] = 0x");
                serial_puthex((uint64_t)sp[i], 8);
                serial_puts("\n");
            }
        }

        /* Compat mode detection: CS == 0x40 means 32-bit PE code */
        int is_compat_mode = ((frame->cs & 0xFFFF) == 0x0040);
        if (is_compat_mode) {
            serial_puts("  [COMPAT32] 32-bit code — exception in compat mode\n");
            /* Walk EBP chain to reconstruct call stack (32-bit frames) */
            uint32_t ebp = (uint32_t)frame->rbp;
            serial_puts("  Call stack (EBP chain):\n");
            for (int depth = 0; depth < 16 && ebp >= 0x10000 && ebp < 0x30000000; depth++) {
                uint32_t *fp = (uint32_t *)(uint64_t)ebp;
                uint32_t ret_addr = fp[1];
                uint32_t prev_ebp = fp[0];
                serial_puts("    [");
                serial_putdec((uint64_t)depth);
                serial_puts("] EBP=0x");
                serial_puthex(ebp, 8);
                serial_puts(" RET=0x");
                serial_puthex(ret_addr, 8);
                serial_puts("\n");
                if (prev_ebp <= ebp) break;  /* prevent infinite loops */
                ebp = prev_ebp;
            }
        }

        /* Framebuffer output */
        fb_puts_color("\n !!! ", 0x00FF0000);
        fb_puts_color(exception_names[vec], 0x00FF0000);
        fb_puts(" at 0x");
        fb_puthex(frame->rip, 16);
        fb_puts("\n");

        /* Fault recovery: if dl_fault_jmpbuf is set, longjmp back instead
         * of killing. Used by INIT_ARRAY to survive unresolved calls. */
        {
            extern uint64_t *dl_fault_jmpbuf;
            extern void kern_longjmp(uint64_t *buf, int val);
            if (dl_fault_jmpbuf) {
                serial_puts("  [DL] Recovering from fault in constructor\n");
                uint64_t *jmp = dl_fault_jmpbuf;
                dl_fault_jmpbuf = NULL;
                kern_longjmp(jmp, 1);
            }
        }

        /* Compat32 crash recovery: if a Win32 app crashes, longjmp back
         * to the shell instead of halting. winexec sets the jmpbuf. */
        {
            extern uint64_t *compat32_crash_jmpbuf;
            extern void kern_longjmp(uint64_t *buf, int val);
            if (compat32_crash_jmpbuf) {
                /*
                 * Null vtable recovery: if the crash is from calling through
                 * a null vtable pointer (RIP in low memory from [0x00000000]),
                 * skip the call and continue PE execution. This happens when
                 * an Unreal object was allocated but its constructor didn't
                 * set the vtable (vtable=0 → [0]=garbage → #UD).
                 *
                 * Recovery: pop the return address from the 32-bit stack,
                 * set EAX=0 (return 0 from the virtual call), and resume.
                 */
                /* Detect crash from null/corrupt vtable: RIP below PE load area.
                 * Limit to 3 recoveries — beyond that, corruption is too deep. */
                static int null_vcall_skip_count = 0;
                if ((vec == 6 || vec == 13) &&
                    frame->rip < 0x02000000 &&
                    (frame->cs == 0x40 || frame->cs == 0x23) &&
                    null_vcall_skip_count < 3) {
                    null_vcall_skip_count++;
                    uint32_t obj_addr = (uint32_t)frame->rax;
                    serial_puts("  [WIN32] Null vtable call: obj=0x");
                    serial_puthex(obj_addr, 8);
                    if (obj_addr >= 0x10000 && obj_addr < 0x80000000UL) {
                        uint32_t *obj = (uint32_t *)(uintptr_t)obj_addr;
                        serial_puts(" vtbl=0x");
                        serial_puthex(obj[0], 8);
                    }
                    serial_puts("\n");

                    /* Pop return address from 32-bit stack */
                    uint32_t *esp32 = (uint32_t *)(uintptr_t)(uint32_t)frame->rsp;
                    uint32_t ret_addr = esp32[0];
                    frame->rsp += 4;  /* pop return address */

                    /* Also pop stdcall args if present (the push 1 before call) */
                    /* Don't pop args — the caller pushed them and will clean up */

                    frame->rip = ret_addr;
                    frame->rax = 0;  /* return 0 from the "virtual call" */
                    serial_puts("  [WIN32] Skipping null vcall → resuming at 0x");
                    serial_puthex(ret_addr, 8);
                    serial_puts("\n");
                    return;  /* resume PE execution */
                }
                serial_puts("  [WIN32] Crash recovery — returning to shell\n");
                uint64_t *jmp = compat32_crash_jmpbuf;
                compat32_crash_jmpbuf = NULL;
                kern_longjmp(jmp, 1);
            }
        }

        /* If a user process is running (PID > 1), kill it instead of
         * halting the system. */
        uint32_t pid = proc_current_pid();
        if (pid > 1) {
            /* Guard against cascading exceptions: if we're already
             * killing this PID and another exception fires (e.g. in
             * the kill path itself), just halt the process silently. */
            static volatile uint32_t exception_kill_pid = 0;
            if (exception_kill_pid == pid) {
                serial_puts("  Cascading exception in PID ");
                serial_putdec(pid);
                serial_puts(" — halting\n");
                __asm__ volatile ("sti");
                for (;;) __asm__ volatile ("hlt");
            }
            exception_kill_pid = pid;

            int sig = 11;  /* SIGSEGV default */
            if (vec == 6) sig = 4;   /* SIGILL for #UD */
            if (vec == 13) sig = 11; /* SIGSEGV for #GP */
            if (vec == 14) sig = 11; /* SIGSEGV for #PF */
            if (vec == 8) sig = 6;   /* SIGABRT for #DF */
            serial_puts("  Killing process PID ");
            serial_putdec(pid);
            serial_puts(" with signal ");
            serial_putdec((uint64_t)sig);
            serial_puts("\n");
            fb_puts_color(" Process killed\n", 0x00FF0000);
            proc_exception_kill(128 + sig);
            /* proc_exception_kill never returns */
        }

        /* Kernel exception (PID 0 or 1) — halt the system */
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

    /* Relocate GDT from UEFI memory to static BSS buffer.
     * MUST happen before any page allocations (paging_init, heap_init)
     * that could overwrite the UEFI GDT in freed boot services memory. */
    gdt_init();
    tss_init();

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

    /* Exceptions that can fire from compat32 mode need IST to avoid
     * pushing 64-bit frames on the 32-bit user stack (which causes
     * cascading #GP). Share IST1 with INT 0x2E — these handlers
     * either halt (#UD) or return quickly (#PF null-page, #DB). */
    idt[1].ist  = 2;  /* #DB — IST2 (TF single-step + null-page tracking) */
    idt[3].ist  = 2;  /* #BP — IST2 (avoids IST1 collision with INT 0x2E) */
    idt[6].ist  = 1;  /* #UD — invalid opcode (corrupted function pointer) */
    idt[13].ist = 1;  /* #GP — general protection */
    idt[14].ist = 1;  /* #PF — page fault (null-page write handling) */
    idt[32].ist = 2;  /* APIC timer — IST2 for safe compat32 preemption */

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
