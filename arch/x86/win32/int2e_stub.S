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
     * Must leave room for compat32_dispatch + _initterm + compat32_callback
     * + kern_setjmp call chain (~1KB). Nested INTs push BELOW this. */
    movq %rsp, %r10
    subq $32768, %r10
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

    /* Stash the user-mode 32-bit EBP (low 32 bits of saved RBP at
     * offset 72 in the GPR-save area) into a global so the C
     * dispatcher and shims can walk the user-mode frame chain.
     * Diagnostic only — used by the VirtualAlloc shim to identify
     * the caller of FMallocWindows::Realloc. */
    movq 72(%rsp), %r10
    movq %r10, g_int2e_user_rbp(%rip)

    /* Also stash ECX (=this for __thiscall), EDX, ESI, EDI for
     * deep instrumentation of FArray::Realloc and similar engine
     * methods. Stack offsets per the layout above. */
    movq 104(%rsp), %r10            /* RCX */
    movq %r10, g_int2e_user_rcx(%rip)
    movq  96(%rsp), %r10            /* RDX */
    movq %r10, g_int2e_user_rdx(%rip)
    movq  88(%rsp), %r10            /* RSI */
    movq %r10, g_int2e_user_rsi(%rip)
    movq  80(%rsp), %r10            /* RDI */
    movq %r10, g_int2e_user_rdi(%rip)

    /* Save RSP to per-depth stack before aligning and calling C dispatcher.
     * Cannot use RBP (kern_longjmp overwrites it) or a single global
     * (nested INT 0x2E from callback returns overwrites it). */
    leaq g_int2e_rsp_stack(%rip), %r10
    leaq g_int2e_rsp_depth(%rip), %r12
    movl (%r12), %r13d
    movq %rsp, (%r10,%r13,8)        /* save RSP at stack[depth] */
    leal 1(%r13d), %r14d
    movl %r14d, (%r12)               /* depth++ */

    and $~0xF, %rsp
    call compat32_dispatch

    /* Restore RSP from per-depth stack */
    leaq g_int2e_rsp_stack(%rip), %r10
    leaq g_int2e_rsp_depth(%rip), %r12
    movl (%r12), %r13d
    subl $1, %r13d                    /* depth-- */
    movl %r13d, (%r12)
    movq (%r10,%r13,8), %rsp         /* restore RSP from stack[depth] */

    /*
     * C++ EH catch dispatch: if compat32_seh_dispatch found a catch
     * handler, g_compat32_unwind_eip/esp/ebp are set. Modify the iret
     * frame to jump to the catch handler with the correct EBP/ESP.
     */
    leaq g_compat32_unwind_eip(%rip), %r10
    movl (%r10), %r11d
    testl %r11d, %r11d
    jz .Lno_unwind

    /* Overwrite iret RIP with catch handler EIP */
    movq %r11, 120(%rsp)
    movl $0, (%r10)                  /* clear eip */

    /* Overwrite iret RSP */
    leaq g_compat32_unwind_esp(%rip), %r10
    movl (%r10), %r11d
    movq %r11, 144(%rsp)
    movl $0, (%r10)                  /* clear esp */

    /* Set EBP in saved register area (RBP is at RSP+72 after 15 pushes + IST1) */
    leaq g_compat32_unwind_ebp(%rip), %r10
    movl (%r10), %r11d
    movq %r11, 72(%rsp)              /* offset 72 = saved RBP */
    movl $0, (%r10)                  /* clear ebp */

.Lno_unwind:
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

.section .bss
.align 8
g_int2e_rsp_stack:
    .fill 32, 8, 0              /* 32 depth levels × 8 bytes */
/* g_int2e_rsp_depth defined in compat32.c */

.section .note.GNU-stack,"",%progbits
