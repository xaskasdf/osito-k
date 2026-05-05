/*
 * kexec trampoline — copies kernel segments to final addresses and jumps to entry.
 *
 * Called with:
 *   RDI = pointer to boot_info_t (preserved for new kernel)
 *   RSI = entry point address
 *   RDX = pointer to segment table [{src, dst, len}, ...]  (24 bytes each)
 *   RCX = number of segments
 *
 * This code must be position-independent and is copied to a safe address
 * before execution (above all kernel segments).
 */

.text
.global kexec_trampoline
.global kexec_trampoline_end

kexec_trampoline:
    /* Save args in callee-saved regs */
    mov     %rdi, %r12          /* boot_info_t* */
    mov     %rsi, %r13          /* entry point */
    mov     %rdx, %r14          /* segment table */
    mov     %rcx, %r15          /* segment count */

    /* Disable interrupts */
    cli

    cld                         /* forward copy direction */

.Lcopy_loop:
    test    %r15, %r15
    jz      .Lcopy_done

    mov     0(%r14), %rsi       /* src address */
    mov     8(%r14), %rdi       /* dst address */
    mov     16(%r14), %rcx      /* byte count */
    rep     movsb               /* copy segment */

    add     $24, %r14           /* next segment entry */
    dec     %r15
    jmp     .Lcopy_loop

.Lcopy_done:
    /* Set up minimal stack (use a safe area at 16MB).
     *
     * x86-64 SysV ABI: at function entry, RSP must satisfy
     * (RSP % 16) == 8.  This accounts for the 8-byte return address
     * pushed by CALL (caller's RSP is 16-aligned, CALL pushes 8 →
     * callee sees RSP % 16 == 8).  The compiler emits SSE moves
     * (movaps) on locals via `[rbp - offset]` assuming RBP % 16 == 0,
     * which is satisfied iff (RSP % 16) == 8 at entry (then `push rbp;
     * mov rsp, rbp` lands at RBP % 16 == 0).
     *
     * Since we use JMP not CALL, no return address is pushed.  We
     * adjust RSP by -8 to simulate it.  Without this fix, kexec'd
     * kernel #GPs at the first SSE local-zero in any non-trivial
     * function — observed at pci_scan+0x458 movaps %xmm0,(%rax).      */
    mov     $0x1000000, %rsp
    sub     $8, %rsp

    /* Jump to new kernel entry with boot_info_t* in RDI */
    mov     %r12, %rdi
    jmp     *%r13

kexec_trampoline_end:
    nop

.section .note.GNU-stack,"",%progbits
