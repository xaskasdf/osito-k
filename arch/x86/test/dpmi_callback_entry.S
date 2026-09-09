/* Raw real FAR calls with repairable callback admission and cancellation. */
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
    .macro rights value
    movw $\value, %cx
    movw $9, %ax
    int $0x31
    jc fail
    .endm
protected_entry:
    movw $0x0900, %ax
    int $0x31
    movw %cs, code_sel
    movw %ss, main_ss
    movw %ss, %bx
    rights (0xf2 | (STACK32 * 0x4000))
    movw %bx, %ss
    movl $(0x8000 + (PM32 * !STACK32 * 0xbeef0000)), %esp
    movl %esp, main_sp
    movw %ds, %bx
    movw $0x000a, %ax
    int $0x31
    jc fail
    movw %ax, buffer_sel
    movw %cs, %bx
    movw $0x000a, %ax
    int $0x31
    jc fail
    movw %ax, callback_sel
    movw %ax, %bx
    rights (0xfa | (PM32 * 0x4000))
    movw %cs, %cx
    movl $repair_handler, %edx
    movw $11, %bx
    movw $0x0213, %ax
    int $0x31
    jc fail
    movw $13, %bx
    movw $0x0213, %ax
    int $0x31
    jc fail
    movw $3, %bx
    movl $nested_handler, %edx
    movw $0x0212, %ax
    int $0x31
    jc fail
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
    movw buffer_sel, %ax
    movw %ax, %es
    movw callback_sel, %ax
    movw %ax, %ds
    movl $callback, %esi
    movl $record, %edi
    movw $0x0303, %ax
    int $0x31
    jc fail
    movw %cs:data_sel, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %cx, callback_pointer + 2
    movw %dx, callback_pointer
next_case:
    movb $0, faults
    movb $0, called
    movb $0, nested
    movb $0, cancelled
    movw buffer_sel, %bx
    rights 0xf2
    xorw %cx, %cx
    movw $0xffff, %dx
    movw $8, %ax
    int $0x31
    jc fail
    movw callback_sel, %bx
    rights (0xfa | (PM32 * 0x4000))
    cmpb $0, scenario
    je invoke
    cmpb $3, scenario
    jne 1f
    rights (0x7a | (PM32 * 0x4000))
    jmp invoke
1:  movw buffer_sel, %bx
    cmpb $1, scenario
    jne 2f
    xorw %cx, %cx
    movw $(record + 48), %dx
    movw $8, %ax
    int $0x31
    jc fail
    jmp invoke
2:  rights 0x72
invoke:
    movw real_seg, %ax
    movw %ax, %cx
    movw %ax, %dx
    movw %ax, %si
    movl $0x6000, %ebx
    movl $real_top, %edi
    ljmp *pm_switch
pm_return:
    cld
    movb $1, %dl
    cmpb $0, scenario
    jne 1f
    xorb %dl, %dl
1:  cmpb $4, scenario
    je 2f
    cmpb $8, scenario
    jne 3f
2:  incb %dl
3:  cmpb %dl, faults
    jne fail
    cmpb $5, scenario
    sete %dl
    cmpb %dl, nested
    jne fail
    cmpb $7, scenario
    sete %dl
    cmpb %dl, cancelled
    jne fail
    xorb $1, %dl
    cmpb %dl, called
    jne fail
    cmpb $7, scenario
    je check_stack
    cmpl $0xcafebabe, observed_eax
    jne fail
    movl $0x12345678, %eax
    cmpb $6, scenario
    jne 4f
    movl $0xdeadbeef, %eax
4:  cmpl %eax, callback_eax
    jne fail
check_stack:
    movw $0x6000, %ax
    cmpb $6, scenario
    jne 5f
    movw $0x5ff0, %ax
