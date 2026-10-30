#include "dos_native_fp.h"

.text
/* Exercise the service-gate save/restore with an actual generic interrupt
 * inside it. Caller publishes a host-only DOS VM token, not native tables. */
.globl dos_fpu_irq_selftest
.type dos_fpu_irq_selftest,@function
dos_fpu_irq_selftest:
    push %rbp
    mov %rsp, %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    pushfq
    cli
    sub $(5 * DOS_NATIVE_FP_FRAME_BYTES + 63), %rsp
    and $-64, %rsp
    mov %rsp, %r12
    mov %r12, %rdi
    call dos_native_fp_save

    fldcw .Lguest_control(%rip)
    fldz
    fldz
    .byte 0xde, 0xf9           /* Pending unmasked invalid in outer client. */
    ldmxcsr .Lguest_mxcsr(%rip)
    .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    pcmpeqd %xmm\n, %xmm\n
    .endr
    cmpl $0, cpu_xsave_active(%rip)
    je 1f
    .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    vinsertf128 $1, %xmm\n, %ymm\n, %ymm\n
    .endr
1:  lea 1088(%r12), %rdi
    call dos_native_fp_save

    fldcw .Lhost_control(%rip)
    fld1
    .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    pxor %xmm\n, %xmm\n
    .endr
    cmpl $0, cpu_xsave_active(%rip)
    je 2f
    .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    vxorps %ymm\n, %ymm\n, %ymm\n
    .endr
2:  lea 2176(%r12), %rdi
    call dos_native_fp_save
    lea 2176(%r12), %rdi
    call dos_native_fp_restore
    int $0xff                 /* Default kernel vector returns via isr_common. */
    lea 3264(%r12), %rdi
    call dos_native_fp_save
    lea 2176(%r12), %rsi
    lea 3264(%r12), %rdi
    call .Lcompare
    mov %eax, %ebx

    lea 1088(%r12), %rdi
    call dos_native_fp_restore
    lea 4352(%r12), %rdi
    call dos_native_fp_save
    lea 1088(%r12), %rsi
    lea 4352(%r12), %rdi
    call .Lcompare
    shl $1, %eax
    or %eax, %ebx

    /* A nested generic ISR must return the exact MP/EM/TS policy it found,
     * even though its C handler and FP save run with EM/TS cleared. */
    mov %cr0, %r15
    and $~0xe, %r15
    xor %r14d, %r14d
.Lcr0_loop:
    mov %r14, %rax
    shl $1, %rax
    or %r15, %rax
    mov %rax, %cr0
    int $0xff
    mov %cr0, %rdx
    mov %r15, %cr0
    mov %r14, %rax
    shl $1, %rax
    or %r15, %rax
    cmp %rax, %rdx
    je .Lcr0_ok
    or $4, %ebx
.Lcr0_ok:
    inc %r14d
    cmp $8, %r14d
    jb .Lcr0_loop
    mov %r12, %rdi
    call dos_native_fp_restore
    mov %ebx, %eax
    lea -48(%rbp), %rsp
    popfq
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret
.size dos_fpu_irq_selftest,.-dos_fpu_irq_selftest

.Lcompare:
    cld
    mov $5, %ecx              /* FCW, FSW and abridged tag, no reserved bytes. */
    repe cmpsb
    jne .Ldifferent
    add $19, %rdi
    add $19, %rsi
    mov $4, %ecx
    repe cmpsb                /* MXCSR */
    jne .Ldifferent
    add $4, %rdi
    add $4, %rsi
    mov $8, %r11d
3:  mov $10, %ecx
    repe cmpsb                /* Eight 80-bit physical x87/MMX registers. */
    jne .Ldifferent
    add $6, %rdi
    add $6, %rsi
    dec %r11d
    jnz 3b
    mov $256, %ecx
    repe cmpsb
    jne .Ldifferent
    cmpl $0, cpu_xsave_active(%rip)
    je .Lequal
    add $160, %rdi
    add $160, %rsi
    mov $256, %ecx
    repe cmpsb
    jne .Ldifferent
.Lequal:
    xor %eax, %eax
    ret
.Ldifferent:
    mov $1, %eax
    ret

/* Compare the gate with direct hardware restores of both architectural
 * layouts. Report pointer/selector coverage separately from preservation. */
