/* Register, invoke and free callbacks after recoverable admission faults. */
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
    movw %ds, %bx
    movw $0x000a, %ax
    int $0x31
    jc fail
    movw %ax, buffer_sel
    movw %ds, %bx
    movw $0x000a, %ax
    int $0x31
    jc fail
    movw %ax, target_sel
    movw %cs, %cx
    movl $fault_handler, %edx
    movw $13, %bx
    movw $0x0203, %ax
    int $0x31
    jc fail
    movl $nested_handler, %edx
    movw $3, %bx
    movw $0x0203, %ax
    int $0x31
    jc fail

next_case:
    cld
    movb $0, faults
    movb $0, nested
    movb $0, called
    movb $0, retrying
    movw data_sel, %ax
    movw %ax, %es
    movl $callback_regs, %edi
    movl $50, %ecx
    movb $0xa5, %al
    rep stosb
    movw buffer_sel, %bx
    movw $0xf2, %cx
    cmpb $3, scenario
    jne buffer_rights
    movw $0xf0, %cx
buffer_rights:
    movw $9, %ax
    int $0x31
    jc fail
    xorw %cx, %cx
    movw $0xffff, %dx
    cmpb $0, scenario
    je buffer_limit
    cmpb $2, scenario
    je buffer_limit
    cmpb $3, scenario
    je buffer_limit
    cmpb $10, scenario
    je buffer_limit
    movw $(callback_regs + 48), %dx
buffer_limit:
    movw $8, %ax
    int $0x31
    jc fail
    movw target_sel, %bx
    movw $(0xfa | (PM32 * 0x4000)), %cx
    cmpb $10, scenario
    jne target_rights
    movw $0xf2, %cx
target_rights:
    movw $9, %ax
    int $0x31
    jc fail
    xorw %cx, %cx
    movw $0xffff, %dx
    cmpb $2, scenario
    jne target_limit
    movw $(callback - 1), %dx
target_limit:
    movw $8, %ax
    int $0x31
    jc fail
invoke:
    movw buffer_sel, %ax
    movw %ax, %es
    movw target_sel, %ax
    movw %ax, %ds
    movl $callback, %esi
    movl $callback_regs, %edi
    .if !PM32
    orl $0x98760000, %esi
    orl $0x12340000, %edi
    .endif
    movl $0x11223344, %ecx
    movl $0x55667788, %edx
    movl $0xcafebabe, %ebp
    movl $0xa5a50303, %eax
    std
    stc
    .rept 13
    .byte 0x3e
    .endr
    int $0x31
after_call:
    jc fail
    cmpb $8, %cs:scenario
    jne check_registers
    cmpb $1, %cs:retrying
    jne fail
    jmp check_registers
cancelled:
    jnc fail
    cmpb $8, %cs:scenario
    jne fail
    cmpb $0, %cs:retrying
    jne fail
    cmpl $0x11223344, %ecx
    jne fail
    cmpl $0x55667788, %edx
    jne fail
check_registers:
    cmpl $0xa5a50303, %eax
    jne fail
    cmpl $0xcafebabe, %ebp
    jne fail
    cmpl %cs:saved_sp, %esp
    jne fail
    cmpb $6, %cs:scenario
    jne original_args
    cmpl $0x89abcdef, %esi
    jne fail
    cmpl $0x12345678, %edi
    jne fail
    movw %ds, %ax
    cmpw %cs:data_sel, %ax
    jne fail
    movw %es, %ax
    cmpw %cs:data_sel, %ax
    jne fail
    jmp args_done
original_args:
    .if PM32
    cmpl $callback, %esi
    jne fail
    cmpl $callback_regs, %edi
    .else
    cmpl $(0x98760000 + callback), %esi
    jne fail
    cmpl $(0x12340000 + callback_regs), %edi
    .endif
    jne fail
    movw %ds, %ax
    cmpw %cs:target_sel, %ax
    jne fail
    movw %es, %ax
    cmpw %cs:buffer_sel, %ax
    jne fail
