/* Native DOS gate admission: loaded selectors, handler edits and client DF. */
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
    movw $1, %ax
    lcall *dpmi_entry
    jc setup_fail
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
setup_fail:
    movw $0x4c70, %ax
    int $0x21
dpmi_entry: .word 0, 0
pm_pointer: .word test_entry, 0
base_hi: .word 0
base_lo: .word 0

    .code32
test_entry:
    movw $0x0900, %ax
    int $0x31
    movw %ds, safe_data
    movw %cs, selectors + 10
    xorl %esi, %esi
allocate:
    movw safe_data, %bx
    movw $0x000a, %ax
    int $0x31
    jc fail
    movw %ax, selectors(,%esi,2)
    incl %esi
    cmpl $5, %esi
    jb allocate
    movw selectors + 8, %bx
    movw $(0xf2 | STACK32 * 0x4000), %cx
    movw $9, %ax
    int $0x31
    jc fail
    movw %bx, %ss
    .if !STACK32
    orl $0xbeef0000, %esp
    .endif
    movl %esp, saved_sp
    movw %ss, saved_ss
    movw %cs, %bx
    movw $0x000a, %ax
    int $0x31
    jc fail
    movw %ax, handler_code
    movw %ax, %bx
    movw $0x40fa, %cx
    movw $9, %ax
    int $0x31
    jc fail
    movw handler_code, %cx
    movl $handler, %edx
    movw $11, %bx
    movw $0x0203, %ax
    int $0x31
    jc fail
    movw $12, %bx
    movw $0x0203, %ax
    int $0x31
    jc fail
    movw $13, %bx
    movw $0x0203, %ax
    int $0x31
    jc fail
    movw selectors, %ax
    movw %ax, %es
    movw selectors + 4, %ax
    movw %ax, %fs
    movw selectors + 6, %ax
    movw %ax, %gs
    movw selectors + 2, %ax
    movw %ax, %ds
next:
    movb $0, faults
    movl target, %esi
    movw selectors(,%esi,2), %bx
    movw %bx, bad_sel
    movw $0xf2, %cx
    cmpl $4, %esi
    jne 1f
    movw $(0xf2 | STACK32 * 0x4000), %cx
1:  cmpl $5, %esi
    jne 2f
    movw $0x40fa, %cx
2:  movw %cx, good_rights
    andw $0xff7f, %cx
    cmpb $1, policy
    jne invoke
    andw $0xff00, %cx
    orw $0x78, %cx
    cmpl $5, %esi
    jne invoke
    xorw $0xa, %cx
invoke:
    movl $0x12345678, %edx
    movl $0xaabbccdd, %edi
    movl $0xcafebabe, %ebp
    movl $0xbeef0009, %eax
    std
    stc
    int $0x31
after_call:
    jc fail
    cmpb $2, policy
    je fail
    jmp check
redirected:
    jc fail
    cmpb $2, policy
    jne fail
check:
    cmpl $0xbeef0009, %eax
    jne fail
    cmpl $0x12345678, %edx
    jne fail
    cmpl $0xaabbccdd, %edi
    jne fail
    cmpl $0xcafebabe, %ebp
    jne fail
    cmpl saved_sp, %esp
    jne fail
    cmpb $1, faults
    jne fail
    pushfl
    popl %eax
    testl $0x400, %eax
    jz fail
    cld
    incb policy
    cmpb $3, policy
    jb next
    movb $0, policy
    incl target
    cmpl $6, target
    jb next
    movl $pass_text, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c2a, %ax
    int $0x21
fail:
    cld
    movw $0x4c71, %ax
    int $0x21

handler:
    pushal
    pushl %ds
    movl %esp, %ebp
    movw %cs:safe_data, %ax
    movw %ax, %ds
    incb faults
    cmpl $after_call, %ss:48(%ebp)
    jne fail
    movw bad_sel, %ax
    andw $0xfffc, %ax
    cmpw %ax, %ss:44(%ebp)
    jne fail
    movw selectors + 10, %ax
    cmpw %ax, %ss:52(%ebp)
    jne fail
    movl saved_sp, %eax
    cmpl %eax, %ss:60(%ebp)
    jne fail
    movw saved_ss, %ax
    cmpw %ax, %ss:64(%ebp)
    jne fail
    testl $0x400, %ss:56(%ebp)
    jz fail
    testl $1, %ss:56(%ebp)
    jnz fail
    cmpb $2, policy
    jne repair
    movl $redirected, %ss:48(%ebp)
repair:
    movw bad_sel, %bx
    movw good_rights, %cx
    movw $9, %ax
    int $0x31
    jc fail
    popl %ds
    popal
    lret

    .balign 4
target: .long 0
saved_sp: .long 0
selectors: .space 12
safe_data: .word 0
handler_code: .word 0
saved_ss: .word 0
bad_sel: .word 0
good_rights: .word 0
policy: .byte 0
faults: .byte 0
pass_text: .ascii "DPMI NATIVE SEGMENTS PASS\r\n$"
