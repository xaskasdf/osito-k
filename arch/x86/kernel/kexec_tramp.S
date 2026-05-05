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
     * NOTE: x86-64 SysV ABI says RSP % 16 == 8 at function entry
     * (post-CALL), so a strict reading of the spec says this should
     * be `mov $0x1000000, %rsp; sub $8, %rsp` before JMP-to-entry.
     * I (Claude) tried that in commit 20c1fbe and it BROKE more than
     * it fixed: instead of #GP at pci_scan+0x458 movaps it caused a
     * PF/UD at smp_init or gpu_init (different symbol each boot).
     *
     * Reverted on user request: "antes no tenia ese problema".
     *
     * Hypothesis: kernel_entry's compile-time stack frame happens to
     * work with RSP % 16 == 0 on entry (movaps offsets coincidentally
     * land on 16-aligned addrs).  Going forward, any new SSE-using
     * function reachable from kexec'd kernel needs to be inspected.
     *
     * Long-term fix: change the JMP to CALL so the standard ABI
     * applies naturally.  But CALL implies the callee can RET, and
     * kernel_entry never returns — needs a guarded fault page as
     * "return address" or similar.  Out of scope right now. */
    mov     $0x1000000, %rsp

    /* Jump to new kernel entry with boot_info_t* in RDI */
    mov     %r12, %rdi
    jmp     *%r13

kexec_trampoline_end:
    nop

.section .note.GNU-stack,"",%progbits