args_done:
    cld
    movw %cs:data_sel, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %dx, real_call + 42
    movw %cx, real_call + 44
    movw %dx, callback_pointer
    movw %cx, callback_pointer + 2
    movb $1, %al
    cmpb $0, scenario
    jne expected_faults
    movb $0, %al
expected_faults:
    cmpb $4, scenario
    je twice
    cmpb $9, scenario
    jne count_faults
twice:
    movb $2, %al
count_faults:
    cmpb faults, %al
    jne fail
    movb $0, %al
    cmpb $5, scenario
    jne count_nested
    movb $1, %al
count_nested:
    cmpb nested, %al
    jne fail
    movl $callback_regs, %esi
    movl $50, %ecx
untouched:
    cmpb $0xa5, (%esi)
    jne fail
    incl %esi
    loop untouched
    cmpb $8, scenario
    jne call_callback
    cmpb $0, retrying
    jne call_callback
    movb $1, retrying
    jmp invoke
call_callback:
    xorw %bx, %bx
    xorw %cx, %cx
    movl $real_call, %edi
    movw $0x0301, %ax
    int $0x31
    jc fail
    cmpb $1, called
    jne fail
    cmpl $0xcafef00d, real_call + 28
    jne fail
    movw callback_pointer + 2, %cx
    movw callback_pointer, %dx
    movw $0x0304, %ax
    int $0x31
    jc fail
    cmpb $7, scenario
    jne case_done
    movw nested_pointer + 2, %cx
    movw nested_pointer, %dx
    movw $0x0304, %ax
    int $0x31
    jc fail
case_done:
    incb scenario
    cmpb $11, scenario
    jb next_case
    jmp resource_test

fault_handler:
    push %ds
    push %es
    pushal
    movl %esp, %ebp
    .if !STACK32
    andl $0xffff, %ebp
    .endif
    movw %cs:data_sel, %ax
    movw %ax, %ds
    incb faults
    cmpw $after_call, %ss:(32 + 5 * FIELD)(%ebp)
    jne fail
    movw %cs, %ax
    cmpw %ax, %ss:(32 + 6 * FIELD)(%ebp)
    jne fail
    .if PM32
    movl saved_sp, %eax
    cmpl %eax, %ss:(32 + 8 * FIELD)(%ebp)
    .else
    movw saved_sp, %ax
    cmpw %ax, %ss:(32 + 8 * FIELD)(%ebp)
    .endif
    jne fail
    xorw %ax, %ax
    cmpb $3, scenario
    jne code_error
    movw buffer_sel, %ax
code_error:
    cmpb $10, scenario
    jne expected_error
    movw target_sel, %ax
expected_error:
    andw $0xfffc, %ax
    cmpw %ax, %ss:(32 + 4 * FIELD)(%ebp)
    jne fail
    cmpb $5, scenario
    jne no_nested_bp
    int $3
no_nested_bp:
    cmpb $4, scenario
    jne repair
    cmpb $1, faults
    je fault_return
repair:
    movw buffer_sel, %bx
    movw $0xf2, %cx
    movw $9, %ax
    int $0x31
    jc fail
    xorw %cx, %cx
    movw $0xffff, %dx
    movw $8, %ax
    int $0x31
    jc fail
    movw target_sel, %bx
    movw $(0xfa | (PM32 * 0x4000)), %cx
    movw $9, %ax
    int $0x31
    jc fail
    xorw %cx, %cx
    movw $0xffff, %dx
    cmpb $9, scenario
    jne repair_target
    cmpb $1, faults
    jne repair_target
    movw $(callback - 1), %dx
