/*
 * OsitoK x86-64 — Minimal setjmp/longjmp
 *
 * Used by process subsystem to save/restore kernel context
 * when executing user programs.
 *
 * jmp_buf layout (8 × uint64_t = 64 bytes):
 *   [0] RBX  [1] RBP  [2] R12  [3] R13
 *   [4] R14  [5] R15  [6] RSP  [7] RIP (return addr)
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
    xor  %eax, %eax          /* Return 0 */
    ret

/* void kern_longjmp(uint64_t *buf, int val)
 * Restores context, kern_setjmp returns val (or 1 if val==0).
 * Also restores CS to the original 64-bit code segment via LRETQ
 * (SYSCALL may have changed CS to a different code segment).
 */
.globl kern_longjmp
kern_longjmp:
    mov  %esi, %eax
    test %eax, %eax
    jnz  1f
    inc  %eax                /* Ensure non-zero return */
1:
    mov    (%rdi), %rbx
    mov   8(%rdi), %rbp
    mov  16(%rdi), %r12
    mov  24(%rdi), %r13
    mov  32(%rdi), %r14
    mov  40(%rdi), %r15
    mov  48(%rdi), %rsp
    jmp *56(%rdi)
