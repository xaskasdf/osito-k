/* Diagnose native-host differences from Intel's x87 edge contracts. This
 * bypasses DOS instruction decoding; a nonzero mask is not an OS diagnosis. */
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
    fninit
    .rept 9
    fld1
    .endr
    fnstsw %ax
    andw $0x0241, %ax
    cmpw $0x0241, %ax
    je 1f
    orb $1, failures
1:
    fninit
    fldcw invalid_control
    movl $0x11223344, result
    fistpl result
    fnstsw %ax
    andw $0x3881, %ax
    cmpw $0x0081, %ax
    jne 2f
    cmpl $0x11223344, result
    je 3f
2:  orb $2, failures
3:
    fninit
    fldcw invalid_control
    pushfl
    popl %eax
    andl $~0x0044, %eax
    orl $0x0891, %eax
    pushl %eax
    popfl
    fcomi %st(1), %st
    pushfl
    popl %edx
    fnstsw %ax
    andl $0x08d5, %edx
    cmpl $1, %edx
    jne 4f
    andw $0x0081, %ax
    cmpw $0x0081, %ax
    je 5f
4:  orb $4, failures
5:
    fninit
    fldcw overflow_control
    fldt huge
    movl $0x11223344, result
    fstps result
    fnstsw %ax
    andw $0x3888, %ax
    cmpw $0x3888, %ax
    jne 6f
    cmpl $0x11223344, result
    je 7f
6:  orb $8, failures
7:
    fninit
    cmpb $0, failures
    jne fail
    movl $passed, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c2a, %ax
    int $0x21
fail:
    movzbl failures, %eax
    movb digits(%eax), %al
    movb %al, mask_digit
    movl $failed, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c71, %ax
    int $0x21

invalid_control: .word 0x037e
overflow_control: .word 0x0377
huge: .quad 0x8000000000000000
    .word 0x4080
result: .long 0
failures: .byte 0
digits: .ascii "0123456789ABCDEF"
passed: .ascii "DOS X87 EDGE PASS\r\n$"
failed: .ascii "DOS X87 EDGE FAIL mask="
mask_digit: .ascii "0\r\n$"
