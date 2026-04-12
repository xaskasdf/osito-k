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
    /* Set up minimal stack (use a safe area at 16MB) */
    mov     $0x1000000, %rsp

    /* Jump to new kernel entry with boot_info_t* in RDI */
    mov     %r12, %rdi
    jmp     *%r13

kexec_trampoline_end:
    nop
