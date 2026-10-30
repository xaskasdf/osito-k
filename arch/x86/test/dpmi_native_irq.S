/* Native locked-IRQ IRETD: trace the return before any caller instruction.
 * The extended #DB frame must contain the edited RF and restored caller,
 * not the host's private INT stub. This probe requires the native backend. */
    .ifndef MASK_IRQ
    .set MASK_IRQ, 0
    .endif
    .set IRQ_DEADLINE_TICKS, 4
    .set IRQ_WATCHDOG_POLLS, 200000
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
    .macro diag_set field, value
    .ifdef DIAGNOSTIC
    movl $\value, diag_\field
    .endif
    .endm
    .macro expect target
    movl $\target, expected_ip
    movl %esp, expected_sp
    movw %ss, expected_ss
    .endm
    .macro check
    diag_set reason, 1
    cmpl saved_sp, %esp
    jne fail
    diag_set reason, 2
    cmpb $0, handler_error
    jne fail
    diag_set reason, 3
    movl trace_count, %eax
    cmpl expected_count, %eax
    jne fail
    .endm
protected_entry:
    movw $0x0900, %ax
    int $0x31
    movw %ss, %bx
    movw $(0xf2 | (STACK32 * 0x4000)), %cx
    movw $9, %ax
    int $0x31
    jc fail
    movw %bx, %ss
    movl $0x8000, %esp
    .if !STACK32
    orl $0xabcd0000, %esp
    .endif
    movl %esp, saved_sp
    movw %cs, expected_cs
    movw %cs, %cx
    movl $step_handler, %edx
    movw $1, %bx
    movw $0x0212, %ax
    int $0x31
    jc fail
    movw %cs, %cx
    movl $outer_handler, %edx
    movw $0x1c, %bx
    movw $0x0205, %ax
    int $0x31
    jc fail
    movw %cs, %cx
    movl $inner_handler, %edx
    movw $0x23, %bx
    movw $0x0205, %ax
    int $0x31
    jc fail

    diag_set stage, 1
    expect first_return
    movl $0, expected_rf
    movl $1, expected_count
    int $0x1c
first_return:
    check
    diag_set stage, 2
    expect second_return
    movl $0x10000, expected_rf
    incl expected_count
    int $0x1c
second_return:
    check
    diag_set stage, 3
    expect nested_return
    movb $1, nested
    movl $0, expected_rf
    addl $2, expected_count
    int $0x1c
nested_return:
    check
    diag_set stage, 4
    expect last_return
    movl $0x10000, expected_rf
    addl $2, expected_count
    int $0x1c
last_return:
    check
    .ifdef HARDWARE
    diag_set stage, 5
    movw %cs, %cx
    movl $timer_handler, %edx
    movw $8, %bx
    movw $0x0205, %ax
    int $0x31
    jc fail
    /* Read BIOS ticks without a service call in the native IRQ wait. */
    movw $0x0040, %bx
    movw $0x0002, %ax
    int $0x31
    jc fail
    movw %ax, %fs
    movl %fs:0x6c, %ebx
    movl $IRQ_WATCHDOG_POLLS, %edi
clock_sync:
    /* Observe a fresh tick so an initially stale BDA cannot shorten the wait. */
    movl $1024, %ebp
clock_pause:
    pause
    decl %ebp
    jnz clock_pause
    movl %fs:0x6c, %eax
    cmpl %ebx, %eax
    jne clock_ready
    decl %edi
    jnz clock_sync
    diag_set reason, 5
    jmp fail
clock_ready:
    movl %eax, %ebx
    movl %esp, expected_sp
    movw %ss, expected_ss
    movl $0x10000, expected_rf
    movb $1, hardware_mode
    incl expected_count
    .ifdef DIAGNOSTIC
    movl %ebx, diag_time+24
    rdtsc
    movl %eax, diag_time
    movl %edx, diag_time+4
    .endif
    movl $IRQ_WATCHDOG_POLLS, %edi
    movw $(0x0901 - MASK_IRQ), %ax
    int $0x31
hardware_begin:
    .ifdef DIAGNOSTIC
    movl %edi, diag_time+36
    .endif
hardware_wait:
    cmpb $0, hardware_seen
    jne hardware_done
    movl %fs:0x6c, %eax
    subl %ebx, %eax
    cmpl $IRQ_DEADLINE_TICKS, %eax
    jae hardware_deadline
    movl $1024, %ebp
