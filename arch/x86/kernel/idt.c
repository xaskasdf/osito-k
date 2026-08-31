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
#include "../include/interrupt.h"
#include "../include/cpu_features.h"
#include "../win32/compat32.h"
#include "smp.h"

/* ── External functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_putchar(char c);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);
extern void fb_puts_color(const char *s, uint32_t color);
extern void fb_putdec(uint64_t val);
extern void fb_puthex(uint64_t val, int digits);
extern void boot_diag_flush(const char *reason) __attribute__((weak));

static inline void idt_diag_flush(const char *reason)
{
    if (boot_diag_flush)
        boot_diag_flush(reason);
}

/* X-SCHED: scheduler tick (process.c) */
extern void sched_tick(void *frame);
extern void sched_yield(void);

/* Paging (paging.c) */
extern int  paging_set_flags(uint64_t virt, uint64_t flags);
extern uint64_t *paging_get_pte(uint64_t virt);
extern uint64_t mem_get_total(void);
extern void mem_debug_dump_page(uint64_t phys);
#define PTE_PRESENT  (1ULL << 0)
#define PTE_WRITABLE (1ULL << 1)
#define PTE_GLOBAL   (1ULL << 8)
#define PTE_NX       (1ULL << 63)

static bool debug_read_u64_in_cr3(uint64_t cr3, uint64_t virt,
                                  uint64_t *value, uint64_t *phys_out)
{
    uint64_t phys = paging_translate_in_cr3(cr3, virt);
    if (phys == UINT64_MAX || (phys & 0xFFFULL) > 0xFF8ULL)
        return false;
    if (value)
        *value = *(volatile uint64_t *)PHYS_TO_VIRT(phys);
    if (phys_out)
        *phys_out = phys;
    return true;
}

static bool current_win32_exe_is(const char *wanted)
{
    extern const char *win32_current_exe_name(void);
    const char *name = win32_current_exe_name();
    const char *base = name;

    if (!name || !wanted) return false;
    for (const char *p = name; *p; p++)
        if (*p == '\\' || *p == '/') base = p + 1;

    while (*base && *wanted) {
        char a = *base++;
        char b = *wanted++;
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return false;
    }
    return *base == 0 && *wanted == 0;
}

static bool current_cr3_range_is_mapped(uint64_t address, uint64_t size)
{
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    for (uint64_t offset = 0; offset < size; offset += 4096)
        if (paging_translate_in_cr3(cr3, address + offset) == UINT64_MAX)
            return false;
    return true;
}

/* A continuable write fault can be part of cross-thread synchronization: one
 * thread temporarily write-protects a page and the fault handler asks the OS
 * to retry once its peer restores write access. Windows can preempt the
 * faulting thread during that retry window. Our compat callback path can mask
 * the LAPIC timer briefly, so yield explicitly while the live PTE remains
 * read-only. The bounded loop preserves normal fault semantics for handlers
 * that did not arrange a protection change. */
static void compat32_wait_for_write_retry(uint64_t address)
{
    const uint32_t max_yields = 64;
    uint32_t yields = 0;
    uint64_t entry_flags;
    uint64_t *pte = paging_get_pte(address);
    uint64_t pte_value = pte
                       ? __atomic_load_n(pte, __ATOMIC_ACQUIRE) : 0;

    if (!(pte_value & PTE_PRESENT))
        return;

    __asm__ volatile ("pushfq; popq %0" : "=r"(entry_flags));
    while (!(pte_value & PTE_WRITABLE) && yields < max_yields) {
        if (!(entry_flags & (1ULL << 9)))
            __asm__ volatile ("sti" ::: "memory");
        sched_yield();
        if (!(entry_flags & (1ULL << 9)))
            __asm__ volatile ("cli" ::: "memory");

        yields++;
        pte = paging_get_pte(address);
        pte_value = pte
                  ? __atomic_load_n(pte, __ATOMIC_ACQUIRE) : 0;
        if (!(pte_value & PTE_PRESENT))
            break;
    }

    if ((pte_value & (PTE_PRESENT | PTE_WRITABLE)) ==
        (PTE_PRESENT | PTE_WRITABLE)) {
        __asm__ volatile ("invlpg (%0)" :: "r"(address) : "memory");
        serial_puts("[SEH32-WRITE-WAIT] resumed address=0x");
        serial_puthex(address, 16);
        serial_puts(" yields=");
        serial_putdec(yields);
        serial_puts("\n");
    } else if (yields == max_yields) {
        static uint32_t timeout_logs;
        if (timeout_logs++ < 16) {
            serial_puts("[SEH32-WRITE-WAIT] still protected address=0x");
            serial_puthex(address, 16);
            serial_puts("\n");
        }
    }
}

/* NULL page policy: page 0 is read-only+NX. Native ELF user code must fault
 * normally on NULL writes so the crash reporter sees the real bug. Compat32 PE
 * faults are routed to SEH. Only kernel/bootstrap code may temporarily enable
 * write-through, then #DB restores the guard flags. */
volatile int g_null_page_dirty = 0;
uint32_t g_base_seh_frame_addr = 0;  /* winexec base SEH frame on PE32 stack */

/* NULL-page cleanup after the narrow kernel/bootstrap write-through path.
 *
 * Do not memset VA 0 here. That crashed demand-paged ELF startup in the past
 * by faulting recursively from the exception handler. The important invariant
 * is to restore page 0 to read-only+NX so later NULL writes fault instead of
 * silently corrupting the guard page. */
static void null_page_clean(void)
{
    paging_set_flags(0, PTE_PRESENT | PTE_GLOBAL | PTE_NX);
    __asm__ volatile ("invlpg (%0)" :: "r"((uint64_t)0) : "memory");
    g_null_page_dirty = 0;
}

