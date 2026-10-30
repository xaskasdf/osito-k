/* Recoverable 0306h FAR JMP admission, without a callback or a return frame. */
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
    .macro rights value
    movw $\value, %cx
    movw $9, %ax
    int $0x31
    jc fail
    .endm
    .macro alias source, destination
    movw \source, %bx
    movw $0x000a, %ax
    int $0x31
    jc fail
    movw %ax, \destination
    .endm
protected_entry:
    movw $0x0900, %ax
    int $0x31
    movw %cs, code_sel
    .if PM32
    movw %cs, canonical + 4
    .else
    movw %cs, canonical + 2
    .endif
    movw %ss, main_ss
    movw %ss, %bx
    rights (0xf2 | (STACK32 * 0x4000))
    movw %bx, %ss
    movl $(0x8000 + (PM32 * !STACK32 * 0xbeef0000)), %esp
    movl %esp, main_sp
    alias data_sel, raw_ds
    alias data_sel, raw_es
    alias main_ss, raw_ss
    alias code_sel, raw_cs
    movw raw_cs, %bx
    rights (0xfa | (PM32 * 0x4000))
    movw %cs, %cx
    movl $repair_handler, %edx
    movw $11, %bx
    movw $0x0213, %ax
    int $0x31
    jc fail
    movw $12, %bx
    movw $0x0213, %ax
    int $0x31
    jc fail
    movw $3, %bx
    movl $nested_handler, %edx
    movw $0x0212, %ax
    int $0x31
    jc fail
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
next_case:
    movb $0, faults
    movb $0, nested
    movb $0, cancelled
    cmpb $0, scenario
    je invoke
    movw raw_cs, %bx
    cmpb $4, scenario
    jne 1f
    rights (0x7a | (PM32 * 0x4000))
    jmp invoke
1:  movw raw_ss, %bx
    cmpb $3, scenario
    jne 2f
    rights (0x72 | (STACK32 * 0x4000))
    jmp invoke
2:  movw raw_es, %bx
    cmpb $2, scenario
    je 3f
    movw raw_ds, %bx
3:  rights (0x72 | (PM32 * 0x4000))
invoke:
    movl $0x12345678, %ebp
    movw real_seg, %ax
    movw %ax, %cx
    movw %ax, %dx
    movw %ax, %si
    movl $0x6000, %ebx
    movl $real_top, %edi
    ljmp *pm_switch
pm_return:
    cmpl $0x12345678, %ebp
    jne fail
    cmpl main_sp, %esp
    jne fail
    movw %ss, %ax
    cmpw raw_ss, %ax
    jne fail
    movw %cs, %ax
    cmpw raw_cs, %ax
    jne fail
    movw %ds, %ax
    cmpw raw_ds, %ax
    jne fail
    movw %es, %ax
    cmpw raw_es, %ax
    jne fail
    movw %fs, %ax
    movw %gs, %dx
    orw %dx, %ax
    jnz fail
    movw $0x0902, %ax
    int $0x31
    testb %al, %al
    jnz fail
    ljmp *canonical
check_return:
    movw data_sel, %ax
    movw %ax, %ds
    movw %ax, %es
    movw main_ss, %ax
    movw %ax, %ss
    movb $1, %dl
    cmpb $0, scenario
    jne 1f
    xorb %dl, %dl
1:  cmpb $5, scenario
    jne 2f
    incb %dl
2:  cmpb %dl, faults
    jne fail
    cmpb $6, scenario
    sete %dl
    cmpb %dl, nested
    jne fail
    cmpb $7, scenario
    sete %dl
    cmpb %dl, cancelled
    jne fail
    incb scenario
    cmpb $8, scenario
    jb next_case
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
    movb scenario, %dl
    addb $'0', %dl
    movb $2, %ah
    int $0x21
    movw $0x4c71, %ax
    int $0x21

repair_handler:
    pushal
    movl %esp, %ebp
    .if !PM32
    andl $0xffff, %ebp
    .endif
    addl $32, %ebp
    movw %cs:data_sel, %ax
    movw %ax, %ds
    incb faults
    cmpb $3, faults
    jae fail
    cmpl $0x30002, %ss:0x34(%ebp) /* Real-mode fault: VM and RF. */
    jne fail
    cmpw $1, %ss:0x32(%ebp)
    jne fail
    movw rm_switch + 2, %ax
    cmpw %ax, %ss:0x30(%ebp)
    jne fail
    movzwl rm_switch, %eax
    addl $2, %eax
    cmpl %eax, %ss:0x2c(%ebp)
    jne fail
    cmpl $0x6000, %ss:0x38(%ebp)
    jne fail
    movw real_seg, %ax
    cmpw %ax, %ss:0x3c(%ebp)
    jne fail
    cmpb $5, scenario
    jne 1f
    cmpb $1, faults
    je handler_return
1:  cmpb $6, scenario
    jne 2f
    int3
2:  movw raw_ds, %bx
    rights (0xf2 | (PM32 * 0x4000))
    movw raw_es, %bx
    rights (0xf2 | (PM32 * 0x4000))
    movw raw_ss, %bx
    rights (0xf2 | (STACK32 * 0x4000))
    movw raw_cs, %bx
    rights (0xfa | (PM32 * 0x4000))
    cmpb $7, scenario
    jne handler_return
    orw $4, %ss:0x32(%ebp)
    movw real_seg, %ax
    movw %ax, %ss:0x30(%ebp)
    movl $real_cancelled, %ss:0x2c(%ebp)
handler_return:
    popal
    addl $32, %esp
    lret
nested_handler:
    incb nested
    addl $32, %esp
    lret

    .code16
real_cancelled:
    cmpw $0x6000, %sp
    jne setup_fail
    incb %cs:cancelled
real_top:
    pushw $2
    popfw
    movw %cs:raw_ds, %ax
    movw %cs:raw_es, %cx
    movw %cs:raw_ss, %dx
    movl %cs:main_sp, %ebx
    movw %cs:raw_cs, %si
    movl $pm_return, %edi
    ljmp *%cs:rm_switch
    .align 4
entry: .word 0, 0
pm_entry: .word protected_entry, 0
canonical:
    .if PM32
    .long check_return
    .else
    .word check_return
    .endif
    .word 0
pm_switch: .long 0
    .word 0
rm_switch: .word 0, 0
real_seg: .word 0
data_sel: .word 0
code_sel: .word 0
main_ss: .word 0
main_sp: .long 0
raw_ds: .word 0
raw_es: .word 0
raw_ss: .word 0
raw_cs: .word 0
scenario: .byte 0
faults: .byte 0
nested: .byte 0
cancelled: .byte 0
pass_msg: .asciz "DPMI RAW ENTRY PASS\r\n$"
fail_msg: .asciz "DPMI RAW ENTRY FAIL case=$"
