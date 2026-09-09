/* Real FAR callback -> IRET -> recoverable host record read -> real caller.
 * Raw switches keep USE32 callbacks outside an enclosing native INT 31h,
 * so the native run exercises the callback-return gate itself. */
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
    movw %di, dpmi_entry
    movw %es, dpmi_entry + 2
    movw $PM32, %ax
    lcall *dpmi_entry
    jc setup_fail
    movw %cs, %bx
    movw $6, %ax
    int $0x31
    jc setup_fail
    movw %cx, base_hi
    movw %dx, base_lo
    .if PM32
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
protected_entry:
    movw $0x0900, %ax
    int $0x31
    movw %ds, data_sel
    movw %cs, code_sel
    movw %ss, main_ss
    movw %ss, %bx
    movw $(0xf2 | (STACK32 * 0x4000)), %cx
    movw $9, %ax
    int $0x31
    jc fail
    movw %bx, %ss
    movl $0x8000, %esp
    .if !STACK32 && PM32
    orl $0xbeef0000, %esp
    .endif
    movl %esp, main_sp
    movw %ds, %bx
    movw $0x000a, %ax
    int $0x31
    jc fail
    movw %ax, buffer_sel
    movw %ds, %bx
    movw $0x000a, %ax
    int $0x31
    jc fail
    movw %ax, callback_ss
    movw %ax, %bx
    movw $(0xf2 | (STACK32 * 0x4000)), %cx
    movw $9, %ax
    int $0x31
    jc fail
    movw %cs, %cx
    movl $buffer_handler, %edx
    movw $13, %bx
    movw $0x0203, %ax
    int $0x31
    jc fail
    movw $11, %bx
    movw $0x0203, %ax
    int $0x31
    jc fail
    movl $nested_handler, %edx
    movw $3, %bx
    movw $0x0203, %ax
    int $0x31
    jc fail
    movw %ds, %ax
    movw %ax, %es
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
    movw %cs, %ax
    movw %ax, %ds
    movl $callback, %esi
    movl $callback_regs, %edi
    movw $0x0303, %ax
    int $0x31
    jc fail
    movw %cs:data_sel, %ax
    movw %ax, %ds
    movw %dx, callback_pointer
    movw %cx, callback_pointer + 2
    movw %cs, %ax
    movw %ax, %ds
    movl $nested_callback, %esi
    movl $nested_regs, %edi
    movw $0x0303, %ax
    int $0x31
    jc fail
    movw %cs:data_sel, %ax
    movw %ax, %ds
    movw %dx, nested_call + 42
    movw %cx, nested_call + 44

next_case:
    movb $0, faults
    movb $0, called
    movb $0, nested
    movb $0, nested_calls
    movb $0, cancellations
    movw buffer_sel, %bx
    movw $0xf0, %cx
    movw $9, %ax
    int $0x31
    jc fail
    movw base_hi, %cx
    movw base_lo, %dx
    movw $7, %ax
    int $0x31
    jc fail
    xorw %cx, %cx
    movw $0xffff, %dx
    movw $8, %ax
    int $0x31
    jc fail
    movw real_seg, %ax
    movw %ax, %cx
    movw %ax, %dx
    movw %ax, %si
    movl $0x6000, %ebx
    movl $rm_top, %edi
    ljmp *pm_switch

pm_return:
    cld
    movb scenario, %al
    cmpb $9, %al
    je relocated
    cmpb $10, %al
    je edited
    cmpl $0xcafebabe, observed_eax
    jne fail
    jmp value_checked
relocated:
    cmpl $0xfedcba98, observed_eax
    jne fail
    jmp value_checked
edited:
    cmpl $0xdeadbeef, observed_eax
    jne fail
value_checked:
    movw observed_flags, %dx
    andw $0x401, %dx
    cmpw $0x401, %dx
    jne fail
    cmpl $0xcafebabe, alternate_regs + 28
    je fail
    cmpb $1, called
    jne fail
    movb $1, %dl
    testb %al, %al
    jz no_fault
    cmpb $3, %al
    jne 1f
no_fault:
    xorb %dl, %dl
1:  cmpb $4, %al
    jne 2f
    incb %dl
