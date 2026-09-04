/*
 * OsitoK — INT 0x2E Handler — TSS IST1 with re-entrant stacking
 *
 * IST1 gives a dedicated stack. For re-entrancy, we update IST1 after
 * saving state, and save the old IST1 ON THE STACK (not in a register),
 * because kern_longjmp may clobber callee-saved registers.
 */

.code64
.text

.globl int2e_stub
int2e_stub:
    /* CPU loaded RSP from TSS.IST1, pushed 40-byte iret frame.
     * RAX = thunk index, RCX = nargs. */

    /* Save all callee-clobbered + callee-saved registers */
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

    /* Save old IST1 value ON THE STACK (survives longjmp) */
    movq tss_ist1_ptr(%rip), %r11
    pushq (%r11)                     /* [RSP] = old IST1 */

    /* Update IST1 to well below current RSP.
     * Must leave room for compat32_dispatch + compat32_seh_dispatch +
     * compat32_callback + kern_setjmp call chain (measured ~1-3KB). Nested
     * INTs push BELOW this. 16KB/level over a 1MB IST1 stack permits 64 nested
     * transitions while leaving enough room for exception re-entry. */
    movq %rsp, %r10
    subq $16384, %r10
    movq %r10, (%r11)

    /*
     * Stack layout (15 values × 8 = 120 + iret 40 = 160 total):
     *   [RSP +   0]:  saved old IST1
     *   [RSP +   8]:  R15
     *   [RSP +  16]:  R14
     *   ...
     *   [RSP + 112]:  RBX
     *   [RSP + 120]:  iret RIP_old
     *   [RSP + 128]:  iret CS_old
     *   [RSP + 136]:  iret RFLAGS_old
     *   [RSP + 144]:  iret RSP_old
     *   [RSP + 152]:  iret SS_old
     */

    /* RDI = thunk_idx */
    mov %eax, %edi

    /* RSI = &stack_args = RSP_old + 4
     * RSP_old at [RSP + 144] */
    movq 144(%rsp), %rsi
    addq $4, %rsi

    /* RDX = saved entry frame. compat32_dispatch copies its i386 register
     * state into the current scheduler slot before invoking any shim. */
    movq %rsp, %rdx

    and $~0xF, %rsp
    call compat32_dispatch

    /* Recover this entry's saved frame from its current IST1 cursor. */
    movq tss_ist1_ptr(%rip), %r10
    movq (%r10), %rsp
    addq $16384, %rsp

    /* i386 returns 64-bit integers in EDX:EAX. EAX already survives the
     * handler; publish the high half through the saved RDX slot. For ordinary
     * 32-bit returns EDX is caller-clobbered, so zero/high propagation is also
     * ABI-correct. */
    movq %rax, %r11
    shrq $32, %r11
    movq %r11, 96(%rsp)

    /* Apply and consume only this scheduler slot's pending SEH/longjmp
     * transfer, then pop the matching per-thread INT2E context. The helper
     * returns the shim result unchanged in RAX. */
    movq %rsp, %rdi
    movq %rax, %rsi
    call compat32_int2e_complete

    /* Restore old IST1 from stack */
    movq tss_ist1_ptr(%rip), %r11
    popq (%r11)                      /* TSS.IST1 = saved old value */

    /* Restore registers */
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

    iretq
.section .note.GNU-stack,"",%progbits
