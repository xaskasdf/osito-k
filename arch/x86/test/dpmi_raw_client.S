/* Raw entry selects a suspended EXEC owner; real exceptions remain primary. */
    .section .text
    .code16
    .global _start
_start:
    cld
    pushw %cs
    popw %ds
    pushw %cs
    popw %es
    movw %cs, real_seg
    .if CHILD
    movw $child_return, 0x91
    .endif
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
    movw $0x000a, %ax
    int $0x31
    jc setup_fail
    movw %ax, pm_entry + 2
    movw %ax, %bx
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
    .endif
protected_entry:
    movw $0x0900, %ax
    int $0x31
    movw %cs, code_sel
    movw %ss, stack_sel
    movw %ss, %bx
    movw $(0xf2 | (STACK32 * 0x4000)), %cx
    movw $9, %ax
    int $0x31
    jc fail
    movw %bx, %ss
    movl $0x8000, %esp
    movw $0x0306, %ax
    int $0x31
    jc fail
    movw %cx, rm_switch
    movw %bx, rm_switch + 2
    .if PM32
    movl %edi, pm_switch
    movw %si, pm_switch + 4
    .else
    movw %di, pm_switch
    movw %si, pm_switch + 2
    .endif
    movw %cs, %cx
    movl $software_handler, %edx
    movw $0x60, %bx
    movw $0x0205, %ax
    int $0x31
    jc fail
    .if CHILD
    movw rm_switch, %ax
    cmpw 0x81, %ax
    jne distinct_entry
    movw rm_switch + 2, %ax
    cmpw 0x83, %ax
    je fail
distinct_entry:
    movw %cs, %cx
    movl $real_exception, %edx
    movw $6, %bx
    movw $0x0213, %ax
    int $0x31
    jc fail
child_invoke:
    movw real_seg, %ax
    movw %ax, %cx
    movw %ax, %dx
    movw %ax, %si
    movl $0xa000, %ebx
    movl $child_real, %edi
    ljmp *pm_switch
child_resumed:
    int $0x60
    cmpl $0x4348494c, %eax
    jne fail
    incb visits
    cmpb $2, visits
    jb child_invoke
    cmpb $2, faults
    jne fail
    movw $0x4c2a, %ax
    int $0x21
real_exception:
    pushal
    movw %cs:data_sel, %ax
    movw %ax, %ds
    incb faults
    movl %esp, %ebp
    .if !PM32
    movzwl %bp, %ebp
    .endif
    addl $2, %ss:0x4c(%ebp)
    movl $0xc0decafe, %ss:28(%ebp)
    popal
    .if PM32
    addl $32, %esp
    .else
    addw $32, %sp
    .endif
    lret
    .else
    movl rm_switch, %eax
    movl %eax, tail + 1
    movw code_sel, %ax
    movw %ax, tail + 5
    movw data_sel, %ax
    movw %ax, tail + 7
    movw stack_sel, %ax
    movw %ax, tail + 9
    movl $raw_owner, tail + 11
    movw real_seg, %ax
    movw %ax, record + 36
    movw %ax, record + 44
    movw %ax, fault_record + 36
    movw %ax, fault_record + 44
    movw %cs, %cx
    movl $outer_exception, %edx
    movw $3, %bx
    movw $0x0212, %ax
    int $0x31
    jc fail
    call run_child
    int3
    cmpb $4, visits
    jne fail
    movl $pass_msg, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c2a, %ax
    int $0x21
run_child:
    movw data_sel, %ax
    movw %ax, %es
    movl $record, %edi
    xorw %bx, %bx
    xorw %cx, %cx
    movw $0x0301, %ax
    int $0x31
    jc fail
    cmpl $42, record + 28
    jne fail
    ret
outer_exception:
    pushal
    call run_child
    popal
    .if PM32
    addl $32, %esp
    .else
    addw $32, %sp
    .endif
    lret
raw_owner:
    int $0x60
    cmpl $0x50415245, %eax
    jne fail
    incb visits
    /* The parent's raw entry must not steal the child's real #UD vector. */
    pushl %ebp
    movw data_sel, %ax
    movw %ax, %es
    movl $fault_record, %edi
    xorw %bx, %bx
    xorw %cx, %cx
    movw $0x0301, %ax
    int $0x31
    jc fail
    popl %ebp
    cmpl $0xc0decafe, fault_record + 28
    jne fail
    movw %bp, %bx
    movw $2, %ax
    int $0x31
    jc fail
    movw %ax, %es
    movzwl %es:0x91, %edi
    movw %bp, %ax
    movw %ax, %cx
    movw %ax, %dx
    movw %ax, %si
    movl $0xa000, %ebx
    ljmp *pm_switch
    .endif
software_handler:
    .if CHILD
    movl $0x4348494c, %eax
    .else
    movl $0x50415245, %eax
    .endif
    iret
fail:
    movw %cs:data_sel, %ax
    movw %ax, %ds
    movl $fail_msg, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c71, %ax
    int $0x21

    .code16
    .if CHILD
child_real:
    movw %ss, %bp
    movw 0x85, %si
    movw 0x87, %ax
    movw %ax, %cx
    movw 0x89, %dx
    movl $0x6000, %ebx
    movl 0x8b, %edi
    ljmp *0x81
child_return:
    movw data_sel, %ax
    movw %ax, %cx
    movw stack_sel, %dx
    movl $0xb000, %ebx
    movw code_sel, %si
    movl $child_resumed, %edi
    ljmp *rm_switch
    .else
real_exec:
    pushw %cs
    popw %ds
    pushw %cs
    popw %es
    movw %cs, parameters + 4
    movw %cs, parameters + 8
    movw %cs, parameters + 12
    movw $child_name, %dx
    movw $parameters, %bx
    movw $0x4b00, %ax
    int $0x21
    jc real_fail
    movw $0x4d00, %ax
    int $0x21
    movzwl %ax, %eax
    lret
real_fail:
    movl $70, %eax
    lret
real_fault:
    ud2
    lret
    .endif
    .balign 4
entry: .word 0, 0
pm_entry: .word protected_entry, 0
rm_switch: .word 0, 0
pm_switch: .long 0
    .word 0
real_seg: .word 0
code_sel: .word 0
data_sel: .word 0
stack_sel: .word 0
visits: .byte 0
faults: .byte 0
    .if !CHILD
record:
    .zero 32
    .word 2, 0, 0, 0, 0, real_exec, 0, 0, 0
fault_record:
    .zero 32
    .word 2, 0, 0, 0, 0, real_fault, 0, 0, 0
tail:
    .byte 15
    .zero 15
    .byte 13
parameters:
    .word 0, tail, 0, 0xffff, 0, 0xffff, 0
child_name:
    .if CHILD32
    .asciz "droc32.com"
    .else
    .asciz "droc16.com"
    .endif
    .endif
pass_msg: .asciz "DPMI RAW CLIENT PASS\r\n$"
fail_msg: .asciz "DPMI RAW CLIENT FAIL\r\n$"