2:  cmpb %dl, faults
    jne fail
    cmpb $5, %al
    sete %dl
    cmpb %dl, nested
    jne fail
    cmpb $7, %al
    sete %dl
    cmpb %dl, cancellations
    jne fail
    cmpb $8, %al
    sete %dl
    cmpb %dl, nested_calls
    jne fail
    movw %ss, %dx
    cmpw main_ss, %dx
    jne fail
    cmpl main_sp, %esp
    jne fail
    incb scenario
    cmpb $11, scenario
    jb next_case
    movw callback_pointer, %dx
    movw callback_pointer + 2, %cx
    movw $0x0304, %ax
    int $0x31
    jc fail
    movw nested_call + 42, %dx
    movw nested_call + 44, %cx
    movw $0x0304, %ax
    int $0x31
    jc fail
    movl $pass_message, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c2a, %ax
    int $0x21
fail:
    movw %cs:data_sel, %ax
    movw %ax, %ds
    movl $fail_message, %edx
    movw $0x0900, %ax
    int $0x21
    movb scenario, %dl
    addb $'0', %dl
    movb $2, %ah
    int $0x21
    movw $0x4c71, %ax
    int $0x21

callback:
    movw (%esi), %ax
    movw %ax, %es:42(%edi)
    movw 2(%esi), %ax
    movw %ax, %es:44(%edi)
    addw $4, %es:46(%edi)
    movl %esp, %ebp
    .if !PM32
    andl $0xffff, %ebp
    .endif
    .if PM32
    movl %ss:0(%ebp), %eax
    movl %eax, %es:iret_frame
    movl %ss:4(%ebp), %eax
    movl %eax, %es:iret_frame + 4
    movl %ss:8(%ebp), %eax
    movl %eax, %es:iret_frame + 8
    .else
    movw %ss:0(%ebp), %ax
    movw %ax, %es:iret_frame
    movw %ss:2(%ebp), %ax
    movw %ax, %es:iret_frame + 2
    movw %ss:4(%ebp), %ax
    movw %ax, %es:iret_frame + 4
    .endif
    pushw %es
    popw %ds
    cld
    movl $callback_regs, %esi
    movl $result_regs, %edi
    movl $25, %ecx
    rep movsw
    movl $result_regs, %esi
    movl $alternate_regs, %edi
    movl $25, %ecx
    rep movsw
    movl $0xcafebabe, result_regs + 28
    movl $0xfedcba98, alternate_regs + 28
    orw $0x401, result_regs + 32
    orw $0x401, alternate_regs + 32
    incb called
    /* The whole COM block is resident. Use a different SS.B from CS.D. */
    movw callback_ss, %ax
    movw %ax, %ss
    movl $0xa000, %esp
    .if !STACK32
    orl $0xcafe0000, %esp
    .endif
    movl %esp, callback_top
    movw buffer_sel, %ax
    movw %ax, %es
    movl $result_regs, %edi
    .if !PM32
    orl $0xa5a50000, %edi
    .endif
    cmpb $0, scenario
    je return_callback
    movw buffer_sel, %bx
    xorw %cx, %cx
    movw $(result_regs + 48), %dx
    cmpb $2, scenario
    jne 1f
    movw $result_regs, %dx
1:  cmpb $3, scenario
    jne 2f
    movw $(result_regs + 49), %dx
2:
    movw $8, %ax
    int $0x31
    jc fail
return_callback:
    .if PM32
    pushl iret_frame + 8
    pushl iret_frame + 4
    pushl iret_frame
    .else
    pushw iret_frame + 4
    pushw iret_frame + 2
    pushw iret_frame
    .endif
    iret

buffer_handler:
    pushal
    movl %esp, %ebp
    .if !STACK32
    andl $0xffff, %ebp
    .endif
    incb faults
    cmpb $3, faults
    jae handler_fail
    cmpw $0, 32 + 2 * FIELD(%ebp)
    jne handler_fail
    .if PM32
    movl iret_frame, %eax
    addl $2, %eax
    cmpl %eax, 32 + 3 * FIELD(%ebp)
    .else
    movw iret_frame, %ax
    addw $2, %ax
    cmpw %ax, 32 + 3 * FIELD(%ebp)
    .endif
    jne handler_fail
    movw iret_frame + FIELD, %ax
    cmpw %ax, 32 + 4 * FIELD(%ebp)
    jne handler_fail
    movw callback_ss, %ax
    cmpw %ax, 32 + 7 * FIELD(%ebp)
    jne handler_fail
    .if PM32
    movl callback_top, %eax
    cmpl %eax, 32 + 6 * FIELD(%ebp)
    .else
    movw callback_top, %ax
    cmpw %ax, 32 + 6 * FIELD(%ebp)
    .endif
    jne handler_fail
    cmpb $4, scenario
    jne repair
    cmpb $1, faults
    je handler_return
