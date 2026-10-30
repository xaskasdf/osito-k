/* DPMI virtual-IF host profile: native CPL3/IOPL0, USE32 with SS.B=0/1.
 * PUSHF reports physical IF; POPF and ordinary IRET cannot change virtual IF.
 * The USE16 alias exercises operand-size defaults without changing client ABI.
 * DPMI 1.0 sections Virtual Interrupts and 0900h-0902h define this profile. */
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
    movw %ax, code16_pointer + 4
    movw %ax, %bx
    movw $0x00fa, %cx
    movw $9, %ax
    int $0x31
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
code16_pointer: .long flags16
    .word 0

.macro vif expected
    movl $0xa5a50902, %eax
    stc
    int $0x31
    jc fail
    cmpl $(0xa5a50900 + \expected), %eax
    jne fail
.endm

.macro physical expected
    pushfl
    popl %eax
    andl $0x3601, %eax
    cmpl $\expected, %eax
    jne fail
.endm

.macro return_frame flags, target
    pushl $\flags
    xorl %eax, %eax
    movw %cs, %ax
    pushl %eax
    pushl $\target
    iretl
.endm

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
    movb $1, stage
    cli
    clc
    physical 0x200
    vif 0

    movb $2, stage
    pushl $0x3603             /* IF/IOPL cannot be granted by POPFD at CPL3. */
    popfl
    physical 0x601
    vif 0
    cld
    sti
    pushl $0x3002             /* Nor can POPFD disable the virtual IF. */
    popfl
    physical 0x200
    vif 1

    movb $3, stage
    return_frame 0x3002, iret_enabled
iret_enabled:
    vif 1
    cli
    return_frame 0x3603, iret_disabled
iret_disabled:
    physical 0x601
    vif 0
    cld

    movb $4, stage
    lcall *code16_pointer
    cmpb $0, handler_error
    jne fail
    vif 0

    movb $5, stage
    movl $0xb6b61234, %ebx
    movl $0xc7c75678, %ecx
    movl $0xd8d89abc, %edx
    movl $0xe9e9def0, %esi
    movl $0xfafa2345, %edi
    movl $0xabab6789, %ebp
    movl $0xa5a50901, %eax
    stc
    std
    int $0x31
    jc fail
    pushfl
    popl saved_flags
    cld
    cmpl $0xa5a50900, %eax
    jne fail
    /* AH=09 is preserved: invoke the returned AX to restore the old state. */
    int $0x31
    jc fail
    cmpl $0xa5a50901, %eax
    jne fail
    cmpl $0xb6b61234, %ebx
    jne fail
    cmpl $0xc7c75678, %ecx
    jne fail
    cmpl $0xd8d89abc, %edx
    jne fail
    cmpl $0xe9e9def0, %esi
    jne fail
    cmpl $0xfafa2345, %edi
    jne fail
    cmpl $0xabab6789, %ebp
    jne fail
    testl $0x400, saved_flags
    jz fail
    vif 0
    movw $0x0900, %ax
    int $0x31
    jc fail
    cmpw $0x0900, %ax
    jne fail

    movb $6, stage
    movw %cs, %cx
    movl $software_handler, %edx
    movw $0x60, %bx
    movw $0x0205, %ax
    int $0x31
    jc fail
    int $0x60
    vif 0
    sti
    movb $1, software_if
    int $0x60
    vif 1
    cli
    movb $0, software_if
    cmpb $2, software_seen
    jne fail
    cmpb $0, handler_error
    jne fail

    movw %cs, %cx
    movl $timer_handler, %edx
    movw $8, %bx
    movw $0x0205, %ax
    int $0x31
    jc fail
    inb $0x21, %al
    andb $0xfe, %al
    outb %al, $0x21
    movb $7, stage
    movw $0x0900, %ax
    int $0x31
    movw $3, %bx
    call wait_ticks
    cmpl $0, timer_seen
    jne fail
    vif 0

    movb $8, stage
    movw $0x0901, %ax
    int $0x31
    jc fail
    cmpw $0x0900, %ax
    jne fail
    call await_timer
    vif 1
    cli
    movl timer_seen, %eax
    movl %eax, timer_before
    movw $3, %bx
    call wait_ticks
    movl timer_seen, %eax
    cmpl timer_before, %eax
    jne fail
    vif 0

    movb $9, stage
    sti
    call await_timer
    vif 1
    cli
    cmpb $0, handler_error
    jne fail
    cmpl saved_sp, %esp
    jne fail
    movl $pass_text, %edx
    movb $9, %ah
    int $0x21
    movw $0x4c2a, %ax
    int $0x21

/* BIOS time must advance despite virtual CLI; use a bounded poll, not a
 * machine-speed-dependent instruction count as proof that IRQs were held. */
wait_ticks:
    pushal
    xorb %ah, %ah
    int $0x1a
    movw %dx, %si
    movl $200000, %edi
1:
    movl $1024, %ebp
