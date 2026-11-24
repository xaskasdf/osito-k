/* Extended PM frames, legacy chaining and restartable host-service faults. */
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
    .macro ext_return
    popal
    .if STACK32
    addl $32, %esp
    .else
    addw $32, %sp
    .endif
    lret
    .endm

protected_entry:
    movw $0x0900, %ax
    int $0x31
    movw %ds, data_sel
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
    movw %cs, %cx
    movl $bp_handler, %edx
    movw $3, %bx
    movw $0x0212, %ax
    int $0x31
    jc fail
    movw $0x0210, %ax
    int $0x31
    jc fail
    cmpw $bp_handler, %dx
    jne fail
    movw %cs, %ax
    cmpw %ax, %cx
    jne fail
    movl $ud_handler, %edx
    movw $6, %bx
    movw $0x0212, %ax
    int $0x31
    jc fail
    movw %cx, previous + FIELD
    movl $host_handler, %edx
    movw $13, %bx
    movw $0x0212, %ax
    int $0x31
    jc fail
    movw %ds, %bx
    movw $0x000a, %ax
    int $0x31
    jc fail
    movw %ax, buffer_sel
    movw $0x000a, %ax
    int $0x31
    jc fail
    movw %ax, nested_sel
    movw $0x000a, %ax
    int $0x31
    jc fail
    movw %ax, target_sel
    movw %ax, %bx
    movw $(0xfa | (PM32 * 0x4000)), %cx
    movw $9, %ax
    int $0x31
    jc fail
    movw data_sel, %ax
    movw %ax, %es
    int3
after_bp:
    cmpb $1, breaks
    jne fail
    movw %es, %ax
    cmpw data_sel, %ax
    jne fail
    movb $1, scenario
    ud2
after_ud:
    cmpl $(0x7000 + (!STACK32 * 0xcafe0000)), %esp
    jne fail
    movl saved_sp, %esp
    movw %es, %ax
    cmpw buffer_sel, %ax
    jne fail
    movw data_sel, %ax
    movw %ax, %es
    movb $2, scenario
    ud2
after_old:
    cmpl saved_sp, %esp
    jne fail
    movb $3, scenario
    movw %cs, %cx
    movl $old_chain, %edx
    movw $6, %bx
    movw $0x0203, %ax
    int $0x31
    jc fail
    ud2
after_chain:
    cmpb $1, chained
    jne fail
    movb $4, scenario

next_host:
    movb $0, faults
    movb $0, nested
    movb $0, breaks
    movw buffer_sel, %bx
    xorw %cx, %cx
    movw $(record + 48), %dx
    movw $8, %ax
    int $0x31
    jc fail
    movw nested_sel, %bx
    movw $8, %ax
    int $0x31
    jc fail
    movw buffer_sel, %ax
    movw %ax, %es
    movw target_sel, %ax
    movw %ax, %ds
    movl $callback, %esi
    movl $record, %edi
    movl $0x11223344, %ecx
    movl $0x55667788, %edx
    movw $0x0303, %ax
    stc
    int $0x31
after_host:
    pushfl
    popl %eax
    cmpb $7, %cs:scenario
    je cancel_check
    testb $1, %al
    jnz fail
    movw %cs:data_sel, %ax
    movw %ax, %ds
    movw $0x0304, %ax
    int $0x31
    jc fail
    cmpb $6, scenario
    jne one_fault
    cmpb $2, faults
    jne fail
    cmpb $1, breaks
    jne fail
    jmp case_done
one_fault:
    cmpb $1, faults
    jne fail
    jmp case_done
cancelled:
    pushfl
    popl %eax
    cmpb $5, %cs:scenario
    jne fail
cancel_check:
    testb $1, %al
    jz fail
    cmpl $0x11223344, %ecx
    jne fail
    cmpl $0x55667788, %edx
    jne fail
    cmpb $1, %cs:faults
    jne fail
    movw %cs:data_sel, %ax
    movw %ax, %ds
case_done:
    cmpl saved_sp, %esp
    jne fail
    incb scenario
    cmpb $8, scenario
    jbe next_host
    jmp fail

bp_handler:
    pushal
    movl %esp, %ebp
    andl $0xffff, %ebp
    testw $7, %ss:(32 + 0x32)(%ebp)
    jnz fail
    incb breaks
    xorw %ax, %ax
    movw %ax, %es
    ext_return

old_chain:
    incb chained
    ljmp *%cs:previous