static uint64_t idt_stack_compare_addr(uint64_t addr)
{
    uint64_t total = mem_get_total();

    if (total != 0 && addr >= KERNEL_VBASE) {
        uint64_t low = addr - KERNEL_VBASE;
        if (low < total) return low;
    }
    return addr;
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

typedef x86_interrupt_frame_t interrupt_frame_t;

static void compat32_apply_cpu_context(interrupt_frame_t *frame,
                                       const compat32_cpu_context_t *context)
{
    /* NtContinue-style return accepts arithmetic/debug state but does not let
     * a PE32 handler alter privileged flags or escape the compat selectors. */
    const uint64_t mutable_eflags = 0x00250DD5ULL;
    frame->rax = context->eax;
    frame->rbx = context->ebx;
    frame->rcx = context->ecx;
    frame->rdx = context->edx;
    frame->rsi = context->esi;
    frame->rdi = context->edi;
    frame->rbp = context->ebp;
    frame->rsp = context->esp;
    frame->rip = context->eip;
    frame->rflags = (frame->rflags & ~mutable_eflags) |
                    ((uint64_t)context->eflags & mutable_eflags);
}

/* NT delivers user-mode CPU faults to SEH before treating them as process
 * crashes. HotSpot relies on this for safepoint polling pages, where a
 * protection fault is expected control flow. Return 1 when SEH handled the
 * exception, 0 when it was unhandled, and -1 for unsupported vectors. */
static int compat32_dispatch_cpu_exception(interrupt_frame_t *frame,
                                           uint64_t vector)
{
    uint16_t cs = (uint16_t)(frame->cs & 0xFFFF);
    if ((cs != 0x40 && cs != 0x23) ||
        (vector != 6 && vector != 13 && vector != 14))
        return -1;

    typedef struct {
        uint32_t ExceptionCode;
        uint32_t ExceptionFlags;
        uint64_t ExceptionRecord;
        uint64_t ExceptionAddress;
        uint32_t NumberParameters;
        uint32_t pad;
        uint64_t ExceptionInformation[15];
    } exception_record64_t;

    exception_record64_t record;
    for (uint32_t i = 0; i < sizeof(record) / sizeof(uint32_t); i++)
        ((uint32_t *)&record)[i] = 0;

    uint64_t fault_address = 0;
    if (vector == 14) {
        __asm__ volatile ("mov %%cr2, %0" : "=r"(fault_address));
        if (fault_address < 0x1000 && !(frame->error_code & 16) &&
            cs != 0x40)
            return -1;
        record.ExceptionCode = 0xC0000005; /* STATUS_ACCESS_VIOLATION */
        record.NumberParameters = 2;
        record.ExceptionInformation[0] =
            (frame->error_code & 2) ? 1 : 0;
        record.ExceptionInformation[1] = (uint32_t)fault_address;
    } else if (vector == 6) {
        record.ExceptionCode = 0xC000001D; /* STATUS_ILLEGAL_INSTRUCTION */
    } else {
        record.ExceptionCode = 0xC0000005; /* STATUS_ACCESS_VIOLATION */
    }
    record.ExceptionAddress = (uint32_t)frame->rip;

    uint16_t seg_ds, seg_es, seg_fs, seg_gs;
    __asm__ volatile ("movw %%ds, %0" : "=r"(seg_ds));
    __asm__ volatile ("movw %%es, %0" : "=r"(seg_es));
    __asm__ volatile ("movw %%fs, %0" : "=r"(seg_fs));
    __asm__ volatile ("movw %%gs, %0" : "=r"(seg_gs));
    compat32_cpu_context_t context = {
        .eax = (uint32_t)frame->rax,
        .ebx = (uint32_t)frame->rbx,
        .ecx = (uint32_t)frame->rcx,
        .edx = (uint32_t)frame->rdx,
        .esi = (uint32_t)frame->rsi,
        .edi = (uint32_t)frame->rdi,
        .ebp = (uint32_t)frame->rbp,
        .esp = (uint32_t)frame->rsp,
        .eip = (uint32_t)frame->rip,
        .eflags = (uint32_t)frame->rflags,
        .seg_cs = (uint32_t)frame->cs,
        .seg_ss = (uint32_t)frame->ss,
        .seg_ds = seg_ds,
        .seg_es = seg_es,
        .seg_fs = seg_fs,
        .seg_gs = seg_gs,
    };

    if (g_null_page_dirty)
        null_page_clean();

    if (!compat32_seh_dispatch_cpu((PEXCEPTION_RECORD)&record, &context))
        return 0;

    extern uint32_t g_compat32_unwind_eip;
    extern uint32_t g_compat32_unwind_esp;
    extern uint32_t g_compat32_unwind_ebp;
    if (g_compat32_unwind_eip) {
        frame->rip = g_compat32_unwind_eip;
        frame->rsp = g_compat32_unwind_esp;
        frame->rbp = g_compat32_unwind_ebp;
        extern uint32_t g_compat32_unwind_restore_nonvolatile;
        if (g_compat32_unwind_restore_nonvolatile) {
            extern uint32_t g_compat32_unwind_ebx;
            extern uint32_t g_compat32_unwind_esi;
            extern uint32_t g_compat32_unwind_edi;
            frame->rbx = g_compat32_unwind_ebx;
            frame->rsi = g_compat32_unwind_esi;
            frame->rdi = g_compat32_unwind_edi;
            g_compat32_unwind_restore_nonvolatile = 0;
        }
        g_compat32_unwind_eip = 0;
        g_compat32_unwind_esp = 0;
        g_compat32_unwind_ebp = 0;
    } else {
        compat32_apply_cpu_context(frame, &context);
    }

    if (vector == 14 && fault_address >= 0x1000 &&
        (frame->error_code & 3) == 3 &&
        frame->rip == record.ExceptionAddress)
        compat32_wait_for_write_retry(fault_address);

    return 1;
}

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
extern void isr_stub_41(void);   /* Win32 __fastfail (INT 0x29) */
extern void isr_stub_42(void);   /* RTL8111 NIC MSI */
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

#define GDT_MAX_ENTRIES 68
#define GDT_TSS_BSP_INDEX 32
#define GDT_TSS_AP_INDEX(cpu_index) (34 + ((cpu_index) * 2))
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
     * Native ELF starts at CS=0x28/SS=0x30. IDT gates use CS=0x38.
     * SYSCALL uses a private 0x90/0x98 pair so it never depends on
     * UEFI's selector 0x10, which may be a compat descriptor on hardware. */
    if (entries < 20) entries = 20;  /* Extend if UEFI GDT was smaller */
    kernel_gdt[5] = 0x00AF9A000000FFFFULL; /* 0x28: 64-bit code (P=1,DPL=0,S=1,type=0xA,L=1,G=1) */
    kernel_gdt[6] = 0x00CF92000000FFFFULL; /* 0x30: 64-bit data (P=1,DPL=0,S=1,type=0x2,G=1) */
    kernel_gdt[7] = 0x00AF9A000000FFFFULL; /* 0x38: 64-bit code (same as 0x28) */
    kernel_gdt[18] = 0x00AF9A000000FFFFULL; /* 0x90: 64-bit SYSCALL code */
    kernel_gdt[19] = 0x00CF92000000FFFFULL; /* 0x98: SYSCALL data */

    /* Keep the complete table visible so AP-specific TSS descriptors can be
     * installed after heap initialization without reloading every CPU's GDTR. */
    for (int i = entries; i < GDT_MAX_ENTRIES; i++)
        kernel_gdt[i] = 0;
    for (int i = GDT_TSS_BSP_INDEX; i < GDT_MAX_ENTRIES; i++)
        kernel_gdt[i] = 0;
    kernel_gdtr.limit = (uint16_t)(sizeof(kernel_gdt) - 1);
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
    uint64_t rsp0;      /* Ring-3 compatibility transitions */
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;      /* IST1: INT 0x2E (compat32 dispatch) */
    uint64_t ist2;      /* IST2: debug and DOS traps */
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
uint64_t *tss_ist3_ptr;  /* = &kernel_tss.ist3, process-owned fault stack */

/* IST1 stack for INT 0x2E — 1MB.
 * int2e_stub.S reserves 16KB per nest (subq $16384). UT99's C++ EH
 * unwind chain re-throws DEEPLY during the boot exception storm (the
 * FName/package recovery throws ~30 C++ exceptions, several nesting via
 * appUnwindf re-throw) → that many nested INT 0x2E entries. 256KB at
 * 32KB/level = only 8 levels: deeper storms walked IST1 BELOW this array
 * into kernel BSS/.text and corrupted it → flaky boot #UD / wild kernel
 * write (CR2 in the kernel-image range, RSP pointing into .text). Adding
 * unrelated kernel BSS shifted what got clobbered and made it
 * deterministic. 1MB at 16KB/level = 64 nesting levels with ample
 * per-level headroom (the kernel call chain per level is ~1-3KB). */
#define IST1_STACK_SIZE 1048576
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

/* Regular asynchronous interrupts use IST4 rather than the interrupted SysV
 * stack. The BSP scheduler copies live frames out before IST4 can be reused. */
#define IST4_STACK_SIZE 262144
#define IST5_STACK_SIZE 32768
#define IST6_STACK_SIZE 32768
#define IST7_STACK_SIZE 32768
static uint8_t ist4_stack[IST4_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t ist5_stack[IST5_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t ist6_stack[IST6_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t ist7_stack[IST7_STACK_SIZE] __attribute__((aligned(16)));

/* APs do not run compat32/DOS dispatch, but every IDT IST selector still needs
 * valid CPU-local storage for faults and unexpected software interrupts. */
#define AP_IST1_STACK_SIZE 32768
#define AP_IST2_STACK_SIZE 32768
#define AP_IST3_STACK_SIZE 65536
#define AP_IST4_STACK_SIZE 262144
#define AP_IST5_STACK_SIZE 32768
#define AP_IST6_STACK_SIZE 32768
#define AP_IST7_STACK_SIZE 32768
#define AP_IST_TOTAL_SIZE (AP_IST1_STACK_SIZE + AP_IST2_STACK_SIZE + \
                           AP_IST3_STACK_SIZE + AP_IST4_STACK_SIZE + \
                           AP_IST5_STACK_SIZE + AP_IST6_STACK_SIZE + \
                           AP_IST7_STACK_SIZE)

typedef struct {
    struct tss64 tss;
    uint8_t *stacks;
    uint16_t selector;
    bool prepared;
} ap_tss_state_t;

static ap_tss_state_t ap_tss[SMP_MAX_CPUS] __attribute__((aligned(16)));

static void tss_install_descriptor(uint32_t index, struct tss64 *tss)
{
    uint64_t base = (uint64_t)tss;
    uint32_t limit = sizeof(*tss) - 1;
    uint64_t lo = 0;

    lo |= (uint64_t)(limit & 0xFFFF);
    lo |= (uint64_t)(base & 0xFFFF) << 16;
    lo |= (uint64_t)((base >> 16) & 0xFF) << 32;
    lo |= (uint64_t)0x89ULL << 40; /* available 64-bit TSS, present */
    lo |= (uint64_t)((limit >> 16) & 0xF) << 48;
    lo |= (uint64_t)((base >> 24) & 0xFF) << 56;

    kernel_gdt[index] = lo;
    kernel_gdt[index + 1] = (base >> 32) & 0xFFFFFFFF;
    __asm__ volatile ("mfence" ::: "memory");
}

static void tss_init(void)
{
    /* BSP TSS uses permanent, mutually independent stacks. */
    memset(&kernel_tss, 0, sizeof(kernel_tss));
    kernel_tss.ist1 = (uint64_t)(ist1_stack + IST1_STACK_SIZE);
    kernel_tss.ist2 = (uint64_t)(ist2_stack + IST2_STACK_SIZE);
    kernel_tss.ist3 = (uint64_t)(ist3_stack + IST3_STACK_SIZE);
    kernel_tss.ist4 = (uint64_t)(ist4_stack + IST4_STACK_SIZE);
    kernel_tss.ist5 = (uint64_t)(ist5_stack + IST5_STACK_SIZE);
    kernel_tss.ist6 = (uint64_t)(ist6_stack + IST6_STACK_SIZE);
    kernel_tss.ist7 = (uint64_t)(ist7_stack + IST7_STACK_SIZE);
    kernel_tss.rsp0 = kernel_tss.ist4;
    kernel_tss.iopb_offset = sizeof(struct tss64);
    tss_ist1_ptr = &kernel_tss.ist1;
    tss_ist2_ptr = &kernel_tss.ist2;
    tss_ist3_ptr = &kernel_tss.ist3;

    tss_install_descriptor(GDT_TSS_BSP_INDEX, &kernel_tss);

    /* Load Task Register */
    uint16_t tss_sel = GDT_TSS_BSP_INDEX * 8;
    __asm__ volatile ("ltr %0" : : "r"(tss_sel));

    serial_puts("[TSS] BSP installed, IST1=0x");
    serial_puthex(kernel_tss.ist1, 16);
    serial_puts(" IST4=0x");
    serial_puthex(kernel_tss.ist4, 16);
    serial_puts("\n");
}

/* The BSP prepares each AP's TSS before SIPI, while allocation and GDT
 * mutation are serialized. */
int x86_tss_prepare_ap(uint32_t cpu_index)
{
    if (cpu_index >= SMP_MAX_CPUS) return -1;
    ap_tss_state_t *state = &ap_tss[cpu_index];
    if (state->prepared) return 0;

    extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);
    void *phys = mem_alloc_aligned(AP_IST_TOTAL_SIZE, 4096);
    if (!phys) return -1;

    uint8_t *cursor = (uint8_t *)PHYS_TO_VIRT(phys);
    state->stacks = cursor;
    memset(cursor, 0, AP_IST_TOTAL_SIZE);
    memset(&state->tss, 0, sizeof(state->tss));

    cursor += AP_IST1_STACK_SIZE; state->tss.ist1 = (uint64_t)cursor;
    cursor += AP_IST2_STACK_SIZE; state->tss.ist2 = (uint64_t)cursor;
    cursor += AP_IST3_STACK_SIZE; state->tss.ist3 = (uint64_t)cursor;
    cursor += AP_IST4_STACK_SIZE; state->tss.ist4 = (uint64_t)cursor;
    cursor += AP_IST5_STACK_SIZE; state->tss.ist5 = (uint64_t)cursor;
    cursor += AP_IST6_STACK_SIZE; state->tss.ist6 = (uint64_t)cursor;
    cursor += AP_IST7_STACK_SIZE; state->tss.ist7 = (uint64_t)cursor;
    state->tss.rsp0 = state->tss.ist4;
    state->tss.iopb_offset = sizeof(struct tss64);

    uint32_t index = GDT_TSS_AP_INDEX(cpu_index);
    state->selector = (uint16_t)(index * 8);
    tss_install_descriptor(index, &state->tss);
    state->prepared = true;
    return 0;
}

/* The AP loads TR before programming its LAPIC timer or enabling IF. */
int x86_tss_load_ap(uint32_t cpu_index)
{
    if (cpu_index >= SMP_MAX_CPUS || !ap_tss[cpu_index].prepared)
        return -1;
    uint16_t selector = ap_tss[cpu_index].selector;
    __asm__ volatile ("ltr %0" : : "r"(selector));
    return 0;
}

void x86_tss_reset_ist1(void)
{
    kernel_tss.ist1 = (uint64_t)(ist1_stack + IST1_STACK_SIZE);
}

void x86_tss_reset_ist3(void)
{
    kernel_tss.ist3 = (uint64_t)(ist3_stack + IST3_STACK_SIZE);
}

void x86_tss_reset_rsp0(void)
{
    kernel_tss.rsp0 = (uint64_t)(ist4_stack + IST4_STACK_SIZE);
}

/* ── State ───────────────────────────────────────────────────── */

static volatile uint64_t tick_count;
static bool apic_enabled;
static uint32_t apic_timer_init_saved;
uint32_t bsp_apic_id_global;
uint32_t isr_tsc_aux_enabled;

/* TSC-deadline mode state */
static bool     tsc_deadline_mode;
static uint64_t tsc_freq;              /* TSC cycles per second */
static uint64_t tsc_last_tick;         /* TSC value of last virtual 100Hz tick */
static uint64_t tsc_time_base;         /* TSC epoch used for monotonic time */
static uint64_t tsc_time_base_ns;      /* Time at tsc_time_base */
#define MSR_IA32_TSC_DEADLINE  0x6E0
#define MSR_IA32_TSC_AUX       0xC0000103
#define APIC_TIMER_TSC_DEADLINE  0x40000  /* LVT bits 18:17 = 10b */

static inline uint64_t idt_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

uint64_t idt_get_ticks(void) { return tick_count; }
uint64_t idt_get_tsc_freq(void) { return tsc_freq; }
uint64_t idt_get_monotonic_ns(void)
{
    uint64_t freq = tsc_freq;
    uint64_t base = tsc_time_base;

    if (!freq || !base)
        return tick_count * 10000000ULL;

    uint64_t elapsed = idt_rdtsc() - base;
    uint64_t seconds = elapsed / freq;
    uint64_t remainder = elapsed % freq;

    return tsc_time_base_ns + seconds * 1000000000ULL +
           (remainder * 1000000000ULL) / freq;
}
bool     idt_tsc_deadline_active(void) { return tsc_deadline_mode; }
uint32_t idt_get_bsp_apic_id(void) { return bsp_apic_id_global; }

void idt_set_current_apic_id(uint32_t apic_id)
{
    if (isr_tsc_aux_enabled)
        wrmsr(MSR_IA32_TSC_AUX, apic_id & 0xFFU);
}

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
                    idt_diag_flush("dos-crash-recover");
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

    /* UT99-specific recovery for attempts to execute invalid bytecode as
     * native instructions. Other PE32 programs legitimately load modules and
     * JIT code above 0x20000000; their faults must reach the SEH dispatcher. */
    extern int g_compat32_ut99;
    if ((frame->cs & 0xFFFF) == 0x40 &&
        g_compat32_ut99 && vec == 6 &&
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
        } else if ((g_swbreak_addr >= 0x00454D08 &&
                    g_swbreak_addr <= 0x00454D8B) ||
                   (g_swbreak_addr >= 0x004B6210 &&
                    g_swbreak_addr <= 0x004B674F)) {
            static const uint32_t steam_bisect[] = {
                0x00454D08, 0x00454D0F, 0x00454D14,
                0x004B6210, 0x004B6211, 0x004B631F, 0x004B6417,
                0x004B6536, 0x004B6602, 0x004B6650, 0x004B66FB,
                0x004B673E, 0x004B6741, 0x004B674F,
                0x00454D41, 0x00454D8B
            };
            serial_puts(g_swbreak_addr == 0x00454D08 ? " EAX=0x" : " EDI=0x");
            serial_puthex(g_swbreak_addr == 0x00454D08
                              ? (uint32_t)frame->rax
                              : (uint32_t)frame->rdi,
                          8);
            if (g_swbreak_addr >= 0x004B6210) {
                uint32_t ebp = (uint32_t)frame->rbp;
                serial_puts(" ESP-EBP=");
                serial_putdec((int32_t)((uint32_t)frame->rsp - ebp));
                serial_puts(" slots=");
                serial_puthex(*(uint32_t *)(uintptr_t)(ebp - 20), 8);
                serial_puts("/");
                serial_puthex(*(uint32_t *)(uintptr_t)(ebp - 16), 8);
                serial_puts("/");
                serial_puthex(*(uint32_t *)(uintptr_t)(ebp - 12), 8);
            }
            serial_puts("\n");

            *(uint8_t *)(uintptr_t)g_swbreak_addr = g_swbreak_saved;
            frame->rip = g_swbreak_addr;
            for (uint32_t i = 0; i + 1 < sizeof(steam_bisect) / sizeof(steam_bisect[0]); i++) {
                if (steam_bisect[i] != g_swbreak_addr)
                    continue;
                g_swbreak_addr = steam_bisect[i + 1];
                g_swbreak_saved = *(uint8_t *)(uintptr_t)g_swbreak_addr;
                *(uint8_t *)(uintptr_t)g_swbreak_addr = 0xCC;
                return;
            }
            g_swbreak_addr = 0;
            return;
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

    /* Native Win32 children currently have no PE64 breakpoint-dispatch path.
     * Continue past an INT3 while debugging CEF instead of killing the helper
     * and leaving the parent blocked forever waiting for its service. The CPU
     * has already advanced RIP past the one-byte instruction. */
    if (vec == 3 && (frame->cs & 0xFFFF) == 0x38 &&
        frame->rip >= 0xFFFF800000000000ULL) {
        extern uint64_t *win32_current_child_jmpbuf(void);
        if (win32_current_child_jmpbuf()) {
            const uint8_t *code = (const uint8_t *)(frame->rip - 1);
            extern void dll_debug_log_address(void *address);
            serial_puts("[PE64-BP] continued at rip=0x");
            serial_puthex(frame->rip, 16);
            serial_puts(" pid=");
            serial_putdec(proc_current_pid());
            serial_puts("\n");
            dll_debug_log_address((void *)(frame->rip - 1));
            serial_puts("[PE64-BP-BYTES]");
            for (int i = 0; i < 16; i++) {
                serial_puts(" ");
                serial_puthex(code[i], 2);
            }
            serial_puts("\n");
            return;
        }
    }

    /* Chromium's official PE64 build uses `int3; ud2; xor eax,eax` for a
     * NOTREACHED diagnostic with a valid false-return fallback immediately
     * after the trap pair. Continue only that exact byte signature. */
    if (vec == 6 && (frame->cs & 0xFFFF) == 0x38 &&
        frame->rip >= 0xFFFF800000000001ULL) {
        const uint8_t *code = (const uint8_t *)frame->rip;
        extern uint64_t *win32_current_child_jmpbuf(void);
        if (win32_current_child_jmpbuf() && code[-1] == 0xCC &&
            code[0] == 0x0F && code[1] == 0x0B &&
            code[2] == 0x31 && code[3] == 0xC0) {
            serial_puts("[PE64-TRAP] continued fallback at rip=0x");
            serial_puthex(frame->rip, 16);
            serial_puts(" pid=");
            serial_putdec(proc_current_pid());
            serial_puts("\n");
            frame->rip += 2;
            return;
        }
    }

    /* UT99 IAT recovery is tied to Engine.dll's fixed PE32 layout. Never
     * probe those addresses in another process: the exception handler itself
     * would otherwise fault while trying to diagnose unrelated PE32 code. */
    if ((vec == 14 || vec == 6 /* #UD */) &&
        current_win32_exe_is("UnrealTournament.exe")) {
        uint64_t fault_rip = frame->rip;
        if (fault_rip >= 0x40000000ULL && fault_rip < 0x80000000ULL &&
            current_cr3_range_is_mapped(0x105A5000ULL, 7 * 4096ULL)) {
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
     * Validates against VMA table. Covers ELF segments, file-backed mmap
     * regions, and anonymous reservations in the low 32-bit VA pool.
     * Skip first 1MB (BIOS/bootloader) and null page (handled below). */
    if (vec == 14 && !(frame->error_code & 1)) {
        uint64_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        if (cr2 >= 0x100000ULL) {
            extern int nt_section_page_fault(uint64_t addr,
                                             uint64_t error_code);
            if (nt_section_page_fault(cr2, frame->error_code) == 0)
                return;
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
        /* Base time on elapsed TSC in both timer modes. sched_yield() raises
         * this vector in software, so blindly incrementing in periodic mode
         * made every cooperative yield advance the clock by 10 ms. */
        if (tsc_freq && tsc_last_tick) {
            uint64_t now_tsc = idt_rdtsc();
            uint64_t tsc_per_tick = tsc_freq / 100;
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

        /* UT99 IAT watchdog: restore Engine.dll StaticLoadClass on every tick.
         * The Unreal package loader overwrites this between INT 0x2E calls,
         * so the compat32_dispatch guard alone isn't fast enough. It runs
         * only for UT99 — without this gate the
         * watchdog would touch unmapped low VA from any process CR3. */
        extern int *proc_win32_compat32_mode_slot(void);
        extern int g_compat32_ut99;
        if (*proc_win32_compat32_mode_slot() && g_compat32_ut99) {
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

        /* Restore NULL-page guard flags after any narrow write-through path.
         * null_page_clean() no longer writes through VA 0; it only clears
         * writable and NX-protects the guard page. */
        if (g_null_page_dirty) null_page_clean();

        /* Always-on profiling: record RIP at each timer tick (~3 cycles) */
        {
            extern void kprof_record(uint64_t rip);
            kprof_record(frame->rip);
        }

        /* X-SCHED: preemptive scheduler — check quantum, switch if expired.
         * frame points to saved GPRs on the BSP's IRQ IST. */
        uint64_t interrupted_rip = frame->rip;
        sched_tick(frame);

        /* With no context switch pending, isr_common consumes this live IST
         * frame. A timer tick must not rewrite its return RIP. */
        {
            extern volatile uint64_t sched_switch_rsp;
            static uint32_t live_rip_clobber_logs;
            if (!sched_switch_rsp && frame->rip != interrupted_rip) {
                if (live_rip_clobber_logs++ < 32) {
                    serial_puts("[SCHED-LIVE-RIP-CLOBBER] old=0x");
                    serial_puthex(interrupted_rip, 16);
                    serial_puts(" new=0x");
                    serial_puthex(frame->rip, 16);
                    serial_puts("\n");
                }
                frame->rip = interrupted_rip;
            }
        }

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

    /* Windows reserves INT 0x29 for __fastfail. Keep it separate from
     * hardware MSI vectors and terminate the active Win32 process without
     * attempting recoverable SEH dispatch. */
    if (vec == 41) {
        const int32_t fast_fail_status = (int32_t)0xC0000409u;
        serial_puts("[WIN32-FASTFAIL] code=0x");
        serial_puthex((uint32_t)frame->rcx, 8);
        serial_puts(" rip=0x");
        serial_puthex(frame->rip, 16);
        serial_puts("\n");

        extern int win32_terminate_current_child(int32_t status);
        extern int win32_terminate_current_main(int32_t status);
        if (win32_terminate_current_child(fast_fail_status) ||
            win32_terminate_current_main(fast_fail_status))
            return;

        extern void proc_exit(int32_t code);
        if (proc_current_pid() > 1)
            proc_exit(fast_fail_status);
        return;
    }

    /* RTL8111 NIC MSI interrupt */
    if (vec == 42) {
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

        /* WRITE fault on page 0.
         *
         * Native ELF user code (CS=0x28) must see a real SIGSEGV/crash
         * report. Only kernel/bootstrap selectors keep the historical
         * write-through path for early page-table setup. Compat32 PE code
         * falls through to the SEH dispatch below. */
        if (cr2 < 0x1000 && (frame->error_code & 2) && !(frame->error_code & 16)) {
            /* Null-pointer writes from PE32 code (CS=0x40).
             * Route through the normal SEH dispatch at the end of this
             * function — the engine's __except handlers (including the
             * appFailAssert crash reporter) catch EXCEPTION_ACCESS_VIOLATION.
             * Write-through was masking real null-pointer dereference bugs
             * (e.g. SoftDrv+0x361D4 writing to NULL+0x198 during post-init
             * rendering), turning them into cascading kernel RIP=0 crashes.
             *
             * Kernel/bootstrap code still gets write-through — boot-time
             * page-table setup touches low addresses legitimately. */
            int is_compat32 = ((frame->cs & 0xFFFF) == 0x40);
            uint16_t cs16 = (uint16_t)(frame->cs & 0xFFFF);
            int is_kernel_context = (cs16 == 0x38 || cs16 == 0x08);
            if (!is_compat32 && is_kernel_context) {
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
                /* FMW pool crash (UT.exe 0x109022BE: `mov [edx+4],eax`,
                 * edx=pool->[0x14]=0). Dump the pool local [ebp-0x18] + fields
                 * to see if it's a valid pool with FirstMem(+0x14)=NULL or a
                 * corrupted local. Logged once. */
                if ((uint32_t)frame->rip == 0x109022BE) {
                    static int fmw_dumped = 0;
                    if (!fmw_dumped) {
                        fmw_dumped = 1;
                        uint32_t ebp = (uint32_t)frame->rbp;
                        uint32_t pool = (ebp >= 0x10000 && ebp < 0x80000000)
                            ? *(volatile uint32_t *)(uintptr_t)(ebp - 0x18) : 0;
                        serial_puts("[FMW-CRASH] edx=0x");
                        serial_puthex((uint32_t)frame->rdx, 8);
                        serial_puts(" ecx=0x"); serial_puthex((uint32_t)frame->rcx, 8);
                        serial_puts(" pool[ebp-18]=0x"); serial_puthex(pool, 8);
                        if (pool >= 0x10000 && pool < 0x80000000) {
                            serial_puts(" +08=0x"); serial_puthex(*(volatile uint32_t *)(uintptr_t)(pool+0x08), 8);
                            serial_puts(" +14=0x"); serial_puthex(*(volatile uint32_t *)(uintptr_t)(pool+0x14), 8);
                            serial_puts(" +18=0x"); serial_puthex(*(volatile uint32_t *)(uintptr_t)(pool+0x18), 8);
                            serial_puts(" +1c=0x"); serial_puthex(*(volatile uint32_t *)(uintptr_t)(pool+0x1c), 8);
                        }
                        serial_puts("\n");
                    }
                }
                /* FMW-POOL-SKIP: a NULL-target write (CR2 in the NULL page)
                 * from UT.exe's FMallocWindows pool manager (Malloc/Free/
                 * Link/Unlink, ~0x10902000..0x10903400). These are
                 * `*pool->PrevLink = …`, `*head = …`, `FirstMem->… = …` etc.
                 * on a pool that isn't linked / has no free blocks (PrevLink
                 * or FirstMem == NULL). Skipping ONLY the faulting (NULL)
                 * write — vs the old blanket NOP that also dropped VALID
                 * writes and corrupted the lists — keeps the list updates
                 * intact and just no-ops the meaningless NULL store. Decode
                 * the mov length and advance past it. */
                if ((uint32_t)cr2 < 0x1000 && (frame->cs & 0xFFFF) == 0x40 &&
                    (uint32_t)frame->rip >= 0x10902000 &&
                    (uint32_t)frame->rip <  0x10903400) {
                    volatile uint8_t *ins = (volatile uint8_t *)(uintptr_t)(uint32_t)frame->rip;
                    uint8_t op = ins[0];
                    if (op == 0x89 || op == 0x8B || op == 0x88 ||
                        op == 0x8A || op == 0xC7) {
                        uint8_t modrm = ins[1];
                        int mod = modrm >> 6, rm = modrm & 7;
                        int len = 2;
                        if (mod != 3 && rm == 4) len++;          /* SIB */
                        if (mod == 1) len += 1;                  /* disp8 */
                        else if (mod == 2) len += 4;             /* disp32 */
                        else if (mod == 0 && rm == 5) len += 4;  /* disp32 no base */
                        if (op == 0xC7) len += 4;                /* imm32 */
                        static int fmw_skip = 0;
                        if (fmw_skip < 24) {
                            fmw_skip++;
                            /* FMW pool alias probe: dump the
                             * freed block ptr ([ebp+8]), the GMalloc this
                             * ([ebp-0x28]), the FPoolInfo node ([ebp-0x14]) and
                             * its pool(+0x10)/PrevLink(+0x1c). The block ptr's
                             * 64KB slot vs a registered VA-ALLOC base tells us
                             * whether the PoolIndirect lookup misses because the
                             * block lives in an unregistered/aliased slot. */
                            uint32_t ebp = (uint32_t)frame->rbp;
                            uint32_t blk = 0, thiz = 0, node = 0;
                            if (ebp >= 0x10000 && ebp < 0x80000000) {
                                blk  = *(volatile uint32_t *)(uintptr_t)(ebp + 0x08);
                                thiz = *(volatile uint32_t *)(uintptr_t)(ebp - 0x28);
                                node = *(volatile uint32_t *)(uintptr_t)(ebp - 0x14);
                            }
                            serial_puts("[FMW-POOL-SKIP] @0x");
                            serial_puthex((uint32_t)frame->rip, 8);
                            serial_puts(" CR2=0x"); serial_puthex((uint32_t)cr2, 4);
                            serial_puts(" blk=0x"); serial_puthex(blk, 8);
                            serial_puts(" slot=0x"); serial_puthex((blk >> 16) & 0xff, 2);
                            serial_puts(" node=0x"); serial_puthex(node, 8);
                            if (node >= 0x10000 && node < 0x80000000) {
                                serial_puts(" pool=0x");
                                serial_puthex(*(volatile uint32_t *)(uintptr_t)(node + 0x10), 8);
                                serial_puts(" prevlink=0x");
                                serial_puthex(*(volatile uint32_t *)(uintptr_t)(node + 0x1c), 8);
                            }
                            serial_puts(" this=0x"); serial_puthex(thiz, 8);
                            serial_puts("\n");
                        }
                        frame->rip += len;
                        if (g_null_page_dirty) null_page_clean();
                        return;
                    }
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

            /* Normal null-page write-through for kernel init / boot path. */
            paging_set_flags(0, PTE_PRESENT | PTE_WRITABLE | PTE_GLOBAL | PTE_NX);
            __asm__ volatile ("invlpg (%0)" :: "r"((uint64_t)0) : "memory");
            frame->rflags |= (1ULL << 8);  /* TF bit — re-protect in #DB */
            g_null_page_dirty = 1;
            return;
            } /* end if (!is_compat32) */

            /* compat32 path (CS=0x40): fall through to SEH dispatch at the
             * end of this function.  The null-page is left read-only+NX;
             * the guest receives EXCEPTION_ACCESS_VIOLATION which the
             * engine's __except handler catches and reports properly. */
        }

        /* The NULL-CALL recovery path below is Win32 PE32 specific: it
         * reads from `frame->rsp & 0xFFFFFFFF` (truncated 32-bit RSP)
         * to recover the return address, dispatches to the engine SEH
         * chain, scans the IAT for redirect candidates, etc. Native
         * x86_64 SYSCALL-launched code (CS=0x28) with a NULL function pointer
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

            /* [RET0-DIAG] Always dump the recent native-shim call ring on the
             * first few near-NULL instruction-fetch faults, even when the
             * call-site can't be decoded as `call *disp32(reg)` (e.g. a RET to a
             * corrupted return address — the char-select-3x crash: RIP=0x13,
             * stack zeroed). The last shim in the ring is the prime suspect for
             * a wrong arg-count that over/under-cleaned the caller's stack. */
            if (null_call_count <= 3) {
                serial_puts("[RET0-DIAG] near-NULL fetch RIP=0x");
                serial_puthex(frame->rip & 0xFFFFFFFF, 8);
                serial_puts(" ESP=0x"); serial_puthex(frame->rsp & 0xFFFFFFFF, 8);
                serial_puts(" EBP=0x"); serial_puthex((uint32_t)frame->rbp, 8);
                serial_puts(" EBX=0x"); serial_puthex((uint32_t)frame->rbx, 8);
                serial_puts(" ESI=0x"); serial_puthex((uint32_t)frame->rsi, 8);
                serial_puts("\n  recent stack dwords:");
                uint32_t *sp = (uint32_t *)(uintptr_t)(frame->rsp & 0xFFFFFFFF);
                for (int si = 0; si < 12; si++) {
                    serial_puts(" 0x"); serial_puthex(sp[si], 8);
                }
                serial_puts("\n");
                extern void compat32_dump_recent_calls(void);
                compat32_dump_recent_calls();
            }

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
                    serial_puts(" callsite=0x"); serial_puthex(retaddr32 - 6, 8);
                    serial_puts(" ret=0x"); serial_puthex(retaddr32, 8);
                    serial_puts("\n");
                    /* Dump recent native calls to find the shim that corrupted
                     * the caller before this NULL virtual call (New-Game crash). */
                    { extern void compat32_dump_recent_calls(void);
                      compat32_dump_recent_calls(); }
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

                /* BROWSE-FIX removed (Phase 2 bisect: 0 fires; the
                 * GameEngine vtable is valid now, real Browse runs). */

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

                /* NULL-REDIRECT (IAT-disasm rescue) removed (Phase 2
                 * bisect: 0 fires with Phase 1 ABI fix + ENGINE-PATCH). */
            }

            if (null_call_count <= 10) {
                uint32_t retaddr =
                    ((uint32_t *)(uintptr_t)(frame->rsp & 0xFFFFFFFF))[0];
                serial_puts("[NULL-CALL] RIP=0x");
                serial_puthex(cr2, 4);
                serial_puts(" retaddr=0x");
                serial_puthex(retaddr, 8);
                serial_puts(" #");
                serial_putdec(null_call_count);
                serial_puts("\n");

                /* A NULL indirect call leaves the return address on the stack.
                 * Preserve the generated call-site bytes and register target so
                 * JIT/native-wrapper failures can be diagnosed after teardown. */
                if (retaddr >= 0x10010 && retaddr < 0x80000000) {
                    const uint8_t *code =
                        (const uint8_t *)(uintptr_t)(retaddr - 16);
                    serial_puts("[NULL-CALL-CODE] @0x");
                    serial_puthex(retaddr - 16, 8);
                    serial_puts(":");
                    for (int i = 0; i < 24; i++) {
                        serial_puts(" ");
                        serial_puthex(code[i], 2);
                    }
                    serial_puts("\n");

                    const uint8_t *call =
                        (const uint8_t *)(uintptr_t)(retaddr - 2);
                    if (call[0] == 0xFF &&
                        (call[1] & 0xF8) == 0xD0) {
                        uint8_t reg = call[1] & 7;
                        uint32_t regvals[8] = {
                            (uint32_t)frame->rax, (uint32_t)frame->rcx,
                            (uint32_t)frame->rdx, (uint32_t)frame->rbx,
                            0, (uint32_t)frame->rbp,
                            (uint32_t)frame->rsi, (uint32_t)frame->rdi
                        };
                        const char *rn[] = {
                            "eax", "ecx", "edx", "ebx",
                            "esp", "ebp", "esi", "edi"
                        };
                        serial_puts("[NULL-CALL-CODE] call *%");
                        serial_puts(rn[reg]);
                        serial_puts(" target=0x");
                        serial_puthex(regvals[reg], 8);
                        serial_puts("\n");
                    }
                }
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

    /* Deliver Win32 user exceptions before producing fatal diagnostics.
     * Handled access violations are normal for mechanisms such as HotSpot's
     * safepoint polling page. */
    int compat32_seh_result =
        compat32_dispatch_cpu_exception(frame, vec);
    if (compat32_seh_result > 0)
        return;

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

        /* Symbolize RIP for native ELF user crashes (CS=0x28, fixed
         * SYSCALL-return CS=0x90, or legacy buggy CS=0x10 dumps). The
         * symbolizer is kmalloc-safe and reads the per-process symbol
         * tables captured at elf_load time. For unmatched addresses or
         * demand-paged binaries (no symtab) it prints nothing. */
        {
            extern void *proc_current(void);
            extern bool user_symbolize(void *p, uint64_t addr,
                                       const char **name, uint64_t *off);
            uint16_t cs16 = (uint16_t)(frame->cs & 0xFFFF);
            if (cs16 == 0x28 || cs16 == 0x90 || cs16 == 0x10) {
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
        if (frame->rsp >= (uint64_t)ist3_stack &&
            frame->rsp < (uint64_t)(ist3_stack + IST3_STACK_SIZE)) {
            serial_puts("  [FAULT-IST-REENTRY] saved RSP is inside IST3\n");
        }

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

        serial_puts("  R8  = 0x"); serial_puthex(frame->r8, 16);
        serial_puts("  R9  = 0x"); serial_puthex(frame->r9, 16); serial_puts("\n");
        serial_puts("  R10 = 0x"); serial_puthex(frame->r10, 16);
        serial_puts("  R11 = 0x"); serial_puthex(frame->r11, 16); serial_puts("\n");
        serial_puts("  R12 = 0x"); serial_puthex(frame->r12, 16);
        serial_puts("  R13 = 0x"); serial_puthex(frame->r13, 16); serial_puts("\n");
        serial_puts("  R14 = 0x"); serial_puthex(frame->r14, 16);
        serial_puts("  R15 = 0x"); serial_puthex(frame->r15, 16); serial_puts("\n");

        /* Resolve the instruction page through the faulting address space and
         * dump its PMM history before process teardown can overwrite it. This
         * catches stale mappings where a live PE code page was freed and then
         * reused by an unrelated kernel allocation. */
        if (vec == 14) {
            uint64_t active_cr3;
            uint64_t rip_phys;

            __asm__ volatile ("mov %%cr3, %0" : "=r"(active_cr3));
            rip_phys = paging_translate_in_cr3(active_cr3, frame->rip);
            serial_puts("  [PF-RIP-PHYS] cr3=0x");
            serial_puthex(active_cr3, 16);
            serial_puts(" rip-phys=0x");
            serial_puthex(rip_phys, 16);
            serial_puts("\n");
            if (rip_phys != UINT64_MAX)
                mem_debug_dump_page(rip_phys);
        }

        /* tier0's page-map decoder leaves enough state in volatile registers
         * to recover the source metadata slot after its decoded AA pointer
         * faults. Read through the physical mirror so a bad diagnostic VA
         * cannot recursively page-fault inside the exception handler. */
        if (vec == 14 && (frame->cs & 0xFFFF) == 0x38 &&
            frame->rdx == 0xFFFFAAAAAAAAAA80ULL) {
            uint64_t cr3;
            uint64_t target = frame->rsi;
            uint64_t cache = frame->r10;
            uint64_t band = (target >> 30) & 0xFULL;
            uint64_t cache_entry = cache + band * 16;
            uint64_t key = 0, table = 0, slot = 0;
            uint64_t slot_value = 0, slot_phys = UINT64_MAX;
            uint64_t output_value = 0, output_phys = UINT64_MAX;
            bool cache_ok;

            __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
            cache_ok = debug_read_u64_in_cr3(cr3, cache_entry, &key, NULL) &&
                       debug_read_u64_in_cr3(cr3, cache_entry + 8,
                                             &table, NULL);
            if (cache_ok && table) {
                slot = table + (((target >> 12) & 0x3FFFFULL) * 8);
                debug_read_u64_in_cr3(cr3, slot, &slot_value, &slot_phys);
            }
            debug_read_u64_in_cr3(cr3, frame->rax, &output_value,
                                  &output_phys);

            serial_puts("  [TIER0-AA] target=0x");
            serial_puthex(target, 16);
            serial_puts(" cache=0x");
            serial_puthex(cache, 16);
            serial_puts(" band=");
            serial_putdec(band);
            serial_puts("\n  [TIER0-AA] key=0x");
            serial_puthex(key, 16);
            serial_puts(" table=0x");
            serial_puthex(table, 16);
            serial_puts(" slot=0x");
            serial_puthex(slot, 16);
            serial_puts("\n  [TIER0-AA] slot-phys=0x");
            serial_puthex(slot_phys, 16);
            serial_puts(" slot-value=0x");
            serial_puthex(slot_value, 16);
            serial_puts(" output-phys=0x");
            serial_puthex(output_phys, 16);
            serial_puts(" output-value=0x");
            serial_puthex(output_value, 16);
            serial_puts("\n");
            if (slot_phys != UINT64_MAX)
                mem_debug_dump_page(slot_phys);
        }

        /* Temporary native PE64 crash probe. Preserve the raw return chain
         * before recovery tears down the process. Keep all reads within the
         * current upper-half user stack. */
        if ((frame->cs & 0xFFFF) == 0x38) {
            extern void dll_debug_log_address(void *address);
            extern void dll_debug_log_delay_failure(void *address,
                                                     void *info);
            dll_debug_log_address((void *)frame->rip);
            if (vec == 3) {
                void *delay_info = (void *)frame->rdx;
                uint64_t active_cr3;
                uint64_t saved_delay_info;

                /* libcef's delay-load fatal helper saves its incoming RSI
                 * after three pushes and a 0x150-byte local frame. The outer
                 * helper keeps DelayLoadInfo in RSI, so the saved value at
                 * RSP+0x160 is the original pointer. Read it through the
                 * active page tables because Win64 stacks live in the lower
                 * half and cannot be dereferenced through the kernel CR3. */
                __asm__ volatile ("mov %%cr3, %0" : "=r"(active_cr3));
                if (frame->rsp <= UINT64_MAX - 0x160ULL &&
                    debug_read_u64_in_cr3(active_cr3, frame->rsp + 0x160ULL,
                                          &saved_delay_info, NULL))
                    delay_info = (void *)saved_delay_info;

                dll_debug_log_delay_failure((void *)frame->rip,
                                            delay_info);
            }
        }

        if ((frame->cs & 0xFFFF) == 0x38 &&
            frame->rsp >= 0xFFFF800000000000ULL &&
            frame->rsp < 0xFFFF900000000000ULL) {
            uint64_t *sp64 = (uint64_t *)frame->rsp;
            extern void dll_debug_log_address(void *address);
            if (sp64[0] >= 0x10000ULL &&
                sp64[0] < 0x0000800000000000ULL)
                dll_debug_log_address((void *)sp64[0]);
            serial_puts("  [PE64-STACK]");
            for (int i = 0; i < 48; i++) {
                serial_puts(" ");
                serial_puthex(sp64[i], 16);
            }
            serial_puts("\n");

            uint64_t fp = frame->rbp;
            serial_puts("  [PE64-FP]");
            for (int i = 0; i < 16; i++) {
                if ((fp & 7) != 0 || fp < frame->rsp ||
                    fp + 16 < fp || fp + 16 > frame->rsp + 0x100000ULL)
                    break;
                uint64_t *frame64 = (uint64_t *)fp;
                serial_puts(" ");
                serial_puthex(frame64[1], 16);
                if (frame64[0] <= fp)
                    break;
                fp = frame64[0];
            }
            serial_puts("\n");
        }

        if (frame->rip == 0 && (frame->cs & 0xFFFF) == 0x38) {
            uint64_t active_cr3;
            uint64_t retaddr = 0;
            uint64_t next = 0;
            bool have_ret;
            bool have_next;

            __asm__ volatile ("mov %%cr3, %0" : "=r"(active_cr3));
            have_ret = debug_read_u64_in_cr3(active_cr3, frame->rsp,
                                              &retaddr, NULL);
            have_next = debug_read_u64_in_cr3(active_cr3, frame->rsp + 8,
                                               &next, NULL);

            serial_puts("  [NULL64] stack=0x");
            serial_puthex(frame->rsp, 16);
            serial_puts(" mapped=");
            serial_putdec(have_ret ? 1 : 0);
            serial_puts(" return=0x");
            serial_puthex(retaddr, 16);
            serial_puts(" next=0x");
            serial_puthex(have_next ? next : 0, 16);
            serial_puts("\n");

            if (have_ret) {
                extern void dll_debug_log_address(void *address);
                dll_debug_log_address((void *)retaddr);
            }

            serial_puts("  [NULL64-STACK]");
            for (int i = 0; i < 12; i++) {
                uint64_t word = 0;
                if (!debug_read_u64_in_cr3(active_cr3,
                                           frame->rsp + (uint64_t)i * 8,
                                           &word, NULL)) {
                    serial_puts(" <unmapped>");
                    break;
                }
                serial_puts(" ");
                serial_puthex(word, 16);
            }
            serial_puts("\n");
        }

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

        /* ── WILD-RIP diagnostic (gcc 0x7BF5xxxx OVMF-region crash) ──
         * When RIP lands above the user-ELF region and below the kernel
         * upper-half mirror, control flow jumped to garbage (the gcc bug:
         * an indirect call/jmp through a corrupted target, e.g. into the
         * OVMF firmware band 0x7Bxxxxxx). Dump the ACTIVE CR3 (to see
         * whether we're under the process or kernel CR3) plus a window of
         * the stack: after a `call`, [RSP] holds the return address — the
         * instruction right after the corrupt call IN THE CALLER. Stack
         * values in the user-ELF range (0x20000000..0x20200000) are the
         * call sites; objdump the binary there to read the `call *reg/mem`
         * and identify the corrupted operand. Reads are guarded: stack
         * only if RSP is in the mirror (always mapped), code only if the
         * RIP page executed (it did — we faulted there). */
        {
            uint64_t wrip = frame->rip;
            uint16_t wcs  = frame->cs & 0xFFFF;
            int is_kernel = (wrip >= 0x2000000ULL  && wrip < 0x4000000ULL);
            int is_user   = (wrip >= 0x20000000ULL && wrip < 0x20200000ULL);
            int is_mirror = (wrip >= 0xFFFF800000000000ULL);
            int is_vdso   = (wrip >= 0x7FFF0000ULL && wrip < 0x80000000ULL);
            int native_cs = (wcs == 0x38 || wcs == 0x28 ||
                             wcs == 0x90 || wcs == 0x10 || wcs == 0x08);
            if (wrip >= 0x10000ULL && native_cs &&
                !is_kernel && !is_user && !is_mirror && !is_vdso) {
                extern uint64_t paging_get_kernel_cr3(void);
                uint64_t cr3;
                __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
                serial_puts("  [WILD] active CR3=0x");
                serial_puthex(cr3, 16);
                serial_puts(" kernel CR3=0x");
                serial_puthex(paging_get_kernel_cr3(), 16);
                serial_puts("\n");
                {
                    extern uint64_t proc_current_cr3(void);
                    extern int      sched_current_get(void);
                    serial_puts("  [WILD] cur_pid=");
                    serial_putdec((uint64_t)proc_current_pid());
                    serial_puts(" sched_idx=");
                    serial_putdec((uint64_t)(uint32_t)sched_current_get());
                    serial_puts(" cur->cr3=0x");
                    serial_puthex(proc_current_cr3(), 16);
                    serial_puts("\n");
                }

                if (frame->rsp >= 0xFFFF800000000000ULL) {
                    uint64_t *sp = (uint64_t *)frame->rsp;
                    for (int i = 0; i < 24; i++) {
                        serial_puts("    [RSP+");
                        serial_puthex((uint64_t)(i * 8), 3);
                        serial_puts("]=0x");
                        serial_puthex(sp[i], 16);
                        if (sp[i] >= 0x20000000ULL && sp[i] < 0x20200000ULL)
                            serial_puts("  <-- user call site");
                        serial_puts("\n");
                    }
                }

                serial_puts("  [WILD] Code @ RIP: ");
                uint8_t *wc = (uint8_t *)wrip;
                for (int bi = 0; bi < 16; bi++) {
                    serial_puthex(wc[bi], 2);
                    serial_puts(" ");
                }
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

            {
                uint64_t active_cr3;
                __asm__ volatile ("mov %%cr3, %0" : "=r"(active_cr3));
                paging_debug_dump_walk_in_cr3(active_cr3, cr2);
                extern void nt_vm_debug_address(uint64_t address);
                nt_vm_debug_address(cr2);
            }

            /* TLS diagnostic: a fault at a low linear address from user
             * code is the signature of a lost FS base (fs:[0] with
             * FS_BASE==0 → CR2==0). Dump live MSR vs the process's stored
             * fs_base to tell apart MSR-clobber from stored-base-zeroed. */
            {
                extern void proc_dump_fs_state(void);
                proc_dump_fs_state();
            }
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

        /* Native ELF backtrace via RBP walking (CS=0x28, fixed
         * SYSCALL-return CS=0x90, or legacy buggy CS=0x10 dumps).
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
        {
            uint16_t cs16 = (uint16_t)(frame->cs & 0xFFFF);
            if (cs16 == 0x28 || cs16 == 0x90 || cs16 == 0x10) {
                extern void *proc_current(void);
                extern bool  user_symbolize(void *p, uint64_t addr,
                                            const char **name, uint64_t *off);

                serial_puts("  Backtrace:\n");
                uint64_t       rbp  = frame->rbp;
                const uint64_t rsp  = frame->rsp;
                const uint64_t rsp_cmp = idt_stack_compare_addr(rsp);
                const uint64_t WIN  = 8ULL * 1024 * 1024;  /* USER_STACK_SIZE */
                void          *proc = proc_current();

                for (int depth = 0; depth < 16; depth++) {
                    uint64_t rbp_cmp = idt_stack_compare_addr(rbp);
                    uint64_t lower = (rsp_cmp >= 256) ? (rsp_cmp - 256) : 0;
                    if (rbp == 0) break;
                    if (rbp & 0x7) break;                  /* unaligned */
                    /* Keep RBP within ±8MB of the faulting RSP — the
                     * kernel-allocated user stack window. Anything else is
                     * either garbage or points outside the stack. */
                    if (rbp_cmp + 16 < rbp_cmp) break;     /* overflow guard */
                    if (rbp_cmp < lower || rbp_cmp > rsp_cmp + WIN) break;

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

                    if (idt_stack_compare_addr(prev_rbp) <= rbp_cmp) break;
                    rbp = prev_rbp;
                }
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
            /* [BPDIAG] On a compat-mode fatal exception (covers #BP at 0xCC
             * thunk-pool tail and #PF), dump the recent native-shim call ring
             * once so we can see which shim/path produced the garbage
             * call/return target (map-select recovery-cascade crash). */
            {
                static int bpdiag_done = 0;
                if (!bpdiag_done) {
                    bpdiag_done = 1;
                    extern void compat32_dump_recent_calls(void);
                    compat32_dump_recent_calls();
                }
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

        if (compat32_seh_result < 0 &&
            ((frame->cs & 0xFFFF) == 0x40 ||
             (frame->cs & 0xFFFF) == 0x23)) {
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
            uint64_t fault_address = 0;
            for (int i = 0; i < (int)sizeof(er)/4; i++)
                ((uint32_t *)&er)[i] = 0;

            if (vec == 14) {
                /* #PF → STATUS_ACCESS_VIOLATION.
                 * NULL page WRITES (cr2 < 0x1000, !instruction-fetch) are
                 * dispatched to the guest SEH chain — the engine's
                 * __except handler catches them and displays the crash
                 * reporter (like NT's Dr. Watson). No more silent
                 * write-through that turns into cascading RIP=0 crashes. */
                uint64_t cr2;
                __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
                /* Non-compat32 null-page writes still go to old recovery
                 * (kernel init code legitimately touches page 0). */
                if (cr2 < 0x1000 && !(frame->error_code & 16) &&
                    (frame->cs & 0xFFFF) != 0x40)
                    goto compat32_null_recovery;
                fault_address = cr2;
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

            int handled = compat32_seh_dispatch((PEXCEPTION_RECORD)&er64);
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

                if (vec == 14 && fault_address >= 0x1000 &&
                    (frame->error_code & 3) == 3 &&
                    frame->rip == er.ExceptionAddress)
                    compat32_wait_for_write_retry(fault_address);

                serial_puts("  [WIN32] SEH handled — resuming via unwind\n");
                return;  /* iretq will now jump to catch handler */
            }
            serial_puts("  [WIN32] SEH unhandled — falling through to recovery\n");
        }
compat32_null_recovery:
        {
            extern uint64_t *win32_current_child_jmpbuf(void);
            extern void kern_longjmp(uint64_t *buf, int val);
            uint64_t *child_jmp = win32_current_child_jmpbuf();
            if (child_jmp) {
                serial_puts("  [WIN32] Child crashed - terminating child\n");
                kern_longjmp(child_jmp, 3);
            }
        }
        /* Legacy crash recovery: longjmp back to shell */
        {
            extern uint64_t *compat32_crash_jmpbuf;
            extern void kern_longjmp(uint64_t *buf, int val);
            /* This buffer belongs to the synchronous shell-run PE context.
             * A spawned Win32 worker has its own kernel PID and must be
             * zombified below; long-jumping it onto the owner's saved stack
             * corrupts both contexts and frees DLL/TLS memory while live. */
            if (compat32_crash_jmpbuf && proc_current_pid() <= 1) {
                serial_puts("  [WIN32] Crash recovery — returning to shell\n");
                /* Restore IST1 BEFORE longjmp — longjmp bypasses
                 * int2e_stub's IST1 restore, leaving it corrupted. */
                x86_tss_reset_ist1();
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
                    idt_diag_flush("win32-jmpbuf-corrupt");
                    __asm__ volatile ("cli");
                    for (;;) __asm__ volatile ("hlt");
                }
                idt_diag_flush("win32-crash-recover");
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
                idt_diag_flush("exception-cascade");
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

            serial_puts("  Killing process group for PID ");
            serial_putdec(pid);
            serial_puts(" with signal ");
            serial_putdec((uint64_t)sig);
            serial_puts("\n");
            fb_puts_color(" Process killed\n", 0x00FF0000);
            idt_diag_flush("process-kill");
            {
                extern void proc_exit_group(int32_t code);
                proc_exit_group(128 + sig);
            }
            /* proc_exit_group never returns */
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
                idt_diag_flush("dos-panic-recover");
                kern_longjmp(dos_native_exit_jmpbuf, 3);
            }
        }

        /* Kernel exception (PID 0 or 1) — halt the system */
        serial_puts("  SYSTEM HALTED\n");
        fb_puts_color(" SYSTEM HALTED\n", 0x00FF0000);
        idt_diag_flush("kernel-exception-halt");
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
    bsp_apic_id_global = (apic_read(APIC_ID) >> 24) & 0xFF;
    isr_tsc_aux_enabled = cpu_features.rdtscp ? 1U : 0U;
    idt_set_current_apic_id(bsp_apic_id_global);

    serial_puts("[IDT] APIC base: 0x");
    serial_puthex(apic_phys, 16);
    serial_puts(", BSP ID=");
    serial_putdec(bsp_apic_id_global);
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
    tsc_time_base_ns = tick_count * 10000000ULL;
    tsc_time_base = idt_rdtsc();
    tsc_last_tick = tsc_time_base;

    if (tsc_deadline_mode) {
        /* ── TSC-Deadline mode: sub-microsecond precision ── */
        apic_write(APIC_LVT_TIMER, APIC_TIMER_TSC_DEADLINE | 32);

        /* Program initial 10ms deadline */
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

    /* Exceptions use fault-class stacks; asynchronous vectors use the
     * CPU-local IRQ stack and never touch the interrupted task's red zone. */
    for (int i = 0; i <= 33; i++) {
        uint8_t ist = (i < 32) ? X86_IST_FAULT : X86_IST_IRQ;
        idt_set_entry(i, stubs[i], ist);
        idt[i].selector = cs;
    }

    /* Vectors 34-255: default stub (just IRET) */
    for (int i = 34; i < 256; i++) {
        idt_set_entry(i, isr_stub_default, X86_IST_IRQ);
        idt[i].selector = cs;
    }

    idt[1].ist  = X86_IST_DEBUG;
    idt[2].ist  = X86_IST_NMI;
    idt[3].ist  = X86_IST_DEBUG;
    idt[8].ist  = X86_IST_DF;
    idt[18].ist = X86_IST_MC;

    /* Override: vector 0x71 = keyboard IRQ (uses isr_stub_33) */
    idt_set_entry(0x71, isr_stub_33, X86_IST_IRQ);
    idt[0x71].selector = cs;

    /* Vector 40: I211 NIC MSI interrupt */
    idt_set_entry(40, isr_stub_40, X86_IST_IRQ);
    idt[40].selector = cs;

    /* Vector 41: Win32 __fastfail (INT 0x29) */
    idt_set_entry(41, isr_stub_41, X86_IST_IRQ);
    idt[41].selector = cs;

    /* Vector 42: RTL8111 NIC MSI interrupt */
    idt_set_entry(42, isr_stub_42, X86_IST_IRQ);
    idt[42].selector = cs;

    /* SMP work IPI: lightweight stub, no fxsave (safe for APs) */
    extern void isr_stub_smp_ipi(void);
    idt_set_entry(0xFE, isr_stub_smp_ipi, X86_IST_IRQ);
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
