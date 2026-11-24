/* Repair DPMI descriptor/information buffers and retain the original request. */
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
    movw %ax, %bx
    movw $0x000a, %ax
    int $0x31
    jc fail
    movw %ax, target_sel
    movw %ax, %bx
    movl $original, %edi
    movw $0x000b, %ax
    int $0x31
    jc fail
    movl original, %eax
    movl %eax, incoming
    movl original + 4, %eax
    movl %eax, incoming + 4
    xorb $0x10, incoming + 6
    movl $result, %edi
    movl $48, %ecx
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
    cmpb $1, kind
    jne output_buffer
    movw $(incoming + 6), %dx
    movw $8, %ax
    int $0x31
    jc fail
    movl $incoming, %edi
    movl $0xa5a5000c, %eax
    jmp invoke
output_buffer:
    movw $8, %ax
    int $0x31
    jc fail
    movw $0xf0, %cx
    movw $9, %ax
    int $0x31
    jc fail
    movl $result, %edi
    movl $0xa5a5000b, %eax
    cmpb $0, kind
    je invoke
    movl $0xa5a50500, %eax
invoke:
    .if !PM32
    orl $0x12340000, %edi
    .endif
    movl %eax, expected_ax
    movl %edi, expected_di
    movl $0xabcd0000, %ebx
    movw target_sel, %bx
    movl %ebx, expected_bx
    movw buffer_sel, %es
    movl $0x11223344, %ecx
    movl $0x55667788, %edx
    movl $0x98765432, %esi
    movl $0xcafebabe, %ebp
    std
    stc
    .rept 13
    .byte 0x3e
    .endr
    int $0x31
after_call:
    pushfl
    popl saved_flags
    cmpb $5, scenario
    je freed_target
    testb $1, saved_flags
    jnz fail
    cmpb $3, scenario
    je fail
    jmp result_registers
freed_target:
    testb $1, saved_flags
    jz fail
    cmpw $0x8022, %ax
    jne fail
    jmp result_common
cancelled:
    pushfl
    popl saved_flags
    testb $1, saved_flags
    jz fail
    cmpb $3, scenario
    jne fail
result_registers:
    cmpl expected_ax, %eax
    jne fail
result_common:
    cmpl saved_sp, %esp
    jne fail
    cmpl $0x11223344, %ecx
    jne fail
    cmpl $0x55667788, %edx
    jne fail
    cmpl $0x98765432, %esi
    jne fail
    cmpl $0xcafebabe, %ebp
    jne fail
    cmpb $6, scenario
    jne original_arguments
    cmpl $0xabcd0000, %ebx
    jne fail
    cmpl $0x0bad1000, %edi
    jne fail
    movw %es, %ax
    cmpw data_sel, %ax
    jne fail
    jmp arguments_done
original_arguments:
    cmpl expected_bx, %ebx
    jne fail
    cmpl expected_di, %edi
    jne fail
    movw %es, %ax
    cmpw buffer_sel, %ax
    jne fail
arguments_done:
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
    cmpb $5, scenario
    je untouched_output
    cmpb $1, kind
    je verify_set
    cmpb $3, scenario
    je untouched_output
    cmpb $2, kind
    je verify_info
    movl $original, %esi
    movl $result, %edi
    movl $8, %ecx
    repe cmpsb
    jne fail
    jmp case_done
verify_info:
    cmpl $0xffffffff, result + 44
    jne fail
    cmpl $0, result
    je fail
    jmp case_done
verify_set:
    movw target_sel, %bx
    movl $query, %edi
    movw $0x000b, %ax
    int $0x31
    jc fail
    movl $incoming, %esi
    cmpb $3, scenario
    jne 3f
    movl $original, %esi
3:  movl $query, %edi
    movl $8, %ecx
    repe cmpsb
    jne fail
untouched_output:
    movl $result, %edi
    movl $48, %ecx
    movb $0xa5, %al
    repe scasb
    jne fail
case_done:
    cmpb $5, scenario
    je next_scenario
    movw target_sel, %bx
    movw $1, %ax
    int $0x31
    jc fail
next_scenario:
    incb scenario
    cmpb $2, kind
    jne 4f
    cmpb $5, scenario
    jne 4f
    incb scenario
4:  cmpb $7, scenario
    jb next_case
    movb $0, scenario
    incb kind
    cmpb $3, kind
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
    cmpb $1, kind
    je 1f
    movw buffer_sel, %ax
    andw $0xfffc, %ax
1:  cmpw %ax, 32 + 2 * FIELD(%ebp)
    jne handler_fail
    .if PM32
    cmpl $after_call, 32 + 3 * FIELD(%ebp)
    .else
    cmpw $after_call, 32 + 3 * FIELD(%ebp)
    .endif
    jne handler_fail
    movw saved_cs, %ax
    cmpw %ax, 32 + 4 * FIELD(%ebp)
    jne handler_fail
    movw saved_ss, %ax
    cmpw %ax, 32 + 7 * FIELD(%ebp)
    jne handler_fail
    .if PM32
    movl saved_sp, %eax
    cmpl %eax, 32 + 6 * FIELD(%ebp)
    .else
    movw saved_sp, %ax
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
    cmpb $5, scenario
    jne 3f
    movw target_sel, %bx
    movw $1, %ax
    int $0x31
    jc handler_fail
3:  cmpb $3, scenario
    jne 4f
    .if PM32
    movl $cancelled, 32 + 3 * FIELD(%ebp)
    .else
    movw $cancelled, 32 + 3 * FIELD(%ebp)
    .endif
4:  cmpb $4, scenario
    jne 5f
    .if PM32
    andl $0xfffffbff, 32 + 5 * FIELD(%ebp)
    .else
    andw $0xfbff, 32 + 5 * FIELD(%ebp)
    .endif
5:  cmpb $6, scenario
    jne handler_return
    movl $0xabcd0000, 16(%ebp)
    movl $0x0bad1000, 0(%ebp)
    movw data_sel, %ax
    movw %ax, %es
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
base_hi: .word 0
base_lo: .word 0
data_sel: .word 0
buffer_sel: .word 0
target_sel: .word 0
saved_cs: .word 0
saved_ss: .word 0
saved_sp: .long 0
saved_flags: .long 0
expected_ax: .long 0
expected_bx: .long 0
expected_di: .long 0
kind: .byte 0
scenario: .byte 0
faults: .byte 0
nested: .byte 0
original: .space 8
incoming: .space 8
query: .space 8
result: .space 48
pass_message: .asciz "DPMI RECORD SERVICE PASS\r\n$"
fail_message: .asciz "DPMI RECORD SERVICE FAIL\r\n$"