ud_handler:
    pushal
    movl %esp, %ebp
    andl $0xffff, %ebp
    testw $7, %ss:(32 + 0x32)(%ebp)
    jnz fail
    movl saved_sp, %eax
    cmpl %eax, %ss:(32 + 0x38)(%ebp)
    jne fail
    movw %cs, %ax
    cmpw %ax, %ss:(32 + 0x30)(%ebp)
    jne fail
    cmpb $2, scenario
    je old_return
    addl $2, %ss:(32 + 0x2c)(%ebp)
    movw $0xffff, %ss:(32 + 4 * FIELD)(%ebp)
    cmpb $1, scenario
    jne ud_return
    movl $(0x7000 + (!STACK32 * 0xcafe0000)), %ss:(32 + 0x38)(%ebp)
    movw buffer_sel, %ax
    movw %ax, %ss:(32 + 0x40)(%ebp)
ud_return:
    ext_return
old_return:
    .if PM32
    addl $2, %ss:(32 + 3 * FIELD)(%ebp)
    .else
    addw $2, %ss:(32 + 3 * FIELD)(%ebp)
    .endif
    movl $fail, %ss:(32 + 0x2c)(%ebp)
    movw $0xffff, %ss:(32 + 0x40)(%ebp)
    popal
    lret

host_handler:
    pushal
    movl %esp, %ebp
    andl $0xffff, %ebp
    movw %cs:data_sel, %ax
    movw %ax, %ds
    cmpw $1, %ss:(32 + 0x32)(%ebp)
    jne fail
    incb faults
    cmpb $0, nested
    jne nested_fault
    cmpl $after_host, %ss:(32 + 0x2c)(%ebp)
    jne fail
    movl saved_sp, %eax
    cmpl %eax, %ss:(32 + 0x38)(%ebp)
    jne fail
    cmpb $8, scenario
    je pass_exit
    cmpb $5, scenario
    je redirect
    cmpb $7, scenario
    je redirect
    cmpb $6, scenario
    jne repair_outer
    int3
    movb $1, nested
    movw nested_sel, %ax
    movw %ax, %es
    movw target_sel, %ax
    movw %ax, %ds
    movl $callback, %esi
    movl $record, %edi
    movw $0x0303, %ax
    int $0x31
after_nested:
    jc fail
    movw %cs:data_sel, %ax
    movw %ax, %ds
    movb $0, nested
    movw $0x0304, %ax
    int $0x31
    jc fail
repair_outer:
    movw buffer_sel, %bx
    jmp repair
nested_fault:
    cmpl $after_nested, %ss:(32 + 0x2c)(%ebp)
    jne fail
    movw nested_sel, %bx
repair:
    xorw %cx, %cx
    movw $0xffff, %dx
    movw $8, %ax
    int $0x31
    jc fail
    /* Default host retry must use the saved continuation, not edited EIP.
     * Clearing the public host bit must not change private ownership. */
    movl $fail, %ss:(32 + 0x2c)(%ebp)
    movw $0, %ss:(32 + 0x32)(%ebp)
    ext_return
redirect:
    orw $4, %ss:(32 + 0x32)(%ebp)
    orl $1, %ss:(32 + 0x34)(%ebp)
    cmpb $7, scenario
    je redirect_return
    movl $cancelled, %ss:(32 + 0x2c)(%ebp)
redirect_return:
    ext_return

callback:
    iret
pass_exit:
    movl $pass_message, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c2a, %ax
    int $0x21
fail:
    cld
    movw %cs:data_sel, %ax
    testw %ax, %ax
    jz early_fail
    movw %ax, %ds
    movb scenario, %al
    addb $'0', %al
    movb %al, fail_case
    movl $fail_message, %edx
    movw $0x0900, %ax
    int $0x21
early_fail:
    movw $0x4c71, %ax
    int $0x21

    .balign 4
dpmi_entry: .word 0, 0
pm_entry: .word protected_entry, 0
base_hi: .word 0
base_lo: .word 0
data_sel: .word 0
buffer_sel: .word 0
nested_sel: .word 0
target_sel: .word 0
previous:
    .if PM32
    .long ud_handler
    .else
    .word ud_handler
    .endif
    .word 0
saved_sp: .long 0
scenario: .byte 0
breaks: .byte 0
faults: .byte 0
nested: .byte 0
chained: .byte 0
pass_message: .ascii "DPMI EXTENDED EXCEPTION PASS\r\n$"
fail_message: .ascii "DPMI EXTENDED EXCEPTION FAIL case="
fail_case: .ascii "0\r\n$"
    .balign 4
record: .space 50
