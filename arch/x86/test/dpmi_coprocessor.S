/* DPMI 0E00h/0E01h: policy, native #NM, ABI isolation, raw switch and EXEC. */
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
    movw %cs, exec_block + 4
    movw %cs, exec_block + 8
    movw %cs, exec_block + 12
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
    movl %esp, saved_sp
    movw %ss, saved_ss
    movw %cs, saved_cs
    movw %ds, saved_ds
.ifdef CHILD_ONLY
    movw $0x0e01, %ax
    xorw %bx, %bx
    int $0x31
    jc fail
    movw $0x0e00, %ax
    int $0x31
    cmpw $0x44, %ax
    jne fail
    fninit
    fldpi
    movw $0x0e01, %ax
    movw $2, %bx
    int $0x31
    jc fail
    movw $0x4c2b, %ax       /* Exit with physical EM set. */
    int $0x21
.endif
    movw $0x0e00, %ax
    int $0x31
    jc fail
    cmpw $0x45, %ax
    jne fail
    movw %cs, %cx
    movl $nm_handler, %edx
    movw $7, %bx
    movw $0x0212, %ax
    int $0x31
    jc fail
    movl $real_nm_handler, %edx
    movw $0x0213, %ax
    int $0x31
    jc fail
    movl $escape_handler, %edx
    movw $6, %bx
    movw $0x0212, %ax
    int $0x31
    jc fail
    movw $13, %bx
    movw $0x0212, %ax
    int $0x31
    jc fail
    movl $software_handler, %edx
    movw $0x2e, %bx
    movw $0x0205, %ax
    int $0x31
    jc fail
    movw $0x80, %bx
    movw $0x0205, %ax
    int $0x31
    jc fail
    movw $0x0306, %ax
    int $0x31
    jc fail
    movw %cx, rm_switch
    movw %bx, rm_switch + 2
    movl %edi, pm_switch
    movw %si, pm_switch + 4
    fninit
    fld1
matrix:
    movb $1, scenario
    movl $0xbadc0e01, %eax
    movl control, %ebx
    orl $0x1234fffc, %ebx
    movl $0x11223344, %ecx
    movl $0x55667788, %edx
    stc
    std
    int $0x31
    pushfl
    popl observed_flags
    cld
    cmpl $0xbadc0e01, %eax
    jne fail
    cmpl $0x11223344, %ecx
    jne fail
    cmpl $0x55667788, %edx
    jne fail
    andl $0x401, observed_flags
    cmpl $0x400, observed_flags
    jne fail
    movl control, %eax
    orl $0x1234fffc, %eax
    cmpl %eax, %ebx
    jne fail
    movl $0xbadc0e00, %eax
    int $0x31
    jc fail
    movl control, %ebx
    orl $0xbadc0044, %ebx
    cmpl %ebx, %eax
    jne fail
    smsw %ax
    andl $6, %eax
    movl control, %ebx
    shll $1, %ebx
    cmpl %ebx, %eax
    jne fail
    movl $matrix_fnop, expected_ip
matrix_fnop:
    fnop
    cmpl saved_sp, %esp
    jne fail
    incl control
    cmpl $4, control
    jb matrix
    cmpb $2, exceptions
    jne fail
    cmpb $0, handler_error
    jne fail

    movb $2, scenario
    movl $retry_fnop, expected_ip
    movb $1, retry
retry_fnop:
    fnop                    /* Handler disables EM, retries without skipping. */
    cmpb $3, exceptions
    jne fail
    fld1
    faddp
    fistpl result
    cmpl $2, result
    jne fail
    movw $0x0e01, %ax
    movw $3, %bx
    int $0x31
    jc fail

    movb $3, scenario
    int $0x2e
    int $0x80
    cmpb $2, software_calls
    jne fail
    movb $4, scenario
    movl $syscall_insn, expected_ip
syscall_insn:
    .byte 0x0f, 0x05
    movl $sysenter_insn, expected_ip
sysenter_insn:
    .byte 0x0f, 0x34
    cmpb $2, escapes
    jne fail
    cmpb $0, handler_error
    jne fail

    movb $5, scenario
    movw $0x3000, %ax
    int $0x21
    movw $0x0f00, %ax
    int $0x10
    cmpw $0x5003, %ax
    jne fail
    movl $1000000, %ecx
1:  pause
    loop 1b
    movw $0x0e00, %ax
    int $0x31
    cmpw $0x47, %ax
    jne fail

    movb $6, scenario
    movw real_seg, %ax
    movw %ax, %cx
    movw %ax, %dx
    movw %ax, %si
    movl $0x6000, %ebx
    movl $real_code, %edi
    ljmp *pm_switch
