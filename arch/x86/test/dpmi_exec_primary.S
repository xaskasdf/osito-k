/* A real EXEC child uses its primary DPMI client's handlers and callbacks. */
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
    movw %di, entry
    movw %es, entry + 2
    .if CHILD
    /* Failed initial entry must leave the primary and its vectors intact. */
    movw $2, %ax
    lcall *entry
    jnc setup_fail
    cmpw $0x8021, %ax
    jne setup_fail
    movl $0x11223344, %eax
    int3
    cmpl $0x504d434c, %eax
    jne setup_fail
    ud2
    int $0x1c
    lcall *0x81
    cmpl $0x1234abcd, %eax
    jne setup_fail
    .if CHILD == 1
    pushw %cs
    popw %es
    movw %cs, parameters + 4
    movw %cs, parameters + 8
    movw %cs, parameters + 12
    movw $leaf_name, %dx
    movw $parameters, %bx
    movw $0x4b00, %ax
    int $0x21
    jc setup_fail
    movw $0x4d00, %ax
    int $0x21
    cmpw $0x2a, %ax
    jne setup_fail
    .endif
    .if CHILD == 3
    xorw %ax, %ax
    lcall *entry
    jc setup_fail
    movw $3, %bx
    movw $0x0211, %ax
    int $0x31
    jc setup_fail
    /* A fresh client's default real #BP vector is its own host stub. */
    cmpw $(0x2000 + 3 * 4), %dx
    jne setup_fail
    .endif
    movw $0x4c2a, %ax
    int $0x21
    .else
    movw %cs, real_segment
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
    .endif
setup_fail:
    movw $0x4c70, %ax
    int $0x21

    .if !CHILD
    .if PM32
    .code32
    .endif
protected_entry:
    movw $0x0900, %ax
    int $0x31
    movw %ss, %bx
    movw $(0xf2 | (STACK32 * 0x4000)), %cx
    movw $9, %ax
    int $0x31
    jc fail
    movw %bx, %ss
    movl $0xc000, %esp
    movw data_sel, %ax
    movw %ax, %es
    movw %cs, %ax
    movw %ax, %ds
    movl $callback, %esi
    movl $callback_record, %edi
    movw $0x0303, %ax
    int $0x31
    jc fail
    movw data_sel, %ax
    movw %ax, %ds
    movw %dx, tail + 1
    movw %cx, tail + 3
    movw %cs, %cx
    movw $3, %bx
    movl $real_bp, %edx
    movw $0x0213, %ax
    int $0x31
    jc fail
    movw $6, %bx
    movl $real_ud, %edx
    movw $0x0213, %ax
    int $0x31
    jc fail
    movw $3, %bx
    movl $outer_pm, %edx
    movw $0x0212, %ax
    int $0x31
    jc fail
    movw real_segment, %ax
    movw %ax, record + 36
    movw %ax, record + 44
    movw %ax, record + 48
    call run_children
    cmpb $4, breaks
    jne fail
    cmpb $4, invalids
    jne fail
    cmpb $4, callbacks
    jne fail
    cmpb $4, timers
    jb fail
    /* Repeat from an existing protected exception's locked stack. */
    int3
    cmpb $8, breaks
    jne fail
    cmpb $8, invalids
    jne fail
    cmpb $8, callbacks
    jne fail
    movw $0x1c, %bx
    movw $0x0204, %ax
    int $0x31
    jc fail
    cmpw $timer, %dx
    jne fail
    movw owned_sel, %dx
    movw $0x0101, %ax
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
    movw $0x4c71, %ax
    int $0x21
run_children:
    movw data_sel, %ax
    movw %ax, %es
    movl $record, %edi
    xorw %bx, %bx
    xorw %cx, %cx
    movw $0x0301, %ax
    int $0x31
    jc fail
    ret
outer_pm:
    pushal
    call run_children
    popal
    .if PM32
    addl $32, %esp
    .else
    addw $32, %sp
    .endif
    lret
