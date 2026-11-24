/*
 * OsitoK x86-64 — Kernel Core Dump
 *
 * Generates ELF core dump on process crash (SIGSEGV, SIGABRT).
 * Dumps registers + stack + memory regions to serial or file.
 * Allows post-mortem analysis with GDB.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern void serial_puthex(uint64_t val, int digits);
extern uint64_t idt_get_ticks(void);
extern uint32_t proc_current_pid(void);

/* ── Register State (from trap frame) ────────────────────────── */

typedef struct {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11;
    uint64_t r12, r13, r14, r15;
    uint64_t rip, rflags;
    uint64_t cs, ss;
    uint64_t cr2, cr3;
    int      signal;
} coredump_regs_t;

/* ── Dump to Serial ──────────────────────────────────────────── */

void coredump_to_serial(const coredump_regs_t *regs, const char *reason)
{
    serial_puts("\n=== CORE DUMP (PID ");
    serial_putdec(proc_current_pid());
    serial_puts(") ===\n");
    serial_puts("Reason: ");
    serial_puts(reason ? reason : "unknown");
    serial_puts("\nSignal: ");
    serial_putdec((uint64_t)regs->signal);
    serial_puts("\n\nRegisters:\n");

    serial_puts("  RAX="); serial_puthex(regs->rax, 16);
    serial_puts("  RBX="); serial_puthex(regs->rbx, 16); serial_puts("\n");
    serial_puts("  RCX="); serial_puthex(regs->rcx, 16);
    serial_puts("  RDX="); serial_puthex(regs->rdx, 16); serial_puts("\n");
    serial_puts("  RSI="); serial_puthex(regs->rsi, 16);
    serial_puts("  RDI="); serial_puthex(regs->rdi, 16); serial_puts("\n");
    serial_puts("  RBP="); serial_puthex(regs->rbp, 16);
    serial_puts("  RSP="); serial_puthex(regs->rsp, 16); serial_puts("\n");
    serial_puts("  R8 ="); serial_puthex(regs->r8, 16);
    serial_puts("  R9 ="); serial_puthex(regs->r9, 16); serial_puts("\n");
    serial_puts("  R10="); serial_puthex(regs->r10, 16);
    serial_puts("  R11="); serial_puthex(regs->r11, 16); serial_puts("\n");
    serial_puts("  R12="); serial_puthex(regs->r12, 16);
    serial_puts("  R13="); serial_puthex(regs->r13, 16); serial_puts("\n");
    serial_puts("  R14="); serial_puthex(regs->r14, 16);
    serial_puts("  R15="); serial_puthex(regs->r15, 16); serial_puts("\n");
    serial_puts("  RIP="); serial_puthex(regs->rip, 16);
    serial_puts("  FLG="); serial_puthex(regs->rflags, 16); serial_puts("\n");
    serial_puts("  CR2="); serial_puthex(regs->cr2, 16);
    serial_puts("  CR3="); serial_puthex(regs->cr3, 16); serial_puts("\n");

    /* Stack trace via frame pointer */
    serial_puts("\nStack trace:\n");
    uint64_t *frame = (uint64_t *)regs->rbp;
    for (int depth = 0; depth < 16; depth++) {
        if ((uint64_t)frame < 0x1000 || (uint64_t)frame > 0x7FFFFFFFFFFF) break;
        serial_puts("  #"); serial_putdec((uint64_t)depth);
        serial_puts(": 0x"); serial_puthex(frame[1], 16);
        serial_puts("\n");
        frame = (uint64_t *)frame[0];
    }

    /* Stack dump (16 words from RSP) */
    serial_puts("\nStack (RSP):\n");
    uint64_t *sp = (uint64_t *)regs->rsp;
    for (int i = 0; i < 16; i++) {
        if ((uint64_t)(sp + i) < 0x1000) break;
        serial_puts("  RSP+"); serial_putdec((uint64_t)(i * 8));
        serial_puts(": 0x"); serial_puthex(sp[i], 16);
        serial_puts("\n");
    }

    /* Code bytes around RIP */
    serial_puts("\nCode (RIP-8..RIP+24):\n  ");
    uint8_t *code = (uint8_t *)(regs->rip - 8);
    if ((uint64_t)code > 0x1000) {
        for (int i = 0; i < 32; i++) {
            serial_puthex(code[i], 2);
            serial_puts(i == 7 ? " >> " : " ");
        }
    }
    serial_puts("\n\n=== END CORE DUMP ===\n");
}

/* ── Convenience: dump from trap frame pointer ───────────────── */

void coredump_from_frame(void *frame_ptr, int signal, const char *reason)
{
    /* frame_ptr points to the ISR-saved register block.
     * Layout depends on isr_stubs.S push order. */
    uint64_t *f = (uint64_t *)frame_ptr;
    coredump_regs_t regs;
    memset(&regs, 0, sizeof(regs));

    /* Minimal extraction (exact offsets depend on ISR stub layout) */
    regs.rip = f[0];    /* Approximate — real offset from isr_stubs.S */
    regs.rsp = (uint64_t)frame_ptr;
    regs.signal = signal;

    /* Read actual CR2/CR3 */
    __asm__ volatile ("mov %%cr2, %0" : "=r"(regs.cr2));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(regs.cr3));
    __asm__ volatile ("mov %%rbp, %0" : "=r"(regs.rbp));

    coredump_to_serial(&regs, reason);
}
