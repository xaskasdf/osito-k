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
    /* Set up minimal stack (use a safe area at 16MB) and CALL — not
     * JMP — into kernel_entry.  This is the architecturally correct
     * primitive: x86-64 SysV ABI mandates RSP % 16 == 8 at function
     * entry (the post-CALL state — RSP was 16-byte aligned before
     * CALL pushed an 8-byte return address).  The compiler emits SSE
     * moves on locals via [rbp - K] assuming this alignment.
     *
     * Earlier this used JMP, leaving RSP at 0x1000000 (RSP % 16 == 0)
     * at function entry — ABI-violating.  Triggered #GP at the first
     * movaps reachable from kexec'd boot path (e.g. pci_scan+0x458).
     *
     * I tried `sub $8, %rsp; jmp` (commit 20c1fbe) which is
     * mathematically equivalent to CALL's stack adjustment.  It
     * caused new crashes (smp_init+0 #PF, garbled boot output).
     * Reverted in commit da8df4d.
     *
     * CALL is strictly better than the ad-hoc sub-and-jmp:
     *   1. Same ABI-compliant stack effect (-8 from a 16-aligned RSP).
     *   2. The pushed return address is the well-known .Lhalt_loop
     *      below, so any runaway RET in kernel_entry lands in a
     *      defined halt instead of garbage memory or attacker-
     *      controllable bytes.
     *   3. Self-documenting: future readers see "this calls kernel
     *      entry" not "this does a sub-then-jmp dance".
     *
     * If this still misbehaves, the issue is NOT stack alignment but
     * something else (paging, IDT, GPU MMIO state) — investigate
     * those instead of fiddling with this prologue. */
    mov     $0x1000000, %rsp
    mov     %r12, %rdi
    call    *%r13

    /* kernel_entry must never return — if it somehow does, halt
     * deterministically rather than executing whatever bytes follow. */
.Lhalt_loop:
    cli
    hlt
    jmp     .Lhalt_loop

kexec_trampoline_end:
    nop

.section .note.GNU-stack,"",%progbits
