/*
 * OsitoK x86-64 — ISR Stub Entry Points
 *
 * Each CPU exception vector needs a small stub that:
 * 1. Pushes a dummy error code (if CPU doesn't push one)
 * 2. Pushes the vector number
 * 3. Saves all general-purpose registers
 * 4. Calls the C handler: isr_handler(interrupt_frame_t *frame)
 * 5. Restores registers and IRETQ
 *
 * Vectors that push an error code: 8, 10, 11, 12, 13, 14, 17, 21, 29, 30
 * All others get a dummy 0 pushed.
 */

.code64
.text

/* ── Common handler tail ─────────────────────────────────────── */

.globl isr_common
isr_common:
    /* Save all GPRs */
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15

    /* Pass frame pointer as argument (RSP points to saved regs) */
    mov %rsp, %rdi

    /* Align stack to 16 bytes (ABI requirement) */
    mov %rsp, %rbp
    and $~0xF, %rsp

    /* Call C handler */
    call isr_handler

    /* Restore stack */
    mov %rbp, %rsp

    /* Restore all GPRs */
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax

    /* Remove vector number and error code from stack */
    add $16, %rsp

    iretq

/* ── Macro for vectors WITHOUT error code ────────────────────── */

.macro ISR_NOERR vector
.globl isr_stub_\vector
isr_stub_\vector:
    push $0              /* dummy error code */
    push $\vector        /* vector number */
    jmp isr_common
.endm

/* ── Macro for vectors WITH error code (CPU pushes it) ───────── */

.macro ISR_ERR vector
.globl isr_stub_\vector
isr_stub_\vector:
    /* error code already on stack from CPU */
    push $\vector        /* vector number */
    jmp isr_common
.endm

/* ── Exception vectors 0-31 ──────────────────────────────────── */

ISR_NOERR 0     /* #DE Divide Error */
ISR_NOERR 1     /* #DB Debug */
ISR_NOERR 2     /* NMI */
ISR_NOERR 3     /* #BP Breakpoint */
ISR_NOERR 4     /* #OF Overflow */
ISR_NOERR 5     /* #BR Bound Range */
ISR_NOERR 6     /* #UD Invalid Opcode */
ISR_NOERR 7     /* #NM No Math Coprocessor */
ISR_ERR   8     /* #DF Double Fault */
ISR_NOERR 9     /* Coprocessor Segment Overrun */
ISR_ERR   10    /* #TS Invalid TSS */
ISR_ERR   11    /* #NP Segment Not Present */
ISR_ERR   12    /* #SS Stack-Segment Fault */
ISR_ERR   13    /* #GP General Protection */
ISR_ERR   14    /* #PF Page Fault */
ISR_NOERR 15    /* (reserved) */
ISR_NOERR 16    /* #MF x87 FPU Error */
ISR_ERR   17    /* #AC Alignment Check */
ISR_NOERR 18    /* #MC Machine Check */
ISR_NOERR 19    /* #XM SIMD Exception */
ISR_NOERR 20    /* #VE Virtualization */
ISR_ERR   21    /* #CP Control Protection */
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29    /* #VC VMM Communication */
ISR_ERR   30    /* #SX Security Exception */
ISR_NOERR 31

/* ── Vector 32: APIC timer ───────────────────────────────────── */

ISR_NOERR 32

/* ── Vector 33: Keyboard IRQ (IRQ 1) ────────────────────────── */

ISR_NOERR 33

/* ── Default stub for vectors 34-255 ─────────────────────────── */

.globl isr_stub_default
isr_stub_default:
    push $0              /* dummy error code */
    push $255            /* generic vector */
    jmp isr_common