hardware_pause:
    pause
    decl %ebp
    jnz hardware_pause
    decl %edi
    jnz hardware_wait
    /* A stopped clock is a separate failure, not an IRQ deadline. */
    diag_set reason, 5
    jmp fail
hardware_deadline:
    diag_set reason, 4
    jmp fail
hardware_done:
    movw $0x0900, %ax
    int $0x31
    check
    .endif
    movl $pass_text, %edx
    movb $9, %ah
    int $0x21
    movw $0x4c2a, %ax
    int $0x21
fail:
    .ifdef DIAGNOSTIC
    movl %esp, diag_sp
    movl %edi, diag_time+32
    rdtsc
    movl %eax, diag_time+8
    movl %edx, diag_time+12
    subl diag_time, %eax
    sbbl diag_time+4, %edx
    movl %eax, diag_time+16
    movl %edx, diag_time+20
    pushfl
    popl diag_irq+4
    movw $0x0900, %ax
    int $0x31
    movzbl %al, %eax
    movl %eax, diag_irq
    inb $0x21, %al
    movzbl %al, %eax
    movl %eax, diag_irq+8
    movb $0x0b, %al
    outb %al, $0x20
    inb $0x20, %al
    movzbl %al, %eax
    movl %eax, diag_irq+12
    movb $0x0a, %al
    outb %al, $0x20
    inb $0x20, %al
    movzbl %al, %eax
    movl %eax, diag_irq+16
    xorb %ah, %ah
    int $0x1a
    movzwl %cx, %eax
    shll $16, %eax
    movw %dx, %ax
    movl %eax, diag_time+28
    .endif
    andl $0xffff, %esp
    movl $fail_text, %edx
    movb $9, %ah
    int $0x21
    .ifdef DIAGNOSTIC
    call diag_dump
    .endif
    movw $0x4c71, %ax
    int $0x21

outer_handler:
    pushal
    cmpb $0, nested
    je outer_return
    pushl expected_ip
    pushl expected_sp
    pushl expected_rf
    pushw expected_ss
    expect inner_return
    xorl $0x10000, expected_rf
    int $0x23
inner_return:
    popw expected_ss
    popl expected_rf
    popl expected_sp
    popl expected_ip
outer_return:
    movl expected_rf, %eax
    orl $2, %eax
    movl %eax, %ss:40(%esp)
    popal
    pushfl
    orl $0x100, (%esp)
    popfl
    iretl

inner_handler:
    pushal
    movl expected_rf, %eax
    orl $2, %eax
    movl %eax, %ss:40(%esp)
    popal
    pushfl
    orl $0x100, (%esp)
    popfl
    iretl

    .ifdef HARDWARE
timer_handler:
    pushal
    movb $0x20, %al
    outb %al, $0x20
    cmpb $0, hardware_seen
    jne timer_already_seen
    movb $1, hardware_seen
    movl $0x10002, %ss:40(%esp)
    popal
    pushfl
    orl $0x100, (%esp)
    popfl
    iretl
timer_already_seen:
    popal
    iretl
    .endif

step_handler:
    pushal
    leal 32(%esp), %ebp
    incl trace_count
    diag_set field, 1
    cmpl $0x8000, %ss:40(%ebp)
    jne step_bad
    .ifdef HARDWARE
    cmpb $0, hardware_mode
    je software_ip
    diag_set field, 2
    cmpl $hardware_begin, %ss:44(%ebp)
    jb step_bad
    cmpl $hardware_done, %ss:44(%ebp)
    ja step_bad
    jmp check_cs
software_ip:
    .endif
    diag_set field, 2
    movl expected_ip, %eax
    cmpl %eax, %ss:44(%ebp)
    jne step_bad
check_cs:
    diag_set field, 3
    movw expected_cs, %ax
    cmpw %ax, %ss:48(%ebp)
    jne step_bad
    diag_set field, 4
    movl %ss:52(%ebp), %eax
    andl $0x10100, %eax
    cmpl expected_rf, %eax
    jne step_bad
    diag_set field, 5
    movl expected_sp, %eax
    cmpl %eax, %ss:56(%ebp)
    jne step_bad
    diag_set field, 6
    movw expected_ss, %ax
    cmpw %ax, %ss:60(%ebp)
    jne step_bad
    jmp step_done