repair:
    cmpb $5, scenario
    jne 3f
    int $3
3:  movw buffer_sel, %bx
    xorw %cx, %cx
    movw $0xffff, %dx
    movw $8, %ax
    int $0x31
    jc handler_fail
    movw $0xf0, %cx
    movw $9, %ax
    int $0x31
    jc handler_fail
    cmpb $6, scenario
    jne 4f
    movl $alternate_regs, 0(%ebp)
    movl $0x0bad0000, 28(%ebp)
    movw data_sel, %ax
    movw %ax, %es
4:  cmpb $7, scenario
    jne 5f
    .if PM32
    movzwl code_sel, %eax
    movl %eax, 32 + 4 * FIELD(%ebp)
    movl $cancelled, 32 + 3 * FIELD(%ebp)
    .else
    movw code_sel, %ax
    movw %ax, 32 + 4 * FIELD(%ebp)
    movw $cancelled, 32 + 3 * FIELD(%ebp)
    .endif
5:  cmpb $8, scenario
    jne 6f
    movw callback_pointer, %dx
    movw callback_pointer + 2, %cx
    movw $0x0304, %ax
    int $0x31
    jnc handler_fail
    cmpw $0x8024, %ax
    jne handler_fail
    pushw %es
    movw data_sel, %ax
    movw %ax, %es
    movl $nested_call, %edi
    xorw %bx, %bx
    xorw %cx, %cx
    movw $0x0301, %ax
    int $0x31
    jc handler_fail
    popw %es
6:  cmpb $9, scenario
    jne 7f
    movzwl base_hi, %edx
    shll $16, %edx
    movw base_lo, %dx
    addl $(alternate_regs - result_regs), %edx
    movl %edx, %ecx
    shrl $16, %ecx
    movw buffer_sel, %bx
    movw $7, %ax
    int $0x31
    jc handler_fail
7:  cmpb $10, scenario
    jne handler_return
    movl $0xdeadbeef, result_regs + 28
handler_return:
    popal
    lret
handler_fail:
    movw $0x4c72, %ax
    int $0x21
nested_handler:
    incb nested
    lret
cancelled:
    incb cancellations
    movw buffer_sel, %ax
    movw %ax, %es
    movl $result_regs, %edi
    jmp return_callback
nested_callback:
    movw (%esi), %ax
    movw %ax, %es:42(%edi)
    movw 2(%esi), %ax
    movw %ax, %es:44(%edi)
    addw $4, %es:46(%edi)
    incb %es:nested_calls
    iret

    .code16
rm_top:
    lcall *%cs:callback_pointer
    movl %eax, %cs:observed_eax
    pushfw
    popw %cs:observed_flags
    movw %cs:data_sel, %ax
    movw %ax, %cx
    movw %cs:main_ss, %dx
    movl %cs:main_sp, %ebx
    movw %cs:code_sel, %si
    movl $pm_return, %edi
    ljmp *%cs:rm_switch

    .align 4
dpmi_entry: .word 0, 0
pm_entry: .word protected_entry, 0
pm_switch: .long 0
    .word 0
rm_switch: .word 0, 0
callback_pointer: .word 0, 0
base_hi: .word 0
base_lo: .word 0
real_seg: .word 0
data_sel: .word 0
code_sel: .word 0
buffer_sel: .word 0
main_ss: .word 0
callback_ss: .word 0
main_sp: .long 0
callback_top: .long 0
observed_eax: .long 0
observed_flags: .word 0
iret_frame: .space 12
scenario: .byte 0
faults: .byte 0
called: .byte 0
nested: .byte 0
nested_calls: .byte 0
cancellations: .byte 0
callback_regs: .space 50
result_regs: .space 50
alternate_regs: .space 50
nested_regs: .space 50
nested_call: .space 50
pass_message: .asciz "DPMI CALLBACK RECOVERY PASS\r\n$"
fail_message: .asciz "DPMI CALLBACK RECOVERY FAIL\r\n$"
