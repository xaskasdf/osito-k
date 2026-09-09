/* Read a real file once, then repair the protected copy-out destination. */
    .section .text
    .code16
    .global _start
_start:
    cld
    pushw %cs
    popw %ds
    pushw %cs
    popw %es
    movw %cs, real_cs
    movw $0x1000, %bx
    movw $0x4a00, %ax
    int $0x21
    jc setup_fail
    movw $filename, %dx
    xorw %cx, %cx
    movw $0x3c00, %ax
    int $0x21
    jc setup_fail
    movw %ax, file_handle
    movw %ax, %bx
    movw $pattern, %dx
    movw $64, %cx
    movw $0x4000, %ax
    int $0x21
    jc setup_fail
    cmpw $64, %ax
    jne setup_fail
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
    movw %ds, %ax
    movw %ax, %es
    movw %ss, %bx
    movw $(0xf2 | (STACK32 * 0x4000)), %cx
    movw $9, %ax
    int $0x31
    jc fail
    movw %bx, %ss
    movl $0x8000, %esp
    .if !STACK32
    orl $0xbeef0000, %esp
    .endif
    movl %esp, saved_sp
    movw %ss, saved_ss
    movw %cs, saved_cs
    movw %ds, %bx
    movw $0x000a, %ax
    int $0x31
    jc fail
    movw %ax, buffer_sel
    xorw %ax, %ax
    movw $1, %cx
    int $0x31
    jc fail
    movw %ax, %bx
    movw %ax, flat_sel
    movw $15, %cx
    movw $0xffff, %dx
    movw $8, %ax
    int $0x31
    jc fail
    movw flat_sel, %fs
    movw %cs, %cx
    movl $buffer_handler, %edx
    movw $11, %bx
    movw $0x0203, %ax
    int $0x31
    jc fail
    movw $13, %bx
    movw $0x0203, %ax
    int $0x31
    jc fail
    movl $nested_handler, %edx
    movw $3, %bx
    movw $0x0203, %ax
    int $0x31
    jc fail
    movw $0x21, %bx
    movw $0x0200, %ax
    int $0x31
    movw %cx, old_int21 + 2
    movw %dx, old_int21
    movw %cs, %ax
    movw %ax, %ds
    movl $callback, %esi
    movl $callback_regs, %edi
    movw $0x0303, %ax
    int $0x31
    jc fail
    movw %es:data_sel, %ax
    movw %ax, %ds
    movw %cx, rm_callback + 2
    movw %dx, rm_callback
    movw real_cs, %cx
    movw $real_hook, %dx
    movw $0x21, %bx
    movw $0x0201, %ax
    int $0x31
    jc fail

next_case:
    movb $0, read_calls
    movb $0, callback_calls
    movb $0, faults
    movb $0, nested
    movw file_handle, %bx
    xorw %cx, %cx
    xorw %dx, %dx
    movw $0x4200, %ax
    int $0x21
    jc fail
    movl $buffer, %edi
    movl $16, %ecx
    movb $0xa5, %al
    rep stosb
    movw buffer_sel, %bx
    movw $(0xf2 | (PM32 * 0x4000)), %cx
    movw $9, %ax
    int $0x31
    jc fail
    movl $0x11220000, %ebx
    movw file_handle, %bx
    movl %ebx, expected_bx
    movl $16, %ecx
    movl $buffer, %edx
    .if !PM32
    orl $0x33440000, %ecx
    orl $0x55660000, %edx
    .endif
    movl %ecx, expected_cx
    movl %edx, expected_dx
    movl $0x12345678, %esi
    movl $0x98765432, %edi
    movl $0xcafebabe, %ebp
    movw buffer_sel, %ds
    movl $0xa5a53f00, %eax
    std
    stc
    int $0x21
after_read:
    jc fail
    cmpw $16, %ax
    jne fail
    cmpb $4, %es:scenario
    je fail
    jmp check_result
cancelled_read:
    jnc fail
    cmpl $0xa5a53f00, %eax
    jne fail
    cmpb $4, %es:scenario
    jne fail
check_result:
    cmpl %es:expected_bx, %ebx
    jne fail
    cmpl %es:expected_cx, %ecx
    jne fail
    cmpl %es:expected_dx, %edx
    jne fail
    cmpl $0x12345678, %esi
    jne fail
    cmpl $0x98765432, %edi
    jne fail
    cmpl $0xcafebabe, %ebp
    jne fail
    cmpl %es:saved_sp, %esp
    jne fail
    pushfl
    popl %eax
    andl $0x400, %eax
    cmpb $5, %es:scenario
    je cleared_df
    cmpl $0x400, %eax
    jne fail
    jmp flags_done
cleared_df:
    testl %eax, %eax
    jnz fail
flags_done:
    cld
    movw %es, %ax
    movw %ax, %ds
    cmpb $1, read_calls
    jne fail
    cmpb $1, callback_calls
    jne fail
    movb $1, %al
    cmpb $2, scenario
    jne fault_count
    incb %al
fault_count:
    cmpb faults, %al
    jne fail
    xorb %al, %al
    cmpb $3, scenario
    jne nested_count
    incb %al
nested_count:
    cmpb nested, %al
    jne fail
    movl $buffer, %edi
    movl $16, %ecx
    cmpb $4, scenario
    jne copied_result
    movb $0xa5, %al
    repe scasb
    jne fail
    jmp check_position