step_bad:
    .ifdef DIAGNOSTIC
    /* Keep the first bad frame: later nested returns can be correct. */
    cmpl $0, diag_error
    jne 991f
    movl diag_field, %eax
    movl %eax, diag_error
    .irp offset, 0, 4, 8, 12, 16, 20
    movl %ss:40+\offset(%ebp), %eax
    movl %eax, diag_frame+\offset
    .endr
    movl expected_ip, %eax
    movl %eax, diag_wanted+4
    movzwl expected_cs, %eax
    movl %eax, diag_wanted+8
    movl expected_rf, %eax
    movl %eax, diag_wanted+12
    movl expected_sp, %eax
    movl %eax, diag_wanted+16
    movzwl expected_ss, %eax
    movl %eax, diag_wanted+20
991:
    .endif
    movb $1, handler_error
step_done:
    andl $~0x100, %ss:52(%ebp)
    popal
    addl $32, %esp
    lret

    .ifdef DIAGNOSTIC
    .macro diag_line label, values, count
    movl $\label, %edx
    movb $9, %ah
    int $0x21
    movl $\values, %esi
    movl $\count, %ecx
    call diag_values
    .endm
diag_dump:
    movzbl hardware_seen, %eax
    movl %eax, diag_summary+12
    movzbl handler_error, %eax
    movl %eax, diag_summary+16
    movl trace_count, %eax
    movl %eax, diag_summary+20
    movl expected_count, %eax
    movl %eax, diag_summary+24
    movl saved_sp, %eax
    movl %eax, diag_summary+32
    diag_line diag_header, diag_summary, 9
    diag_line diag_frame_text, diag_frame, 6
    diag_line diag_wanted_text, diag_wanted, 6
    diag_line diag_range_text, diag_range, 2
    diag_line diag_time_text, diag_time, 10
    diag_line diag_irq_text, diag_irq, 5
    ret
diag_values:
    movl (%esi), %eax
    call diag_hex
    addl $4, %esi
    movb $' ', %dl
    movb $2, %ah
    int $0x21
    loop diag_values
    movl $diag_newline, %edx
    movb $9, %ah
    int $0x21
    ret
diag_hex:
    pushal
    movl %eax, %ebx
    movl $8, %ecx
992:
    roll $4, %ebx
    movb %bl, %dl
    andb $15, %dl
    addb $'0', %dl
    cmpb $'9', %dl
    jbe 993f
    addb $7, %dl
993:
    movb $2, %ah
    int $0x21
    loop 992b
    popal
    ret
diag_summary:
diag_stage: .long 0
diag_reason: .long 0
diag_error: .long 0
    .long 0, 0, 0, 0
diag_sp: .long 0
    .long 0
diag_field: .long 0
diag_frame: .space 24
diag_wanted: .long 0x8000, 0, 0, 0, 0, 0
diag_range: .long hardware_begin, hardware_done
diag_time: .space 40
diag_irq: .space 20
diag_header: .ascii "IRQ DIAG stage/reason/field/seen/error/trace/expected/sp/saved:\r\n$"
diag_frame_text: .ascii "IRQ FRAME code/ip/cs/flags/sp/ss:\r\n$"
diag_wanted_text: .ascii "IRQ WANTED code/ip/cs/rf/sp/ss:\r\n$"
diag_range_text: .ascii "IRQ HARDWARE range:\r\n$"
diag_time_text: .ascii "IRQ TIME tsc0lo/hi/tsc1lo/hi/deltalo/hi/tick0/tick1/polls/polls0:\r\n$"
diag_irq_text: .ascii "IRQ STATE vif/flags/mask/isr/irr:\r\n$"
diag_newline: .ascii "\r\n$"
    .endif

saved_sp: .long 0
expected_ip: .long 0
expected_sp: .long 0
expected_rf: .long 0
expected_count: .long 0
trace_count: .long 0
expected_cs: .word 0
expected_ss: .word 0
handler_error: .byte 0
nested: .byte 0
    .ifdef HARDWARE
hardware_mode: .byte 0
hardware_seen: .byte 0
    .endif
pass_text: .ascii "DPMI NATIVE IRQ PASS\r\n$"
fail_text: .ascii "DPMI NATIVE IRQ FAIL\r\n$"
