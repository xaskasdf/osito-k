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
#include "../include/hwbp.h"
#include "../include/audio_sched.h"
#include "../win32/compat32.h"
#include "../win32/kernel32_shim.h"
#include "../win32/unwind64.h"
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

static NTSTATUS win32_exception_code_for_vector(uint64_t vector)
{
    switch (vector) {
    case 0:  return EXCEPTION_INT_DIVIDE_BY_ZERO;
    case 3:  return EXCEPTION_BREAKPOINT;
    case 4:  return EXCEPTION_INT_OVERFLOW;
    case 5:  return EXCEPTION_ARRAY_BOUNDS_EXCEEDED;
    case 6:  return EXCEPTION_ILLEGAL_INSTRUCTION;
    case 13:
    case 14: return EXCEPTION_ACCESS_VIOLATION;
    default: return STATUS_SUCCESS;
    }
}

/* NT delivers user-mode CPU faults to SEH before treating them as process
 * crashes. HotSpot relies on this for safepoint polling pages, where a
 * protection fault is expected control flow. Return 1 when SEH handled the
 * exception, 0 when it was unhandled, and -1 for unsupported vectors. */
static int compat32_dispatch_cpu_exception(interrupt_frame_t *frame,
                                           uint64_t vector,
                                           NTSTATUS *status_out)
{
    uint16_t cs = (uint16_t)(frame->cs & 0xFFFF);
    NTSTATUS status = win32_exception_code_for_vector(vector);
    if ((cs != 0x40 && cs != 0x23) || status == STATUS_SUCCESS)
        return -1;

    if (status_out)
        *status_out = status;

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
    uint32_t exception_address = (uint32_t)frame->rip;
    if (vector == 3 && exception_address)
        exception_address--;

    record.ExceptionCode = status;
    if (vector == 14) {
        __asm__ volatile ("mov %%cr2, %0" : "=r"(fault_address));
        if (fault_address < 0x1000 && !(frame->error_code & 16) &&
            cs != 0x40)
            return -1;
        record.NumberParameters = 2;
        record.ExceptionInformation[0] =
            (frame->error_code & 16) ? 8 :
            ((frame->error_code & 2) ? 1 : 0);
        record.ExceptionInformation[1] = (uint32_t)fault_address;
    }
    record.ExceptionAddress = exception_address;

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
        .eip = exception_address,
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

    (void)compat32_apply_pending_unwind(&context);
    compat32_apply_cpu_context(frame, &context);

    if (vector == 14 && fault_address >= 0x1000 &&
        (frame->error_code & 3) == 3 &&
        frame->rip == record.ExceptionAddress)
        compat32_wait_for_write_retry(fault_address);

    return 1;
}

static void win64_context_from_frame(CONTEXT *context,
                                     const interrupt_frame_t *frame,
                                     uint64_t instruction_pointer)
{
    BYTE *bytes = (BYTE *)context;
    for (SIZE_T i = 0; i < sizeof(*context); i++)
        bytes[i] = 0;

    context->ContextFlags = CONTEXT_FULL;
    __asm__ volatile ("stmxcsr %0" : "=m"(context->MxCsr));
    __asm__ volatile ("movw %%ds, %0" : "=r"(context->SegDs));
    __asm__ volatile ("movw %%es, %0" : "=r"(context->SegEs));
    __asm__ volatile ("movw %%fs, %0" : "=r"(context->SegFs));
    __asm__ volatile ("movw %%gs, %0" : "=r"(context->SegGs));
    context->SegCs = (WORD)frame->cs;
    context->SegSs = (WORD)frame->ss;
    context->EFlags = (DWORD)frame->rflags;
    context->Rax = frame->rax;
    context->Rcx = frame->rcx;
    context->Rdx = frame->rdx;
    context->Rbx = frame->rbx;
    context->Rsp = frame->rsp;
    context->Rbp = frame->rbp;
    context->Rsi = frame->rsi;
    context->Rdi = frame->rdi;
    context->R8 = frame->r8;
    context->R9 = frame->r9;
    context->R10 = frame->r10;
    context->R11 = frame->r11;
    context->R12 = frame->r12;
    context->R13 = frame->r13;
    context->R14 = frame->r14;
    context->R15 = frame->r15;
    context->Rip = instruction_pointer;
}

