/* Restart host register-buffer accesses without repeating a completed RM call. */
    .ifndef LOADED_NP
    .equ LOADED_NP, 0
    .endif
    .section .text
    .code16
    .global _start
_start:
    cld
    pushw %cs
    popw %ds
    pushw %cs
    popw %es
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

    .if PM32
    .code32
    .equ FIELD, 4
    .else
    .equ FIELD, 2
    .endif
test_entry:
    movw $0x0900, %ax
    int $0x31
    movw %ss, %bx
    movw $(0x00f2 | (STACK32 * 0x4000)), %cx
    movw $9, %ax
    int $0x31
    jc setup_fail
    movw %bx, %ss
    .if !STACK32
    orl $0xbeef0000, %esp
    .endif
    movl %esp, saved_sp
    movw %ss, saved_ss
    movw %cs, saved_cs
    movw %ds, data_sel
    movw %ds, %bx
    movw $0x000a, %ax
    int $0x31
    jc fail
    movw %ax, output_sel
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
    movw %ds, %ax
    movw %ax, %es
    movw %cs, %ax
    movw %ax, %ds
    movl $callback, %esi
    movl $callback_regs, %edi
    movw $0x0303, %ax
    int $0x31
    jc fail
    movw %cs:data_sel, %ax
    movw %ax, %ds
    movw %cx, rm_call + 44
    movw %dx, rm_call + 42
    movw $0x60, %bx
    movw $0x0201, %ax
    int $0x31
    jc fail

next_probe:
    movb $0, calls
    movb $0, faults
    movb $0, nested
    movl $0x87654321, rm_call + 28
    movw $0x0202, rm_call + 32
    movw $0, rm_call + 46
    movw $0, rm_call + 48
    movw output_sel, %bx
    movw $0x00f2, %cx
    movw $9, %ax
    int $0x31
    jc fail
    movw %bx, %es
    cmpb $1, scenario
    jne 1f
    movw $0x00f0, %cx
    jmp 2f
1:
    .if LOADED_NP
    /* Separate reproduction: native gate reload of a now-absent loaded ES. */
    cmpb $2, scenario
    jne invoke
    movw $0x0072, %cx
    .else
    jmp invoke
    .endif
2:  movw $9, %ax
    int $0x31
after_access:
    jc fail
invoke:
    movzwl kind, %eax
    orl $0xa5a50300, %eax
    movl %eax, expected_ax
    movl $0xabcd0060, %ebx
    xorl %ecx, %ecx
    movl $0x11223344, %edx
    movl $0x55667788, %esi
    movl $rm_call, %edi
    .if !PM32
    orl $0xa5a50000, %edi
    .endif
    movl $0xcafebabe, %ebp
    std
    stc
    .rept 13
    .byte 0x3e
    .endr
    int $0x31
after_call:
    jc fail
    cmpb $5, scenario
    je fail
    jmp check_result
cancelled:
    jnc fail
    cmpb $5, scenario
    jne fail
check_result:
    cmpl saved_sp, %esp
    jne fail
    cmpl expected_ax, %eax
    jne fail
    cmpl $0xabcd0060, %ebx
    jne fail
    testl %ecx, %ecx
    jnz fail
    cmpl $0x11223344, %edx
    jne fail
    cmpl $0x55667788, %esi
    jne fail
    .if PM32
    cmpl $rm_call, %edi
    .else
    cmpl $(0xa5a50000 + rm_call), %edi
    .endif
    jne fail
    cmpl $0xcafebabe, %ebp
    jne fail
    pushfl
    popl %eax
    andl $0x400, %eax
    cmpb $6, scenario
    je 3f
    cmpl $0x400, %eax
    jne fail
    jmp 4f
3:  testl %eax, %eax
    jnz fail
4:  cld
    cmpb $1, calls
    jne fail
    movb $1, %al
    .if LOADED_NP
    cmpb $2, scenario
    je 5f
    .endif
    cmpb $3, scenario
    jne 6f