raw_return:
    movb $7, scenario
    cmpl saved_sp, %esp
    jne fail
    movw $0x0e00, %ax
    int $0x31
    cmpw $0x45, %ax
    jne fail
    smsw %ax
    andw $6, %ax
    cmpw $2, %ax
    jne fail
    cmpb $2, real_exceptions
    jne fail
    cmpb $0, handler_error
    jne fail
    movw $0x0e01, %ax
    movw $3, %bx
    int $0x31
    jc fail
    movl $pass_msg, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c2a, %ax       /* Parent also exits without re-enabling the FPU. */
    int $0x21
fail:
    cld
    movw $0x0e01, %ax
    movw $1, %bx
    int $0x31
    movl $fail_msg, %edx
    movw $0x0900, %ax
    int $0x21
    movb scenario, %dl
    addb $'0', %dl
    movb $2, %ah
    int $0x21
    movw $0x4c71, %ax
    int $0x21

nm_handler:
    pushal
    movl %esp, %ebp
    incb exceptions
    movl expected_ip, %eax
    cmpl %eax, %ss:44(%ebp)
    jne handler_bad
    cmpl $0, %ss:40(%ebp)
    jne handler_bad
    testl $0x10000, %ss:52(%ebp)
    jz handler_bad
    cmpb $0, retry
    je skip_insn
    movw $0x0e01, %ax
    movw $1, %bx
    int $0x31
    jc handler_bad
    popal
    lret
escape_handler:
    pushal
    movl %esp, %ebp
    incb escapes
    movl expected_ip, %eax
    cmpl %eax, %ss:44(%ebp)
    jne handler_bad
    cmpl $0, %ss:40(%ebp)
    jne handler_bad
skip_insn:
    addl $2, %ss:44(%ebp)
    popal
    lret
handler_bad:
    movb $1, handler_error
    jmp skip_insn
software_handler:
    pushal
    incb software_calls
    movw $0x0e00, %ax
    int $0x31
    cmpw $0x47, %ax
    je 1f
    movb $1, handler_error
1:  popal
    iretl

real_nm_handler:
    pushal
    movw %cs:saved_ds, %ax
    movw %ax, %ds
    movl %esp, %ebp
    incb real_exceptions
    movl expected_ip, %eax
    cmpl %eax, %ss:0x4c(%ebp)
    jne real_handler_bad
    movw real_seg, %ax
    cmpw %ax, %ss:0x50(%ebp)
    jne real_handler_bad
    testl $0x20000, %ss:0x54(%ebp)
    jz real_handler_bad
    cmpb $1, real_exceptions
    je real_handler_skip
    movw $0x0e01, %ax
    movw $1, %bx
    int $0x31
    jc real_handler_bad
    jmp real_handler_return
real_handler_bad:
    movb $1, handler_error
real_handler_skip:
    addl $2, %ss:0x4c(%ebp)
real_handler_return:
    popal
    addl $32, %esp
    lret

.code16
real_code:
    smsw %ax
    andw $6, %ax
    cmpw $6, %ax
    jne real_fail
    movl $real_fnop, expected_ip
real_fnop:
    fnop
    pushw %ds
    popw %es
    movw $child_name, %dx
    movw $exec_block, %bx
    movw $0x4b00, %ax
    int $0x21
    jc real_fail
    movw $0x4d00, %ax
    int $0x21
    cmpw $0x002b, %ax
    jne real_fail
    smsw %ax
    andw $6, %ax
    cmpw $6, %ax
    jne real_fail
    movl $real_retry_fnop, expected_ip
real_retry_fnop:
    fnop
    smsw %ax
    andw $6, %ax
    cmpw $2, %ax
    jne real_fail
    movw saved_ds, %ax
    movw %ax, %cx
    movw saved_ss, %dx
    movl saved_sp, %ebx
    movw saved_cs, %si
    movl $raw_return, %edi
    ljmp *rm_switch
real_fail:
    movw $0x4c72, %ax
    int $0x21

.balign 4
control: .long 0
observed_flags: .long 0
expected_ip: .long 0
result: .long 0
saved_sp: .long 0
saved_ss: .word 0
saved_cs: .word 0
saved_ds: .word 0
real_seg: .word 0
pm_switch: .long 0; .word 0
rm_switch: .word 0, 0
scenario: .byte 0
exceptions: .byte 0
real_exceptions: .byte 0
escapes: .byte 0
handler_error: .byte 0
software_calls: .byte 0
retry: .byte 0
child_name: .asciz "dpcpc32.com"
exec_block: .word 0, command_tail, 0, 0x5c, 0, 0x6c, 0
command_tail: .byte 0, 13
pass_msg: .ascii "DPMI COPROCESSOR PASS\r\n$"
fail_msg: .ascii "DPMI COPROCESSOR FAIL case=$"
