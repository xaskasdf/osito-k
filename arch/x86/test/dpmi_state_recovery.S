/* Restart the protected 0305h procedure without losing its FAR caller. */
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
    movw %cs, saved_cs
    movw %ss, saved_ss
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
    movw %ds, %ax
    movw %ax, %es
    movw $0x0305, %ax
    int $0x31
    jc fail
    testw %ax, %ax
    jz fail
    cmpw $128, %ax
    ja fail
    movw %ax, state_size
    movw %si, host_cs
    movl %edi, host_resume
    addl $2, host_resume
    .if PM32
    movl %edi, pm_state
    movw %si, pm_state + 4
    .else
    movw %di, pm_state
    movw %si, pm_state + 2
    .endif
    movl $baseline, %edi
    movb $0, %al
    lcall *pm_state
    movw %ds, %bx
    movw $0x000a, %ax
    int $0x31
    jc fail
    movw %ax, buffer_sel
    movw %cs, %cx
    movl $buffer_handler, %edx
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
    movw data_sel, %ax
    movw %ax, %es
    movl $baseline, %esi
    movl $incoming, %edi
    movzwl state_size, %ecx
    rep movsb
    movl $result, %edi
    movl $128, %ecx
    movb $0xa5, %al
    rep stosb
    movb $0, faults
    movb $0, nested
    movw buffer_sel, %bx
    movw $0xf2, %cx
    movw $9, %ax
    int $0x31
    jc fail
    xorw %cx, %cx
    movw $0xffff, %dx
    cmpb $0, operation
    je save_buffer
    movw state_size, %dx
    addw $(incoming - 2), %dx
    movw $8, %ax
    int $0x31
    jc fail
    movl $incoming, %edi
    jmp invoke
save_buffer:
    movw $8, %ax
    int $0x31
    jc fail
    movw $0xf0, %cx
    movw $9, %ax
    int $0x31
    jc fail
    movl $result, %edi
invoke:
    .if !PM32
    orl $0x12340000, %edi
    .endif
    movl %edi, expected_di
    movl $0xa5a5bc00, %eax
    movb operation, %al
    movl %eax, expected_ax
    movw buffer_sel, %es
    movl $0xabcdef01, %ebx
    movl $0x11223344, %ecx
    movl $0x55667788, %edx
    movl $0x98765432, %esi
    movl $0xcafebabe, %ebp
    std
    stc
    lcall *pm_state
after_call:
    pushfl
    popl saved_flags
    cmpb $3, scenario
    je fail
    jmp result_registers
cancelled:
    pushfl
    popl saved_flags
    cmpb $3, scenario
    jne fail
    /* Redirection leaves the suspended procedure's FAR caller on SS. */
    addl $(2 * FIELD), %esp
result_registers:
    cmpb $5, scenario
    jne original_arguments
    cmpl changed_ax, %eax
    jne fail
    cmpl $0x0bad0100, %edi
    jne fail
    movw %es, %ax
    cmpw data_sel, %ax
    jne fail
    jmp arguments_done
original_arguments:
    cmpl expected_ax, %eax
    jne fail
    cmpl expected_di, %edi
    jne fail
    movw %es, %ax
    cmpw buffer_sel, %ax
    jne fail
arguments_done:
    cmpl saved_sp, %esp
    jne fail
    cmpl $0xabcdef01, %ebx
    jne fail
    cmpl $0x11223344, %ecx
    jne fail
    cmpl $0x55667788, %edx
    jne fail
    cmpl $0x98765432, %esi
    jne fail
    cmpl $0xcafebabe, %ebp
    jne fail
    testb $1, saved_flags
    jz fail
    movl saved_flags, %eax
    andl $0x400, %eax
    cmpb $4, scenario
    je cleared_df
    cmpl $0x400, %eax
    jne fail
    jmp flags_done
cleared_df:
    testl %eax, %eax
    jne fail
flags_done:
    cld
    movb $1, %al
    cmpb $1, scenario
    jne 1f
    incb %al
1:  cmpb %al, faults
    jne fail
    xorb %al, %al
    cmpb $2, scenario
    jne 2f
    incb %al
2:  cmpb %al, nested
    jne fail
    movw data_sel, %ax
    movw %ax, %es
    cmpb $1, operation
    je verify_restore
    cmpb $3, scenario
    je untouched_output
    movl $baseline, %esi
    movl $result, %edi
    movzwl state_size, %ecx
    repe cmpsb
    jne fail
    jmp case_done
verify_restore:
    movl $query, %edi
    movb $0, %al
    lcall *pm_state
    movl $baseline, %esi
    movl $query, %edi
    movzwl state_size, %ecx
    repe cmpsb
    jne fail
