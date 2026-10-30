/* Native #MF is an exception; software INT 10h remains the BIOS vector. */
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
    smsw %ax
    testw $0x20, %ax
    jz unsupported
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
    fninit
    /* No 0205h handler yet: exercise the host's default BIOS translator. */
    movw $0x0f00, %ax
    int $0x10
    cmpw $0x5003, %ax
    jne fail
    testb %bh, %bh
    jnz fail
    movw $0x0f00, %ax
    .rept 13
    .byte 0x3e
    .endr
    int $0x10
    cmpw $0x5003, %ax
    jne fail
    testb %bh, %bh
    jnz fail
    cmpl saved_sp, %esp
    jne fail
    movw $0x0204, %ax
    movw $0x10, %bx
    int $0x31
    jc fail
    movw %cx, old_video_sel
    movl %edx, old_video_off
    movw %cs, %cx
    movl $software_video, %edx
    movw $0x0205, %ax
    int $0x31
    jc fail
    movw %cs, %cx
    movl $mf_handler, %edx
    movw $0x0203, %ax
    int $0x31
    jc fail

    movl $1f, expected_ip
    stc
    std
    int $0x10
1:  cld
    cmpb $1, software_calls
    jne fail
    movl $2f, expected_ip
    stc
    std
    .rept 13
    .byte 0x3e
    .endr
    int $0x10
2:  cld
    cmpb $2, software_calls
    jne fail
    cmpb $0, handler_error
    jne fail

    fninit
    fldcw unmask_invalid
    fldz
    fldz
    .byte 0xde, 0xf9       /* FDIVP ST(1),ST(0): 0/0, pending invalid operation. */
    movw $1, expected_status
    movl $wait_invalid, expected_ip
wait_invalid:
    fwait
    cmpb $0, wrong_vector
    jne misrouted
    cmpb $1, exceptions
    jne fail
    fnstsw %ax
    testw $0xff, %ax
    jnz fail

    fninit
    fldcw unmask_zero
    fld1
    fldz
    .byte 0xde, 0xf9       /* 1/0, pending zero divide. */
    movw $4, expected_status
    movl $wait_zero, expected_ip
wait_zero:
    fwait
    cmpb $0, wrong_vector
    jne misrouted
    cmpb $2, exceptions
    jne fail
    fnstsw %ax
    testw $0xff, %ax
    jnz fail
    fninit
    cmpl saved_sp, %esp
    jne fail
    cmpb $0, handler_error
    jne fail
    cmpb $2, software_calls
    jne fail

    movw old_video_sel, %cx
    movl old_video_off, %edx
    movw $0x10, %bx
    movw $0x0205, %ax
    int $0x31
    jc fail
    movw $0x0f00, %ax
    int $0x10
    cmpw $0x5003, %ax
    jne fail
    testb %bh, %bh
    jnz fail
    movl $pass_text, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c2a, %ax
    int $0x21

unsupported:
    movl $unsupported_text, %edx
    jmp report_fail
misrouted:
    movl $misrouted_text, %edx
    jmp report_fail
fail:
    movl $fail_text, %edx
report_fail:
    fninit
    cld
    movw $0x0900, %ax
    int $0x21
    movw $0x4c71, %ax
    int $0x21

software_video:
    pushal
    movzwl %sp, %ebp
    .if STACK32
    movl %esp, %ebp
    .endif
    fnstsw %ax
    testw $0x80, %ax
    jz software_ok
    /* Bound the old-host failure: clear the pending error before retrying
     * FWAIT, then let the caller report the incorrect vector and exit. */
    fnclex
    incb wrong_vector
    jmp software_done
software_ok:
    incl software_calls
    movl expected_ip, %eax
    cmpl %eax, %ss:32(%ebp)
    jne software_bad
    testl $0x10000, %ss:40(%ebp)
    jnz software_bad
    movl %ss:40(%ebp), %eax
    andl $0x401, %eax
    cmpl $0x401, %eax
    je software_done
software_bad:
    movb $1, handler_error
software_done:
    popal
    iretl

mf_handler:
    pushal
    movl %esp, %ebp
    fnstsw %ax
    movw %ax, observed_status
    fnclex
    movw expected_status, %dx
    andw $0x3f, %ax
    cmpw %dx, %ax
    jne mf_bad
    testw $0x80, observed_status
    jz mf_bad
    cmpl $0, %ss:40(%ebp)
    jne mf_bad
    movl expected_ip, %eax
    cmpl %eax, %ss:44(%ebp)
    jne mf_bad
    movw saved_cs, %ax
    cmpw %ax, %ss:48(%ebp)
    jne mf_bad
    movl saved_sp, %eax
    cmpl %eax, %ss:56(%ebp)
    jne mf_bad
    movw saved_ss, %ax
    cmpw %ax, %ss:60(%ebp)
    jne mf_bad
    testl $0x10000, %ss:52(%ebp)
    jz mf_bad
    incb exceptions
    jmp mf_done
mf_bad:
    movb $1, handler_error
mf_done:
    popal
    lret

    .balign 4
saved_sp: .long 0
expected_ip: .long 0
old_video_off: .long 0
software_calls: .long 0
saved_ss: .word 0
saved_cs: .word 0
old_video_sel: .word 0
expected_status: .word 0
observed_status: .word 0
unmask_invalid: .word 0x037e
unmask_zero: .word 0x037b
exceptions: .byte 0
handler_error: .byte 0
wrong_vector: .byte 0
pass_text: .ascii "DPMI X87 ROUTING PASS\r\n$"
fail_text: .ascii "DPMI X87 ROUTING FAIL\r\n$"
misrouted_text: .ascii "DPMI MF MISROUTED TO BIOS\r\n$"
unsupported_text: .ascii "DPMI X87 REQUIRES CR0.NE\r\n$"
