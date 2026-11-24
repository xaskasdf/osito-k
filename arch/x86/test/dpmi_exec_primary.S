/* A real EXEC child uses its primary DPMI client's handlers and callbacks. */
    .ifndef RESOURCES
    .set RESOURCES, 0
    .endif
    .ifndef CHILD32
    .set CHILD32, 0
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
    .if RESOURCES == 1 || RESOURCES == 3 || RESOURCES == 4
    movw %cs, child_real
    .endif
    .if CHILD32
    movw $1, %ax
    .else
    xorw %ax, %ax
    .endif
    lcall *entry
    jc setup_fail
    movw $3, %bx
    movw $0x0211, %ax
    int $0x31
    jc setup_fail
    /* A fresh client's default real #BP vector is its own host stub. */
    cmpw $(0x2000 + 3 * 4), %dx
    jne setup_fail
    .if CHILD32
    /* Initial entry returns in USE16 even for a 32-bit DPMI client. */
    movw %cs, %bx
    movw $6, %ax
    int $0x31
    jc setup_fail
    movw %cx, child_base + 2
    movw %dx, child_base
    movw $1, %cx
    xorw %ax, %ax
    int $0x31
    jc setup_fail
    movw %ax, child_pm_entry + 4
    movw %ax, %bx
    movw child_base + 2, %cx
    movw child_base, %dx
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
    ljmpl *child_pm_entry
    .code32
child_protected:
    .endif
    .if RESOURCES == 1 || RESOURCES == 4
    movw %ds, child_data
    .if RESOURCES == 4
    movw %cs, %cx
    movl $child_real_exception, %edx
    movw $6, %bx
    movw $0x0213, %ax
    int $0x31
    jc child_fail
    movl $child_timer, %edx
    movw $0x1c, %bx
    movw $0x0205, %ax
    int $0x31
    jc child_fail
    .endif
    movw %ds, %ax
    movw %ax, %es
    movw %cs, %ax
    movw %ax, %ds
    .if CHILD32
    movl $child_callback, %esi
    movl $child_record, %edi
    .else
    movw $child_callback, %si
    movw $child_record, %di
    .endif
    movw $0x0303, %ax
    int $0x31
    jc child_fail
    movw %cs:child_data, %ax
    movw %ax, %ds
    movw %cx, child_address + 2
    movw %dx, child_address
    /* The child's local slot zero must not alias the parent's far pointer. */
    cmpw 0x83, %cx
    jne callback_distinct
    cmpw 0x81, %dx
    je child_fail
callback_distinct:
    movw child_real, %ax
    movw %ax, child_real_record + 36
    movw %ax, child_real_record + 44
    movw %ax, child_real_record + 48
    movw %ds, %ax
    movw %ax, %es
    .if CHILD32
    movl $child_real_record, %edi
    .else
    movw $child_real_record, %di
    .endif
    xorw %cx, %cx
    xorw %bx, %bx
    movw $0x0301, %ax
    int $0x31
    jc child_fail
    cmpl $0x24681357, child_real_record + 28
    jne child_fail
    .if RESOURCES == 4
    cmpb $1, child_real_faults
    jne child_fail
    cmpb $1, child_timers
    jb child_fail
    .endif
    movw 0x83, %cx
    movw 0x81, %dx
    movw $0x0304, %ax
    int $0x31
    jnc child_fail
    cmpw $0x8024, %ax
    jne child_fail
    movw child_address + 2, %cx
    movw child_address, %dx
    movw $0x0304, %ax
    int $0x31
    jc child_fail
    .elseif RESOURCES == 3
    movw child_real, %ax
    movw %ax, child_real_record + 44
    movw %ds, %ax
    movw %ax, %es
    movl $child_real_record, %edi
    xorw %cx, %cx
    xorw %bx, %bx
    movw $0x0301, %ax
    int $0x31
    jc child_fail
    cmpl $0x24681357, child_real_record + 28
    jne child_fail
    .elseif RESOURCES == 2
    movw $3, %bx
    movw %cs, %cx
    .if CHILD32
    movl $child_exception, %edx
    .else
    movw $child_exception, %dx
    .endif
    movw $0x0203, %ax
    int $0x31
    jc child_fail
    int3
    cmpb $1, child_exceptions
    jne child_fail
    .endif
    .endif
    movw $0x4c2a, %ax
    int $0x21
    .if CHILD32
child_fail:
    movw $0x4c70, %ax
    int $0x21
    .code16
    .else
    .set child_fail, setup_fail
    .endif
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
    .if RESOURCES == 4
    /* Exhaust the first client's local table so the child's callback uses
     * an address beyond the first sixteen ROM stubs. */
    movw $15, %bp
more_callbacks:
    movw %cs, %ax
    movw %ax, %ds
    movl $callback, %esi
    movl $callback_record, %edi
    movw $0x0303, %ax
    int $0x31
    jc fail
    decw %bp
    jnz more_callbacks
    movw data_sel, %ax
    movw %ax, %ds
    movw real_segment, %ax
    movw %ax, cross_record + 44
    .endif
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
    .if RESOURCES != 3
    movw %ax, record + 48
    .endif
    call run_children
    cmpb $4, breaks
    jne fail
    cmpb $4, invalids
    jne fail
    cmpb $(4 - (RESOURCES == 4)), callbacks
    jne fail
    cmpb $4, timers
    jb fail
    /* Repeat from an existing protected exception's locked stack. */
    int3
    cmpb $8, breaks
    jne fail
    cmpb $8, invalids
    jne fail
    cmpb $(8 - 2 * (RESOURCES == 4)), callbacks
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
    .if RESOURCES == 4
    /* PUSHAD's saved EAX follows the saved DS word. */
    cmpl $0x99aabbcc, %ss:30(%esp)
    jne ordinary_callback
    pushw %es
    movw data_sel, %ax
    movw %ax, %es
    movl $cross_record, %edi
    xorw %cx, %cx
    xorw %bx, %bx
    movw $0x0301, %ax
    int $0x31
    jc fail
    popw %es