static void win64_apply_cpu_context(interrupt_frame_t *frame,
                                    const CONTEXT *context)
{
    const uint64_t mutable_rflags = 0x00250DD5ULL;
    frame->rax = context->Rax;
    frame->rcx = context->Rcx;
    frame->rdx = context->Rdx;
    frame->rbx = context->Rbx;
    frame->rsp = context->Rsp;
    frame->rbp = context->Rbp;
    frame->rsi = context->Rsi;
    frame->rdi = context->Rdi;
    frame->r8 = context->R8;
    frame->r9 = context->R9;
    frame->r10 = context->R10;
    frame->r11 = context->R11;
    frame->r12 = context->R12;
    frame->r13 = context->R13;
    frame->r14 = context->R14;
    frame->r15 = context->R15;
    frame->rip = context->Rip;
    frame->rflags = (frame->rflags & ~mutable_rflags) |
                    ((uint64_t)context->EFlags & mutable_rflags);
}

/* PE64 exceptions follow NT ordering: VEH first, frame handlers second, and
 * the process-level unhandled filter only after both searches decline. */
static int win64_dispatch_cpu_exception(interrupt_frame_t *frame,
                                        uint64_t vector,
                                        NTSTATUS *status_out)
{
    NTSTATUS status = win32_exception_code_for_vector(vector);
    if ((frame->cs & 0xFFFF) != GDT_SEL_CODE64 ||
        status == STATUS_SUCCESS)
        return -1;

    uint64_t exception_address = frame->rip;
    if (vector == 3) {
        if (!exception_address)
            return -1;
        exception_address--;
    }
    if (!win32_user_range_executable(
            (const void *)(ULONG_PTR)exception_address, 1, FALSE))
        return -1;

    if (status_out)
        *status_out = status;

    EXCEPTION_RECORD record;
    BYTE *record_bytes = (BYTE *)&record;
    for (SIZE_T i = 0; i < sizeof(record); i++)
        record_bytes[i] = 0;
    record.ExceptionCode = status;
    record.ExceptionAddress = (PVOID)(ULONG_PTR)exception_address;
    if (vector == 14) {
        uint64_t fault_address;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(fault_address));
        record.NumberParameters = 2;
        record.ExceptionInformation[0] =
            (frame->error_code & 16) ? 8 :
            ((frame->error_code & 2) ? 1 : 0);
        record.ExceptionInformation[1] = fault_address;
    }

    CONTEXT context;
    win64_context_from_frame(&context, frame, exception_address);
    LONG disposition =
        kernel32_dispatch_vectored_exception(&record, &context);
    if (disposition == EXCEPTION_CONTINUE_EXECUTION) {
        win64_apply_cpu_context(frame, &context);
        return 1;
    }

    NTSTATUS seh_status =
        win32_unwind64_dispatch_exception(&record, &context);
    if (seh_status == STATUS_SUCCESS) {
        win64_apply_cpu_context(frame, &context);
        return 1;
    }
    if (seh_status != STATUS_UNHANDLED_EXCEPTION) {
        if (status_out) *status_out = seh_status;
        return 0;
    }

    PVOID filter = kernel32_get_unhandled_exception_filter();
    if (filter && win32_user_range_executable(filter, 1, FALSE)) {
        EXCEPTION_POINTERS pointers = {
            .ExceptionRecord = &record,
            .ContextRecord = &context,
        };
        typedef LONG (WINAPI *top_level_filter_fn)(PEXCEPTION_POINTERS);
        disposition = ((top_level_filter_fn)filter)(&pointers);
        if (disposition == EXCEPTION_CONTINUE_EXECUTION) {
            win64_apply_cpu_context(frame, &context);
            return 1;
        }
    }

    return 0;
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
 * int2e_stub.S reserves 16KB per nested transition. This supports 64 levels
 * of callback and exception re-entry while retaining ample headroom for the
 * 1-3KB kernel call chain at each level. */
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

void x86_tss_reset_ist2(void)
{
    kernel_tss.ist2 = (uint64_t)(ist2_stack + IST2_STACK_SIZE);
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
    hwbp_set(0, (uint64_t)addr, HWBP_WRITE, HWBP_LEN_4, "idt-write4");
}

void idt_break_exec(uint64_t addr)
{
    hwbp_set(0, addr, HWBP_EXECUTE, HWBP_LEN_1, "idt-exec");
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
    NTSTATUS win32_exception_status = STATUS_SUCCESS;

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

    /* Native DPMI clients run at CPL3. Privileged port I/O and virtual IF
     * instructions intentionally fault for host emulation; selector faults
     * may be repaired after a client edits its LDT. */
    if (vec == 13) {
        extern int dos_native_handle_privileged_fault(
            x86_interrupt_frame_t *frame);
        extern int dos_native_refresh_selector(uint16_t error_code);
        if (dos_native_handle_privileged_fault(frame) ||
            dos_native_refresh_selector((uint16_t)frame->error_code))
            return;
    }

    if (vec == 14) {
        extern bool dos_native_handle_memory_fault(
            x86_interrupt_frame_t *frame, uint64_t fault_address);
        uint64_t fault_address;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(fault_address));
        if (dos_native_handle_memory_fault(frame, fault_address))
            return;
    }

    /* Host mediation gets first refusal. Remaining CPL3 DOS faults belong
     * to the client's DPMI chain, not to generic kernel panic recovery. */
    if (vec < 32) {
        extern bool dos_native_handle_exception(x86_interrupt_frame_t *frame);
        if (dos_native_handle_exception(frame))
            return;
    }

