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
#include "../include/paging.h"

/* ── External functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_putchar(char c);
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
extern uint64_t *paging_get_pte(uint64_t virt);
#define PTE_PRESENT  (1ULL << 0)
#define PTE_WRITABLE (1ULL << 1)
#define PTE_GLOBAL   (1ULL << 8)
#define PTE_NX       (1ULL << 63)

/* NULL page write-through: page 0 is read-only+NX. When compat32 code
 * writes to NULL (e.g., FString copies), we temporarily make it writable,
 * set TF (single-step), let the write execute, then in #DB re-protect
 * and re-zero the page. This prevents corruption that turns NULL reads
 * from 0 into garbage values like 1. */
volatile int g_null_page_dirty = 0;
uint32_t g_base_seh_frame_addr = 0;  /* winexec base SEH frame on PE32 stack */

/* Win32/PE compat32 helper: re-zero and re-protect the NULL page.
 *
 * Currently a NO-OP. The original implementation did
 *   memset((void *)0, 0, 4096); paging_set_flags(0, RO|NX); invlpg(0);
 * which crashed zsh during demand-paged ELF startup with #DF (the write
 * to address 0 faulted even though the PTE was marked writable, possibly
 * due to TLB / SMP coherence or stale large-page mapping at vaddr 0).
 *
 * This helper is only meaningful for the Win32/PE compat32 layer, where
 * NULL pointer derefs are tolerated by temporarily marking page 0
 * writable in the #PF handler. For non-PE workloads (zsh, GTA5, anything
 * ELF-loaded), the post-write cleanup is unnecessary — those programs
 * shouldn't be writing to NULL in the first place, and if they do it's a
 * SIGSEGV.
 *
 * TODO: re-introduce the cleanup gated on a "compat32 active" flag set
 * by winexec_main. Until then we just clear the dirty flag so subsequent
 * timer ticks don't loop. */
static void null_page_clean(void)
{
    g_null_page_dirty = 0;
}

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
extern void isr_stub_40(void);   /* I211 NIC MSI */
extern void isr_stub_41(void);   /* RTL8111 NIC MSI */
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
uint64_t *tss_ist2_ptr;  /* = &kernel_tss.ist2, for DOS INT stubs */

/* IST1 stack for INT 0x2E — 256KB.
 * int2e_stub.S reserves 32KB per nest (subq $32768). UT99's C++ EH
 * unwind chain re-throws through 3-4 catches → 3-4 nested INT 0x2E
 * entries → 96-128KB needed. 64KB was overflowing into garbage and
 * corrupting RtlRaiseException's locals → unwind globals stayed set
 * from a prior catch → next int2e_stub iret jumped to stale catch
 * with stale ESP/EBP → user-stack execution → #BR. */
#define IST1_STACK_SIZE 262144
uint8_t ist1_stack[IST1_STACK_SIZE] __attribute__((aligned(16)));

/* IST2 stack for DOS INTs + #DB — 32KB */
#define IST2_STACK_SIZE 32768
uint8_t ist2_stack[IST2_STACK_SIZE] __attribute__((aligned(16)));

/* IST3 stack for #PF / #UD / #GP — 32KB.
 * Dedicated to fault handlers so they DON'T share IST1 with INT 0x2E.
 * Sharing IST1 caused the kernel-#PF inside isr_handler crash: nested
 * INT 0x2E entries lower IST1, leaving a #PF that fires later to load
 * a corrupt RSP from the lowered IST1 → garbage RBP → kernel deref. */
#define IST3_STACK_SIZE 32768
uint8_t ist3_stack[IST3_STACK_SIZE] __attribute__((aligned(16)));

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
    kernel_tss.ist3 = (uint64_t)(ist3_stack + IST3_STACK_SIZE);
    kernel_tss.iopb_offset = sizeof(struct tss64);
    tss_ist1_ptr = &kernel_tss.ist1;
    tss_ist2_ptr = &kernel_tss.ist2;

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
static uint32_t apic_timer_init_saved;

/* TSC-deadline mode state */
static bool     tsc_deadline_mode;
static uint64_t tsc_freq;              /* TSC cycles per second */
static uint64_t tsc_last_tick;         /* TSC value of last virtual 100Hz tick */
#define MSR_IA32_TSC_DEADLINE  0x6E0
#define APIC_TIMER_TSC_DEADLINE  0x40000  /* LVT bits 18:17 = 10b */

static inline uint64_t idt_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

uint64_t idt_get_ticks(void) { return tick_count; }
uint64_t idt_get_tsc_freq(void) { return tsc_freq; }
bool     idt_tsc_deadline_active(void) { return tsc_deadline_mode; }

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

    /* Report fpu_state_ptr corruption (detected by isr_common guard) */
    {
        extern uint64_t fpu_corrupt_val;
        if (fpu_corrupt_val) {
            serial_puts("[ISR] !!! fpu_state_ptr CORRUPT: 0x");
            serial_puthex(fpu_corrupt_val, 16);
            serial_puts(" (vec=");
            serial_putdec(vec);
            serial_puts(")\n");
            /* Reset so we only report once per corruption event */
            extern uint8_t *fpu_state_ptr;
            extern uint8_t fpu_state_kernel[];
            fpu_state_ptr = fpu_state_kernel;  /* restore safe default */
            fpu_corrupt_val = 0;
        }
    }

#ifdef COMPAT_TRACE
    /* Panorama probe: any exception taken while RSP lies inside IST1 (Win32
     * INT 0x2E) or IST2 (DOS native INTs). A hit on a page fault here is the
     * single line that confirms risk #3 (lower-half identity map dropped
     * in commit f8bd01c) is actually biting inside compat dispatch.
     *
     * Also covers DOS-native faults when RSP is OUTSIDE both IST windows —
     * DOOM occasionally faults with its own SS:RSP active, in which case
     * neither IST zone matches but we still want the surgical emulator
     * (and the long-jump recovery) to run. The "_dos_active" guard lets
     * the block enter when a DOS native session is in flight. */
    if (vec < 32) {
        extern uint8_t ist1_stack[];
        extern uint8_t ist2_stack[];
        extern uint64_t *dos_native_exit_jmpbuf;
        uint64_t _sp = frame->rsp;
        uint64_t _i1 = (uint64_t)ist1_stack;
        uint64_t _i2 = (uint64_t)ist2_stack;
        const char *_zone = 0;
        if (_sp >= _i1 && _sp < _i1 + 65536)      _zone = "IST1";
        else if (_sp >= _i2 && _sp < _i2 + 32768) _zone = "IST2";
        /* DOS-active: any LDT-CS fault, OR a fault from one of the
         * DOS4GW GDT aliases (sel 0x18 / 0x20) we install in DOS-native
         * mode. Without the GDT-alias arm the emulator wouldn't run for
         * code that DOS4GW transitioned into via `LJMPW $0x18:$N`. */
        uint16_t _cs16 = (uint16_t)frame->cs;
        int _dos_active = (dos_native_exit_jmpbuf != 0)
                       && ((frame->cs & 0x04) ||
                           _cs16 == 0x18 || _cs16 == 0x20);
        if (!_zone && _dos_active) _zone = "DOS";
        if (_zone) {
            /* Rate-limit: when the same RIP keeps faulting (e.g. a
             * segment-load loop), sample 1-of-1024 to keep the trace
             * readable. Different RIPs always print. */
            static uint64_t _last_rip   = 0xFFFFFFFFFFFFFFFFULL;
            static uint32_t _same_count = 0;
            int _verbose_pf = (frame->rip != _last_rip)
                           || ((++_same_count & 0x3FF) == 0);
            if (frame->rip != _last_rip) {
                _last_rip = frame->rip;
                _same_count = 0;
            }
            if (!_verbose_pf) goto pf_ist_skip_log;
            serial_puts("[pf-ist] on ");
            serial_puts(_zone);
            serial_puts(" vec=");
            serial_putdec(vec);
            serial_puts(" rip=0x");
            serial_puthex(frame->rip, 16);
            if (vec == 14) {
                uint64_t _cr2;
                __asm__ volatile ("mov %%cr2, %0" : "=r"(_cr2));
                serial_puts(" cr2=0x");
                serial_puthex(_cr2, 16);
            }
            serial_puts(" err=0x");
            serial_puthex(frame->error_code, 4);
            serial_puts(" cs=0x"); serial_puthex(frame->cs & 0xFFFF, 4);
            serial_puts("\n");

            /* For DOS native faults with CS = LDT sel, delegate to the
             * DOS layer to dump the instruction bytes AND the caller's
             * stack top so we can see who CALL-FAR'd into the bad RIP. */
            if (frame->cs & 0x04) {
                extern void dos_native_dump_rip(uint16_t cs, uint32_t rip,
                                                uint16_t ss_hint,
                                                uint64_t frame_rsp);
                dos_native_dump_rip((uint16_t)frame->cs,
                                    (uint32_t)frame->rip,
                                    (uint16_t)frame->ss,
                                    frame->rsp);
            }
        pf_ist_skip_log:
            (void)_verbose_pf;

            /* DOS-native #DB (vec=1) recovery: DOOM does
             *   POPF; INT 21h; PUSHF
             * with TF=1 in the popped flags. After the INT handler's
             * IRETQ restores TF, the next instruction triggers a
             * single-step #DB. We don't have a userspace debugger
             * attached to DOOM, so just clear TF in the saved RFLAGS
             * and resume — DOOM keeps running without spurious traps.
             * Also clear DR6 single-step bit so a subsequent debug
             * exception doesn't latch on stale state. */
            if (vec == 1 && (frame->cs & 0x04) /* DOS-native code */) {
                frame->rflags &= ~(uint64_t)0x100;  /* TF off */
                uint64_t dr6 = 0xFFFF0FF0; /* clear B0-B3, BS, BT */
                __asm__ volatile ("mov %0, %%dr6" :: "r"(dr6));
                serial_puts("[DOS-NT] #DB caught at rip=0x");
                serial_puthex(frame->rip, 8);
                serial_puts(" — TF cleared, resuming\n");
                return;
            }

            /* DOS4GW surgical recovery — try LRETW software emulation
             * first (keeps current CS, rewrites RIP to target linear),
             * then fall back to descriptor promote+retry. Accept LDT
             * selectors AND the DOS4GW GDT aliases at sel 0x18 / 0x20. */
            if (vec == 13 && ((frame->cs & 0x04) ||
                              _cs16 == 0x18 || _cs16 == 0x20)) {
                extern int dos_native_emulate_lretw(void *frame);
                if (dos_native_emulate_lretw(frame)) {
                    return;  /* iretq will land at emulated target */
                }
                extern int dos_native_promote_to_code(uint16_t sel);
                if ((frame->error_code & 0x04) &&
                    dos_native_promote_to_code((uint16_t)frame->error_code)) {
                    serial_puts("[pf-ist] promoted sel 0x");
                    serial_puthex(frame->error_code & 0xFFFF, 4);
                    serial_puts(" DATA→CODE, resuming\n");
                    return;  /* retry faulting instruction */
                }
            }

            /* If a DOS native program faulted (LDT-CS, DOS4GW GDT
             * alias, or running on IST2), long-jump back to the shell
             * instead of halting. Restores kernel CR3 + GDTR. */
            if ((frame->cs & 0x04) || _cs16 == 0x18 || _cs16 == 0x20
                || (_zone && _zone[3] == '2')) {
                extern uint64_t *dos_native_exit_jmpbuf;
                extern uint64_t paging_get_kernel_cr3(void);
                extern void kern_longjmp(uint64_t *buf, int val);
                if (dos_native_exit_jmpbuf) {
                    serial_puts("[DOS-NT] crash -> long-jump to shell\n");
                    uint64_t kcr3 = paging_get_kernel_cr3();
                    if (kcr3) __asm__ volatile ("mov %0, %%cr3"
                                                 :: "r"(kcr3) : "memory");
                    kern_longjmp(dos_native_exit_jmpbuf, 2);
                }
            }
        }
    }
