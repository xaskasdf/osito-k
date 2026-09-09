#include "../include/compat_iret.h"

.code64
.text
.globl x86_compat_iret
.type x86_compat_iret, @function
x86_compat_iret:
    /* Input is the five-qword hardware IRET frame; all GPRs are live. */
    cli
    testb $3, 8(%rsp)
    jz .Liret_direct
    push %rax
    push %rcx
    push %rdx
    movzwl 56(%rsp), %eax
    larl %eax, %edx
    jnz .Liret_restore
    testl $0x400000, %edx          /* SS.B: full-width stacks need no alias */
    jnz .Liret_restore
    movzwl 32(%rsp), %eax
    larl %eax, %edx
    jnz .Liret_restore
    testl $0x200000, %edx          /* CS.L: native 64-bit return */
    jnz .Liret_restore

    /* TSC_AUX stores the xAPIC ID, as in isr_common. No C call, allocation,
     * SIMD use or user-stack write is allowed in this final return path. */
    cmpl $0, isr_tsc_aux_enabled(%rip)
    je .Liret_apic_id
    rdtscp
    movl %ecx, %eax
    jmp .Liret_cpu
.Liret_apic_id:
    movabs $0xFFFF8000FEE00020, %rax
    movl (%rax), %eax
    shrl $24, %eax
.Liret_cpu:
    andl $255, %eax
    shll $6, %eax
    leaq x86_espfix_slots(%rip), %rdx
    addq %rax, %rdx
    xorl %ecx, %ecx
.Liret_copy:
    movq (%rsp,%rcx,8), %rax
    movq %rax, (%rdx,%rcx,8)
    incl %ecx
    cmpl $8, %ecx
    jb .Liret_copy

    leaq x86_espfix_slots(%rip), %rcx
    subq %rcx, %rdx
    movl 48(%rsp), %eax
    andl $0xFFFF0000, %eax
    orq %rdx, %rax
    movabs $X86_ESPFIX_BASE, %rcx
    orq %rcx, %rax
    movq %rax, %rsp
    /* An NMI/fault here uses its IST stack. A kernel-mode return never
     * reuses this CPU slot, so it cannot overwrite this pending frame. */
.Liret_restore:
    pop %rdx
    pop %rcx
    pop %rax
.Liret_direct:
    iretq
.size x86_compat_iret, .-x86_compat_iret
.section .note.GNU-stack,"",@progbits
