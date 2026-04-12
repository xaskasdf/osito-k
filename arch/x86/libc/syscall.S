/*
 * OsitoK x86-64 — Minimal syscall stubs for freestanding C programs.
 *
 * Linux-compatible ABI: RAX=nr, RDI/RSI/RDX/R10/R8/R9=args.
 * These are called by the C wrappers in crt.c.
 */

.code64
.text

.globl __syscall0
__syscall0:
    mov %rdi, %rax      /* nr */
    syscall
    ret

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

.globl __syscall5
__syscall5:
    mov %rdi, %rax
    mov %rsi, %rdi
    mov %rdx, %rsi
    mov %rcx, %rdx
    mov %r8, %r10
    mov %r9, %r8
    syscall
    ret

.globl __syscall6
__syscall6:
    mov %rdi, %rax
    mov %rsi, %rdi
    mov %rdx, %rsi
    mov %rcx, %rdx
    mov %r8, %r10
    mov %r9, %r8
    mov 8(%rsp), %r9      /* 7th C arg is on stack */
    syscall
    ret

/*
 * _setjmp / longjmp — save/restore execution context.
 * jmp_buf layout (8 x 8 = 64 bytes):
 *   [0] RBX  [1] RBP  [2] R12  [3] R13
 *   [4] R14  [5] R15  [6] RSP  [7] RIP (return address)
 */

.globl _setjmp
.globl setjmp
_setjmp:
setjmp:
    mov %rbx, (%rdi)
    mov %rbp, 8(%rdi)
    mov %r12, 16(%rdi)
    mov %r13, 24(%rdi)
    mov %r14, 32(%rdi)
    mov %r15, 40(%rdi)
    lea 8(%rsp), %rax       /* RSP before call (skip return addr) */
    mov %rax, 48(%rdi)
    mov (%rsp), %rax        /* return address */
    mov %rax, 56(%rdi)
    xor %eax, %eax          /* return 0 */
    ret

/*
 * _start — ELF entry point.
 * Reads argc/argv from stack before any prologue can corrupt RSP.
 * Stack layout from ELF loader: RSP → argc, argv[0], ..., NULL, envp
 */
.globl _start
_start:
    xor %rbp, %rbp          /* Clear frame pointer (ABI requirement) */
    mov (%rsp), %rdi         /* argc */
    lea 8(%rsp), %rsi        /* argv = &sp[1] */
    
    /* Set up environ: envp starts after argv NULL terminator */
    mov %rdi, %rax
    lea 16(%rsp, %rax, 8), %rdx /* rdx = &sp[argc + 2] */
    movq %rdx, environ(%rip)

    and $-16, %rsp           /* Align stack to 16 bytes */
    call main
    mov %eax, %edi           /* exit(main return value) */
    mov $60, %eax            /* SYS_exit */
    syscall
    jmp .                    /* unreachable */

.extern environ


.globl longjmp
longjmp:
    mov %esi, %eax          /* return value */
    test %eax, %eax
    jnz 1f
    inc %eax                /* longjmp(buf, 0) returns 1 */
1:
    mov (%rdi), %rbx
    mov 8(%rdi), %rbp
    mov 16(%rdi), %r12
    mov 24(%rdi), %r13
    mov 32(%rdi), %r14
    mov 40(%rdi), %r15
    mov 48(%rdi), %rsp
    jmp *56(%rdi)           /* jump to saved RIP */
