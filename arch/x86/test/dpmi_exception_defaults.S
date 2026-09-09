/* Save and restore every DPMI exception vector without private host APIs. */
    .section .text
    .code16
    .global _start
_start:
    cld
    pushw %cs
    popw %ds
    movw %cs, real_segment
    movw $0x1687, %ax
    int $0x2f
    testw %ax, %ax
    jnz setup_fail
    movw %di, dpmi_entry
    movw %es, dpmi_entry + 2
    movw $PM32, %ax
    lcall *dpmi_entry
    jc setup_fail
    .if PM32
    movw %cs, %bx
    movw $6, %ax
    int $0x31
    jc setup_fail
    movw %cx, base_hi
    movw %dx, base_lo
    movw $1, %cx
    xorw %ax, %ax
    int $0x31
    jc setup_fail
    movw %ax, pm_pointer + 2
    movw %ax, %bx
    movw base_hi, %cx
    movw base_lo, %dx
    movw $7, %ax
    int $0x31
    jc setup_fail
    xorw %cx, %cx
    movw $0xffff, %dx
    movw $8, %ax
    int $0x31
    jc setup_fail
    movw $0x40fa, %cx
    movw $9, %ax
    int $0x31
    jc setup_fail
    ljmp *pm_pointer
    .else
    jmp test_entry
    .endif
setup_fail:
    movw $0x4c70, %ax
    int $0x21
dpmi_entry: .word 0, 0
pm_pointer: .word test_entry, 0
base_hi: .word 0
base_lo: .word 0
real_segment: .word 0

rm_debug_handler:
    incl %cs:real_calls
    iret

    .if PM32
    .code32
    .endif
test_entry:
    movl %esp, saved_sp
    movl $saved_vectors, %edi
    movw $0xa500, %bx
save_vector:
    movl $0xb6b60000, %ecx
    movl $0xc7c70000, %edx
    movw $0x0202, %ax
    int $0x31
    jc fail
    testw %cx, %cx
    jz fail
    movzwl %cx, %esi
    larl %esi, %esi
    jnz fail
    andl $0x009a00, %esi
    cmpl $0x009a00, %esi
    jne fail
    .if !PM32
    movl %edx, %esi
    shrl $16, %esi
    cmpl $0xc7c7, %esi
    jne fail
    .endif
    movl %edx, (%edi)
    movw %cx, 4(%edi)
    addl $6, %edi
    incb %bl
    cmpb $32, %bl
    jb save_vector

    /* Replace each handler, then restore the saved pointers with AX loaded
     * only once, as an extender's shutdown loop may do. */
    movw $0xa500, %bx
    movw $0x0203, %ax
    movw %cs, %cx
    movl $test_handler, %edx
install_vector:
    int $0x31
    jc fail
    incb %bl
    cmpb $32, %bl
    jb install_vector

    movw $0xa500, %bx
    movw $0x0203, %ax
    movl $saved_vectors, %edi
restore_vector:
    movl (%edi), %edx
    movw 4(%edi), %cx
    int $0x31
    jc fail
    cmpw $0x0203, %ax
    jne fail
    addl $6, %edi
    incb %bl
    cmpb $32, %bl
    jb restore_vector

    movw $0xa500, %bx
    movl $saved_vectors, %edi
check_vector:
    movl $0xc7c70000, %edx
    movw $0x0202, %ax
    int $0x31
    jc fail
    cmpl (%edi), %edx
    jne fail
    cmpw 4(%edi), %cx
    jne fail
    addl $6, %edi
    incb %bl
    cmpb $32, %bl
    jb check_vector
    cmpl saved_sp, %esp
    jne fail

    /* Tail-chain #DB through the host to a real-mode handler. The PM
     * handler redirects the saved IP, exercising the editable frame ABI. */
    movb $1, %bl
    movw $0x0200, %ax
    int $0x31
    jc fail
    movw %cx, old_rm_segment
    movw %dx, old_rm_offset
    movw real_segment, %cx
    movw $rm_debug_handler, %dx
    movw $0x0201, %ax
    int $0x31
    jc fail
    call save_chain
    movw %cs, %cx
    movl $debug_handler, %edx
    movw $0x0203, %ax
    int $0x31
    jc fail
    movl $0xA1B2C3D4, %eax
    .byte 0xf1
after_debug:
    cmpl $0xA1B2C3D4, %eax
    jne fail
    cmpl $1, pm_calls
    jne fail
    cmpl $1, real_calls
    jne fail
    cmpl saved_sp, %esp
    jne fail
    movb $1, %bl
    movw old_rm_segment, %cx
    movw old_rm_offset, %dx
    movw $0x0201, %ax
    int $0x31
    jc fail
    .if PM32
    movl old_chain, %edx
    movw old_chain + 4, %cx
    .else
    movw old_chain, %dx
    movw old_chain + 2, %cx
    .endif
    movw $0x0203, %ax
    int $0x31
    jc fail

    .if FATAL_CHAIN
    movb $13, %bl
    call save_chain
    movw %cs, %cx
    movl $fatal_handler, %edx
    movw $0x0203, %ax
    int $0x31
    jc fail
    movw $1, %cx
    xorw %ax, %ax
    int $0x31
    jc fail
    movw %ax, %bx
    xorw %cx, %cx
    xorw %dx, %dx
    movw $8, %ax
    int $0x31
    jc fail
    movw %bx, %es
    xorl %edi, %edi
    movl $1, %ecx
    movw $0x3c9, %dx
fatal_instruction:
    rep insl
    jmp fail
    .endif
    movl $pass, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c2a, %ax
    int $0x21
fail:
    movw $0x4c71, %ax
    int $0x21
test_handler:
    lret
save_chain:
    movw $0x0202, %ax
    int $0x31
    jc fail
    .if PM32
    movl %edx, old_chain
    movw %cx, old_chain + 4
    .else
    movw %dx, old_chain
    movw %cx, old_chain + 2
    .endif
    ret
debug_handler:
    .if PM32
    pushl %ebp
    movl %esp, %ebp
    movl $after_debug, %ss:16(%ebp)
    popl %ebp
    .else
    pushw %bp
    movw %sp, %bp
    movw $after_debug, %ss:8(%bp)
    popw %bp
    .endif
    incl pm_calls
    ljmp *old_chain
fatal_handler:
    ljmp *old_chain
saved_sp: .long 0
old_chain: .long 0
    .word 0
old_rm_segment: .word 0
old_rm_offset: .word 0
pm_calls: .long 0
real_calls: .long 0
saved_vectors: .space 32 * 6
    .if PM32
pass: .ascii "DPMI32 EXCEPTION VECTOR CONTRACT PASS\r\n$"
    .else
pass: .ascii "DPMI16 EXCEPTION VECTOR CONTRACT PASS\r\n$"
    .endif
