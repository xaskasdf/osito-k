/* Instruction-length faults must reach the client before virtual device I/O. */
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
    movw $PM32, %ax
    lcall *entry
    jc setup_fail
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
entry: .word 0, 0
pm_entry: .word protected_entry, 0

    .if PM32
    .code32
    .endif
    .macro overlong opcode, immediate=0, operand=0
    movl $bad_start\@, expected_ip
    movl $bad_end\@, resume_ip
    incl expected_count
    movl $0x6b553737, %eax
    movl $0x12340083, %edx
bad_start\@:
    .rept 15 - \immediate
    .byte 0x2e
    .endr
    .byte \opcode
    .if \immediate
    .byte \operand
    .endif
bad_end\@:
    cmpl $0x6b553737, %eax
    jne fail
    cmpl $0x12340083, %edx
    jne fail
    cmpl saved_sp, %esp
    jne fail
    movl exception_count, %eax
    cmpl expected_count, %eax
    jne fail
    cmpb $0, handler_error
    jne fail
    inb $0x83, %al
    cmpb $0x25, %al
    jne fail
    .endm
protected_entry:
    movw %ss, %bx
    movw $(0xf2 | (STACK32 * 0x4000)), %cx
    movw $9, %ax
    int $0x31
    jc fail
    movw %bx, %ss
    movl $0x8000, %esp
    movl %esp, saved_sp
    movw %cs, %cx
    movl $gp_handler, %edx
    movw $13, %bx
    movw $0x0212, %ax
    int $0x31
    jc fail

    /* Valid fifteen-byte immediate and DX forms must still execute. */
    movb $0x25, %al
    .rept 13
    .byte 0x2e
    .endr
    outb %al, $0x83
    .rept 13
    .byte 0x2e
    .endr
    inb $0x83, %al
    cmpb $0x25, %al
    jne fail
    movw $0x83, %dx
    .rept 14
    .byte 0x2e
    .endr
    outb %al, %dx
    .rept 14
    .byte 0x2e
    .endr
    inb %dx, %al
    cmpb $0x25, %al
    jne fail
    cmpl $0, exception_count
    jne fail

    overlong 0xe4, 1, 0x83
    overlong 0xe5, 1, 0x83
    overlong 0xe6, 1, 0x83
    overlong 0xe7, 1, 0x83
    overlong 0xec
    overlong 0xed
    overlong 0xee
    overlong 0xef
    overlong 0xcd, 1, 0x60
    overlong 0xcc
    overlong 0xce
    movl $pass, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c2a, %ax
    int $0x21
fail:
    movl $failure, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c71, %ax
    int $0x21

gp_handler:
    pushl %ebp
    pushl %eax
    movl %esp, %ebp
    .if !STACK32
    andl $0xffff, %ebp
    .endif
    /* The extended frame stays DWORD-sized for USE16 and USE32 clients. */
    cmpl $0, %ss:0x30(%ebp)
    jne handler_bad
    movl expected_ip, %eax
    cmpl %eax, %ss:0x34(%ebp)
    jne handler_bad
    testl $0x10000, %ss:0x3c(%ebp)
    jz handler_bad
    incl exception_count
handler_return:
    movl resume_ip, %eax
    movl %eax, %ss:0x34(%ebp)
    popl %eax
    popl %ebp
    .if STACK32
    addl $0x20, %esp
    .else
    addw $0x20, %sp
    .endif
    lret
handler_bad:
    movb $1, handler_error
    jmp handler_return

    .balign 4
saved_sp: .long 0
expected_ip: .long 0
resume_ip: .long 0
expected_count: .long 0
exception_count: .long 0
handler_error: .byte 0
pass: .asciz "DPMI DECODE PASS\r\n$"
failure: .asciz "DPMI DECODE FAIL\r\n$"
