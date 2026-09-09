/* Real exceptions, separate PM ownership, nesting and default IVT chaining. */
    .section .text
    .code16
    .global _start
_start:
    cld
    pushw %cs
    popw %ds
    pushw %cs
    popw %es
    movw %cs, real_segment
    movw $0x1000, %bx
    movw $0x4a00, %ax
    int $0x21
    jc setup_fail
    movw $0x1687, %ax
    int $0x2f
    testw %ax, %ax
    jnz setup_fail
    movw %di, entry
    movw %es, entry + 2
    movw $PM32, %ax
    lcall *entry
    jc setup_fail
    movw %ds, data_sel
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
    movw %ax, pm_entry + 2
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
    ljmp *pm_entry
    .else
    jmp protected_entry
    .endif
setup_fail:
    movw $0x4c70, %ax
    int $0x21

    .if PM32
    .code32
    .equ FIELD, 4
    .else
    .equ FIELD, 2
    .endif
    .macro extended_return
    popal
    .if STACK32
    addl $32, %esp
    .else
    addw $32, %sp
    .endif
    lret
    .endm
    .macro real_frame
    pushal
    movw %cs:data_sel, %ax
    movw %ax, %ds
    .if STACK32
    leal 32(%esp), %ebp
    .else
    movzwl %sp, %ebp
    addl $32, %ebp
    .endif
    testl $0x20000, %ss:0x34(%ebp)
    jz fail
    testw $1, %ss:0x32(%ebp)
    jnz fail
    movw real_segment, %ax
    cmpw %ax, %ss:0x30(%ebp)
    jne fail
    cmpw %ax, %ss:0x3c(%ebp)
    jne fail
    .endm

protected_entry:
    movw $0x0900, %ax
    int $0x31
    movw %ss, %bx
    movw $(0xf2 | (STACK32 * 0x4000)), %cx
    movw $9, %ax
    int $0x31
    jc fail
    movw %bx, %ss
    movw %bx, private_stack_sel
    movl $0xc000, %esp
    movw real_segment, %ax
    movw %ax, record + 44
    movw %ax, record + 48
    movw %ax, record + 36
    movw %ax, nested_record + 44
    movw %ax, nested_record + 48
    movw %ax, nested_record + 36
    movw %ax, %cx
    movw $real_ivt, %dx
    movw $3, %bx
    movw $0x0201, %ax
    int $0x31
    jc fail
    movw $0x0211, %ax
    int $0x31
    jc fail
    movw %cx, previous + FIELD
    .if PM32
    movl %edx, previous
    .else
    movw %dx, previous
    .endif
    movw %cs, %cx
    movl $real_bp, %edx
    movw $0x0213, %ax
    int $0x31
    jc fail
    movw $0x0211, %ax
    int $0x31
    jc fail
    cmpw $real_bp, %dx
    jne fail
    movl $real_ud, %edx
    movw $6, %bx
    movw $0x0213, %ax
    int $0x31
    jc fail
    movl $outer_pm, %edx
    movw $3, %bx
    movw $0x0212, %ax
    int $0x31
    jc fail
    int3
    cmpb $2, breaks
    jne fail
    cmpb $3, invalids
    jne fail
    cmpb $3, reflected
    jne fail
    cmpb $1, nested_pm
    jne fail
    movl $pass_msg, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c2a, %ax
    int $0x21
fail:
    movw %cs:data_sel, %ax
    movw %ax, %ds
    movl $fail_msg, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c71, %ax
    int $0x21

outer_pm:
    pushal
    movl %esp, saved_lock_sp
    movw %ss, saved_lock_ss
    movw private_stack_sel, %ax
    movw %ax, %ss
    movl $(0x9000 + (!STACK32 * 0xbeef0000)), %esp
    movw %cs, %cx
    movl $inner_pm, %edx
    movw $3, %bx
    movw $0x0212, %ax
    int $0x31
    jc fail
1:
    movw data_sel, %ax
    movw %ax, %es
    movl $record, %edi
    xorw %bx, %bx
    xorw %cx, %cx
    movw $0x0301, %ax
    int $0x31
    jc fail
    incb scenario
    cmpb $2, scenario
    jb 1b
    movw saved_lock_ss, %ax
    movw %ax, %ss
    movl saved_lock_sp, %esp
    popal
    /* The host's outer locked stack has the client ABI's stack width. */
    .if PM32
    addl $32, %esp
    .else
    addw $32, %sp
    .endif
    lret
inner_pm:
    pushal
    incb nested_pm
    extended_return
real_bp:
    real_frame
    incb breaks
    cmpb $1, scenario
    je chain
    int3
    movw data_sel, %ax
    movw %ax, %es
    movl $nested_record, %edi
    xorw %cx, %cx
    xorw %bx, %bx
    movw $0x0301, %ax
    int $0x31
    jc fail
    extended_return
chain:
    popal
    ljmp *%cs:previous
real_ud:
    real_frame
    incb invalids
    addl $2, %ss:0x2c(%ebp)
    extended_return

    .code16
real_procedure:
    int3
    ud2
    .byte 0xcd, 3 /* Software INT 3 still belongs to the real IVT. */
    lret
real_nested:
    ud2
    lret
real_ivt:
    incb %cs:reflected
    iret

    .balign 4
record:
    .zero 32
    .word 0x202, 0, 0, 0, 0, real_procedure, 0, 0xa000, 0
nested_record:
    .zero 32
    .word 0x202, 0, 0, 0, 0, real_nested, 0, 0xb000, 0
entry: .word 0, 0
pm_entry: .word protected_entry, 0
base_hi: .word 0
base_lo: .word 0
real_segment: .word 0
data_sel: .word 0
private_stack_sel: .word 0
previous: .long 0, 0
saved_lock_sp: .long 0
saved_lock_ss: .word 0
scenario: .byte 0
breaks: .byte 0
invalids: .byte 0
reflected: .byte 0
nested_pm: .byte 0
pass_msg: .asciz "DPMI REAL EXCEPTION PASS\r\n$"
fail_msg: .asciz "DPMI REAL EXCEPTION FAIL\r\n$"
