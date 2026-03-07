/*
 * OsitoK x86-64 — SYSCALL Entry Point
 *
 * On SYSCALL:
 *   - RCX = saved RIP (return address)
 *   - R11 = saved RFLAGS
 *   - CS/SS loaded from STAR MSR (kernel segments)
 *   - RFLAGS masked by FMASK (IF cleared → interrupts off)
 *
 * ABI: RAX = syscall number
 *       RDI = arg1, RSI = arg2, RDX = arg3
 *       R10 = arg4, R8 = arg5, R9 = arg6
 * Return: RAX = result
 *
 * Per x86-64 SYSCALL ABI: only RCX and R11 are destroyed.
 * All other registers (RDI, RSI, RDX, R10, R8, R9, RBX, RBP,
 * R12-R15) must be preserved. We save them all on the stack,
 * shuffle copies into C calling convention, call dispatch,
 * then restore everything.
 */

.code64
.text

.globl syscall_entry
syscall_entry:
    /*
     * We're now in kernel mode with interrupts disabled.
     * RCX = user RIP, R11 = user RFLAGS.
     * RSP is still the user stack (ring-0 only).
     */

    /* Save ALL user registers we might clobber.
     * SYSCALL ABI: only RAX (return value), RCX, R11 may change.
     * We must preserve: RDI, RSI, RDX, R10, R8, R9,
     *                   RBP, RBX, R12-R15.
     * Stack frame (14 pushes = 112 bytes):
     */
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    push %rcx        /* saved RIP */
    push %r11        /* saved RFLAGS */
    /* Save argument registers that the shuffle will destroy */
    push %rdi
    push %rsi
    push %rdx
    push %r10
    push %r8
    push %r9

    /* Set up arguments for syscall_dispatch(nr, a1, a2, a3, a4, a5)
     * C calling convention: RDI, RSI, RDX, RCX, R8, R9
     * From user: RAX=nr, RDI=a1, RSI=a2, RDX=a3, R10=a4, R8=a5
     *
     * We read from the SAVED values on stack to avoid ordering issues.
     * Stack layout (from RSP): r9, r8, r10, rdx, rsi, rdi, ...
     */
    mov %r8, %r9      /* a5 = R8 → R9 (still has user value) */
    mov %r10, %r8     /* a4 = R10 → R8 */
    mov %rdx, %rcx    /* a3 = RDX → RCX */
    mov %rsi, %rdx    /* a2 = RSI → RDX */
    mov %rdi, %rsi    /* a1 = RDI → RSI */
    mov %rax, %rdi    /* nr = RAX → RDI */

    /* Align stack to 16 bytes (ABI requirement) */
    mov %rsp, %rbp
    and $~0xF, %rsp

    /* Interrupts stay disabled during syscall (non-preemptive kernel).
     * kern_longjmp (proc_exit) bypasses the return path, so keeping
     * interrupts off avoids races with the APIC timer. */
    call syscall_dispatch

    /* Restore stack (undo alignment) */
    mov %rbp, %rsp

    /* Restore argument registers */
    pop %r9
    pop %r8
    pop %r10
    pop %rdx
    pop %rsi
    pop %rdi

    /* Restore user return info */
    pop %r11          /* saved RFLAGS */
    pop %rcx          /* saved RIP */

    /* Restore callee-saved registers */
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp

    /*
     * Return to caller (ring-0 only).
     * RAX = syscall return value (from syscall_dispatch).
     * All other registers restored to user values.
     *
     * Use STI (not POPFQ) to re-enable interrupts.
     * STI has a 1-instruction interrupt shadow, so the JMP
     * executes atomically before any pending interrupt fires.
     */
    sti
    jmp *%rcx           /* Jump to saved RIP (in STI shadow) */
