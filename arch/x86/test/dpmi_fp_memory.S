/* Native x87 state must round-trip through host-mediated VGA data faults. */
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
    movw $1, %ax
    lcall *entry
    jc setup_fail
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
setup_fail:
    movw $0x4c70, %ax
    int $0x21
entry: .word 0, 0
pm_entry: .word protected_entry, 0

    .code32
protected_entry:
    movw %ss, %bx
    movw $(0xf2 | STACK32 * 0x4000), %cx
    movw $9, %ax
    int $0x31
    jc fail
    movw %bx, %ss
    movl $(0x8000 + !STACK32 * 0xbeef0000), %esp
    movw %cs, %cx
    movl $fault_handler, %edx
    movw $14, %bx
    movw $0x0203, %ax
    int $0x31
    jc fail
    movw $1, %cx
    xorw %ax, %ax
    int $0x31
    jc fail
    movw %ax, %bx
    xorw %cx, %cx
    xorw %dx, %dx
    movw $7, %ax
    int $0x31
    jc fail
    movw $15, %cx
    movw $0xffff, %dx
    movw $8, %ax
    int $0x31
    jc fail
    movw $0x40f2, %cx
    movw $9, %ax
    int $0x31
    jc fail
    movw %bx, %es
    movw $0x0013, %ax
    int $0x10

    fninit
    movl $7, %es:0xa0000
    fld1
    movb $1, scenario
    fildl %es:0xa0000
    faddp
    fistpl result
    cmpl $8, result
    jne fail
    movb $2, scenario
    fldpi
    fstpt %es:0xa0004
    fldt %es:0xa0004
    fstpt result
    cmpl $0x2168c235, result
    jne fail
    cmpl $0xc90fdaa2, result + 4
    jne fail
    cmpw $0x4000, result + 8
    jne fail
    movb $3, scenario
    fld1
    faddl %es:0xa0010       /* zero-filled VRAM */
    fstpl %es:0xa0010
    fldl %es:0xa0010
    fistpl result
    cmpl $1, result
    jne fail
    movb $4, scenario
    fld1
    fnsave %es:0xa0020
    fnstsw %ax
    testw %ax, %ax
    jnz fail
    frstor %es:0xa0020
    fistpl result
    cmpl $1, result
    jne fail
    movb $5, scenario
    fldcw truncate_control
    fldpi
    fistps %es:0xa0090
    cmpw $3, %es:0xa0090
    jne fail
    fnstcw result
    cmpw $0x0f7f, result
    jne fail
    movb $6, scenario
    fninit
    .ifndef INTERPRETED
    movdqu pattern, %xmm0
    movdqu pattern + 4, %xmm7
    .endif
    movl $64, %ecx
1:  fld1
    fstpl %es:0xa00a0
    fldl %es:0xa00a0
    fistpl result
    cmpl $1, result
    jne fail
    loop 1b
    .ifndef INTERPRETED
    movdqu %xmm0, vectors
    movdqu %xmm7, vectors + 16
    .irp n,0,4,8,12
    movl pattern + \n, %eax
    cmpl %eax, vectors + \n
    jne fail
    movl pattern + 4 + \n, %eax
    cmpl %eax, vectors + 16 + \n
    jne fail
    .endr
    .endif
    movl $pass_msg, %edx
    movb $0x2a, exit_code
    jmp report
fail:
    movl $fail_msg, %edx
report:
    fninit
    pushl %edx
    movw $3, %ax
    int $0x10
    popl %edx
    movw $0x0900, %ax
    int $0x21
    movb scenario, %dl
    addb $'0', %dl
    movb $2, %ah
    int $0x21
    movb exit_code, %al
    movb $0x4c, %ah
    int $0x21
fault_handler:
    pushal
    movl %esp, %ebp
    movl $fail, %ss:44(%ebp)
    popal
    lret

    .balign 16
result: .fill 16, 1, 0
vectors: .fill 32, 1, 0
pattern: .long 0x12345678, 0x9abcdef0, 0x10203040, 0x50607080, 0x91929394
truncate_control: .word 0x0f7f
scenario: .byte 0
exit_code: .byte 0x71
pass_msg: .ascii "DPMI FP MEMORY PASS case=$"
fail_msg: .ascii "DPMI FP MEMORY FAIL case=$"
