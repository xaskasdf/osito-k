/*
 * OsitoK x86-64 — Minimal syscall stubs for freestanding C programs.
 *
 * Linux-compatible ABI: RAX=nr, RDI/RSI/RDX/R10/R8/R9=args.
 * These are called by the C wrappers in crt.c.
 */

.code64
.text

.globl __syscall1
__syscall1:
    mov %rdi, %rax      /* nr */
    mov %rsi, %rdi      /* a1 */
    syscall
    ret

.globl __syscall2
__syscall2:
    mov %rdi, %rax
    mov %rsi, %rdi
    mov %rdx, %rsi
    syscall
    ret

.globl __syscall3
__syscall3:
    mov %rdi, %rax
    mov %rsi, %rdi
    mov %rdx, %rsi
    mov %rcx, %rdx
    syscall
    ret

.globl __syscall4
__syscall4:
    mov %rdi, %rax
    mov %rsi, %rdi
    mov %rdx, %rsi
    mov %rcx, %rdx
    mov %r8, %r10
    syscall
    ret
