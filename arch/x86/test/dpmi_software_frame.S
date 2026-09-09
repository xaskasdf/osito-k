/* Checked software-entry faults, including already-completed native gates. */
    .section .text
    .code16
    .global _start
_start:
    cld
    pushw %cs
    popw %ds
    pushw %cs
    popw %es
    /* Leave DOS memory for the protected console translator's real buffer. */
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
    jmp test_entry
    .endif
setup_fail:
    movw $0x4c70, %ax
    int $0x21
dpmi_entry: .word 0, 0
pm_pointer: .word test_entry, 0
base_hi: .word 0
base_lo: .word 0

    .if PM32
    .code32
    .equ FIELD, 4
    .else
    .equ FIELD, 2
    .endif
test_entry:
    movw $0x0900, %ax
    int $0x31
    movw %ss, %bx
    .if STACK32
    movw $0x40f2, %cx
    .else
    movw $0x00f2, %cx
    .endif
    movw $9, %ax
    int $0x31
    jc fail
    movw %bx, %ss
    .if !STACK32
    orl $0xbeef0000, %esp
    .endif
    movl %esp, saved_sp
    movw %ss, saved_ss
    movw %cs, saved_cs
    movw %cs, %cx
    movl $stack_handler, %edx
    movw $12, %bx
    movw $0x0203, %ax
    int $0x31
    jc fail
    movl $software_handler, %edx
    movw $0x60, %bx
    movw $0x0205, %ax
    int $0x31
    jc fail
    movw $0x33, %bx
    movw $0x0205, %ax
    int $0x31
    jc fail

    .macro probe vector, prefixes, count
    .if NATIVE_GATE && (\vector == 0x33)
    movl $after_\@, expected_ip
    .else
    movl $invoke_\@, expected_ip
    .endif
    movl saved_sp, %eax
    andl $0xffff0000, %eax
    orl $1, %eax
    .if STACK32
    movl $1, %eax
    .endif
    movl %eax, bad_sp
    movl %eax, %esp
    movl $0xa1b2c3d4, %eax
    movl $0x11223344, %ebx
    movl $0x55667788, %ecx
    movl $0x99aabbcc, %edx
    movl $0x12345678, %esi
    movl $0x87654321, %edi
    movl $0xcafebabe, %ebp
invoke_\@:
    .rept \prefixes
    .byte 0x3e
    .endr
    int $\vector
after_\@:
    cmpb $\count, faults
    jne fail
    cmpb $\count, invokes
    jne fail
    cmpl saved_sp, %esp
    jne fail
    cmpl $0xa1b2c3d4, %eax
    jne fail
    cmpl $0x11223344, %ebx
    jne fail
    cmpl $0x55667788, %ecx
    jne fail
    cmpl $0x99aabbcc, %edx
    jne fail
    cmpl $0x12345678, %esi
    jne fail
    cmpl $0x87654321, %edi
    jne fail
    cmpl $0xcafebabe, %ebp
    jne fail
    .endm

    probe 0x60, 0, 1
    probe 0x60, 13, 2
    probe 0x33, 0, 3
    probe 0x33, 13, 4
    movl $pass, %edx
    movw $0x0900, %ax
    int $0x21
    cmpw $0x0924, %ax
    jne fail
    movw $0x4c2a, %ax
    int $0x21
fail:
    movw saved_ss, %ax
    testw %ax, %ax
    jz setup_fail
    movw %ax, %ss
    movl saved_sp, %esp
    movw $0x4c80, %ax
    int $0x21

stack_handler:
    cmpl $0xa1b2c3d4, %eax
    jne fail
    cmpl $0x11223344, %ebx
    jne fail
    cmpl $0x55667788, %ecx
    jne fail
    cmpl $0x99aabbcc, %edx
    jne fail
    cmpl $0x12345678, %esi
    jne fail
    cmpl $0x87654321, %edi
    jne fail
    cmpl $0xcafebabe, %ebp
    jne fail
    pushl %ebp
    movl %esp, %ebp
    pushl %eax
    movw %ss, %ax
    cmpw saved_ss, %ax
    je fail
    .if PM32
    cmpl $0, %ss:4 + 2*FIELD(%ebp)
    jne fail
    movl %ss:4 + 3*FIELD(%ebp), %eax
    cmpl expected_ip, %eax
    jne fail
    movl %ss:4 + 6*FIELD(%ebp), %eax
    cmpl bad_sp, %eax
    jne fail
    movl saved_sp, %eax
    movl %eax, %ss:4 + 6*FIELD(%ebp)
    .else
    cmpw $0, %ss:4 + 2*FIELD(%ebp)
    jne fail
    movw %ss:4 + 3*FIELD(%ebp), %ax
    cmpw expected_ip, %ax
    jne fail
    movw %ss:4 + 6*FIELD(%ebp), %ax
    cmpw bad_sp, %ax
    jne fail
    movw saved_sp, %ax
    movw %ax, %ss:4 + 6*FIELD(%ebp)
    .endif
    movw %ss:4 + 4*FIELD(%ebp), %ax
    cmpw saved_cs, %ax
    jne fail
    movw %ss:4 + 7*FIELD(%ebp), %ax
    cmpw saved_ss, %ax
    jne fail
    incb faults
    popl %eax
    popl %ebp
    lret

software_handler:
    incb invokes
    iret

saved_sp: .long 0
bad_sp: .long 0
expected_ip: .long 0
saved_ss: .word 0
saved_cs: .word 0
faults: .byte 0
invokes: .byte 0
pass: .ascii "DPMI SOFTWARE FRAME CONTRACT PASS\r\n$"