.globl dos_fpu_native_selftest
.type dos_fpu_native_selftest,@function
dos_fpu_native_selftest:
    push %rbp
    mov %rsp, %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    pushfq
    cli
    mov %rdi, %r13
    mov %rsi, %r14
    movl $0, (%r14)
    sub $(6 * DOS_NATIVE_FP_FRAME_BYTES + 63), %rsp
    and $-64, %rsp
    mov %rsp, %r12
    mov %r12, %rdi
    call dos_native_fp_save
    xor %ebx, %ebx
    lea 1088(%r12), %rdi
    xor %eax, %eax
    mov $(5 * DOS_NATIVE_FP_FRAME_BYTES / 8), %ecx
    rep stosq
    fldcw .Lguest_control(%rip)
    fldz
    fldz
    .byte 0xde, 0xf9
    lea 1088(%r12), %rdi
    call dos_native_fp_save
    movabs $0xffff800012345678, %rax
    mov %rax, (1088 + 8)(%r12)
    movabs $0xffff800087654321, %rax
    mov %rax, (1088 + 16)(%r12)
    lea 1088(%r12), %rsi
    lea 5440(%r12), %rdi
    mov $(DOS_NATIVE_FP_BYTES / 8), %ecx
    rep movsq

    lea 1088(%r12), %rdi
    cmpl $0, cpu_xsave_active(%rip)
    je .Lnative_reference_fx64
    mov $7, %eax
    xor %edx, %edx
    xrstor64 (%rdi)
    jmp .Lnative_reference_saved64
.Lnative_reference_fx64:
    fxrstor64 (%rdi)
.Lnative_reference_saved64:
    lea 3264(%r12), %rdi
    call dos_native_fp_save
    mov (1088 + 8)(%r12), %rax
    cmp (3264 + 8)(%r12), %rax
    jne 1f
    mov (1088 + 16)(%r12), %rax
    cmp (3264 + 16)(%r12), %rax
    jne 1f
    orl $1, (%r14)
1:  lea 1088(%r12), %rdi
    call dos_native_fp_restore
    int $0xff
    lea 4352(%r12), %rdi
    call dos_native_fp_save
    call .Lnative_compare
    or %eax, %ebx

    mov %r13, %rdi
    lea 1088(%r12), %rsi
    call dos_native_fp_return
    cmpl $1, (1088 + DOS_NATIVE_FP_COMPAT_OFFSET)(%r12)
    jne .Lnative_metadata_bad
    cmpw $0x137, (1088 + DOS_NATIVE_FP_BYTES + 16)(%r12)
    jne .Lnative_metadata_bad
    cmpw $0x147, (1088 + DOS_NATIVE_FP_BYTES + 24)(%r12)
    je .Lnative_metadata_ok
.Lnative_metadata_bad:
    or $8, %ebx
.Lnative_metadata_ok:
    lea 1088(%r12), %rsi
    lea 2176(%r12), %rdi
    mov $(DOS_NATIVE_FP_BYTES / 8), %ecx
    rep movsq
    movl $0x137, (2176 + 12)(%r12)
    movl $0x147, (2176 + 20)(%r12)
    lea 2176(%r12), %rdi
    cmpl $0, cpu_xsave_active(%rip)
    je .Lnative_reference_fx32
    mov $7, %eax
    xor %edx, %edx
    xrstor (%rdi)
    jmp .Lnative_reference_saved32
.Lnative_reference_fx32:
    fxrstor (%rdi)
.Lnative_reference_saved32:
    lea 3264(%r12), %rdi
    call dos_native_fp_save
    cmpw $0x137, (3264 + DOS_NATIVE_FP_BYTES + 16)(%r12)
    jne 2f
    cmpw $0x147, (3264 + DOS_NATIVE_FP_BYTES + 24)(%r12)
    jne 2f
    orl $2, (%r14)
2:  cmpq $0x12345678, (3264 + 8)(%r12)
    jne 3f
    mov $0x87654321, %eax
    cmp %rax, (3264 + 16)(%r12)
    jne 3f
    orl $4, (%r14)
3:  lea 1088(%r12), %rdi
    call dos_native_fp_restore
    int $0xff
    lea 4352(%r12), %rdi
    call dos_native_fp_save
    call .Lnative_compare
    shl $1, %eax
    or %eax, %ebx

    lea 1088(%r12), %rsi
    lea 5440(%r12), %rdi
    mov $(DOS_NATIVE_FP_BYTES / 8), %ecx
    repe cmpsq
    je 4f
    or $4, %ebx
