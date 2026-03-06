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
 * Note: Linux uses R10 instead of RCX for arg4 because SYSCALL
 * clobbers RCX (used to save RIP).
 */

.code64
.text

.globl syscall_entry
syscall_entry:
    /*
     * We're now in kernel mode with interrupts disabled.
     * RCX = user RIP, R11 = user RFLAGS.
     * RSP is still the user stack (no automatic stack switch in long mode SYSCALL).
     *
     * For ring-0 only (no user processes yet), RSP is already kernel stack.
     * When we add user processes, we'll need to swap to kernel stack here.
     */

    /* Save callee-saved registers that syscall_dispatch might clobber */
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15

    /* Save user return info */
    push %rcx        /* saved RIP */
    push %r11        /* saved RFLAGS */

    /* Set up arguments for syscall_dispatch(nr, a1, a2, a3, a4, a5)
     * nr = RAX (already in place as 1st arg via RDI shuffle below)
     * a1 = RDI, a2 = RSI, a3 = RDX, a4 = R10, a5 = R8
     *
     * C calling convention: RDI, RSI, RDX, RCX, R8, R9
     * So: RDI=nr(RAX), RSI=a1(RDI), RDX=a2(RSI), RCX=a3(RDX), R8=a4(R10), R9=a5(R8)
     */
    mov %r8, %r9      /* a5 = R8 → R9 */
    mov %r10, %r8     /* a4 = R10 → R8 */
    mov %rdx, %rcx    /* a3 = RDX → RCX */
    mov %rsi, %rdx    /* a2 = RSI → RDX */
    mov %rdi, %rsi    /* a1 = RDI → RSI */
    mov %rax, %rdi    /* nr = RAX → RDI */

    /* Align stack to 16 bytes (ABI requirement) */
    mov %rsp, %rbp
    and $~0xF, %rsp

    /* Re-enable interrupts for syscall handler */
    sti

    call syscall_dispatch

    /* Disable interrupts for SYSRET */
    cli

    /* Restore stack */
    mov %rbp, %rsp

    /* Restore user return info */
    pop %r11          /* RFLAGS */
    pop %rcx          /* RIP */

    /* Restore callee-saved registers */
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp

    /* Return to caller.
     * SYSRET loads: RIP=RCX, RFLAGS=R11, CS/SS from STAR
     * For ring-0 only: use SYSRETQ.
     * RAX already has return value from syscall_dispatch. */
    sysretq