ordinary_callback:
    .endif
    popw %ds
    popal
    iret

    .code16
    .if RESOURCES == 4
cross_real:
    ud2
    int $0x1c
    lret
cross_record:
    .zero 32
    .word 0x2, 0, 0, 0, 0, cross_real, 0, 0, 0
    .endif
real_procedure:
    .if RESOURCES == 3
    movw $32, %cx
real_canary_push:
    pushw $0x5ac3
    loop real_canary_push
    .endif
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
    .if RESOURCES == 3
    movw $32, %cx
real_canary_pop:
    popw %ax
    cmpw $0x5ac3, %ax
    jne setup_fail
    loop real_canary_pop
    .endif
    lret

record:
    .zero 32
    .if RESOURCES == 3
    .word 0x202, 0, 0, 0, 0, real_procedure, 0, 0, 0
    .else
    .word 0x202, 0, 0, 0, 0, real_procedure, 0, 0xa000, 0
    .endif
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
    .if RESOURCES == 1
    .if CHILD32
new_name: .asciz "dxc32.com"
    .else
new_name: .asciz "dxcnew.com"
    .endif
    .elseif RESOURCES == 2
    .if CHILD32
new_name: .asciz "dxs32.com"
    .else
new_name: .asciz "dxsnew.com"
    .endif
    .elseif RESOURCES == 3
    .if CHILD32
new_name: .asciz "dxrmc32.com"
    .else
new_name: .asciz "dxrmc16.com"
    .endif
    .elseif RESOURCES == 4
    .if CHILD32
new_name: .asciz "dxcb32.com"
    .else
new_name: .asciz "dxcb16.com"
    .endif
    .else
new_name: .asciz "dpxnew.com"
    .endif
missing_name: .asciz "absent.com"
    .if RESOURCES == 1
pass_msg: .asciz "DPMI CLIENT CALLBACK PASS\r\n$"
fail_msg: .asciz "DPMI CLIENT CALLBACK FAIL\r\n$"
    .elseif RESOURCES == 2
pass_msg: .asciz "DPMI CLIENT STACK PASS\r\n$"
fail_msg: .asciz "DPMI CLIENT STACK FAIL\r\n$"
    .elseif RESOURCES == 3
pass_msg: .asciz "DPMI CLIENT REAL STACK PASS\r\n$"
fail_msg: .asciz "DPMI CLIENT REAL STACK FAIL\r\n$"
    .elseif RESOURCES == 4
pass_msg: .asciz "DPMI CLIENT ROUTING PASS\r\n$"
fail_msg: .asciz "DPMI CLIENT ROUTING FAIL\r\n$"
    .else
pass_msg: .asciz "DPMI EXEC PRIMARY PASS\r\n$"
fail_msg: .asciz "DPMI EXEC PRIMARY FAIL\r\n$"
    .endif
    .endif

    .if CHILD == 3
    .if CHILD32
    .code32
    .endif
    .if RESOURCES == 1 || RESOURCES == 4
    .if RESOURCES == 4
child_real_exception:
    pushal
    movw %cs:child_data, %ax
    movw %ax, %ds
    incb child_real_faults
    movl %esp, %ebp
    .if !CHILD32
    movzwl %bp, %ebp
    .endif
    addl $2, %ss:0x4c(%ebp)
    popal
    .if CHILD32
    addl $32, %esp
    .else
    addw $32, %sp
    .endif
    lret
child_timer:
    pushw %ds
    pushw %ax
    movw %cs:child_data, %ax
    movw %ax, %ds
    incb child_timers
    popw %ax
    popw %ds
    iret
child_real_faults: .byte 0
child_timers: .byte 0
    .endif
child_callback:
    pushal
    .if CHILD32
    movw (%esi), %ax
    movw %ax, %es:42(%edi)
    movw 2(%esi), %ax
    movw %ax, %es:44(%edi)
    addw $4, %es:46(%edi)
    movl $0x24681357, %es:28(%edi)
    .else
    movw (%si), %ax
    movw %ax, %es:42(%di)
    movw 2(%si), %ax
    movw %ax, %es:44(%di)
    addw $4, %es:46(%di)
    movl $0x24681357, %es:28(%di)
    .endif
    popal
    iret
    .code16
child_real_procedure:
    pushw %cs
    popw %ds
    lcall *child_address
    .if RESOURCES == 4
    cmpl $0x24681357, %eax
    jne setup_fail
    movl $0x99aabbcc, %eax
    lcall *0x81
    cmpl $0x1234abcd, %eax
    jne setup_fail
    movl $0x24681357, %eax
    .endif
    lret
child_record: .zero 50
child_real_record:
    .zero 32
    .word 0x202, 0, 0, 0, 0, child_real_procedure, 0, 0x9000, 0
child_data: .word 0
child_real: .word 0
child_address: .word 0, 0
    .elseif RESOURCES == 3
    .code16
child_real_procedure:
    movw $32, %cx
child_canary_push:
    pushw $0x39a7
    loop child_canary_push
    addw $64, %sp
    movl $0x24681357, %eax
    lret
child_real_record:
    .zero 32
    .word 0x202, 0, 0, 0, 0, child_real_procedure, 0, 0, 0
child_real: .word 0
    .elseif RESOURCES == 2
child_exception:
    pushal
    incb child_exceptions
    popal
    lret
child_exceptions: .byte 0
    .endif
    .code16
    .if CHILD32
child_pm_entry: .long child_protected
    .word 0
child_base: .long 0
    .endif
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
