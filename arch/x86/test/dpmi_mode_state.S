/* RM INT 1Ch -> PM handler -> nested raw round trip -> RM INT 1Ch.
 * Both 0305h entry points must restore the dormant stack, not the CPU. */
    .section .text
    .code16
    .global _start
_start:
    cld
    pushw %cs
    popw %ds
    movw %cs, real_seg
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
    jmp test_entry
    .endif
setup_fail:
    movw $0x4c70, %ax
    int $0x21
dpmi_entry: .word 0, 0
pm_entry: .word test_entry, 0
base_hi: .word 0
base_lo: .word 0

    .if PM32
    .code32
    .endif
test_entry:
    movw %ss, main_ss
    movl %esp, main_sp
    movw %ds, data_sel
    movw %cs, code_sel
    movw %ds, %ax
    movw %ax, %es
    movw $0x0900, %ax
    int $0x31
    movw $0x0305, %ax
    int $0x31
    jc fail
    testw %ax, %ax
    jz fail
    cmpw $128, %ax
    ja fail
    movw %cx, rm_state
    movw %bx, rm_state + 2
    .if PM32
    movl %edi, pm_state
    movw %si, pm_state + 4
    .else
    movw %di, pm_state
    movw %si, pm_state + 2
    .endif
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
    /* Exercise the gates from the native caller too, outside reflection. */
    movl $saved_main, %edi
    movb $0, %al
    std
    stc
    lcall *pm_state
    pushfl
    popl %edx
    cld
    andl $0x401, %edx
    cmpl $0x401, %edx
    jne fail
    movw real_seg, %ax
    movw %ax, %cx
    movw %ax, %dx
    movw %ax, %si
    movl $0x6000, %ebx
    movl $rm_top, %edi
    ljmp *pm_switch
pm_top_return:
    movl $saved_main, %edi
    movb $1, %al
    lcall *pm_state
    movw $0x0902, %ax
    int $0x31
    testb %al, %al
    jnz fail
    movw $0x0204, %ax
    movb $0x1c, %bl
    int $0x31
    jc fail
    .if PM32
    movl %edx, old_timer
    movw %cx, old_timer + 4
    .else
    movw %dx, old_timer
    movw %cx, old_timer + 2
    .endif
    movw real_seg, %cx
    movw $rm_timer, %dx
    movw $0x0201, %ax
    int $0x31
    jc fail
    movw %cs, %cx
    movl $pm_timer, %edx
    movw $0x0205, %ax
    int $0x31
    jc fail
    movw real_seg, %ax
    movw %ax, outer_call + 36
    movw %ax, outer_call + 44
    movw %ax, leaf_call + 36
    movw %ax, leaf_call + 44
    movl $outer_call, %edi
    xorw %bx, %bx
    xorw %cx, %cx
    movw $0x0301, %ax
    int $0x31
    jc fail
    cmpb $3, timer_count
    jne fail
    cmpb $1, real_count
    jne fail
    cmpb $1, leaf_count
    jne fail
    cmpb $1, outer_done
    jne fail
    cmpb $0, error
    jne fail
    /* 0300h must reflect even when a real IVT handler is installed. */
    movb $1, simple_timer
    movl $outer_call, %edi
    movw $0x1c, %bx
    xorw %cx, %cx
    movw $0x0300, %ax
    int $0x31
    jc fail
    cmpb $4, timer_count
    jne fail
    cmpb $1, real_count
    jne fail
    movw $0x4c2a, %ax
    int $0x21
fail:
    movw $0x4c81, %ax
    int $0x21