real_bp:
    pushal
    movl $0x504d434c, %ss:28(%esp)
    movw %cs:data_sel, %ax
    movw %ax, %ds
    movw $0x5100, %ax
    int $0x21
    cmpw real_segment, %bx
    je fail
    incb breaks
    /* Persist a vector edit made while the DOS PSP belongs to the child. */
    movw %cs, %cx
    movl $timer, %edx
    movw $0x1c, %bx
    movw $0x0205, %ax
    int $0x31
    jc fail
    cmpw $0, owned_sel
    jne real_done
    movw $4, %bx
    movw $0x0100, %ax
    int $0x31
    jc fail
    movw %dx, owned_sel
real_done:
    popal
    .if PM32
    addl $32, %esp
    .else
    addw $32, %sp
    .endif
    lret
real_ud:
    pushal
    movw %cs:data_sel, %ax
    movw %ax, %ds
    incb invalids
    movl %esp, %ebp
    .if !PM32
    movzwl %bp, %ebp
    .endif
    addl $2, %ss:0x4c(%ebp)
    jmp real_done
timer:
    pushw %ds
    pushw %ax
    movw %cs:data_sel, %ax
    movw %ax, %ds
    incb timers
    popw %ax
    popw %ds
    iret
callback:
    pushal
    movw (%esi), %ax
    movw %ax, %es:42(%edi)
    movw 2(%esi), %ax
    movw %ax, %es:44(%edi)
    addw $4, %es:46(%edi)
    movl $0x1234abcd, %es:28(%edi)
    pushw %ds
    movw %cs:data_sel, %ax
    movw %ax, %ds
    incb callbacks
    popw %ds
    popal
    iret

    .code16
real_procedure:
    pushw %cs
    popw %ds
    pushw %cs
    popw %es
    movw %cs, parameters + 4
    movw %cs, parameters + 8
    movw %cs, parameters + 12
    movw $real_name, %dx
    movw $parameters, %bx
    movw $0x4b00, %ax
    int $0x21
    jc setup_fail
    movw $0x4d00, %ax
    int $0x21
    cmpw $0x2a, %ax
    jne setup_fail
    movw $missing_name, %dx
    movw $parameters, %bx
    movw $0x4b00, %ax
    int $0x21
    jnc setup_fail
    cmpw $2, %ax
    jne setup_fail
    movb $0, loaded
    movw $leaf_name, %dx
    movw $parameters, %bx
    movw $0x4b01, %ax
    int $0x21
    jc setup_fail
    cmpb $0, loaded
    jne loaded_return
    movb $1, loaded
    movw parameters + 14, %di
    movw parameters + 16, %bp
    cli
    movw %bp, %ss
    movw %di, %sp
    ljmp *parameters + 18
loaded_return:
    movw $0x4d00, %ax
    int $0x21
    cmpw $0x2a, %ax
    jne setup_fail
    movw $new_name, %dx
    movw $parameters, %bx
    movw $0x4b00, %ax
    int $0x21
    jc setup_fail
    movw $0x4d00, %ax
    int $0x21
    cmpw $0x2a, %ax
    jne setup_fail
    lret

record:
    .zero 32
    .word 0x202, 0, 0, 0, 0, real_procedure, 0, 0xa000, 0
callback_record: .zero 50
tail: .byte 4
    .word 0, 0
    .byte 13
real_segment: .word 0
data_sel: .word 0
base_hi: .word 0
base_lo: .word 0
pm_entry: .word protected_entry, 0
owned_sel: .word 0
breaks: .byte 0
invalids: .byte 0
timers: .byte 0
callbacks: .byte 0
loaded: .byte 0
real_name: .asciz "dpxreal.com"
new_name: .asciz "dpxnew.com"
missing_name: .asciz "absent.com"
pass_msg: .asciz "DPMI EXEC PRIMARY PASS\r\n$"
fail_msg: .asciz "DPMI EXEC PRIMARY FAIL\r\n$"
    .endif

entry: .word 0, 0
leaf_name: .asciz "dpxleaf.com"
parameters:
    .word 0
    .if CHILD
    .word 0x80, 0, 0x5c, 0, 0x6c, 0
    .else
    .word tail, 0, 0x5c, 0, 0x6c, 0
    .endif
    .zero 8
