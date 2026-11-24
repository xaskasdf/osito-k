/* Raw EXEC must retire an exited child's abandoned callback, not its owner. */
    .ifndef LOADONLY
    .set LOADONLY, 0
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
protected_entry:
    movw $0x0900, %ax
    int $0x31
    movw %cs, code_sel
    movw %ss, stack_sel
    movw %ss, %bx
    movw $(0xf2 | (STACK32 * 0x4000)), %cx
    movw $9, %ax
    int $0x31
    jc fail
    movw %bx, %ss
    movl $0x8000, %esp
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
    .if CHILD
    movw real_seg, %ax
    movw %ax, %cx
    movw %ax, %dx
    movw %ax, %si
    movl $0xa000, %ebx
    movl $child_real, %edi
    ljmp *pm_switch
child_resumed:
    movw $0x4c2a, %ax
    int $0x21
    .else
    movl rm_switch, %eax
    movl %eax, tail + 1
    movw code_sel, %ax
    movw %ax, tail + 5
    movw data_sel, %ax
    movw %ax, tail + 7
    movw stack_sel, %ax
    movw %ax, tail + 9
    movl $raw_owner, tail + 11
    movw %cs, %cx
    movl $outer_exception, %edx
    movw $3, %bx
    movw $0x0212, %ax
    int $0x31
    jc fail
    call run_pair
    int3
    cmpb $4, visits
    jne fail
    movl $pass_msg, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c2a, %ax
    int $0x21
run_pair:
    movb $0, abandon
    call run_child
    movb $1, abandon
    call run_child
    ret
run_child:
    movl %esp, protected_sp
    movw %ss, protected_ss
    movw data_sel, %ax
    movw %ax, %es
    movw code_sel, %ax
    movw %ax, %ds
    movl $worker_callback, %esi
    movl $callback_record, %edi
    movw $0x0303, %ax
    int $0x31
    jc fail
    movw %cx, %es:callback + 2
    movw %dx, %es:callback
    movw %es, %ax
    movw %ax, %ds
    movw real_seg, %ax
    movw %ax, %cx
    movw %ax, %dx
    movw %ax, %si
    movl $0x9000, %ebx
    movl $real_exec, %edi
    ljmp *pm_switch
parent_resumed:
    cmpw $42, child_exit
    jne fail
    movw callback + 2, %cx
    movw callback, %dx
    movw $0x0304, %ax
    int $0x31
    jc busy_fail
    ret
outer_exception:
    pushal
    call run_pair
    popal
    .if PM32
    addl $32, %esp
    .else
    addw $32, %sp
    .endif
    lret
raw_owner:
    movl %ebp, escape_return
    movw real_seg, %ax
    movw %ax, %cx
    movw %ax, %dx
    movw %ax, %si
    movl $0xb000, %ebx
    movl $real_callback, %edi
    ljmp *pm_switch
worker_callback:
    /* IRET consumes the real FAR CALL, while raw escape abandons it. */
    movw (%esi), %ax
    cmpw $real_callback_done, %ax
    jne frame_fail
    movw %ax, %es:42(%edi)
    movw 2(%esi), %ax
    cmpw %cs:real_seg, %ax
    jne frame_fail
    movw %ax, %es:44(%edi)
    addw $4, %es:46(%edi)
    incb %es:visits
    cmpb $0, %es:abandon
    jne callback_escape
    iret
callback_escape:
    movw %cs:data_sel, %ax
    movw %ax, %ds
    movw escape_return + 2, %ax
    movw %ax, %cx
    movw %ax, %dx
    movw %ax, %si
    movzwl escape_return, %edi
    movl $0xa000, %ebx
    ljmp *pm_switch
busy_fail:
    movl $busy_msg, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c72, %ax
    int $0x21
frame_fail:
    movw %cs:data_sel, %ax
    movw %ax, %ds
    movl $frame_msg, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c74, %ax
    int $0x21
    .endif
fail:
    movw %cs:data_sel, %ax
    movw %ax, %ds
    movl $fail_msg, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c71, %ax
    int $0x21

    .code16
    .if CHILD
child_real:
    movl $child_return, %ebp
    movzwl real_seg, %ebx
    shll $16, %ebx
    orl %ebx, %ebp
    movw 0x85, %si
    movw 0x87, %ax
    movw %ax, %cx
    movw 0x89, %dx
    movl $0x6000, %ebx
    movl 0x8b, %edi
    ljmp *0x81
child_return:
    pushw %cs
    popw %ds
    movw data_sel, %ax
    movw %ax, %cx
    movw stack_sel, %dx
    movl $0x8000, %ebx
    movw code_sel, %si
    movl $child_resumed, %edi
    ljmp *rm_switch
    .else
real_callback:
    lcall *%cs:callback
real_callback_done:
    ljmp *%cs:escape_return
real_exec:
    pushw %cs
    popw %ds
    pushw %cs
    popw %es
    movw %cs, parameters + 4
    movw %cs, parameters + 8
    movw %cs, parameters + 12
    movw $child_name, %dx
    movw $parameters, %bx
    .if LOADONLY
    movb $0, loaded
    movw $0x4b01, %ax
    int $0x21
    jc real_fail
    cmpb $0, loaded
    jne loaded_done
    movb $1, loaded
    movw parameters + 14, %di
    movw parameters + 16, %bp
    cli
    movw %bp, %ss
    movw %di, %sp
    ljmp *parameters + 18
loaded_done:
    .else
    movw $0x4b00, %ax
    int $0x21
    jc real_fail
    .endif
    movw $0x4d00, %ax
    int $0x21
    movw %ax, child_exit
    movw data_sel, %ax
    movw %ax, %cx
    movw protected_ss, %dx
    movl protected_sp, %ebx
    movw code_sel, %si
    movl $parent_resumed, %edi
    ljmp *rm_switch
real_fail:
    movw $0x4c73, %ax
    int $0x21
    .endif
    .balign 4
entry: .word 0, 0
pm_entry: .word protected_entry, 0
rm_switch: .word 0, 0
pm_switch: .long 0
    .word 0
real_seg: .word 0
code_sel: .word 0
data_sel: .word 0
stack_sel: .word 0
    .if !CHILD
protected_sp: .long 0
protected_ss: .word 0
escape_return: .long 0
callback: .long 0
callback_record: .zero 50
child_exit: .word 0
visits: .byte 0
abandon: .byte 0
tail:
    .byte 15
    .zero 15
    .byte 13
parameters:
    .word 0, tail, 0, 0xffff, 0, 0xffff, 0
    .if LOADONLY
    .zero 8
loaded: .byte 0
    .endif
child_name:
    .if CHILD32
    .asciz "dxuc32.com"
    .else
    .asciz "dxuc16.com"
    .endif
    .endif
pass_msg: .asciz "DPMI EXEC UNWIND PASS\r\n$"
fail_msg: .asciz "DPMI EXEC UNWIND FAIL\r\n$"
busy_msg: .asciz "DPMI EXEC UNWIND CALLBACK BUSY\r\n$"
frame_msg: .asciz "DPMI EXEC UNWIND INVALID REAL FRAME\r\n$"