pm_timer:
    pushal
    push %ds
    push %es
    movw %cs:data_sel, %ax
    movw %ax, %ds
    movw %ax, %es
    incb timer_count
    cmpb $0, simple_timer
    jne timer_return
    cmpb $1, timer_count
    jne nested_timer
    movw %ss, %ax
    cmpw main_ss, %ax
    je timer_fail
    movw %ax, handler_ss
    pushl $0x13579bdf
    movl %esp, handler_sp

    movl $saved_rm, %edi
    movl $0x12345600, %eax
    std
    stc
    lcall *pm_state
    pushfl
    popl %edx
    cld
    cmpl $0x12345600, %eax
    jne timer_fail
    andl $0x401, %edx
    cmpl $0x401, %edx
    jne timer_fail

    movw real_seg, %ax
    movw %ax, %cx
    movw %ax, %dx
    movw %ax, %si
    movl $0x7000, %ebx
    movl $rm_detour, %edi
    ljmp *pm_switch
pm_back:
    movl $saved_rm, %edi
    movb $1, %al
    lcall *pm_state
    movl $leaf_call, %edi
    xorw %bx, %bx
    xorw %cx, %cx
    movw $0x0301, %ax
    int $0x31
    jc timer_fail
    /* Chaining must reach the real IVT once, without reflecting back here. */
    pushf
    lcall *old_timer
    popl %eax
    cmpl $0x13579bdf, %eax
    jne timer_fail
timer_return:
    pop %es
    pop %ds
    popal
    iret
nested_timer:
    cmpb $3, timer_count
    ja timer_fail
    movw %ss, %ax
    cmpw handler_ss, %ax
    jne timer_fail
    cmpl handler_sp, %esp
    jae timer_fail
    jmp timer_return
timer_fail:
    movb $1, error
    movw $0x4c82, %ax
    int $0x21

pm_detour:
    movw real_seg, %ax
    movw %ax, %cx
    movw %ax, %dx
    movw %ax, %si
    movl $0x7000, %ebx
    movl $rm_after, %edi
    ljmp *pm_switch

    .code16
rm_top:
    movw data_sel, %ax
    movw %ax, %cx
    movw main_ss, %dx
    movl main_sp, %ebx
    movw code_sel, %si
    movl $pm_top_return, %edi
    ljmp *rm_switch
rm_outer:
    movw %ss, outer_ss
    movw %sp, outer_sp
    pushl $0x2468ace0
    int $0x1c
    popl %eax
    cmpl $0x2468ace0, %eax
    jne rm_fail
    movb $1, outer_done
    lret
rm_leaf:
    movw %ss, %ax
    cmpw outer_ss, %ax
    jne rm_fail
    cmpw outer_sp, %sp
    jae rm_fail
    incb leaf_count
    lret
rm_timer:
    pushw %ds
    pushw %cs
    popw %ds
    incb real_count
    int $0x1c
    popw %ds
    iret
rm_detour:
    movw $saved_pm, %di
    movb $0, %al
    lcall *rm_state
    movw data_sel, %ax
    movw %ax, %cx
    movw main_ss, %dx
    movl $0x8000, %ebx
    movw code_sel, %si
    movl $pm_detour, %edi
    ljmp *rm_switch
rm_after:
    movw $saved_pm, %di
    movb $1, %al
    lcall *rm_state
    int $0x1c
    movw data_sel, %ax
    movw %ax, %cx
    movw handler_ss, %dx
    movl handler_sp, %ebx
    movw code_sel, %si
    movl $pm_back, %edi
    ljmp *rm_switch
rm_fail:
    movb $1, error
    movw $0x4c83, %ax
    int $0x21

real_seg: .word 0
main_ss: .word 0
main_sp: .long 0
data_sel: .word 0
code_sel: .word 0
handler_ss: .word 0
handler_sp: .long 0
outer_ss: .word 0
outer_sp: .word 0
rm_state: .space 4
rm_switch: .space 4
pm_state: .space 6
pm_switch: .space 6
old_timer: .space 6
timer_count: .byte 0
real_count: .byte 0
leaf_count: .byte 0
outer_done: .byte 0
error: .byte 0
simple_timer: .byte 0
    .balign 4
saved_main: .space 128
saved_rm: .space 128
saved_pm: .space 128
outer_call:
    .space 42
    .word rm_outer, 0, 0, 0
leaf_call:
    .space 42
    .word rm_leaf, 0, 0, 0
