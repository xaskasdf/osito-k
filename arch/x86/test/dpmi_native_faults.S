/* Native CPU faults must reach DPMI handlers, with an editable FAR frame. */
    .section .text
    .code16
    .global _start
_start:
    cld
    pushw %cs
    popw %ds
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
    movw %ss, %bx
    .if STACK32
    movw $0x40f2, %cx
    .else
    movw $0x00f2, %cx
    .endif
    movw $9, %ax
    int $0x31
    jc setup_fail32
    movw %bx, %ss
    .if !STACK32
    orl $0xa5a50000, %esp
    .endif
    movw %ds, saved_ds
    movw %ss, saved_ss
    movw %cs, saved_cs
    movl %esp, saved_sp
    movl %esp, expected_sp
    movb $6, %bl
    movw $0x0202, %ax
    int $0x31
    jc setup_fail32
    movl %edx, default_ud
    movw %cx, default_ud + 4
    movl $handler_table, %esi
    movl $handler_count, %edi
install_handler:
    movb (%esi), %bl
    movl 4(%esi), %edx
    movw %cs, %cx
    movw $0x0203, %ax
    int $0x31
    jc setup_fail32
    addl $8, %esi
    decl %edi
    jnz install_handler

    /* A non-present data selector tests #NP and #SS without corrupting the
     * active stack; the handler must see the selector's CPU error code. */
    movw $1, %cx
    xorw %ax, %ax
    int $0x31
    jc setup_fail32
    movw %ax, absent_sel
    movw %ax, %bx
    movw $0x4072, %cx
    movw $9, %ax
    int $0x31
    jc setup_fail32

    movw $1, %cx
    xorw %ax, %ax
    int $0x31
    jc setup_fail32
    movw %ax, flat_sel
    movw %ax, %bx
    xorw %cx, %cx
    xorw %dx, %dx
    movw $7, %ax
    int $0x31
    jc setup_fail32
    movw $0xffff, %cx
    movw $0xffff, %dx
    movw $8, %ax
    int $0x31
    jc setup_fail32

    .macro expect vector, ip, resume, error=0
    movl $\vector, expected_vector
    movl $\ip, expected_ip
    movl $\resume, resume_ip
    movl $\error, expected_error
    .endm

    expect 0, divide_zero, after_divide
    xorl %ecx, %ecx
    xorl %edx, %edx
    movl $0xa1b2c3d4, %eax
    call snapshot
divide_zero:
    divl %ecx
after_divide:
    call verify

    expect 6, invalid_opcode, after_invalid
    call snapshot
invalid_opcode:
    ud2
after_invalid:
    call verify

    expect 13, invalid_selector, after_selector, 0xfffc
    movw $0xfffc, %ax
    call snapshot
invalid_selector:
    movw %ax, %es
after_selector:
    call verify

    expect 11, absent_data, after_data
    movzwl absent_sel, %eax
    andl $0xfffc, %eax
    movl %eax, expected_error
    movw absent_sel, %ax
    call snapshot
absent_data:
    movw %ax, %es
after_data:
    call verify

    expect 12, absent_stack, after_stack
    movzwl absent_sel, %eax
    andl $0xfffc, %eax
    movl %eax, expected_error
    movw absent_sel, %ax
    call snapshot
absent_stack:
    movw %ax, %ss
after_stack:
    call verify

    /* No space remains for a dword push. The host must use its resident
     * stack, report the original SP, and honor the repaired saved SP. */
    expect 12, exhausted_stack, after_exhausted
    call snapshot
    movl $2, expected_sp
    movl $2, %esp
exhausted_stack:
    pushl %eax
after_exhausted:
    call verify
    movl %esp, expected_sp

    expect 3, after_breakpoint, after_breakpoint
    call snapshot
    int3
after_breakpoint:
    call verify

    expect 4, after_overflow, after_overflow
    movl $0x7fffffff, %eax
    addl $1, %eax
    call snapshot
    into
after_overflow:
    call verify

    expect 3, after_prefixed_break, after_prefixed_break
    call snapshot
    .byte 0x66, 0x2e, 0xcc
