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

    /* Save FPU/SSE state into the currently-scheduled process's per-
     * process slot before calling the C handler. The handler may clobber
     * xmm registers (memset/memcpy are emitted as SSE moves by gcc -O2),
     * and a user-mode SSE instruction that triggered a demand-page-fault
     * needs its xmm state preserved verbatim so the retried store writes
     * the right bytes. If the handler decides to context-switch, it
     * rewrites `fpu_state_ptr` to the incoming process's slot via
     * `set_current_proc`, and the fxrstor64 on exit pulls the *new*
     * process's state into the FPU — atomic with the GPR swap a few
     * instructions below. For the boot window before any process exists,
     * `fpu_state_ptr` points at the kernel-default buffer in process.c. */
    /* Check if we're BSP (LAPIC ID 0) — only BSP needs fxsave because
     * fpu_state_ptr is a global (not per-CPU). APs skip FPU save/restore. */
    movabs $0xFFFF800000000000 + 0xFEE00020, %rax  /* PHYS_TO_VIRT(LAPIC_ID) */
    movl   (%rax), %eax
    shrl   $24, %eax
    testl  %eax, %eax
    jnz    .Lfxsave_done    /* Non-zero LAPIC ID = AP → skip */

    mov fpu_state_ptr(%rip), %rax
    test $0xF, %rax
    jnz .Lskip_fxsave
    bt $63, %rax
    jnc .Lskip_fxsave
    fxsave64 (%rax)
    jmp .Lfxsave_done
.Lskip_fxsave:
    mov %rax, fpu_corrupt_val(%rip)
.Lfxsave_done:

    /* Align RSP to 16 bytes for the C ABI. Use %rbp as a scratch to
     * save the GPR-area rsp so we can restore cleanly after the call;
     * %rbp is callee-saved by the handler and the bottom of this stub
     * pops %rbp from the saved GPR area, so its original value is
     * restored at iretq time. */
    mov %rsp, %rbp
    and $~0xF, %rsp

    /* Call C handler */
    call isr_handler

    /* Restore rsp to the GPR area and load FPU state from whichever
     * process is live *now* (the scheduler may have rewritten
     * `fpu_state_ptr` while we were in C). */
    mov %rbp, %rsp

    /* Skip fxrstor on APs (same reason as fxsave above) */
    movabs $0xFFFF800000000000 + 0xFEE00020, %rax
    movl   (%rax), %eax
    shrl   $24, %eax
    testl  %eax, %eax
    jnz    .Lskip_fxrstor

    mov fpu_state_ptr(%rip), %rax
    test $0xF, %rax
    jnz .Lskip_fxrstor
    bt $63, %rax
    jnc .Lskip_fxrstor
    fxrstor64 (%rax)
.Lskip_fxrstor:

    /* ── Scheduler: check for context switch (X-SCHED) ────────── */
    /* If sched_tick() set sched_switch_rsp, switch to new stack   */
    /* before popping GPRs — the new stack has the next process's  */
    /* saved interrupt frame, so pops + iretq resume that process. */
    /* IMPORTANT: Use XCHG (implicit LOCK) to atomically read and  */
    /* clear sched_switch_rsp. This prevents SMP race where an AP  */
    /* reads the value set by BSP's sched_tick and switches to the */
    /* wrong context. Only the first CPU to XCHG gets the value.   */
    xor %rax, %rax
    xchg sched_switch_rsp(%rip), %rax
    test %rax, %rax
    jz .Lno_switch
    mov %rax, %rsp
.Lno_switch:

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
/* On APs (LAPIC ID != 0), just EOI and return without touching
 * any shared state. On BSP, go through full isr_common. */

.globl isr_stub_32
isr_stub_32:
    push %rax
    /* Check LAPIC ID — if non-zero, we're an AP: fast EOI path */
    movabs $0xFFFF800000000000 + 0xFEE00020, %rax
    movl   (%rax), %eax
    shrl   $24, %eax
    testl  %eax, %eax
    jnz    .Ltimer_ap_eoi
    pop    %rax
    /* BSP: normal ISR path */
    push   $0
    push   $32
    jmp    isr_common
.Ltimer_ap_eoi:
    /* AP: just EOI and return */
    movabs $0xFFFF800000000000 + 0xFEE000B0, %rax
    movl   $0, (%rax)
    pop    %rax
    iretq

/* ── Vector 33: Keyboard IRQ (IRQ 1) ────────────────────────── */

ISR_NOERR 33

/* ── Vector 0xFE: SMP work IPI (lightweight, no fxsave) ─────── */
/* APs receive this to wake from HLT. Only does APIC EOI — no GPR
 * save, no FPU save, no scheduler. The AP's work loop polls
 * work_pending after waking, so the ISR itself is a no-op. */

.globl isr_stub_smp_ipi
isr_stub_smp_ipi:
    push %rax
    /* APIC EOI: write 0 to PHYS_TO_VIRT(0xFEE000B0) */
    movabs $0xFFFF800000000000 + 0xFEE000B0, %rax
    movl   $0, (%rax)
    pop    %rax
    iretq

/* ── Default stub for vectors 34-255 ─────────────────────────── */

.globl isr_stub_default
isr_stub_default:
    push $0              /* dummy error code */
    push $255            /* generic vector */
    jmp isr_common

/* ── Scheduler RSP switch variable (X-SCHED) ──────────────── */
/* When non-zero, ISR stub loads this as RSP before popping    */
/* GPRs, effectively switching to another process's context.   */

.bss
.align 8
.globl sched_switch_rsp
sched_switch_rsp:
    .quad 0
