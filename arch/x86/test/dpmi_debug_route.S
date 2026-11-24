/* INT n uses the software vector, unlike INT3/INTO processor exceptions. */
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
    .macro install service, vector, handler
    movw %cs, %cx
    movl $\handler, %edx
    movw $\vector, %bx
    movw $\service, %ax
    int $0x31
    jc fail
    .endm
    .macro invoke vector, number, prefix=0
    movl $1f, expected_ip
    movl $0x12345678, %ebx
    stc
    std
    .if \prefix
    .byte 0x2e, 0x66
    .endif
    .byte 0xcd, \vector
1:
    pushfl
    popl %eax
    cld
    andw $0x401, %ax
    cmpw $0x401, %ax
    jne fail
    cmpl $0x12345678, %ebx
    jne fail
    cmpl saved_sp, %esp
    jne fail
    cmpb $0, debug_calls
    jne wrong_debug
    cmpb $\vector, last_software
    jne fail
    cmpb $\number, software_calls
    jne fail
    cmpb $0, handler_error
    jne fail
    .endm
protected_entry:
    movw %ss, %bx
    movw $(0xf2 | (STACK32 * 0x4000)), %cx
    movw $9, %ax
    int $0x31
    jc fail
    movw %bx, %ss
    movl $(0x8000 + (PM32 * !STACK32 * 0xbeef0000)), %esp
    movl %esp, saved_sp
    install 0x0203, 1, debug_handler
    install 0x0203, 3, trap_handler
    install 0x0203, 4, trap_handler
    install 0x0205, 1, software_one
    install 0x0205, 3, software_three
    install 0x0205, 4, software_four
    invoke 1, 1
    invoke 1, 2, 1
    invoke 3, 3
    invoke 4, 4

    movl $after_int3, expected_ip
    stc
    std
    int3
after_int3:
    cld
    cmpb $1, trap_calls
    jne fail
    movl $after_into, expected_ip
    movb $0x7f, %al
    addb $1, %al
    stc
    std
    .byte 0xce
after_into:
    cld
    cmpb $2, trap_calls
    jne fail
    cmpb $4, software_calls
    jne fail
    cmpb $0, handler_error
    jne fail
    cmpl saved_sp, %esp
    jne fail
    movl $pass, %edx
    movw $0x0900, %ax
    int $0x21
    cmpw $0x0924, %ax
    jne output_fail
    movw $0x4c2a, %ax
    int $0x21
wrong_debug:
    movl $misrouted, %edx
    jmp report_failure
fail:
    movl $failure, %edx
report_failure:
    cld
    movw $0x0900, %ax
    int $0x21
    cmpw $0x0924, %ax
    jne output_fail
    movw $0x4c71, %ax
    int $0x21
output_fail:
    movb $0x4c, %ah
    int $0x21

software_one:
    movb $1, last_software
    jmp software_handler
software_three:
    movb $3, last_software
    jmp software_handler
software_four:
    movb $4, last_software
software_handler:
    pushl %ebp
    movl %esp, %ebp
    .if !STACK32
    andl $0xffff, %ebp
    .endif
    pushl %eax
    .if PM32
    movl %ss:4(%ebp), %eax
    .else
    movzwl %ss:4(%ebp), %eax
    .endif
    cmpl expected_ip, %eax
    jne software_bad
    incb software_calls
    jmp software_return
software_bad:
    movb $1, handler_error
software_return:
    popl %eax
    popl %ebp
    .if PM32
    iretl
    .else
    iretw
    .endif

debug_handler:
    incb debug_calls
    lret

trap_handler:
    .if PM32
    pushl %ebp
    movl %esp, %ebp
    pushl %eax
    movl %ss:16(%ebp), %eax
    .else
    pushw %bp
    movw %sp, %bp
    pushl %eax
    movzwl %ss:8(%bp), %eax
    .endif
    cmpl expected_ip, %eax
    jne trap_bad
    incb trap_calls
    jmp trap_return
trap_bad:
    movb $2, handler_error
trap_return:
    popl %eax
    .if PM32
    popl %ebp
    .else
    popw %bp
    .endif
    lret

saved_sp: .long 0
expected_ip: .long 0
software_calls: .byte 0
last_software: .byte 0
debug_calls: .byte 0
trap_calls: .byte 0
handler_error: .byte 0
pass: .ascii "DPMI DEBUG ROUTING PASS\r\n$"
failure: .ascii "DPMI DEBUG ROUTING FAIL\r\n$"
misrouted: .ascii "DPMI INT1 MISROUTED TO DEBUG\r\n$"