#endif

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
        /* STALE-PTR detector: if user-mode code jumped to 0xDEADC0DE
         * (our VirtualFree MEM_RELEASE tombstone pattern) it means a
         * pointer that lived inside a freed VA range was read AFTER
         * the free and used as a code/data pointer.  Logs the calling
         * context so we know who held the stale ref. */
        uint32_t rip32 = (uint32_t)frame->rip;
        if (rip32 == 0xDEADC0DE) {
            static int stale_count = 0;
            stale_count++;
            if (stale_count <= 10) {
                uint32_t *sp = (uint32_t *)(uintptr_t)(frame->rsp & 0xFFFFFFFF);
                serial_puts("[STALE-PTR] call/jmp -> 0xDEADC0DE (freed mem) #");
                serial_putdec(stale_count);
                serial_puts(" retaddr=0x");
                serial_puthex((uint64_t)sp[0], 8);
                serial_puts(" EBP=0x");
                serial_puthex((uint32_t)frame->rbp, 8);
                serial_puts(" ESI=0x");
                serial_puthex((uint32_t)frame->rsi, 8);
                serial_puts("\n");
            }
        } else if (vec == 5) {
            /* #BR Bound Range in compat32: engine jumped into data (the
             * byte at RIP is 0x62 = BOUND opcode, but it's actually a
             * UTF-16 ASCII char or similar data).  This always means
             * deep state corruption — the engine's vtable/fn-ptr was
             * pointing to a data buffer.  Skip the chaotic crash dump
             * and exit the PE process cleanly. */
            extern void proc_exit(int32_t code);
            static int br_count = 0;
            if (++br_count <= 3) {
                serial_puts("[BR] compat32 #BR at RIP=0x");
                serial_puthex(rip32, 8);
                serial_puts(" — proc_exit\n");
            }
            proc_exit(0xC0000026 /* STATUS_INVALID_PARAMETER_5 */);
            /* unreachable */
        } else if (vec == 14) {
            /* Data deref of a tombstoned value: CR2 == 0xDEADC0DE
             * (the engine treated a freed-range word as a pointer). */
            uint64_t cr2;
            __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
            if ((uint32_t)cr2 == 0xDEADC0DE) {
                static int stale_deref = 0;
                stale_deref++;
                if (stale_deref <= 10) {
                    serial_puts("[STALE-PTR] deref of 0xDEADC0DE at RIP=0x");
                    serial_puthex(rip32, 8);
                    serial_puts(" #");
                    serial_putdec(stale_deref);
                    serial_puts("\n");
                }
            }
            /* NX-execute fault in Win32 VA range: engine called through
             * a function pointer that landed in a non-executable heap
             * page.  Dump the IAT slot at Engine.dll +0x2A5E08 = the
             * `UObject::StaticLoadClass` import, which is the value the
             * 0x103887C0 function loads into EBX and calls.  If the IAT
             * slot still points to a valid Core.dll .text addr, EBX got
             * clobbered USER-SIDE between the first and second call —
             * a virtual call through a corrupt vtable picked a non-ABI-
             * compliant callee that didn't preserve EBX. */
            uint32_t err_iexec = (frame->error_code & 0x10);
            if (err_iexec && rip32 >= 0x40000000 && rip32 < 0x80000000) {
                static int iat_dump_count = 0;
                iat_dump_count++;
                if (iat_dump_count <= 3) {
                    volatile uint32_t *iat = (volatile uint32_t *)
                        (uintptr_t)0x105A5E08;
                    serial_puts("[IAT-PROBE] *0x105A5E08=0x");
                    serial_puthex((uint32_t)*iat, 8);
                    serial_puts(" (StaticLoadClass — expected Core.dll .text)");
                    if ((uint32_t)*iat >= 0x10100000 && (uint32_t)*iat < 0x10300000) {
                        serial_puts(" -> IAT OK, EBX clobbered USER-SIDE");
                    } else {
                        serial_puts(" -> IAT IS CORRUPT");
                    }
                    serial_puts("\n");
                }
            }
        }
    }

    /* EARLY check: code execution outside DLL range in compat32 mode.
     * Must run BEFORE crash dump which reads Code@RIP — reading from
     * unmapped addresses (0xFFFF1017) would cause a nested exception. */
    if ((frame->cs & 0xFFFF) == 0x40 &&
        (vec == 6 || vec == 13) &&
        (frame->rip < 0x10000000 || frame->rip >= 0x20000000)) {
        static int bytecode_fix_count = 0;
        bytecode_fix_count++;
        if (bytecode_fix_count <= 10) {
            serial_puts("[UD-FIX] RIP=0x");
            serial_puthex((uint32_t)frame->rip, 8);
            serial_puts(" #");
            serial_putdec(bytecode_fix_count);
            serial_puts("\n");
            /* Full diagnostic for first hit */
            if (bytecode_fix_count == 1) {
                serial_puts("  EAX=0x"); serial_puthex((uint32_t)frame->rax, 8);
                serial_puts(" EBX=0x"); serial_puthex((uint32_t)frame->rbx, 8);
                serial_puts(" ECX=0x"); serial_puthex((uint32_t)frame->rcx, 8);
                serial_puts(" EDX=0x"); serial_puthex((uint32_t)frame->rdx, 8);
                serial_puts("\n  ESI=0x"); serial_puthex((uint32_t)frame->rsi, 8);
                serial_puts(" EDI=0x"); serial_puthex((uint32_t)frame->rdi, 8);
                serial_puts(" EBP=0x"); serial_puthex((uint32_t)frame->rbp, 8);
                serial_puts(" ESP=0x"); serial_puthex((uint32_t)frame->rsp, 8);
                uint32_t iat_val = *(volatile uint32_t *)(uintptr_t)0x105A5E08;
                serial_puts("\n  IAT[5E08]=0x"); serial_puthex(iat_val, 8);
                uint32_t *sp = (uint32_t *)(uintptr_t)(frame->rsp & 0xFFFFFFFF);
                serial_puts(" retaddr=0x"); serial_puthex(sp[0], 8);
                if (sp[0] > 0x10000000 && sp[0] < 0x20000000) {
                    uint8_t *caller = (uint8_t *)(uintptr_t)(sp[0] - 8);
                    serial_puts("\n  caller: ");
                    for (int bi = 0; bi < 12; bi++) {
                        serial_puthex(caller[bi], 2);
                        serial_puts(" ");
                    }
                }
                serial_puts("\n");
            }
        }
        uint32_t *sp32 = (uint32_t *)(uintptr_t)(frame->rsp & 0xFFFFFFFF);
        frame->rip = sp32[0];
        frame->rsp += 4;
        frame->rax = 0;
        return;
    }

    /* #BP (INT3) — software breakpoint for tracing PE32 execution */
    /* ConstructObject return trap — patched INT3 at EXE+0xBC72 */
    if (vec == 3 && (frame->cs & 0xFFFF) == 0x40) {
        uint32_t rip32 = (uint32_t)frame->rip;
        /* INT3 advances RIP by 1, so RIP = breakpoint_addr + 1 */
        uint32_t bp = rip32 - 1;
        if ((bp & 0xFFFF0000) == 0x10900000 && ((bp & 0xFFFF) == 0xBC72 || (bp & 0xFFFF) == 0xBC81)) {
            uint32_t off = bp & 0xFFFF;
            if (off == 0xBC72) {
                /* After ConstructObject: EAX = result */
                serial_puts("[BP-BC72] EAX=0x");
                serial_puthex((uint32_t)frame->rax, 8);
                uint32_t eax = (uint32_t)frame->rax;
                serial_puts(eax >= 0x40000000 ? " HEAP✓" : eax >= 0x1C000000 ? " STACK✗" : eax == 0 ? " NULL!" : " ???");
                serial_puts(" EBP=0x");
                serial_puthex((uint32_t)frame->rbp, 8);
                serial_puts("\n");
                uint8_t *code = (uint8_t *)(uintptr_t)bp;
                *code = 0x89; /* restore: mov [ebp-0x7a4],eax */
                frame->rip = bp;
            } else {
                /* Before Init: ECX should = [EBP-0x14] = GEngine */
                uint32_t ebp = (uint32_t)frame->rbp;
                uint32_t ge_local = *(volatile uint32_t *)(uintptr_t)(ebp - 0x14);
                serial_puts("[BP-BC81] [EBP-14]=0x");
                serial_puthex(ge_local, 8);
                serial_puts(ge_local >= 0x40000000 ? " HEAP✓" : ge_local >= 0x1C000000 ? " STACK✗" : " ???");
                serial_puts(" EBP=0x");
                serial_puthex(ebp, 8);
                serial_puts("\n");
                uint8_t *code = (uint8_t *)(uintptr_t)bp;
                *code = 0x8B; /* restore: mov -0x14(%ebp),%ecx */
                frame->rip = bp;
            }
            return;
        }
    }

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

    /* IAT auto-recovery: if execution reaches heap (0x40xxxxxx), the engine
     * jumped to a corrupted IAT entry. Scan .idata for the corrupt value,
     * resolve the original function via dll_resolve_iat_original(), fix it. */
    if (vec == 14 || vec == 6 /* #UD */) {
        uint64_t fault_rip = frame->rip;
        if (fault_rip >= 0x40000000ULL && fault_rip < 0x80000000ULL) {
            uint32_t corrupt = (uint32_t)fault_rip;
            /* Scan Engine.dll .idata (0x105A5000, 7 pages) */
            volatile uint32_t *idata = (volatile uint32_t *)(uintptr_t)0x105A5000;
            for (uint32_t i = 0; i < 7 * 1024; i++) {
                if (idata[i] == corrupt) {
                    /* Try to resolve the original value */
                    extern uint32_t dll_resolve_iat_original(uint32_t iat_va);
                    uint32_t original = dll_resolve_iat_original(0x105A5000 + i * 4);
                    if (original && original >= 0x10000000 && original < 0x20000000) {
                        idata[i] = original;
                        frame->rip = (uint64_t)original;
                        if ((uint32_t)frame->rbx == corrupt)
                            frame->rbx = (uint64_t)original;
                        if ((uint32_t)frame->rdi == corrupt)
                            frame->rdi = (uint64_t)original;
                        /* Add to dynamic guard table */
                        extern void iat_guard_add(uint32_t addr, uint32_t value);
                        iat_guard_add(0x105A5000 + i * 4, original);
                        return;
                    }
                }
            }
            /* Hardcoded fallback: StaticLoadClass */
            if (fault_rip == 0x4027C870ULL) {
                frame->rip = 0x10101820ULL;
                if ((uint32_t)frame->rbx == 0x4027C870)
                    frame->rbx = 0x10101820ULL;
                volatile uint32_t *iat = (volatile uint32_t *)(uintptr_t)0x105A5E08;
                *iat = 0x10101820;
                return;
            }
        }
    }

    /* UT99 quirk: short-circuit the bogus 2GB rep-movsl in the
     * Engine.dll memcpy helper at 0x1010723E. The helper is given a
     * corrupt TArray Max (~537M elements = ~2GB bytes) by a buggy
     * caller chain. Force ECX=0 to terminate REP MOVSL immediately. */
    if (vec == 14 &&
        (frame->rip & 0xFFFFFFFFULL) == 0x1010723EULL &&
        (frame->rcx & 0xFFFFFFFFULL) > 0x40000ULL) {
        static uint32_t shortcut_log = 0;
        if (shortcut_log < 4) {
            serial_puts("[VA-SHORT] cap rep-movsl @0x1010723E cs=0x");
            serial_puthex(frame->cs & 0xFFFF, 4);
            serial_puts(" ecx=0x");
            serial_puthex(frame->rcx & 0xFFFFFFFFULL, 8);
            serial_puts(" -> 0\n");
            shortcut_log++;
        }
        frame->rcx = 0;
        return;
    }
    /* Diagnostic: log #PF in compat32 CS for any RIP, sampled */
    if (vec == 14 && (frame->cs & 0xFFFF) == 0x40) {
        static uint32_t pf32_log = 0;
        pf32_log++;
        if (pf32_log < 4 || (pf32_log & 0x3FFFF) == 0) {
            serial_puts("[PF32] rip=0x");
            serial_puthex(frame->rip & 0xFFFFFFFFULL, 8);
            serial_puts(" ecx=0x");
            serial_puthex(frame->rcx & 0xFFFFFFFFULL, 8);
            serial_puts(" cnt=");
            serial_putdec(pf32_log);
            serial_puts("\n");
        }
    }

    /* Demand paging — handle #PF FIRST, before any diagnostic output.
     * Validates against VMA table. Covers ELF segments (0x400000+),
     * mmap regions, and anonymous reservations (0x500000000+).
     * Skip first 1MB (BIOS/bootloader) and null page (handled below). */
    if (vec == 14 && !(frame->error_code & 1)) {
        uint64_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        if (cr2 >= 0x100000ULL) {
            extern int demand_page_fault(uint64_t addr, uint64_t error_code);
            if (demand_page_fault(cr2, frame->error_code) == 0)
                return;
        }
    }

    /* APIC timer tick */
    if (vec == 32) {
        /* Present DOS VGA mode 13h vram to the real framebuffer (no-op
         * unless a native DOS VM is active and in mode 13h). */
        extern void dos_vga_mode13_present(void);
        dos_vga_mode13_present();
        /* In TSC-deadline mode, advance tick_count based on elapsed TSC
         * to maintain a stable ~100Hz virtual tick for TCP/timers/display.
         * In periodic mode, simply increment. */
        if (tsc_deadline_mode) {
            uint64_t now_tsc = idt_rdtsc();
            uint64_t tsc_per_tick = tsc_freq / 100;
            if (tsc_last_tick == 0) tsc_last_tick = now_tsc;
            while (now_tsc - tsc_last_tick >= tsc_per_tick) {
                tick_count++;
                tsc_last_tick += tsc_per_tick;
            }
        } else {
            tick_count++;
        }

        /* VDSO: update shared data page (seqlock, ~20 instructions) */
        extern void vdso_update(void);
        vdso_update();

        /* IAT watchdog: restore Engine.dll StaticLoadClass on every tick.
         * The Unreal package loader overwrites this between INT 0x2E calls,
         * so the compat32_dispatch guard alone isn't fast enough. Only
         * runs when a PE32 binary is loaded — without this gate the
         * watchdog would touch unmapped low VA from any process CR3. */
        extern int g_compat32_mode;
        if (g_compat32_mode) {
            static uint32_t iat_orig = 0;
            volatile uint32_t *iat = (volatile uint32_t *)(uintptr_t)0x105A5E08;
            if (!iat_orig && *iat >= 0x10100000 && *iat < 0x10200000)
                iat_orig = *iat;
            if (iat_orig && *iat != iat_orig)
                *iat = iat_orig;
        }

#ifdef COMPAT32_TIMER_DEBUG
        /* Watchdog: log PE32 execution state (enable with -DCOMPAT32_TIMER_DEBUG) */
        {
            static int compat32_ticks = 0;
            uint16_t cs = (uint16_t)(frame->cs & 0xFFFF);
            if (cs == 0x40) {
                compat32_ticks++;
                if ((compat32_ticks % 100) == 1) {
                    serial_puts("[TIMER] PE32 RIP=0x");
                    serial_puthex(frame->rip, 8);
                    serial_puts(" t=");
                    serial_putdec(compat32_ticks);
                    serial_puts("\n");
                }
            }
        }
#endif

        /* Re-zero NULL page for compat32: compat32 can't use TF single-step
         * (#DB from compat mode causes #GP without IST), so page 0 stays
         * writable after a compat32 write. Re-zero + re-protect here so that
         * future NULL pointer derefs read 0 (vtable=0) instead of stale data.
         * Without this, a NULL object pointer reads garbage from page 0 as a
         * vtable and jumps to BIOS IVT addresses → #UD. */
        if (g_null_page_dirty) null_page_clean();

        /* Always-on profiling: record RIP at each timer tick (~3 cycles) */
        {
            extern void kprof_record(uint64_t rip);
            kprof_record(frame->rip);
        }

        /* X-SCHED: preemptive scheduler — check quantum, switch if expired.
         * frame points to saved GPRs on the current process's stack. */
        sched_tick(frame);

        /* TSC-deadline: reprogram next deadline based on current process's QoS.
         * In periodic mode, the APIC timer auto-reloads — nothing to do. */
        if (tsc_deadline_mode) {
            extern uint64_t sched_get_next_deadline_us(void);
            uint64_t next_us = sched_get_next_deadline_us();
            if (next_us > 0) {
                uint64_t deadline = idt_rdtsc() + (tsc_freq * next_us / 1000000);
                wrmsr(MSR_IA32_TSC_DEADLINE, deadline);
            }
            /* next_us == 0: tickless idle — no deadline, CPU will HLT
             * until NIC/keyboard/IPI interrupt wakes it */
        }

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

    /* I211 NIC MSI interrupt */
    if (vec == 40) {
        extern void i211_isr(void);
        i211_isr();
        apic_write(APIC_EOI, 0);
        return;
    }

    /* RTL8111 NIC MSI interrupt */
    if (vec == 41) {
        extern void rtl8111_isr(void) __attribute__((weak));
        if (rtl8111_isr) rtl8111_isr();
        apic_write(APIC_EOI, 0);
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
        /* Phase 9: dispatch to generic hwbp manager first. If any slot
         * in the user-visible hwbps[] table is active and DR6 matches,
         * we've handled it. Otherwise fall through to the PE32-specific
         * debug path below. */
        {
            extern bool hwbp_dispatch(void *frame);
            if (hwbp_dispatch(frame)) return;
        }

        static int db_hit_count = 0;
        uint64_t dr6;
        __asm__ volatile ("mov %%dr6, %0" : "=r"(dr6));

        if (dr6 & 0x1) {  /* B0: breakpoint 0 hit (dd_vtbl32) */
            uint64_t dr0;
            __asm__ volatile ("mov %%dr0, %0" : "=r"(dr0));
            uint32_t val = *(volatile uint32_t *)dr0;

            if (db_hit_count < 20) {
                serial_puts("[WP] val=0x");
                serial_puthex(val, 8);
                serial_puts(" RIP=0x");
                serial_puthex(frame->rip, 8);
                serial_puts("\n");
            }
            db_hit_count++;

            __asm__ volatile ("mov %0, %%dr6" : : "r"((uint64_t)0));
            return;
        }

        if (dr6 & 0x2) {  /* B1: ConstructObject execution breakpoint */
            static int bp1_count = 0;
            bp1_count++;

            serial_puts("[CONSTRUCT-BP] hit #");
            serial_putdec(bp1_count);
            serial_puts(" RIP=0x");
            serial_puthex((uint32_t)frame->rip, 8);
            serial_puts(" ECX=0x");
            serial_puthex((uint32_t)frame->rcx, 8);
            serial_puts(" ESI=0x");
            serial_puthex((uint32_t)frame->rsi, 8);
            serial_puts(" ESP=0x");
            serial_puthex((uint32_t)(frame->rsp & 0xFFFFFFFF), 8);
            serial_puts("\n");

            /* Dump the UClass* argument (ESI = first pushed arg for
             * this specific call site at Engine.dll+0x84267) */
            uint32_t esi = (uint32_t)frame->rsi;
            if (esi >= 0x10000 && esi < 0x50000000) {
                /* UClass has name at a known offset. The first few
                 * dwords might reveal the class identity. */
                uint32_t *cls = (uint32_t *)(uintptr_t)esi;
                serial_puts("  UClass[0..3]: 0x");
                serial_puthex(cls[0], 8);
                serial_puts(" 0x"); serial_puthex(cls[1], 8);
                serial_puts(" 0x"); serial_puthex(cls[2], 8);
                serial_puts(" 0x"); serial_puthex(cls[3], 8);
                serial_puts("\n");
            }

            __asm__ volatile ("mov %0, %%dr6" : : "r"((uint64_t)0));
            return;
        }

        /* TF single-step after NULL page write: re-protect + re-zero page 0 */
        if (g_null_page_dirty) {
            frame->rflags &= ~(1ULL << 8);  /* clear TF */
            null_page_clean();
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
            /* Null-pointer writes from PE32 code.
             * For code in DLL range (0x10-0x12M): write-through (safe,
             * these are benign init-time writes the engine expects).
             * For code in heap range (0x40-0x80M): dispatch to base SEH
             * handler to trigger the engine's __except error path.
             * Without SEH, heap code falls through to garbage after the
             * write and hits CC padding → #BP crash. */
            {
                static int nw_count = 0;
                nw_count++;
                int is_heap = (frame->cs & 0xFFFF) == 0x40 &&
                    frame->rip >= 0x40000000 && frame->rip < 0x80000000;
                if (nw_count <= 5 && (frame->cs & 0xFFFF) == 0x40) {
                    serial_puts("[NULL-WRITE] RIP=0x");
                    serial_puthex((uint32_t)frame->rip, 8);
                    serial_puts(" CR2=0x");
                    serial_puthex((uint32_t)cr2, 4);
                    serial_puts(is_heap ? " (heap→SEH)\n" : " (DLL→wt)\n");
                }
                if (is_heap) {
                    /* Heap code executing data as code → EBX was corrupted
                     * by a callback, causing call *%ebx to jump to heap data.
                     * Fix: reload EBX from the IAT entry it was supposed to
                     * contain, and redirect to the CORRECT function. */
                    volatile uint32_t *iat = (volatile uint32_t *)(uintptr_t)0x105A5E08;
                    uint32_t correct_fn = *iat;
                    static int heap_fix_count = 0;
                    heap_fix_count++;

                    if (heap_fix_count <= 5) {
                        serial_puts("[HEAP-FIX] EBX=0x");
                        serial_puthex((uint32_t)frame->rbx, 8);
                        serial_puts(" → 0x");
                        serial_puthex(correct_fn, 8);
                        serial_puts(" RIP was 0x");
                        serial_puthex((uint32_t)frame->rip, 8);
                        serial_puts("\n");
                    }

                    /* Fix EBX and redirect execution to the correct function.
                     * The PE32 code did `call *%ebx` which jumped to garbage.
                     * We undo the call: pop the return address from stack,
                     * fix EBX, and redirect RIP to the correct function.
                     * The retaddr on stack is the instruction after `call *%ebx`
                     * in Init() — we leave it there so the function can RET. */
                    frame->rbx = correct_fn;
                    frame->rip = correct_fn;
                    /* EAX was clobbered by executing garbage (add [eax],al).
                     * Restore EAX to a sane value (0 is safe — it was the
                     * null pointer that caused the fault). */
                    frame->rax = 0;
                    return;  /* resume at the correct function */
                }
            }

            /* Normal null-page write-through */
            paging_set_flags(0, PTE_PRESENT | PTE_WRITABLE | PTE_GLOBAL | PTE_NX);
            __asm__ volatile ("invlpg (%0)" :: "r"((uint64_t)0) : "memory");
            if ((frame->cs & 0xFFFF) != 0x40) {
                frame->rflags |= (1ULL << 8);  /* TF bit */
            }
            g_null_page_dirty = 1;
            return;
        }

        /* The NULL-CALL recovery path below is Win32 PE32 specific: it
         * reads from `frame->rsp & 0xFFFFFFFF` (truncated 32-bit RSP)
         * to recover the return address, dispatches to the engine SEH
         * chain, scans the IAT for redirect candidates, etc. Native
         * x86_64 user code (CS=0x38) with a NULL function pointer
         * should just take the regular #PF path and die. */
        if (cr2 < 0x1000 && (frame->error_code & 16) &&
            ((frame->cs & 0xFFFF) == 0x40 ||
             (frame->cs & 0xFFFF) == 0x23)) {  /* INSTRUCTION-FETCH on page 0 */
            /* NULL function pointer call from stale register.
             * Try IAT redirect first (same technique as BC-REDIRECT):
             * scan backwards from retaddr to find the IAT load instruction
             * and redirect to the correct function. */
            static int null_call_count = 0;
            null_call_count++;

            /* Diagnostic: for indirect calls (call *offset(reg)), dump
             * the vtable pointer and the target entry so we can see why
             * the function pointer is NULL. */
            if (null_call_count <= 5 && (frame->cs & 0xFFFF) == 0x40) {
                uint32_t retaddr32 = ((uint32_t *)(uintptr_t)(frame->rsp & 0xFFFFFFFF))[0];
                uint8_t *ca = (uint8_t *)(uintptr_t)(retaddr32 - 6);
                /* call *disp32(%reg) = FF 92 xx xx xx xx (for edx) */
                if (ca[0] == 0xFF && (ca[1] & 0xC0) == 0x80) {
                    uint8_t modrm = ca[1];
                    uint8_t reg = modrm & 0x07;
                    uint32_t disp = *(uint32_t *)(ca + 2);
                    /* Get register value that held the vtable */
                    uint32_t regvals[8] = {
                        (uint32_t)frame->rax, (uint32_t)frame->rcx,
                        (uint32_t)frame->rdx, (uint32_t)frame->rbx,
                        0/*esp*/, (uint32_t)frame->rbp,
                        (uint32_t)frame->rsi, (uint32_t)frame->rdi
                    };
                    uint32_t vtbl = regvals[reg];
                    serial_puts("[NULL-DIAG] call *0x");
                    serial_puthex(disp, 4);
                    serial_puts("(%");
                    const char *rn[] = {"eax","ecx","edx","ebx","esp","ebp","esi","edi"};
                    serial_puts(rn[reg]);
                    serial_puts(") vtbl=0x"); serial_puthex(vtbl, 8);
                    serial_puts(" this=0x"); serial_puthex((uint32_t)frame->rdi, 8);
                    serial_puts("\n");
                    /* Dump registers and object for Browse call */
                    if (disp == 0xB0) {
                        serial_puts("  ECX=0x"); serial_puthex((uint32_t)frame->rcx, 8);
                        serial_puts(" EDI=0x"); serial_puthex((uint32_t)frame->rdi, 8);
                        serial_puts(" EDX=0x"); serial_puthex((uint32_t)frame->rdx, 8);
                        serial_puts(" EBP=0x"); serial_puthex((uint32_t)frame->rbp, 8);
                        serial_puts("\n");
                        /* Dump both EDI and EDX targets */
                        uint32_t edi_val = (uint32_t)frame->rdi;
                        uint32_t edx_val = (uint32_t)frame->rdx;
                        if (edi_val >= 0x1000 && edi_val < 0x50000000) {
                            uint32_t *o = (uint32_t *)(uintptr_t)edi_val;
                            serial_puts("  [EDI]="); serial_puthex(o[0], 8);
                            serial_puts(" [EDI+4]="); serial_puthex(o[1], 8);
                            serial_puts(" [EDI+44]="); serial_puthex(o[0x44/4], 8);
                            serial_puts(" [EDI+48]="); serial_puthex(o[0x48/4], 8);
                            serial_puts("\n");
                        }
                        /* Init() caller return address */
                        uint32_t ebp = (uint32_t)frame->rbp;
                        if (ebp >= 0x1000 && ebp < 0x50000000) {
                            uint32_t *fp = (uint32_t *)(uintptr_t)ebp;
                            serial_puts("  Init caller: [EBP+4]=0x");
                            serial_puthex(fp[1], 8);
                            serial_puts(" [EBP]=0x");
                            serial_puthex(fp[0], 8);
                            serial_puts("\n");
                            /* Walk one more frame */
                            uint32_t caller_ebp = fp[0];
                            if (caller_ebp >= 0x1000 && caller_ebp < 0x50000000) {
                                uint32_t *cfp = (uint32_t *)(uintptr_t)caller_ebp;
                                serial_puts("  Caller's caller: [EBP+4]=0x");
                                serial_puthex(cfp[1], 8);
                                serial_puts("\n");
                            }
                        }
                    }
                    /* Dump registers and stack */
                    serial_puts("  EBP=0x"); serial_puthex((uint32_t)frame->rbp, 8);
                    serial_puts(" ESP=0x"); serial_puthex((uint32_t)(frame->rsp & 0xFFFFFFFF), 8);
                    serial_puts(" ECX=0x"); serial_puthex((uint32_t)frame->rcx, 8);
                    serial_puts(" EAX=0x"); serial_puthex((uint32_t)frame->rax, 8);
                    serial_puts("\n");
                    /* Dump stack words from ESP upward to find return addresses */
                    uint32_t esp32 = (uint32_t)(frame->rsp & 0xFFFFFFFF);
                    if (esp32 >= 0x10000 && esp32 < 0x50000000) {
                        uint32_t *sp = (uint32_t *)(uintptr_t)esp32;
                        serial_puts("  STACK:");
                        for (int i = 0; i < 16; i++) {
                            if (i % 4 == 0) { serial_puts("\n    +"); serial_puthex(i*4, 2); serial_puts(":"); }
                            serial_puts(" 0x"); serial_puthex(sp[i], 8);
                        }
                        serial_puts("\n");
                    }
                    /* Dump vtable entries around the offset */
                    if (vtbl >= 0x10000000 && vtbl < 0x20000000) {
                        uint32_t *vt = (uint32_t *)(uintptr_t)vtbl;
                        int idx = disp / 4;
                        for (int i = (idx > 2 ? idx - 2 : 0); i <= idx + 2; i++) {
                            serial_puts("  vt[0x");
                            serial_puthex(i * 4, 3);
                            serial_puts("]=0x");
                            serial_puthex(vt[i], 8);
                            if (i == idx) serial_puts(" *** NULL TARGET");
                            serial_puts("\n");
                        }
                    }

                    /* Self-referencing vtable (vtbl == this) or NULL vtable
                     * call from Window.dll stub objects: RET 0 to caller.
                     * Window.dll calls methods on stub GWindowManager/GLogWindow
                     * that have no real implementation. Without RET 0, the
                     * #UD/#GP cascade prevents the engine from reaching Init(). */
                    {
                        int is_self_ref = (vtbl == (uint32_t)frame->rdi && vtbl < 0x10000000);
                        uint32_t *sp = (uint32_t *)(uintptr_t)(frame->rsp & 0xFFFFFFFF);
                        uint32_t ret = sp[0];
                        int from_window = (ret >= 0x11100000 && ret < 0x11200000);
                        if (is_self_ref || from_window) {
                            frame->rip = ret;
                            frame->rsp += 4;
                            frame->rax = 0;
                            if (g_null_page_dirty) null_page_clean();
                            return;
                        }
                    }
                }
            }

            if ((frame->cs & 0xFFFF) == 0x40) {
                uint32_t *sp32 = (uint32_t *)(uintptr_t)(frame->rsp & 0xFFFFFFFF);
                uint32_t retaddr = sp32[0];

                /* Browse() redirect: Init() at Engine.dll+0x888F5 calls
                 * Browse via call *0xB0(%edx) but the vtable is corrupt
                 * (heap ini strings instead of C++ vtable). The REAL
                 * Browse function is at Engine.dll+0xBC210 (0x1038C210).
                 * Redirect the NULL-CALL to the real Browse. The args
                 * are already pushed on the stack, ECX=this from EDI. */
                if (retaddr >= 0x103888F0 && retaddr <= 0x10388900) {
                    uint32_t real_browse = 0x1038C210;
                    static int browse_fix_count = 0;
                    browse_fix_count++;
                    if (browse_fix_count <= 3) {
                        serial_puts("[BROWSE-FIX] this=0x");
                        serial_puthex((uint32_t)frame->rdi, 8);
                        serial_puts("\n");
                        /* Dump UClass hierarchy to diagnose ConstructObject failure */
                        /* UGameEngine::PrivateStaticClass at 0x105928A0 */
                        volatile uint32_t *ge_cls = (volatile uint32_t *)(uintptr_t)0x105928A0;
                        serial_puts("  UGameEngine::SC vtbl=0x");
                        serial_puthex(ge_cls[0], 8);
                        serial_puts(" SuperField=0x");
                        serial_puthex(ge_cls[0x28/4], 8);
                        serial_puts(" Name=0x");
                        serial_puthex(ge_cls[0x0C/4], 8);  /* FName at offset 0x0C in UObject */
                        serial_puts("\n");
                        /* GObjRegistrants: TArray at 0x102A0360 */
                        volatile uint32_t *reg = (volatile uint32_t *)(uintptr_t)0x102A0360;
                        serial_puts("  GObjRegistrants: Data=0x");
                        serial_puthex(reg[0], 8);
                        serial_puts(" Num=");
                        serial_putdec(reg[1]);
                        serial_puts(" Max=");
                        serial_putdec(reg[2]);
                        serial_puts("\n");
                        /* UEngine::PrivateStaticClass — find from EXE IAT 0x10958D74 */
                        volatile uint32_t *ue_iat = (volatile uint32_t *)(uintptr_t)0x10958D74;
                        uint32_t ue_cls_addr = *ue_iat;
                        serial_puts("  UEngine::SC (from IAT) = 0x");
                        serial_puthex(ue_cls_addr, 8);
                        if (ue_cls_addr >= 0x10000 && ue_cls_addr < 0x20000000) {
                            volatile uint32_t *ue_cls = (volatile uint32_t *)(uintptr_t)ue_cls_addr;
                            serial_puts(" SuperField=0x");
                            serial_puthex(ue_cls[0x28/4], 8);
                        }
                        serial_puts("\n");
                    }
                    /* Also fix the vtable pointer so Browse can use it */
                    uint32_t this_ptr = (uint32_t)frame->rdi;
                    if (this_ptr >= 0x1000 && this_ptr < 0x50000000) {
                        *(uint32_t *)(uintptr_t)this_ptr = 0x10434650; /* real UGameEngine vtable */
                    }
                    frame->rip = real_browse;
                    frame->rcx = frame->rdi; /* this = GameEngine */
                    if (g_null_page_dirty) null_page_clean();
                    return;
                }

                /* Stub object NULL-CALL: if retaddr is in Window.dll or
                 * any DLL that calls methods on our stub UObjects, RET 0.
                 * These are calls to unimplemented virtual methods on
                 * GWindowManager/GLogWindow stubs. */
                if (retaddr >= 0x11100000 && retaddr < 0x11200000) {
                    frame->rip = retaddr;
                    frame->rsp += 4;
                    frame->rax = 0;
                    if (g_null_page_dirty) null_page_clean();
                    return;
                }

                /* Try IAT redirect: find 'call reg' (FF Dx) at retaddr-2,
                 * then scan backwards for 'mov reg, [imm32]' (IAT load) */
                uint8_t *caller = (uint8_t *)(uintptr_t)(retaddr - 2);
                uint32_t iat_addr = 0;
                uint8_t mov_opcode = 0;

                if (caller[0] == 0xFF && (caller[1] & 0xF8) == 0xD0) {
                    uint8_t reg = caller[1] & 0x07;
                    mov_opcode = 0x05 + reg * 8;
                }

                if (mov_opcode) {
                    uint8_t *scan = caller - 1;
                    for (int i = 0; i < 200 && !iat_addr; i++, scan--) {
                        if (scan[0] == 0x8B && scan[1] == mov_opcode)
                            iat_addr = *(uint32_t *)(scan + 2);
                    }
                }

                if (iat_addr >= 0x10000000 && iat_addr < 0x20000000) {
                    uint32_t correct_fn = *(uint32_t *)(uintptr_t)iat_addr;
                    if (correct_fn >= 0x10000000 && correct_fn < 0x20000000) {
                        if (null_call_count <= 10) {
                            serial_puts("[NULL-REDIRECT] -> 0x");
                            serial_puthex(correct_fn, 8);
                            serial_puts(" (IAT 0x");
                            serial_puthex(iat_addr, 8);
                            serial_puts(") retaddr=0x");
                            serial_puthex(retaddr, 8);
                            serial_puts("\n");
                        }
                        frame->rip = correct_fn;
                        return;
                    }
                }
            }

            if (null_call_count <= 10) {
                serial_puts("[NULL-CALL] RIP=0x");
                serial_puthex(cr2, 4);
                serial_puts(" retaddr=0x");
                serial_puthex(((uint32_t *)(uintptr_t)(frame->rsp & 0xFFFFFFFF))[0], 8);
                serial_puts(" #");
                serial_putdec(null_call_count);
                serial_puts("\n");
            }
            /* After too many NULL calls, force crash recovery instead of
             * looping forever. Was 50 — raised to 5000 so UT99 can
             * survive the bursts of TArray-corruption-driven NULL
             * iterations during engine init (each FName::Add or
             * FString::Append on a corrupt array triggers tens of
             * NULL calls before the engine moves on). 50 was enough
             * to break out of an infinite GLog/GError tail-loop, but
             * not enough for normal init flow. */
            if (null_call_count > 50000) {
                serial_puts("[NULL-CALL] Too many (#");
                serial_putdec(null_call_count);
                serial_puts(") — forcing crash recovery\n");
                goto compat32_null_recovery;
            }
            if ((frame->cs & 0xFFFF) == 0x40 || (frame->cs & 0xFFFF) == 0x23) {
                /* First NULL-CALL: dispatch to SEH so the engine can show
                 * its error message and begin graceful shutdown.
                 * Subsequent NULL-CALLs: simulate RET 0 to let cleanup
                 * continue — the catch handler writes to NULL during
                 * StaticShutdownAfterError, re-dirtying page 0, and
                 * dispatching to SEH again would cause infinite recursion. */
                /* If SEH chain is corrupt, RET 0 directly — SEH dispatch would
                 * fail and crash recovery fires. RET 0 lets the engine receive
                 * NULL from the invalid function call and handle the error. */
                {
                    extern uint32_t g_teb32;  /* first field = ExceptionList */
                    uint32_t seh = g_teb32;
                    serial_puts("[NULL-CALL] SEH=0x");
                    serial_puthex(seh, 8);
                    serial_puts("\n");
                    if (seh >= 0x10000000 && seh < 0x20000000) {
                        /* SEH chain corrupt → RET with EAX matching caller's
                         * comparison register. MSVC code after null-calls often
                         * does 'cmp eax,esi; sete bl'. Return EAX=ESI so the
                         * comparison succeeds and the engine takes the "match"
                         * path instead of the error path. */
                        uint32_t *sp32 = (uint32_t *)(uintptr_t)(frame->rsp & 0xFFFFFFFF);
                        frame->rip = sp32[0];
                        frame->rsp += 4;
                        frame->rax = frame->rsi & 0xFFFFFFFF;
                        if (g_null_page_dirty) null_page_clean();
                        return;
                    }
                }
                /* First NULL-CALL with valid SEH: re-zero page 0 and dispatch */
                if (g_null_page_dirty) null_page_clean();
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

        /* Symbolize RIP for native ELF user crashes (CS=0x28). The
         * symbolizer is kmalloc-safe and reads the per-process symbol
         * tables captured at elf_load time. For unmatched addresses or
         * demand-paged binaries (no symtab) it prints nothing. */
        {
            extern void *proc_current(void);
            extern bool user_symbolize(void *p, uint64_t addr,
                                       const char **name, uint64_t *off);
            if ((frame->cs & 0xFFFF) == 0x28) {
                const char *sym_name = 0;
                uint64_t    sym_off  = 0;
                if (user_symbolize(proc_current(), frame->rip,
                                   &sym_name, &sym_off)) {
                    serial_puts("  in ");
                    serial_puts(sym_name);
                    serial_puts("+0x");
                    serial_puthex(sym_off, 4);
                    serial_puts("\n");
                }
            }
        }

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

        /* Kernel-mode exception extras (CS==0x08): dump CR3, SS and
         * the dword at [RSP] so post-kexec #UD/#GP can be triaged.
         * The kexec'd kernel landing on a mid-instruction RIP is most
         * often a corrupt indirect call/ret — knowing the return slot
         * + the active page tables narrows that down fast.           */
        if ((frame->cs & 0xFFFF) == 0x08) {
            uint64_t cr3;
            __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
            serial_puts("  CR3 = 0x");
            serial_puthex(cr3, 16);
            serial_puts("  SS  = 0x");
            serial_puthex(frame->ss, 4);
            serial_puts("\n");
            /* Dump 4 quadwords at top of stack — kernel stacks live in
             * the upper-half mirror so this is safe even pre-paging. */
            uint64_t *kp = (uint64_t *)frame->rsp;
            for (int i = 0; i < 4; i++) {
                serial_puts("    [RSP+");
                serial_puthex((uint64_t)(i * 8), 2);
                serial_puts("] = 0x");
                serial_puthex(kp[i], 16);
                serial_puts("\n");
            }
        }

        /* Dump bytes at RIP (useful for crashes on stack/corrupted code).
         * Skip the NULL page (Phase C: user PML4s have no mapping there),
         * and skip when not running compat32 user code — the dump is
         * primarily a Win32 debug aid and dereferencing arbitrary user
         * pointers can fault re-entrantly into the handler. */
        if (frame->rip >= 0x10000ULL && frame->rip < 0x100000000ULL &&
            ((frame->cs & 0xFFFF) == 0x40 ||
             (frame->cs & 0xFFFF) == 0x23)) {
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

        /* Stack dump: show 16 dwords from RSP for crash diagnosis.
         * Only dump for compat32 contexts — native x86_64 user crashes
         * have RSP in the upper-half mirror and dumping it here can
         * fault again on unmigrated mappings, which would cascade. */
        if ((frame->cs & 0xFFFF) == 0x40 ||
            (frame->cs & 0xFFFF) == 0x23) {
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

        /* Native ELF backtrace via RBP walking (CS=0x28).
         *
         * Reads `[rbp] = prev_rbp, [rbp+8] = ret_addr` up to 16 frames,
         * symbolizing each return address. Safety: RBP must be
         * 8-aligned and stay within the 8MB user stack window anchored
         * at the faulting RSP. This avoids walking page tables (which
         * don't handle the 1GB huge pages in the upper-half direct map
         * correctly) while still preventing cascade faults when RBP is
         * garbage from -fomit-frame-pointer code.
         *
         * Binaries compiled without `-fno-omit-frame-pointer` usually
         * yield only frame #0 reliably (RIP via the symbolize hook
         * above); the walk bails out at the first RBP out-of-window. */
        if ((frame->cs & 0xFFFF) == 0x28) {
            extern void *proc_current(void);
            extern bool  user_symbolize(void *p, uint64_t addr,
                                        const char **name, uint64_t *off);

            serial_puts("  Backtrace:\n");
            uint64_t       rbp  = frame->rbp;
            const uint64_t rsp  = frame->rsp;
            const uint64_t WIN  = 8ULL * 1024 * 1024;  /* USER_STACK_SIZE */
            void          *proc = proc_current();

            for (int depth = 0; depth < 16; depth++) {
                if (rbp == 0) break;
                if (rbp & 0x7) break;                  /* unaligned */
                /* Keep RBP within ±8MB of the faulting RSP — the
                 * kernel-allocated user stack window. Anything else is
                 * either garbage or points outside the stack. */
                if (rbp + 16 < rbp) break;             /* overflow guard */
                if (rbp < rsp - 256 || rbp > rsp + WIN) break;

                uint64_t prev_rbp = ((uint64_t *)rbp)[0];
                uint64_t ret      = ((uint64_t *)rbp)[1];

                serial_puts("    #");
                serial_putdec((uint64_t)depth);
                serial_puts(" 0x");
                serial_puthex(ret, 16);

                const char *nm  = 0;
                uint64_t    off = 0;
                if (user_symbolize(proc, ret, &nm, &off)) {
                    serial_puts(" ");
                    serial_puts(nm);
                    serial_puts("+0x");
                    serial_puthex(off, 4);
                }
                serial_puts("\n");

                if (prev_rbp <= rbp) break;            /* loop / end */
                rbp = prev_rbp;
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

        /* Compat32 exception dispatch: for hardware exceptions in PE32 code,
         * dispatch to the 32-bit SEH chain FIRST. This lets the engine's
         * __except filters catch #PF/#UD/#GP and handle them properly
         * (e.g., "General protection fault!" with call stack history).
         *
         * On Windows, the kernel dispatches hardware exceptions to the
         * user-mode SEH chain. We replicate this by calling
         * compat32_seh_dispatch() with the appropriate EXCEPTION_RECORD.
         *
         * If no SEH handler catches it, fall through to crash recovery. */
        /* (Bytecode-exec handler moved to early check above line ~463) */

        if ((frame->cs & 0xFFFF) == 0x40 || (frame->cs & 0xFFFF) == 0x23) {
            /* Build EXCEPTION_RECORD for PE32 SEH dispatch */
            typedef struct {
                uint32_t ExceptionCode;
                uint32_t ExceptionFlags;
                uint32_t ExceptionRecord;
                uint32_t ExceptionAddress;
                uint32_t NumberParameters;
                uint32_t ExceptionInformation[15];
            } EXCEPTION_RECORD32;

            EXCEPTION_RECORD32 er;
            for (int i = 0; i < (int)sizeof(er)/4; i++)
                ((uint32_t *)&er)[i] = 0;

            if (vec == 14) {
                /* #PF → STATUS_ACCESS_VIOLATION.
                 * NULL page WRITES (cr2 < 0x1000, !instruction-fetch) are handled
                 * by the write-through handler above — skip SEH for those.
                 * NULL page INSTRUCTION FETCHES (null function call) MUST go to SEH
                 * because silently returning 0 causes cascading NULL pointer usage. */
                uint64_t cr2;
                __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
                if (cr2 < 0x1000 && !(frame->error_code & 16))
                    goto compat32_null_recovery; /* NULL page write: keep old handling */
                er.ExceptionCode = 0xC0000005;  /* STATUS_ACCESS_VIOLATION */
                er.NumberParameters = 2;
                er.ExceptionInformation[0] = (frame->error_code & 2) ? 1 : 0;
                er.ExceptionInformation[1] = (uint32_t)cr2;
            } else if (vec == 6) {
                er.ExceptionCode = 0xC000001D;  /* STATUS_ILLEGAL_INSTRUCTION */
            } else if (vec == 13) {
                er.ExceptionCode = 0xC0000005;  /* STATUS_ACCESS_VIOLATION */
            } else {
                goto compat32_null_recovery;  /* other vectors: use old recovery */
            }
            er.ExceptionAddress = (uint32_t)frame->rip;
            er.ExceptionFlags = 0;  /* continuable */

            serial_puts("  [WIN32] Dispatching to PE32 SEH: code=0x");
            serial_puthex(er.ExceptionCode, 8);
            serial_puts(" addr=0x");
            serial_puthex(er.ExceptionAddress, 8);
            serial_puts("\n");

            /* Dispatch to PE32 SEH chain */
            extern int compat32_seh_dispatch(void *ExceptionRecord);
            /* Cast to the 64-bit EXCEPTION_RECORD expected by dispatch.
             * Build a temporary 64-bit version from our 32-bit one. */
            typedef struct {
                uint32_t ExceptionCode;
                uint32_t ExceptionFlags;
                uint64_t ExceptionRecord;
                uint64_t ExceptionAddress;
                uint32_t NumberParameters;
                uint32_t pad;
                uint64_t ExceptionInformation[15];
            } EXCEPTION_RECORD64;
            EXCEPTION_RECORD64 er64;
            for (int i = 0; i < (int)sizeof(er64)/4; i++)
                ((uint32_t *)&er64)[i] = 0;
            er64.ExceptionCode = er.ExceptionCode;
            er64.ExceptionFlags = er.ExceptionFlags;
            er64.ExceptionAddress = (uint64_t)er.ExceptionAddress;
            er64.NumberParameters = er.NumberParameters;
            for (uint32_t i = 0; i < er.NumberParameters && i < 15; i++)
                er64.ExceptionInformation[i] = er.ExceptionInformation[i];

            /* Re-zero null page before dispatching to SEH catch handlers.
             * Compat32 writes to page 0 skip TF (would cause #GP), so page
             * stays dirty until the next APIC timer tick. If an exception
             * fires before the tick, catch handlers read stale data from
             * page 0 (e.g., GMalloc vtable deref → cascading NULL call). */
            if (g_null_page_dirty) null_page_clean();

            int handled = compat32_seh_dispatch((void *)&er64);
            if (handled) {
                /* Apply unwind globals to the iretq frame.
                 * int2e_stub.S does this for software exceptions (lines 77-96);
                 * we must do it here for hardware exceptions since
                 * isr_stubs.S doesn't check unwind globals. */
                extern uint32_t g_compat32_unwind_eip;
                extern uint32_t g_compat32_unwind_esp;
                extern uint32_t g_compat32_unwind_ebp;

                if (g_compat32_unwind_eip) {
                    frame->rip = g_compat32_unwind_eip;
                    frame->rsp = g_compat32_unwind_esp;
                    frame->rbp = g_compat32_unwind_ebp;
                    g_compat32_unwind_eip = 0;
                    g_compat32_unwind_esp = 0;
                    g_compat32_unwind_ebp = 0;
                }

                serial_puts("  [WIN32] SEH handled — resuming via unwind\n");
                return;  /* iretq will now jump to catch handler */
            }
            serial_puts("  [WIN32] SEH unhandled — falling through to recovery\n");
        }
compat32_null_recovery:
        /* Legacy crash recovery: longjmp back to shell */
        {
            extern uint64_t *compat32_crash_jmpbuf;
            extern void kern_longjmp(uint64_t *buf, int val);
            if (compat32_crash_jmpbuf) {
                serial_puts("  [WIN32] Crash recovery — returning to shell\n");
                /* Restore IST1 BEFORE longjmp — longjmp bypasses
                 * int2e_stub's IST1 restore, leaving it corrupted. */
                extern uint8_t ist1_stack[];
                kernel_tss.ist1 = (uint64_t)(ist1_stack + IST1_STACK_SIZE);
                uint64_t *jmp = compat32_crash_jmpbuf;
                compat32_crash_jmpbuf = NULL;

                /* Sanity-check the jmpbuf — UT99 in compat32 shares the
                 * kernel CR3 and could have wild-written into shell.c's
                 * static `winexec_jmpbuf[]`. If the saved cr3/rsp/rip
                 * look bogus, halt cleanly instead of jumping to RIP=0
                 * with random RSP and triple-faulting in kernel mode. */
                uint64_t s_rsp = jmp[6], s_rip = jmp[7], s_cr3 = jmp[8];
                serial_puts("  [WIN32] jmpbuf rip=0x"); serial_puthex(s_rip, 16);
                serial_puts(" rsp=0x"); serial_puthex(s_rsp, 16);
                serial_puts(" cr3=0x"); serial_puthex(s_cr3, 16);
                serial_puts("\n");
                int valid = 1;
                /* RIP should be in kernel high half (>= 0xFFFF800000000000) */
                if (s_rip < 0xFFFF800000000000ULL) {
                    serial_puts("  [WIN32] jmpbuf RIP corrupt 0x");
                    serial_puthex(s_rip, 16); serial_puts("\n");
                    valid = 0;
                }
                /* RSP should be a kernel stack — either high half OR
                 * within an ist_stack range. Allow low-half if it's a
                 * boot-time kernel RSP (around 0x7FExxxxx UEFI region). */
                if (s_rsp < 0x100000ULL) {
                    serial_puts("  [WIN32] jmpbuf RSP corrupt 0x");
                    serial_puthex(s_rsp, 16); serial_puts("\n");
                    valid = 0;
                }
                /* CR3 must be page-aligned and < 4 GB */
                if ((s_cr3 & 0xFFFULL) || s_cr3 >= 0x100000000ULL ||
                    s_cr3 == 0) {
                    serial_puts("  [WIN32] jmpbuf CR3 corrupt 0x");
                    serial_puthex(s_cr3, 16); serial_puts("\n");
                    valid = 0;
                }
                if (!valid) {
                    serial_puts("  [WIN32] REFUSING longjmp — jmpbuf "
                                "corrupted by user code, halting\n");
                    __asm__ volatile ("cli");
                    for (;;) __asm__ volatile ("hlt");
                }
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
            /* Save structured crash report to OsitoFS */
            {
                extern void crash_report_save(uint64_t *frame, uint32_t vec,
                    uint64_t fault, const char *name, uint32_t pid);
                extern const char *proc_current_name(void);
                uint64_t cr2 = 0;
                if (vec == 14) __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
                crash_report_save((uint64_t *)frame, vec, cr2,
                                  proc_current_name(), (uint32_t)pid);
            }

            serial_puts("  Killing process PID ");
            serial_putdec(pid);
            serial_puts(" with signal ");
            serial_putdec((uint64_t)sig);
            serial_puts("\n");
            fb_puts_color(" Process killed\n", 0x00FF0000);
            proc_exception_kill(128 + sig);
            /* proc_exception_kill never returns */
        }

        /* DOS-native fallback: if a DOS native session is active
         * (dos_native_exit_jmpbuf is set by `dosrun` before LRETQ to
         * DOOM), longjmp to the shell instead of halting. The [pf-ist]
         * probe above only fires when RSP is inside one of our IST
         * stacks, but DOS-native code can fault with RSP pointing at
         * DOOM's own SS (stack outside the IST window). Catch that here. */
        {
            extern uint64_t *dos_native_exit_jmpbuf;
            extern uint64_t paging_get_kernel_cr3(void);
            extern void kern_longjmp(uint64_t *buf, int val);
            if (dos_native_exit_jmpbuf) {
                serial_puts("  [DOS-NT] panic recovery -> long-jump to shell\n");
                uint64_t kcr3 = paging_get_kernel_cr3();
                if (kcr3) __asm__ volatile ("mov %0, %%cr3"
                                             :: "r"(kcr3) : "memory");
                kern_longjmp(dos_native_exit_jmpbuf, 3);
            }
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

    /* APIC MMIO via the upper-half mirror so reads/writes work from
     * any process CR3. paging_init's first-4GB mirror covers this
     * range; explicitly map as MMIO (uncacheable) just in case the
     * mirror flags don't already include WT/CD. */
    extern int paging_map_mmio(uint64_t phys, uint64_t size);
    paging_map_mmio(apic_phys, 4096);
    apic_base = (volatile uint32_t *)PHYS_TO_VIRT(apic_phys);

    serial_puts("[IDT] APIC base: 0x");
    serial_puthex(apic_phys, 16);
    serial_puts("\n");

    /* Enable APIC via SVR (spurious vector = 0xFF) */
    apic_write(APIC_SVR, APIC_SVR_ENABLE | 0xFF);

    /* Clear ESR */
    apic_write(APIC_ESR, 0);

    /* ── Detect TSC-deadline support ── */
    {
        uint32_t eax, ebx, ecx, edx;
        __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
        bool has_tsc_deadline = (ecx >> 24) & 1;

        /* Also require invariant TSC (CPUID.80000007H:EDX bit 8) */
        bool has_invariant_tsc = false;
        __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000007));
        has_invariant_tsc = (edx >> 8) & 1;

        tsc_deadline_mode = has_tsc_deadline && has_invariant_tsc;
    }

    /* ── PIT-based calibration (used for both periodic and TSC-deadline) ── */
    #define PIT_FREQ   1193182ULL
    #define PIT_10MS   11932       /* PIT_FREQ / 100 */
    #define PIT_CH2_GATE 0x61
    #define PIT_CH2_MODE 0x43
    #define PIT_CH2_DATA 0x42

    /* Program PIT channel 2 for one-shot countdown */
    outb(PIT_CH2_MODE, 0xB0);         /* ch2, lobyte/hibyte, mode 0, binary */
    outb(PIT_CH2_DATA, PIT_10MS & 0xFF);
    outb(PIT_CH2_DATA, PIT_10MS >> 8);

    /* Gate on: start PIT countdown */
    uint8_t gate = inb(PIT_CH2_GATE);
    outb(PIT_CH2_GATE, (gate & 0xFD) | 0x01);  /* bit 0 = gate, bit 1 = spkr off */

    /* Start APIC timer with max count + record TSC */
    apic_write(APIC_TIMER_DIV, 0x03);  /* divide by 16 */
    apic_write(APIC_LVT_TIMER, APIC_TIMER_PERIODIC | 32);
    apic_write(APIC_TIMER_INIT, 0xFFFFFFFF);
    uint64_t tsc_cal_start = idt_rdtsc();

    /* Wait for PIT to finish (bit 5 of port 0x61 goes high) */
    while (!(inb(PIT_CH2_GATE) & 0x20))
        __asm__ volatile ("pause");

    /* Stop APIC timer, read elapsed ticks + TSC */
    apic_write(APIC_LVT_TIMER, APIC_LVT_MASKED);
    uint32_t elapsed = 0xFFFFFFFF - apic_read(APIC_TIMER_CURR);
    uint64_t tsc_cal_end = idt_rdtsc();

    /* Calibrate TSC: PIT measured ~10ms, so TSC freq = elapsed_tsc * 100 */
    tsc_freq = (tsc_cal_end - tsc_cal_start) * 100;

    if (tsc_deadline_mode) {
        /* ── TSC-Deadline mode: sub-microsecond precision ── */
        apic_write(APIC_LVT_TIMER, APIC_TIMER_TSC_DEADLINE | 32);

        /* Program initial 10ms deadline */
        tsc_last_tick = idt_rdtsc();
        wrmsr(MSR_IA32_TSC_DEADLINE, tsc_last_tick + tsc_freq / 100);

        apic_timer_init_saved = 0;
        apic_enabled = true;

        serial_puts("[IDT] TSC-deadline mode, freq=");
        serial_putdec(tsc_freq / 1000000);
        serial_puts(" MHz\n");
    } else {
        /* ── Periodic mode fallback ── */
        uint32_t init_count = elapsed;
        if (init_count < 1000 || init_count > 100000000)
            init_count = 625000;

        apic_write(APIC_LVT_TIMER, APIC_TIMER_PERIODIC | 32);
        apic_write(APIC_TIMER_INIT, init_count);
        apic_timer_init_saved = init_count;
        apic_enabled = true;

        serial_puts("[IDT] APIC timer: periodic, init=");
        serial_putdec(init_count);
        serial_puts(" (~100 Hz), TSC ");
        serial_putdec(tsc_freq / 1000000);
        serial_puts(" MHz\n");
    }
}

uint32_t idt_get_apic_timer_init(void) { return apic_timer_init_saved; }

/* ── IDT init (public API) ───────────────────────────────────── */

void __initk idt_init(void)
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
    idt[6].ist  = 3;  /* #UD — IST3 (decoupled from INT 0x2E IST1 drift) */
    idt[13].ist = 3;  /* #GP — IST3 */
    idt[14].ist = 3;  /* #PF — IST3 (was IST1 — caused garbage-RBP crash
                       * when nested INT 0x2E lowered IST1 then a #PF
                       * fired and re-loaded RSP from the stale value) */
    /* idt[32].ist intentionally 0: timer uses current process stack so
     * kernel_rsp is unique per-process → context switch works correctly.
     * compat32 ring-0 RSP is always a valid 64-bit kernel address, safe. */

    /* Override: vector 0x71 = keyboard IRQ (uses isr_stub_33) */
    idt_set_entry(0x71, isr_stub_33, 0);
    idt[0x71].selector = cs;

    /* Vector 40: I211 NIC MSI interrupt */
    idt_set_entry(40, isr_stub_40, 0);
    idt[40].selector = cs;

    /* Vector 41: RTL8111 NIC MSI interrupt */
    idt_set_entry(41, isr_stub_41, 0);
    idt[41].selector = cs;

    /* SMP work IPI: lightweight stub, no fxsave (safe for APs) */
    extern void isr_stub_smp_ipi(void);
    idt_set_entry(0xFE, isr_stub_smp_ipi, 0);
    idt[0xFE].selector = cs;

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
