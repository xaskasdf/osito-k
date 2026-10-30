.text
.globl nt64_register_probe
nt64_register_probe:
    push %rbx
    push %rbp
    push %rsi
    push %rdi
    push %r12
    push %r13
    push %r14
    push %r15
    sub $88, %rsp
    movdqu %xmm6, 48(%rsp)
    movdqu %xmm15, 64(%rsp)
    mov %rcx, %rax
    mov $0x11223344, %ebx
    mov $0x22334455, %ebp
    mov $0x33445566, %esi
    mov $0x44556677, %edi
    mov $0x55667788, %r12d
    mov $0x66778899, %r13d
    mov $0x778899aa, %r14d
    mov $0x12345678, %r15d
    pcmpeqd %xmm6, %xmm6
    pcmpeqd %xmm15, %xmm15
    lea 32(%rsp), %rcx
    lea 40(%rsp), %rdx
    call *%rax
    test %eax, %eax
    jne 1f
    cmp $0x11223344, %rbx
    jne 1f
    cmp $0x22334455, %rbp
    jne 1f
    cmp $0x33445566, %rsi
    jne 1f
    cmp $0x44556677, %rdi
    jne 1f
    cmp $0x55667788, %r12
    jne 1f
    cmp $0x66778899, %r13
    jne 1f
    cmp $0x778899aa, %r14
    jne 1f
    cmp $0x12345678, %r15
    jne 1f
    pcmpeqd %xmm0, %xmm0
    pcmpeqb %xmm0, %xmm6
    pmovmskb %xmm6, %eax
    cmp $65535, %eax
    jne 1f
    pcmpeqb %xmm0, %xmm15
    pmovmskb %xmm15, %eax
    cmp $65535, %eax
    jne 1f
    mov $1, %eax
    jmp 2f
1:  xor %eax, %eax
2:  movdqu 48(%rsp), %xmm6
    movdqu 64(%rsp), %xmm15
    add $88, %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rdi
    pop %rsi
    pop %rbp
    pop %rbx
    ret
.globl nt64_stack_probe
nt64_stack_probe:
    push %rbx
    mov %rsp, %rbx
    mov %rcx, %rax
    /* The return address and shadow space are valid; argument five begins
     * exactly at the inaccessible page. Leave ample stack below for CPL0. */
    lea -32(%rdx), %rsp
    mov $-1, %rcx
    xor %edx, %edx
    mov $48, %r9d
    call *%rax
    mov %rbx, %rsp
    pop %rbx
    ret