4:  mov %r12, %rdi
    call dos_native_fp_restore
    mov %ebx, %eax
    lea -48(%rbp), %rsp
    popfq
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret
.size dos_fpu_native_selftest,.-dos_fpu_native_selftest

.Lnative_compare:
    lea 3264(%r12), %rsi
    lea 4352(%r12), %rdi
    /* Include FOP and the complete FIP/FDP, but not reserved bytes. */
    add $6, %rsi
    add $6, %rdi
    mov $18, %ecx
    repe cmpsb
    jne .Ldifferent
    .irp offset,16,24
    movw (3264 + DOS_NATIVE_FP_BYTES + \offset)(%r12), %ax
    cmpw (4352 + DOS_NATIVE_FP_BYTES + \offset)(%r12), %ax
    jne .Ldifferent
    .endr
    lea 3264(%r12), %rsi
    lea 4352(%r12), %rdi
    jmp .Lcompare

/* Backend observation, not a conformance assertion. No C calls or maskable
 * IRQs intervene; CPUID forces hypervisor exits with IF clear (not an NMI test).
 * Bits 0/1 report masked pointer/selector loss, bits 2/3 pending loss.
 * Coverage uses the same bits for fields which accepted the seeded values. */
.globl dos_fpu_backend_observe
.type dos_fpu_backend_observe,@function
dos_fpu_backend_observe:
    push %rbp
    mov %rsp, %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    pushfq
    cli
    mov %rdi, %r14
    movl $0, (%r14)
    sub $(3 * DOS_NATIVE_FP_FRAME_BYTES + 63), %rsp
    and $-64, %rsp
    mov %rsp, %r12
    mov %r12, %rdi
    call dos_native_fp_save
    xor %r13d, %r13d
    xor %r15d, %r15d
.Lbackend_seed:
    fninit
    test %r13d, %r13d
    jz 1f
    fldcw .Lguest_control(%rip)
1:  fldz
    fldz
    .byte 0xde, 0xf9
    fnstenv 1088(%r12)
    movl $0x12345678, (1088 + 12)(%r12)
    movw $0x137, (1088 + 16)(%r12)
    movl $0x23456789, (1088 + 20)(%r12)
    movw $0x147, (1088 + 24)(%r12)
    fnclex
    fldenv 1088(%r12)
    fnstenv 2176(%r12)
    fldcw 1088(%r12)
    mov $100000, %r8d
2:  xor %eax, %eax
    cpuid
    dec %r8d
    jnz 2b
    fnstenv 2208(%r12)
    fninit
    mov $1, %edx
    mov %r13d, %ecx
    shl %cl, %edx
    cmpl $0x12345678, (2176 + 12)(%r12)
    jne 3f
    cmpl $0x23456789, (2176 + 20)(%r12)
    jne 3f
    or %edx, (%r14)
3:  mov (2176 + 12)(%r12), %eax
    cmp (2208 + 12)(%r12), %eax
    jne 4f
    mov (2176 + 20)(%r12), %eax
    cmp (2208 + 20)(%r12), %eax
    je 5f
4:  or %edx, %r15d
5:  shl $1, %edx
    cmpw $0x137, (2176 + 16)(%r12)
    jne 6f
    cmpw $0x147, (2176 + 24)(%r12)
    jne 6f
    or %edx, (%r14)
6:  movw (2176 + 16)(%r12), %ax
    cmpw (2208 + 16)(%r12), %ax
    jne 7f
    movw (2176 + 24)(%r12), %ax
    cmpw (2208 + 24)(%r12), %ax
    je 8f
7:  or %edx, %r15d
8:  add $2, %r13d
    cmp $4, %r13d
    jne .Lbackend_seed
    mov %r12, %rdi
    call dos_native_fp_restore
    mov %r15d, %eax
    lea -48(%rbp), %rsp
    popfq
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret
.size dos_fpu_backend_observe,.-dos_fpu_backend_observe

.section .rodata
.balign 4
.Lguest_mxcsr: .long 0x5f80
.Lguest_control: .word 0x037e
.Lhost_control: .word 0x077f
.section .note.GNU-stack,"",@progbits