after_prefixed_break:
    call verify

    expect 4, after_prefixed_overflow, after_prefixed_overflow
    movl $0x7fffffff, %eax
    addl $1, %eax
    call snapshot
    .byte 0xf3, 0x67, 0xce
after_prefixed_overflow:
    call verify

    /* INTO with OF clear is a no-op; INT n is a software interrupt even
     * when its number is also used by a processor exception. */
    xorl %eax, %eax
    into
    movw %cs, %cx
    movl $software_handler, %edx
    movw $3, %bx
    movw $0x0205, %ax
    int $0x31
    jc setup_fail32
    incb %bl
    int $0x31
    jc setup_fail32
    .byte 0xcd, 0x03
    .byte 0xcd, 0x04
    cmpl $2, software_calls
    jne fail

    expect 5, bound_range, after_bound
    movl $9, %eax
    call snapshot
bound_range:
    boundl %eax, bounds
after_bound:
    call verify

    /* This address is outside this test VM's DOS RAM, not a VGA trap. */
    expect 14, missing_page, after_page, 4
    movw flat_sel, %ax
    movw %ax, %es
    call snapshot
missing_page:
    movl %es:0x10000000, %eax
after_page:
    call verify
    movw saved_ds, %ax
    movw %ax, %es

    cmpl $12, calls
    jne fail
    movw %cs, %cx
    xorl %ebx, %ebx
    movl $outer_handler, %edx
    movw $0x0203, %ax
    int $0x31
    jc setup_fail32
    movb $6, %bl
    movl $inner_handler, %edx
    movw $0x0203, %ax
    int $0x31
    jc setup_fail32
    expect 0, nested_divide, after_nested
    xorl %ecx, %ecx
    xorl %edx, %edx
    movl $0xa1b2c3d4, %eax
    call snapshot
nested_divide:
    divl %ecx
after_nested:
    call verify
    cmpl $14, calls
    jne fail
    cmpl saved_sp, %esp
    jne fail
    movl $pass, %edx
    movw $0x0900, %ax
    int $0x21
    .if FATAL_DEFAULT
    movl default_ud, %edx
    movw default_ud + 4, %cx
    movb $6, %bl
    movw $0x0203, %ax
    int $0x31
    jc setup_fail32
unhandled_opcode:
    ud2
    jmp fail
    .endif
    movw $0x4c2a, %ax
    int $0x21
setup_fail32:
    movw $0x4c70, %ax
    int $0x21
fail:
    cld
    movb fail_stage, %al
    movb $0x4c, %ah
    int $0x21

    /* CALL does not alter FLAGS. Record the instruction's original image. */
snapshot:
    movl %eax, before_eax
    movl %ebx, before_ebx
    movl %ecx, before_ecx
    movl %edx, before_edx
    movl %esi, before_esi
    movl %edi, before_edi
    movl %ebp, before_ebp
    movw %es, before_es
    movw %fs, before_fs
    movw %gs, before_gs
    pushfl
    popl before_flags
    ret
verify:
    /* No handler is allowed to leak GPR/data-segment or arithmetic changes. */
    movb $0x81, fail_stage
    pushfl
    popl after_flags
    cmpl before_eax, %eax
    jne fail
    cmpl before_ebx, %ebx
    jne fail
    cmpl before_ecx, %ecx
    jne fail
    cmpl before_edx, %edx
    jne fail
    cmpl before_esi, %esi
    jne fail
    cmpl before_edi, %edi
    jne fail
    cmpl before_ebp, %ebp
    jne fail
    pushl %eax
    movb $0x82, fail_stage
    movw %ds, %ax
    cmpw saved_ds, %ax
    jne fail
    movw %es, %ax
    cmpw before_es, %ax
    jne fail
    movw %fs, %ax
    cmpw before_fs, %ax
    jne fail
    movw %gs, %ax
    cmpw before_gs, %ax
    jne fail
    movl after_flags, %eax
    movb $0x83, fail_stage
    xorl before_flags, %eax
    andl $0x0cd5, %eax
    jnz fail
    popl %eax
    ret

    .macro handler vector
handler_\vector:
    pushl $\vector
    jmp exception_handler
    .endm
    handler 0
    handler 3
    handler 4
    handler 5
    handler 6
    handler 11
    handler 12
    handler 13
    handler 14