5:  incb %al
6:  cmpb faults, %al
    jne fail
    xorb %al, %al
    cmpb $4, scenario
    jne 7f
    incb %al
7:  cmpb nested, %al
    jne fail
    movl $0xcafebabe, %eax
    cmpb $5, scenario
    jne 8f
    movl $0x87654321, %eax
8:  cmpl rm_call + 28, %eax
    jne fail
    incb scenario
    cmpb $7, scenario
    jb next_probe
    movb $0, scenario
    incw kind
    cmpw $3, kind
    jb next_probe
    movw rm_call + 44, %cx
    movw rm_call + 42, %dx
    movw $0x0304, %ax
    int $0x31
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
    cmpw saved_ss, %ax
    je handler_fail
    movw %ss:32 + 2*FIELD(%ebp), %ax
    movw output_sel, %bx
    andw $0xfffc, %bx
    cmpw %bx, %ax
    jne handler_fail
    movw %ss:32 + 3*FIELD(%ebp), %ax
    .if LOADED_NP
    /* Reload failure belongs to the completed 0009h call, before 030xh. */
    movw $after_call, %dx
    cmpb $2, scenario
    jne 13f
    cmpb $0, faults
    jne 13f
    movw $after_access, %dx
13: cmpw %dx, %ax
    .else
    cmpw $after_call, %ax
    .endif
    jne handler_fail
    movw %ss:32 + 4*FIELD(%ebp), %ax
    cmpw saved_cs, %ax
    jne handler_fail
    movw %ss:32 + 6*FIELD(%ebp), %ax
    cmpw saved_sp, %ax
    jne handler_fail
    movw %ss:32 + 7*FIELD(%ebp), %ax
    cmpw saved_ss, %ax
    jne handler_fail
    incb faults
    cmpb $4, scenario
    jne 9f
    int $3
9:  cmpb $3, scenario
    jne repair
    cmpb $1, faults
    je handler_done
repair:
    movw output_sel, %bx
    movw $0x00f2, %cx
    movw $9, %ax
    int $0x31
    jc handler_fail
    cmpb $5, scenario
    jne 10f
    .if PM32
    movl $cancelled, %ss:32 + 3*FIELD(%ebp)
    .else
    movw $cancelled, %ss:32 + 3*FIELD(%ebp)
    .endif
10: cmpb $6, scenario
    jne handler_done
    andw $0xfbff, %ss:32 + 5*FIELD(%ebp)
handler_done:
    popal
    lret
nested_handler:
    incb nested
    lret

callback:
    /* Discard the original real FAR/IRET frame in the returned RM record. */
    movw (%esi), %ax
    movw %ax, %es:42(%edi)
    movw 2(%esi), %ax
    movw %ax, %es:44(%edi)
    movw $4, %ax
    cmpw $1, %es:kind
    je 11f
    movw 4(%esi), %ax
    movw %ax, %es:32(%edi)
    movw $6, %ax
11: addw %ax, %es:46(%edi)
    movl $0xcafebabe, %es:28(%edi)
    pushw %es
    popw %ds
    incb calls
    movw output_sel, %bx
    movw $0x00f0, %cx
    .if !LOADED_NP
    cmpb $2, scenario
    jne 12f
    movw $0x0072, %cx
12:
    .endif
    movw $9, %ax
    int $0x31
    jc handler_fail
    iret

saved_sp: .long 0
expected_ax: .long 0
saved_ss: .word 0
saved_cs: .word 0
data_sel: .word 0
output_sel: .word 0
kind: .word 0
scenario: .byte 0
calls: .byte 0
faults: .byte 0
nested: .byte 0
    .balign 4
rm_call: .space 50
callback_regs: .space 50
pass: .ascii "DPMI SERVICE BUFFER CONTRACT PASS\r\n$"