copied_result:
    movl $pattern, %esi
    repe cmpsb
    jne fail
check_position:
    movw file_handle, %bx
    xorw %cx, %cx
    xorw %dx, %dx
    movw $0x4201, %ax
    int $0x21
    jc fail
    cmpw $16, %ax
    jne fail
    testw %dx, %dx
    jnz fail
    incb scenario
    cmpb $7, scenario
    jb next_case
    movw old_int21 + 2, %cx
    movw old_int21, %dx
    movw $0x21, %bx
    movw $0x0201, %ax
    int $0x31
    jc fail
    movw rm_callback + 2, %cx
    movw rm_callback, %dx
    movw $0x0304, %ax
    int $0x31
    jc fail
    movw file_handle, %bx
    movw $0x3e00, %ax
    int $0x21
    jc fail
    movl $filename, %edx
    movw $0x4100, %ax
    int $0x21
    jc fail
    movl $pass, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c2a, %ax
    int $0x21
fail:
    cld
    movw $0x4c80, %ax
    int $0x21
handler_fail:
    movw $0x4c81, %ax
    int $0x21

buffer_handler:
    pushal
    movl %esp, %ebp
    movw %ss, %ax
    cmpw %es:saved_ss, %ax
    je handler_fail
    movw %ss:32+2*FIELD(%ebp), %ax
    movw %es:buffer_sel, %bx
    andw $0xfffc, %bx
    cmpw %bx, %ax
    jne handler_fail
    movw %ss:32+3*FIELD(%ebp), %ax
    cmpw $after_read, %ax
    jne handler_fail
    movw %ss:32+4*FIELD(%ebp), %ax
    cmpw %es:saved_cs, %ax
    jne handler_fail
    movw %ss:32+6*FIELD(%ebp), %ax
    cmpw %es:saved_sp, %ax
    jne handler_fail
    movw %ss:32+7*FIELD(%ebp), %ax
    cmpw %es:saved_ss, %ax
    jne handler_fail
    incb %es:faults
    cmpb $3, %es:scenario
    jne no_nested
    int $3
no_nested:
    cmpb $2, %es:scenario
    jne repair
    cmpb $1, %es:faults
    je handler_done
repair:
    movw %es:buffer_sel, %bx
    movw $(0xf2 | (PM32 * 0x4000)), %cx
    movw $9, %ax
    int $0x31
    jc handler_fail
    cmpb $4, %es:scenario
    jne no_cancel
    .if PM32
    movl $cancelled_read, %ss:32+3*FIELD(%ebp)
    .else
    movw $cancelled_read, %ss:32+3*FIELD(%ebp)
    .endif
no_cancel:
    cmpb $5, %es:scenario
    jne no_df_edit
    andw $0xfbff, %ss:32+5*FIELD(%ebp)
no_df_edit:
    cmpb $6, %es:scenario
    jne handler_done
    movl %es:scratch_address, %eax
    movb $0xee, %fs:(%eax)
handler_done:
    popal
    lret
nested_handler:
    incb %es:nested
    lret

callback:
    movw (%esi), %ax
    movw %ax, %es:42(%edi)
    movw 2(%esi), %ax
    movw %ax, %es:44(%edi)
    addw $4, %es:46(%edi)
    movzwl %es:36(%edi), %eax
    shll $4, %eax
    movzwl %es:20(%edi), %edx
    addl %edx, %eax
    movl %eax, %es:scratch_address
    incb %es:callback_calls
    movw %es:buffer_sel, %bx
    movw $(0xf0 | (PM32 * 0x4000)), %cx
    cmpb $1, %es:scenario
    jne callback_rights
    movw $(0x72 | (PM32 * 0x4000)), %cx
callback_rights:
    movw $9, %ax
    int $0x31
    jc handler_fail
    iret

    .code16
real_hook:
    cmpb $0x3f, %ah
    jne real_chain
    incb %cs:read_calls
    pushfw
    lcall *%cs:old_int21
    pushfw
    lcall *%cs:rm_callback
    /* Propagate the completed DOS status through the outer interrupt frame. */
    pushw %bp
    movw %sp, %bp
    pushw %ax
    movw 2(%bp), %ax
    movw %ax, 8(%bp)
    popw %ax
    popw %bp
    addw $2, %sp
    iret
real_chain:
    ljmp *%cs:old_int21

    .balign 4
dpmi_entry: .word 0, 0
pm_pointer: .word protected_entry, 0
old_int21: .word 0, 0
rm_callback: .word 0, 0
real_cs: .word 0
base_hi: .word 0
base_lo: .word 0
file_handle: .word 0
data_sel: .word 0
buffer_sel: .word 0
flat_sel: .word 0
saved_sp: .long 0
saved_ss: .word 0
saved_cs: .word 0
expected_bx: .long 0
expected_cx: .long 0
expected_dx: .long 0
scratch_address: .long 0
scenario: .byte 0
read_calls: .byte 0
callback_calls: .byte 0
faults: .byte 0
nested: .byte 0
    .balign 4
callback_regs: .space 50
buffer: .space 16
filename: .asciz "DPIOREC.DAT"
pattern: .ascii "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
pass: .ascii "DPMI IO RECOVERY PASS\r\n$"