exception_handler:
    pushl %ebp
    movl %esp, %ebp
    pushl %eax
    movb $0x90, fail_stage
    movw %ss, %ax
    cmpw saved_ss, %ax
    je fail
    movb $0x91, fail_stage
    movl 4(%ebp), %eax
    cmpl expected_vector, %eax
    jne fail
    movb $0x92, fail_stage
    movl 16(%ebp), %eax
    cmpl expected_error, %eax
    jne fail
    movb $0x93, fail_stage
    movl 20(%ebp), %eax
    cmpl expected_ip, %eax
    jne fail
    movb $0x94, fail_stage
    movw 24(%ebp), %ax
    cmpw saved_cs, %ax
    jne fail
    movb $0x95, fail_stage
    movl 28(%ebp), %eax
    xorl before_flags, %eax
    andl $0x0cd5, %eax /* arithmetic flags plus DF; RF is CPU-generated */
    jnz fail
    movb $0x96, fail_stage
    movl 28(%ebp), %eax
    andl $0x10000, %eax
    cmpl $3, expected_vector
    je trap_flags
    cmpl $4, expected_vector
    je trap_flags
    cmpl $0x10000, %eax
    jne fail
    jmp checked_rf
trap_flags:
    testl %eax, %eax
    jnz fail
checked_rf:
    movb $0x97, fail_stage
    movl 32(%ebp), %eax
    cmpl expected_sp, %eax
    jne fail
    movb $0x98, fail_stage
    movw 36(%ebp), %ax
    cmpw saved_ss, %ax
    jne fail
    movl saved_sp, %eax
    movl %eax, 32(%ebp)
    movl resume_ip, %eax
    movl %eax, 20(%ebp)
    incl calls
    popl %eax
    popl %ebp
    addl $4, %esp
    lret

software_handler:
    incl software_calls
    iretl

outer_handler:
    movl %esp, nested_sp
    movw %ss, nested_ss
    std
nested_invalid:
    ud2
after_nested_invalid:
    jnc fail
    pushl %eax
    pushfl
    popl %eax
    testl $0x400, %eax
    jz fail
    cld
    popl %eax
    cmpl nested_sp, %esp
    jne fail
    pushl $0
    jmp exception_handler
inner_handler:
    pushl %ebp
    movl %esp, %ebp
    pushl %eax
    movl 16(%ebp), %eax
    cmpl $nested_invalid, %eax
    jne fail
    movl 28(%ebp), %eax
    cmpl nested_sp, %eax
    jne fail
    movw %ss, %ax
    cmpw nested_ss, %ax
    jne fail
    cmpw 32(%ebp), %ax
    jne fail
    testl $0x400, 24(%ebp)
    jz fail
    movl $after_nested_invalid, 16(%ebp)
    orl $1, 24(%ebp)
    incl calls
    popl %eax
    popl %ebp
    lret

    .balign 4
handler_table:
    .long 0, handler_0, 3, handler_3, 4, handler_4, 5, handler_5
    .long 6, handler_6, 11, handler_11, 12, handler_12
    .long 13, handler_13, 14, handler_14
    .set handler_count, (. - handler_table) / 8
bounds: .long 1, 5
expected_vector: .long 0
expected_ip: .long 0
expected_error: .long 0
resume_ip: .long 0
calls: .long 0
software_calls: .long 0
fail_stage: .byte 0x80
nested_sp: .long 0
nested_ss: .word 0
saved_sp: .long 0
expected_sp: .long 0
saved_cs: .word 0
saved_ss: .word 0
saved_ds: .word 0
absent_sel: .word 0
flat_sel: .word 0
default_ud: .long 0
    .word 0
before_eax: .long 0
before_ebx: .long 0
before_ecx: .long 0
before_edx: .long 0
before_esi: .long 0
before_edi: .long 0
before_ebp: .long 0
before_flags: .long 0
after_flags: .long 0
before_es: .word 0
before_fs: .word 0
before_gs: .word 0
pass: .ascii "DPMI NATIVE FAULT CONTRACT PASS\r\n$"