#ifdef COMPAT_TRACE
    /* Panorama probe: any exception taken while RSP lies inside IST1 (Win32
     * INT 0x2E) or IST2 (DOS native INTs). A hit on a page fault here is the
     * single line that confirms risk #3 (lower-half identity map dropped
     * in commit f8bd01c) is actually biting inside compat dispatch.
     *
     * Also covers DOS-native faults when RSP is outside both IST windows.
     * Session state, rather than a particular selector layout, determines
     * whether descriptor refresh and shell recovery apply. */
    if (vec < 32) {
        extern uint8_t ist1_stack[];
        extern uint8_t ist2_stack[];
        extern uint64_t *dos_native_exit_jmpbuf;
        extern int dos_native_session_active(void);
        uint64_t _sp = frame->rsp;
        uint64_t _i1 = (uint64_t)ist1_stack;
        uint64_t _i2 = (uint64_t)ist2_stack;
        const char *_zone = 0;
        if (_sp >= _i1 && _sp < _i1 + 65536)      _zone = "IST1";
        else if (_sp >= _i2 && _sp < _i2 + 32768) _zone = "IST2";
        int _dos_active = (dos_native_exit_jmpbuf != 0)
                       && dos_native_session_active();
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

            /* Resolve CS and SS through the client's current descriptor
             * tables so diagnostics work for either GDT or LDT selectors. */
            if (_dos_active) {
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

            /* The native backend has no protected-mode debugger endpoint.
             * Consume a client single-step request and clear its DR6 state. */
            if (vec == 1 && _dos_active) {
                frame->rflags &= ~(uint64_t)0x100;  /* TF off */
                uint64_t dr6 = 0xFFFF0FF0; /* clear B0-B3, BS, BT */
                __asm__ volatile ("mov %0, %%dr6" :: "r"(dr6));
                serial_puts("[DOS-NT] #DB caught at rip=0x");
                serial_puthex(frame->rip, 8);
                serial_puts(" — TF cleared, resuming\n");
                return;
            }

            /* A fault that cannot be reflected or repaired terminates only
             * the active DOS session and restores the host machine state. */
            if (_dos_active) {
                extern void dos_native_cleanup_active(void);
                extern void kern_longjmp(uint64_t *buf, int val);
                if (dos_native_exit_jmpbuf) {
                    serial_puts("[DOS-NT] crash -> long-jump to shell\n");
                    idt_diag_flush("dos-crash-recover");
                    dos_native_cleanup_active();
                    __asm__ volatile ("cli" ::: "memory");
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
    }

    /* #BP (INT3) — software breakpoint for tracing PE32 execution */
    if (vec == 3 && g_swbreak_addr &&
        (uint32_t)frame->rip == g_swbreak_addr + 1) {
        serial_puts("[SWBREAK] Hit at 0x");
        serial_puthex(g_swbreak_addr, 8);
        serial_puts("\n");
        *(uint8_t *)(uintptr_t)g_swbreak_addr = g_swbreak_saved;
        frame->rip = g_swbreak_addr;
        g_swbreak_addr = 0;
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
        /* The process scheduler, clock and device deadlines below own BSP
         * state. APs run only the SMP work-stealing loop; reject a stray or
         * software timer vector there before it can switch the global CR3 and
         * process slot out from under the BSP. */
        if (apic_enabled) {
            uint32_t current_apic_id =
                (apic_read(APIC_ID) >> 24) & 0xFFU;
            if (current_apic_id != bsp_apic_id_global) {
                apic_write(APIC_EOI, 0);
                return;
            }
        }

        /* Present DOS VGA mode 13h VRAM to the real framebuffer (no-op
         * unless a DOS VM is bound, in mode 13h, and needs a refresh). */
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

        audio_check_deadline();

        /* Reflect completed Sound Blaster DMA blocks before the scheduler
         * captures this protected-mode client frame. */
        bool dos_native_active;
        {
            extern bool dos_native_service_audio_irq(
                x86_interrupt_frame_t *frame);
            extern bool dos_native_service_timer_irq(
                x86_interrupt_frame_t *frame);
            extern bool dos_native_service_keyboard_irq(
                x86_interrupt_frame_t *frame);
            extern int dos_native_session_active(void);
            if (!dos_native_service_timer_irq(frame) &&
                !dos_native_service_keyboard_irq(frame))
                (void)dos_native_service_audio_irq(frame);
            dos_native_active = dos_native_session_active() != 0;
        }

        /* X-SCHED: preemptive scheduler — check quantum, switch if expired.
         * frame points to saved GPRs on the BSP's IRQ IST. */
        uint64_t interrupted_rip = frame->rip;
        /* Native DPMI temporarily owns process-global GDT/LDT and DOS IDT
         * gates. Keep timer/audio IRQs live, but do not switch tasks until
         * the backend restores the host machine context. */
        if (!dos_native_active)
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
            uint64_t audio_us = audio_get_next_deadline_us();
            if (audio_us && (!next_us || audio_us < next_us))
                next_us = audio_us;
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
        /* Dispatch registered hardware breakpoints before handling an
         * internal single-step used by the guarded NULL-page path. */
        {
            if (hwbp_dispatch(frame)) return;
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
        if (cr2 < 0x1000 && (frame->error_code & 2) &&
            !(frame->error_code & 16)) {
            uint16_t cs = (uint16_t)(frame->cs & 0xFFFF);
            if (cs == 0x38 || cs == 0x08) {
                paging_set_flags(
                    0, PTE_PRESENT | PTE_WRITABLE | PTE_GLOBAL | PTE_NX);
                __asm__ volatile ("invlpg (%0)" :: "r"((uint64_t)0)
                                  : "memory");
                frame->rflags |= (1ULL << 8);
                g_null_page_dirty = 1;
                return;
            }
        }

    }

    /* Deliver Win32 user exceptions before producing fatal diagnostics.
     * Handled access violations are normal for mechanisms such as HotSpot's
     * safepoint polling page. */
    int compat32_seh_result =
        compat32_dispatch_cpu_exception(frame, vec,
                                        &win32_exception_status);
    if (compat32_seh_result > 0)
        return;

    int win64_seh_result =
        win64_dispatch_cpu_exception(frame, vec, &win32_exception_status);
    if (win64_seh_result > 0)
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

        /* Preserve generic module context before recovery tears down a native
         * PE64 process. */
        if ((frame->cs & 0xFFFF) == 0x38) {
            extern void dll_debug_log_address(void *address);
            dll_debug_log_address((void *)frame->rip);
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
                /* Hardware faults return through the generic ISR, so consume
                 * this scheduler slot's pending PE32 unwind here. */
                compat32_cpu_context_t unwind_context = {
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
                };
                if (compat32_apply_pending_unwind(&unwind_context))
                    compat32_apply_cpu_context(frame, &unwind_context);

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
        if (win32_exception_status != STATUS_SUCCESS) {
            extern int win32_terminate_current_child(int32_t status);
            extern int win32_terminate_current_main(int32_t status);
            serial_puts("  [WIN32] Unhandled exception status=0x");
            serial_puthex((uint32_t)win32_exception_status, 8);
            serial_puts(" - terminating process\n");
            if (win32_terminate_current_child(win32_exception_status) ||
                win32_terminate_current_main(win32_exception_status))
                return;
        }
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
                extern void sched_reset_current_compat_ist1(void);
                sched_reset_current_compat_ist1();
                uint64_t *jmp = compat32_crash_jmpbuf;
                compat32_crash_jmpbuf = NULL;

                /* Validate the synchronous launch context before returning to
                 * it. A corrupted buffer must terminate cleanly rather than
                 * transfer to an invalid kernel RIP/RSP and triple-fault. */
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
         * (dos_native_exit_jmpbuf is set by `dosrun` before LRETQ),
         * longjmp to the shell instead of halting. The [pf-ist]
         * probe above only fires when RSP is inside one of our IST
         * stacks, but DOS-native code can fault with RSP pointing at its
         * guest SS (outside the IST window). Catch that here. */
        {
            extern uint64_t *dos_native_exit_jmpbuf;
            extern int dos_native_session_active(void);
            extern void dos_native_cleanup_active(void);
            extern void kern_longjmp(uint64_t *buf, int val);
            if (dos_native_exit_jmpbuf && dos_native_session_active()) {
                serial_puts("  [DOS-NT] panic recovery -> long-jump to shell\n");
                idt_diag_flush("dos-panic-recover");
                dos_native_cleanup_active();
                __asm__ volatile ("cli" ::: "memory");
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