repair_target:
    movw $8, %ax
    int $0x31
    jc fail
    cmpb $7, scenario
    jne no_nested_registration
    movw data_sel, %ax
    movw %ax, %es
    movw target_sel, %ax
    movw %ax, %ds
    movl $callback, %esi
    movl $nested_regs, %edi
    movw $0x0303, %ax
    int $0x31
    jc fail
    movw %cs:data_sel, %ax
    movw %ax, %ds
    movw %dx, nested_pointer
    movw %cx, nested_pointer + 2
no_nested_registration:
    cmpb $6, scenario
    jne no_changed_args
    movw data_sel, %ax
    movw %ax, %ss:32(%ebp)
    movw %ax, %ss:(32 + FIELD)(%ebp)
    movl $0x12345678, %ss:0(%ebp)
    movl $0x89abcdef, %ss:4(%ebp)
no_changed_args:
    cmpb $8, scenario
    jne fault_return
    .if PM32
    movl $cancelled, %ss:(32 + 5 * FIELD)(%ebp)
    .else
    movw $cancelled, %ss:(32 + 5 * FIELD)(%ebp)
    .endif
fault_return:
    .if PM32
    andl $~0x400, %ss:(32 + 7 * FIELD)(%ebp)
    .else
    andw $~0x400, %ss:(32 + 7 * FIELD)(%ebp)
    .endif
    popal
    pop %es
    pop %ds
    lret

nested_handler:
    incb nested
    lret

callback:
    movw (%esi), %ax
    movw %ax, %es:42(%edi)
    movw 2(%esi), %ax
    movw %ax, %es:44(%edi)
    addw $4, %es:46(%edi)
    movl $0xcafef00d, %es:28(%edi)
    pushw %ds
    movw %cs:data_sel, %ax
    movw %ax, %ds
    incb called
    popw %ds
    iret

resource_test:
    movw $0, allocated
allocate_more:
    movw buffer_sel, %ax
    movw %ax, %es
    movw target_sel, %ax
    movw %ax, %ds
    movl $callback, %esi
    movl $callback_regs, %edi
    movw $0x0303, %ax
    int $0x31
    jc exhausted
    movw %cs:data_sel, %ax
    movw %ax, %ds
    movzwl allocated, %edi
    shll $2, %edi
    movw %dx, addresses(%edi)
    movw %cx, addresses + 2(%edi)
    incw allocated
    cmpw $64, allocated
    jb allocate_more
    jmp fail
exhausted:
    cmpw $0x8015, %ax
    jne fail
    movw %cs:data_sel, %ax
    movw %ax, %ds
    cmpw $16, allocated
    jb fail
release_all:
    decw allocated
    movzwl allocated, %edi
    shll $2, %edi
    movw addresses(%edi), %dx
    movw addresses + 2(%edi), %cx
    movw $0x0304, %ax
    int $0x31
    jc fail
    cmpw $0, allocated
    jne release_all
    movw $0x0304, %ax
    int $0x31
    jnc fail
    cmpw $0x8024, %ax
    jne fail
    cld
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
    cmpb $10, %al
    jb fail_digit
    addb $('A' - 10), %al
    jmp fail_record
fail_digit:
    addb $'0', %al
fail_record:
    movb %al, fail_case
    movb faults, %al
    addb $'0', %al
    movb %al, fail_faults
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
target_sel: .word 0
saved_sp: .long 0
scenario: .byte 0
faults: .byte 0
nested: .byte 0
called: .byte 0
retrying: .byte 0
allocated: .word 0
callback_pointer: .word 0, 0
nested_pointer: .word 0, 0
real_call:
    .space 32, 0
    .word 0x202
    .space 16, 0
callback_regs: .space 50, 0xa5
nested_regs: .space 50, 0x5a
addresses: .space 256, 0
pass_message: .asciz "DPMI CALLBACK REGISTER PASS\r\n$"
fail_message: .ascii "DPMI CALLBACK REGISTER FAIL case="
fail_case: .ascii "? faults="
fail_faults: .asciz "?\r\n$"