3:
    pause
    decl %ebp
    jnz 3b
    xorb %ah, %ah
    int $0x1a
    subw %si, %dx
    cmpw %bx, %dx
    jae 2f
    decl %edi
    jnz 1b
    jmp fail
2:
    popal
    ret

await_timer:
    movw $4, %bx
    call wait_ticks
    movl timer_seen, %eax
    subl timer_before, %eax
    cmpl $2, %eax
    jb fail
    cmpb $0, handler_error
    jne fail
    ret

software_handler:
    pushal
    movw $0x0902, %ax
    int $0x31
    cmpb software_if, %al
    jne handler_bad
    incb software_seen
    popal
    iretl
handler_bad:
    movb $1, handler_error
    popal
    iretl

timer_handler:
    pushal
    movw $0x0902, %ax
    int $0x31
    cmpw $0x0900, %ax
    jne timer_bad
    pushfl
    popl %eax
    andl $0x3200, %eax
    cmpl $0x200, %eax
    jne timer_bad
    int $0x60
    movw $0x0902, %ax
    int $0x31
    cmpw $0x0900, %ax
    jne timer_bad
    incl timer_seen
    jmp timer_eoi
timer_bad:
    movb $1, handler_error
timer_eoi:
    movb $0x20, %al
    outb %al, $0x20
    popal
    /* Portable DPMI handlers explicitly enable virtual interrupts on exit. */
    sti
    iretl

fail:
.ifdef NATIVE_IF_DIAGNOSTICS
    movl %eax, diagnostic_eax
    movl %esp, diagnostic_sp
    pushfl
    popl diagnostic_flags
.endif
    cld
    cli
.ifdef NATIVE_IF_DIAGNOSTICS
    call failure_diagnostics
.endif
    movl $fail_text, %edx
    movb $9, %ah
    int $0x21
    movb stage, %dl
    addb $'0', %dl
    movb $2, %ah
    int $0x21
    movb stage, %al
    addb $0x70, %al
    movb $0x4c, %ah
    int $0x21

.ifdef NATIVE_IF_DIAGNOSTICS
/* Failure-only state; default fixtures keep their original bytes and timing. */
failure_diagnostics:
    pushal
    movl $diagnostic_text, %edx
    movb $9, %ah
    int $0x21
    movzbl stage, %eax
    call diagnostic_hex
    movl diagnostic_eax, %eax
    call diagnostic_hex
    movl diagnostic_flags, %eax
    call diagnostic_hex
    movl diagnostic_sp, %eax
    call diagnostic_hex
    movl saved_sp, %eax
    call diagnostic_hex
    movl timer_seen, %eax
    call diagnostic_hex
    movl timer_before, %eax
    call diagnostic_hex
    movzbl handler_error, %eax
    call diagnostic_hex
    movb $13, %dl
    movb $2, %ah
    int $0x21
    movb $10, %dl
    int $0x21
    popal
    ret

diagnostic_hex:
    pushal
    movl %eax, %ebx
    movl $8, %ecx
1:
    roll $4, %ebx
    movb %bl, %dl
    andb $15, %dl
    addb $'0', %dl
    cmpb $'9', %dl
    jbe 2f
    addb $('A' - '0' - 10), %dl
2:
    movb $2, %ah
    int $0x21
    loop 1b
    movb $' ', %dl
    movb $2, %ah
    int $0x21
    popal
    ret

diagnostic_eax: .long 0
diagnostic_sp: .long 0
diagnostic_flags: .long 0
diagnostic_text: .asciz "DPMI NATIVE IF STATE stage/eax/flags/sp/saved_sp/timer_seen/timer_before/handler_error:\r\n$"
.endif

.code16
flags16:
    cli
    pushw $0x3603
    popfw
    pushfw
    popw %ax
    andw $0x3601, %ax
    cmpw $0x601, %ax
    jne flags16_bad
    cld
    movw $0x0902, %ax
    int $0x31
    cmpw $0x0900, %ax
    jne flags16_bad
    pushw $0x3202
    pushw %cs
    pushw $iret16_disabled
    iretw
iret16_disabled:
    movw $0x0902, %ax
    int $0x31
    cmpw $0x0900, %ax
    jne flags16_bad
    sti
    pushw $0x3002
    popfw
    pushw $0x3002
    pushw %cs
    pushw $iret16_enabled
    iretw
iret16_enabled:
    movw $0x0902, %ax
    int $0x31
    cmpw $0x0901, %ax
    jne flags16_bad
    cli
    lretl
flags16_bad:
    movb $1, handler_error
    cld
    cli
    lretl

stage: .byte 0
handler_error: .byte 0
software_if: .byte 0
software_seen: .byte 0
saved_sp: .long 0
saved_flags: .long 0
timer_seen: .long 0
timer_before: .long 0
pass_text: .asciz "DPMI NATIVE IF PASS\r\n$"
fail_text: .asciz "DPMI NATIVE IF FAIL stage=$"
