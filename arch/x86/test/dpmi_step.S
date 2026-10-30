/* Trace retirement and DPMI's virtual DR6, independent of CS.D and SS.B. */
    .section .text
    .code16
    .global _start
    .ifdef HANDOFF
    .set TRACE_ENTRY, 1
    .endif
    .ifdef IRET_HANDOFF
    .set TRACE_ENTRY, 1
    .endif
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
    .ifdef TRACE_ENTRY
    movw pm_entry + 2, %cx
    movl $step_handler, %edx
    movw $1, %bx
    movw $0x0212, %ax
    int $0x31
    jc setup_fail
    movl $protected_entry, expected_ip
    .ifdef IRET_HANDOFF
    movl $0, expected_tf
    /* Return with TF clear while the IRET itself is still being traced. */
    pushfl
    movzwl pm_entry + 2, %eax
    pushl %eax
    pushl $protected_entry
    .else
    movl $0x100, expected_tf
    .endif
    movl $1, expected_count
    pushfl
    orl $0x100, (%esp)
    popfl
    .endif
    .ifdef IRET_HANDOFF
    iretl
    .else
    ljmp *pm_entry
    .endif
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
    .macro arm target, tf=0x100
    movl $\target, expected_ip
    movl $\tf, expected_tf
    incl expected_count
    pushfl
    orl $0x100, (%esp)
    popfl
    .endm
    .macro check
    pushfl
    andl $~0x100, (%esp)
    popfl
    movl step_count, %eax
    cmpl expected_count, %eax
    jne fail
    cmpb $0, handler_error
    jne fail
    cmpl saved_sp, %esp
    jne fail
    .endm
protected_entry:
    .ifdef TRACE_ENTRY
    cmpb $0, handler_error
    jne fail
    cmpl $1, step_count
    jne fail
    .endif
    movw %ss, %bx
    movw $(0xf2 | (STACK32 * 0x4000)), %cx
    movw $9, %ax
    int $0x31
    jc fail
    movw %bx, %ss
    movl $0x8000, %esp
    movl %esp, saved_sp
    movw %cs, %cx
    movl $step_handler, %edx
    movw $1, %bx
    movw $0x0212, %ax
    int $0x31
    jc fail
    movw %cs, %cx
    movl $software_handler, %edx
    movw $0x60, %bx
    movw $0x0205, %ax
    int $0x31
    jc fail

    arm after_nop
    nop
after_nop:
    check

    /* TF sampled before POPF still traps when that POPF clears it. */
    pushfl
    arm after_clear, 0
    popfl
after_clear:
    check

    movw %ss, %dx
    arm after_movss
    movw %dx, %ss
    nop
after_movss:
    check

    pushw %ss
    arm after_popss
    popw %ss
    nop
after_popss:
    check

    arm after_sti
    sti
after_sti:
    check
    arm after_cli
    cli
after_cli:
    check

    movw $0x3da, %dx
    arm after_in
    inb %dx, %al
after_in:
    check
    movw $0x80, %dx
    arm after_out
    outb %al, %dx
after_out:
    check

    pushw %ds
    popw %es
    movl $source, %esi
    movl $destination, %edi
    movl $3, %ecx
    movl $after_movs, repeat_end
    addl $2, expected_count
    arm repeated_movs
repeated_movs:
    rep movsb
after_movs:
    movl $0, repeat_end
    check
    cmpw $0x4241, destination
    jne fail
    cmpb $0x43, destination + 2
    jne fail

    movl $source, %esi
    movl $destination, %edi
    movl $3, %ecx
    movl $after_cmps, repeat_end
    addl $2, expected_count
    arm repeated_cmps
repeated_cmps:
    repe cmpsb
after_cmps:
    jne fail
    movl $0, repeat_end
    check

    arm after_software
    int $0x60
    nop
after_software:
    check
    cmpb $1, software_count
    jne fail

    xorl %eax, %eax
    arm after_into
    .byte 0xce
after_into:
    check
    movl $pass, %edx
    movw $0x0900, %ax
    int $0x21
    cmpw $0x0924, %ax
    jne output_fail
    movw $0x4c2a, %ax
    int $0x21
fail:
    pushfl
    andl $~0x100, (%esp)
    popfl
    movl $failure, %edx
    movw $0x0900, %ax
    int $0x21
    cmpw $0x0924, %ax
    jne output_fail
    movw $0x4c71, %ax
    int $0x21
output_fail:
    movb $0x4c, %ah
    int $0x21

step_handler:
    pushl %ebp
    pushl %eax
    movl %esp, %ebp
    .if !STACK32
    andl $0xffff, %ebp
    .endif
    /* Two saved DWORDs precede the always-DWORD extended frame at +20h. */
    cmpl $0x8000, 0x30(%ebp)
    jne handler_bad
    movl 0x3c(%ebp), %eax
    andl $0x100, %eax
    cmpl expected_tf, %eax
    jne handler_bad
    movl expected_ip, %eax
    cmpl $0, repeat_end
    je compare_ip
    testl %ecx, %ecx
    jnz compare_ip
    movl repeat_end, %eax
compare_ip:
    cmpl %eax, 0x34(%ebp)
    jne handler_bad
    incl step_count
    cmpl $0, repeat_end
    je clear_trace
    testl %ecx, %ecx
    jnz handler_return
clear_trace:
    andl $~0x100, 0x3c(%ebp)
handler_return:
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
    jmp clear_trace
software_handler:
    incb software_count
    iret

    .balign 4
saved_sp: .long 0
expected_ip: .long 0
expected_tf: .long 0
expected_count: .long 0
step_count: .long 0
repeat_end: .long 0
handler_error: .byte 0
software_count: .byte 0
source: .ascii "ABC"
destination: .space 3
pass: .asciz "DPMI SINGLE STEP PASS\r\n$"
failure: .asciz "DPMI SINGLE STEP FAIL\r\n$"
