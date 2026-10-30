/* Position independent. RDI points to handoff_t; no old stack/table survives. */
.text
.global kexec_trampoline
.global kexec_trampoline_end
.global kexec_trampoline_fault
kexec_trampoline:
    cli
    cld
    mov 0(%rdi), %r8             /* private CR3 */
    mov 8(%rdi), %rsp            /* private, 16-byte aligned physical stack */
    mov 16(%rdi), %r13           /* entry */
    mov 24(%rdi), %r12           /* boot info */
    mov 32(%rdi), %r14           /* segment table */
    mov 40(%rdi), %r15           /* count */
    lgdt 48(%rdi)
    lidt 58(%rdi)
    mov %cr4, %r10
    mov %r10, %rax
    and $-129, %rax              /* invalidate global mappings as well */
    mov %rax, %cr4
    mov %r8, %cr3
    mov %r10, %cr4
    xor %ebp, %ebp
.Lcopy:
    test %r15, %r15
    jz .Lentry
    mov 0(%r14), %rsi
    mov 8(%r14), %rdi
    mov 16(%r14), %rcx
    rep movsb
    mov 24(%r14), %rcx
    sub 16(%r14), %rcx
    xor %eax, %eax
    rep stosb                    /* BSS is zeroed only at its final destination */
    add $32, %r14
    dec %r15
    jmp .Lcopy
.Lentry:
    mov $10000, %ecx
    mov $0x3fd, %dx
.Luart:
    inb %dx, %al
    cmp $0xff, %al
    je .Lcall
    test $0x20, %al
    jnz .Lprobe
    loop .Luart
    jmp .Lcall
.Lprobe:
    mov $0x3f8, %dx
    mov $'T', %al
    outb %al, %dx
.Lcall:
    mov %r12, %rdi
    call *%r13                   /* SysV entry RSP % 16 == 8 */
kexec_trampoline_fault:
    cli
    hlt
    jmp kexec_trampoline_fault
kexec_trampoline_end:
.section .note.GNU-stack,"",%progbits
