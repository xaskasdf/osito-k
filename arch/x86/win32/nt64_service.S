/* Native NTDLL service gateway. Like the existing PE64 import boundary this
 * runs at CPL0 on the calling thread's stack, not a security boundary. All
 * state is on that stack so waits, callbacks and non-local exits cannot leave
 * a global dispatch cursor behind. */
.text
.code64
.globl nt64_service_entry
nt64_service_entry:
    push %r11
    push %rcx
    push %rbx
    push %rbp
    push %rsi
    push %rdi
    push %r12
    push %r13
    push %r14
    push %r15
    push %r10
    push %rdx
    push %r8
    push %r9
    mov %rsp, %rbp
    mov %eax, %edi
    mov %rbp, %rsi
    and $-16, %rsp
    sub $512, %rsp
    fxsave64 (%rsp)
    cld
    sti
    call nt64_service_dispatch
    cli
    fxrstor64 (%rsp)
    mov %rbp, %rsp
    pop %r9
    pop %r8
    pop %rdx
    pop %r10
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rdi
    pop %rsi
    pop %rbp
    pop %rbx
    pop %rcx
    pop %r11
    /* Preserve the existing PE64 boundary's interrupt-enabled invariant.
     * RCX/R11 and arithmetic flags are volatile across a native service. */
    sti
    jmp *%rcx

/* SysV (target, zero-filled args[16]) -> Microsoft x64 integer/pointer ABI.
 * Surplus stack slots are harmless; the descriptor bounds caller reads. */
.globl nt64_service_invoke
nt64_service_invoke:
    sub $136, %rsp
    mov %rdi, %r11
    mov %rsi, %rax
    lea 32(%rsi), %rsi
    lea 32(%rsp), %rdi
    mov $12, %ecx
    rep movsq
    mov 0(%rax), %rcx
    mov 8(%rax), %rdx
    mov 16(%rax), %r8
    mov 24(%rax), %r9
    call *%r11
    add $136, %rsp
    ret
.section .note.GNU-stack,"",%progbits