untouched_output:
    movl $result, %edi
    movl $128, %ecx
    movb $0xa5, %al
    repe scasb
    jne fail
case_done:
    cmpb $6, scenario
    jne next_scenario
    movl $baseline, %esi
    movl $nested_state, %edi
    movzwl state_size, %ecx
    repe cmpsb
    jne fail
next_scenario:
    incb scenario
    cmpb $7, scenario
    jb next_case
    movb $0, scenario
    incb operation
    cmpb $2, operation
    jb next_case
    movl $pass_message, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c2a, %ax
    int $0x21
fail:
    cld
    movl $fail_message, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c71, %ax
    int $0x21

buffer_handler:
    pushal
    movl %esp, %ebp
    incb faults
    xorw %ax, %ax
    cmpb $1, operation
    je 1f
    movw buffer_sel, %ax
    andw $0xfffc, %ax
1:  cmpw %ax, 32 + 2 * FIELD(%ebp)
    jne handler_fail
    .if PM32
    movl host_resume, %eax
    cmpl %eax, 32 + 3 * FIELD(%ebp)
    .else
    movw host_resume, %ax
    cmpw %ax, 32 + 3 * FIELD(%ebp)
    .endif
    jne handler_fail
    movw host_cs, %ax
    cmpw %ax, 32 + 4 * FIELD(%ebp)
    jne handler_fail
    movw saved_ss, %ax
    cmpw %ax, 32 + 7 * FIELD(%ebp)
    jne handler_fail
    movl saved_sp, %eax
    subl $(2 * FIELD), %eax
    .if PM32
    cmpl %eax, 32 + 6 * FIELD(%ebp)
    .else
    cmpw %ax, 32 + 6 * FIELD(%ebp)
    .endif
    jne handler_fail
    cmpb $1, scenario
    jne repair
    cmpb $1, faults
    je handler_return
repair:
    cmpb $2, scenario
    jne 2f
    int $3
2:  movw buffer_sel, %bx
    xorw %cx, %cx
    movw $0xffff, %dx
    movw $8, %ax
    int $0x31
    jc handler_fail
    movw $0xf2, %cx
    movw $9, %ax
    int $0x31
    jc handler_fail
    cmpb $3, scenario
    jne 3f
    movw saved_cs, %ax
    .if PM32
    movzwl %ax, %eax
    movl %eax, 32 + 4 * FIELD(%ebp)
    movl $cancelled, 32 + 3 * FIELD(%ebp)
    .else
    movw %ax, 32 + 4 * FIELD(%ebp)
    movw $cancelled, 32 + 3 * FIELD(%ebp)
    .endif
3:  cmpb $4, scenario
    jne 4f
    .if PM32
    andl $0xfffffbff, 32 + 5 * FIELD(%ebp)
    .else
    andw $0xfbff, 32 + 5 * FIELD(%ebp)
    .endif
4:  cmpb $5, scenario
    jne 5f
    movl $0x0bad0000, %eax
    movb operation, %al
    xorb $1, %al
    movl %eax, changed_ax
    movl %eax, 28(%ebp)
    movl $0x0bad0100, 0(%ebp)
    movw data_sel, %ax
    movw %ax, %es
5:  cmpb $6, scenario
    jne handler_return
    pushw %es
    movw data_sel, %ax
    movw %ax, %es
    movl $nested_state, %edi
    movb $0, %al
    lcall *pm_state
    movl $baseline, %edi
    movb $1, %al
    lcall *pm_state
    popw %es
handler_return:
    popal
    lret
handler_fail:
    movw $0x4c72, %ax
    int $0x21
nested_handler:
    incb nested
    lret

    .align 4
dpmi_entry: .word 0, 0
pm_pointer: .word protected_entry, 0
pm_state: .long 0
    .word 0
base_hi: .word 0
base_lo: .word 0
data_sel: .word 0
buffer_sel: .word 0
saved_cs: .word 0
saved_ss: .word 0
host_cs: .word 0
state_size: .word 0
host_resume: .long 0
saved_sp: .long 0
saved_flags: .long 0
expected_ax: .long 0
expected_di: .long 0
changed_ax: .long 0
operation: .byte 0
scenario: .byte 0
faults: .byte 0
nested: .byte 0
baseline: .space 128
incoming: .space 128
query: .space 128
result: .space 128
nested_state: .space 128
pass_message: .asciz "DPMI STATE RECOVERY PASS\r\n$"
fail_message: .asciz "DPMI STATE RECOVERY FAIL\r\n$"