5:  cmpw %ax, observed_sp
    jne fail
    cmpl main_sp, %esp
    jne fail
    movw %ss, %ax
    cmpw main_ss, %ax
    jne fail
    incb scenario
    cmpb $9, scenario
    jb next_case
    movw callback_pointer, %dx
    movw callback_pointer + 2, %cx
    movw $0x0304, %ax
    int $0x31
    jc fail
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
    movb scenario, %dl
    addb $'0', %dl
    movb $2, %ah
    int $0x21
    movw $0x4c71, %ax
    int $0x21

callback:
    movl %eax, %es:callback_eax
    movw (%esi), %ax
    movw %ax, %es:42(%edi)
    movw 2(%esi), %ax
    movw %ax, %es:44(%edi)
    addw $4, %es:46(%edi)
    movl $0xcafebabe, %es:28(%edi)
    incb %es:called
    iret

repair_handler:
    pushal
    movl %esp, %ebp
    .if !PM32
    andl $0xffff, %ebp
    .endif
    addl $32, %ebp
    movw %cs:data_sel, %ax
    movw %ax, %ds
    incb faults
    cmpb $3, faults
    jae fail
    testl $0x20000, %ss:0x34(%ebp)
    jz fail
    cmpw $1, %ss:0x32(%ebp)
    jne fail
    cmpw $0xf000, %ss:0x30(%ebp)
    jne fail
    movzwl callback_pointer, %eax
    addl $2, %eax
    cmpl %eax, %ss:0x2c(%ebp)
    jne fail
    cmpl $0x5ffc, %ss:0x38(%ebp)
    jne fail
    movw real_seg, %ax
    cmpw %ax, %ss:0x3c(%ebp)
    jne fail
    cmpb $4, scenario
    jne repair
    cmpb $1, faults
    je handler_return
repair:
    cmpb $5, scenario
    jne 1f
    int3
1:  movw buffer_sel, %bx
    rights 0xf2
    xorw %cx, %cx
    movw $0xffff, %dx
    movw $8, %ax
    int $0x31
    jc fail
    movw callback_sel, %bx
    rights (0xfa | (PM32 * 0x4000))
    cmpb $8, scenario
    jne 2f
    cmpb $1, faults
    jne 2f
    rights (0x7a | (PM32 * 0x4000))
2:  cmpb $6, scenario
    jne 3f
    movl $0xdeadbeef, %ss:-4(%ebp)
    movl 0x5ffc, %eax
    movl %eax, 0x5fec
    movl $0x5fec, %ss:0x38(%ebp)
3:  cmpb $7, scenario
    jne handler_return
    orw $4, %ss:0x32(%ebp)
    movzwl real_seg, %eax
    movw %ax, %ss:0x30(%ebp)
    movl $real_cancelled, %ss:0x2c(%ebp)
handler_return:
    popal
    addl $32, %esp
    lret
nested_handler:
    incb nested
    addl $32, %esp
    lret

    .code16
real_top:
    movl $0x12345678, %eax
    lcall *%cs:callback_pointer
real_done:
    movl %eax, %cs:observed_eax
    movw %sp, %cs:observed_sp
    movw %cs:data_sel, %ax
    movw %ax, %cx
    movw %cs:main_ss, %dx
    movl %cs:main_sp, %ebx
    movw %cs:code_sel, %si
    movl $pm_return, %edi
    ljmp *%cs:rm_switch
real_cancelled:
    addw $4, %sp
    incb %cs:cancelled
    jmp real_done
    .align 4
entry: .word 0, 0
pm_entry: .word protected_entry, 0
pm_switch: .long 0
    .word 0
rm_switch: .word 0, 0
callback_pointer: .word 0, 0
real_seg: .word 0
data_sel: .word 0
code_sel: .word 0
callback_sel: .word 0
buffer_sel: .word 0
main_ss: .word 0
main_sp: .long 0
observed_eax: .long 0
observed_sp: .word 0
callback_eax: .long 0
scenario: .byte 0
faults: .byte 0
called: .byte 0
nested: .byte 0
cancelled: .byte 0
record: .space 50
pass_msg: .asciz "DPMI CALLBACK ENTRY PASS\r\n$"
fail_msg: .asciz "DPMI CALLBACK ENTRY FAIL case=$"
