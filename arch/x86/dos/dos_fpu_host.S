#include "dos_native_fp.h"

.text
/* All opcodes are trusted, fixed thunks. The caller validates the encoding,
 * stages the complete operand, and checks pending exceptions before entry.
 * No C or interruptible window runs with guest floating-point state loaded. */
.globl dos_fpu_host_step
.type dos_fpu_host_step,@function
dos_fpu_host_step:
    push %rbp
    mov %rsp, %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    pushfq
#ifndef DOS_FPU_HOST_TEST
    cli
#endif
    mov %rdi, %r12             /* VM FXSAVE image */
    mov %rsi, %r13             /* trusted opcode thunk */
    mov %rdx, %r14             /* staged operand */
    mov %ecx, %r15d            /* guest arithmetic FLAGS */
    sub $(DOS_NATIVE_FP_BYTES + 63), %rsp
    and $-64, %rsp
    mov %rsp, %rbx
    movq $0, 512(%rbx)
    movq $0, 520(%rbx)
    movq $0, 528(%rbx)
    movq $0, 536(%rbx)
    movq $0, 544(%rbx)
    movq $0, 552(%rbx)
    movq $0, 560(%rbx)
    movq $0, 568(%rbx)
    cmpl $0, cpu_xsave_active(%rip)
    je .Lfp_save_fx
    mov $7, %eax
    xor %edx, %edx
    xsave64 (%rbx)
    jmp .Lfp_load
.Lfp_save_fx:
    fxsave64 (%rbx)
.Lfp_load:
    fxrstor64 (%r12)
    mov %r14, %rdi
    mov %r15d, %eax
    shl $8, %eax
    sahf
    call *%r13
    movzwl %ax, %r14d
    pushfq
    pop %r15
    fxsave64 (%r12)
    fninit
    cmpl $0, cpu_xsave_active(%rip)
    je .Lfp_restore_fx
    mov $7, %eax
    xor %edx, %edx
    xrstor64 (%rbx)
    jmp .Lfp_return
.Lfp_restore_fx:
    fxrstor64 (%rbx)
.Lfp_return:
    mov %r15, %rax
    shl $32, %rax
    or %r14, %rax
    lea -48(%rbp), %rsp
    popfq
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret
.size dos_fpu_host_step,.-dos_fpu_host_step

/* Eight-byte slots permit bounded indexing without executable guest bytes.
 * Undefined slots exist in the table but are never admitted by the decoder. */
.balign 8
.globl dos_fpu_register_ops
dos_fpu_register_ops:
.set fp_opcode, 0xd8
.rept 8
    .set fp_modrm, 0xc0
    .rept 64
        .byte fp_opcode, fp_modrm, 0xc3
        .fill 5, 1, 0xcc
        .set fp_modrm, fp_modrm + 1
    .endr
    .set fp_opcode, fp_opcode + 1
.endr
.balign 8
.globl dos_fpu_memory_ops
dos_fpu_memory_ops:
.set fp_opcode, 0xd8
.rept 8
    .set fp_group, 0
    .rept 8
        .byte fp_opcode, (fp_group * 8 + 7), 0xc3 /* [RDI] */
        .fill 5, 1, 0xcc
        .set fp_group, fp_group + 1
    .endr
    .set fp_opcode, fp_opcode + 1
.endr
.globl dos_fpu_noop
dos_fpu_noop:
    ret
.section .note.GNU-stack,"",%progbits
