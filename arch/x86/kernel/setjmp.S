/*
 * OsitoK x86-64 — Minimal setjmp/longjmp
 *
 * Used by process subsystem to save/restore kernel context
 * when executing user programs.
 *
 * jmp_buf layout (9 × uint64_t = 72 bytes):
 *   [0] RBX  [1] RBP  [2] R12  [3] R13
 *   [4] R14  [5] R15  [6] RSP  [7] RIP (return addr)
 *   [8] CR3  (address space at the time of setjmp)
 */

.code64
.text

/* int kern_setjmp(uint64_t *buf)
 * Returns 0 on save, non-zero on longjmp restore.
 */
.globl kern_setjmp
kern_setjmp:
    mov  %rbx,   (%rdi)
    mov  %rbp,  8(%rdi)
    mov  %r12, 16(%rdi)
    mov  %r13, 24(%rdi)
    mov  %r14, 32(%rdi)
    mov  %r15, 40(%rdi)
    lea  8(%rsp), %rax       /* RSP after return (skip return address) */
    mov  %rax, 48(%rdi)
    mov  (%rsp), %rax        /* Return address */
    mov  %rax, 56(%rdi)
    mov  %cr3, %rax          /* Save current address space */
    mov  %rax, 64(%rdi)
    xor  %eax, %eax          /* Return 0 */
    ret

/* void kern_longjmp(uint64_t *buf, int val)
 * Restores context, kern_setjmp returns val (or 1 if val==0).
 *
 * The previous version loaded the new CR3 first and THEN read the
 * remaining state from rdi. That triple-faulted when the new CR3
 * happened to lack a mapping for the rdi page (e.g., longjmp from
 * within a transient address space whose kernel mappings differed
 * from the setjmp-time one — caught with UT99 after _initterm done).
 *
 * Fix: read EVERYTHING from rdi into kernel-side registers BEFORE
 * touching CR3. After the CR3 switch we never dereference rdi again,
 * so the new CR3 only needs to map the saved RIP + RSP — both of
 * which are kernel addresses by construction (rdi was a kernel BSS
 * jmpbuf and the saved RSP was the kernel-stack RSP at setjmp time).
 */
.globl kern_longjmp
kern_longjmp:
    mov  %esi, %eax
    test %eax, %eax
    jnz  1f
    inc  %eax                /* Ensure non-zero return */
1:
    /* Pre-load all 9 jmp_buf words while rdi is still valid. We park
     * the four "destination" registers (rbx/rbp/r12-r15) directly,
     * the new CR3 in rcx, and the new RSP and RIP in r10/r11. */
    mov    (%rdi), %rbx
    mov   8(%rdi), %rbp
    mov  16(%rdi), %r12
    mov  24(%rdi), %r13
    mov  32(%rdi), %r14
    mov  40(%rdi), %r15
    mov  48(%rdi), %r10      /* new RSP */
    mov  56(%rdi), %r11      /* new RIP */
    mov  64(%rdi), %rcx      /* new CR3 */

    /* Optimization (and safety net): if the saved CR3 matches the
     * current CR3, skip the cr3 mov entirely. mov-to-cr3 flushes the
     * TLB and triggers a page-table walk for the next fetch — both
     * unnecessary if the address space hasn't changed, and triple-
     * faulting if the saved CR3's PML4[256] (kernel high-half PDPT)
     * has gone stale relative to the running kernel's. r9 is scratch;
     * rax holds the return value and must not be clobbered. */
    mov  %cr3, %r9
    cmp  %r9, %rcx
    je   2f
    mov  %rcx, %cr3
2:

    /* Land on the saved kernel stack and continue at the saved RIP. */
    mov  %r10, %rsp
    jmp  *%r11
