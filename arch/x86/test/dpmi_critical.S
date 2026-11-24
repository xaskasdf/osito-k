/* INT 24h reflected through INT 31h/0300h, in USE16 and native USE32. */
    .section .text
    .code16
    .global _start
_start:
    cld
    pushw %cs
    popw %ds
    pushw %cs
    popw %es
    movw %cs, real_segment
    movw %cs, rm_regs + 34
    movw %cs, rm_regs + 36
    movw %cs, nested_regs + 48
    movw $0x5ff0, %sp
    movw $0x600, %bx
    movw $0x4a00, %ax
    int $0x21
    jc initial_fail
    movw $0x3400, %ax
    int $0x21
    movw %bx, indos_offset
    movw %es, indos_segment
    /* A real-mode hook must not override the protected client's policy. */
    movw $real_handler, %dx
    movw $0x2524, %ax
    int $0x21
    movw $0x1687, %ax
    int $0x2f
    testw %ax, %ax
    jnz initial_fail
    movw %di, entry
    movw %es, entry + 2
    .ifdef CLIENT32
    movw $1, %ax
    .else
    xorw %ax, %ax
    .endif
    lcall *entry
    jc initial_fail
    movw %ds, data_selector
    .ifdef CLIENT32
    movw %cs, %bx
    movw $6, %ax
    int $0x31
    jc initial_fail
    movw %cx, base_hi
    movw %dx, base_lo
    movw $1, %cx
    xorw %ax, %ax
    int $0x31
    jc initial_fail
    movw %ax, %bx
    movw %ax, code_pointer + 2
    movw base_hi, %cx
    movw base_lo, %dx
    movw $7, %ax
    int $0x31
    jc initial_fail
    xorw %cx, %cx
    movw $0xffff, %dx
    movw $8, %ax
    int $0x31
    jc initial_fail
    movw $0x40fa, %cx
    movw $9, %ax
    int $0x31
    jc initial_fail
    ljmp *code_pointer
    .else
    jmp protected_entry
    .endif
initial_fail:
    movw $0x4c01, %ax
    int $0x21
real_handler:
    incw %cs:real_calls
    movb $0, %al
    iret
real_io:
    int $0x21
after_real_io:
    lret
real_critical:
    .set WORD, 0x310b
    .rept 12
    pushw $WORD
    .set WORD, WORD - 1
    .endr
    int $0x24
    addw $24, %sp
    lret

    .ifdef CLIENT32
    .code32
    .set IRET_BYTES, 12
    .else
    .set IRET_BYTES, 6
    .endif
protected_entry:
    movw data_selector, %ax
    movw %ax, %es
    movw %ss, client_ss
    movw indos_segment, %bx
    movw $2, %ax
    int $0x31
    jc fail
    movw %ax, indos_selector
    movb $2, stage
    call device_call /* Unhooked is Fail, even with the real-mode hook. */
    call expect_fail
    cmpw $0, real_calls
    jne fail

    movw $0x0204, %ax
    movb $0x24, %bl
    int $0x31
    jc fail
    .ifdef CLIENT32
    movl %edx, old_vector
    movw %cx, old_vector + 4
    .else
    movw %dx, old_vector
    movw %cx, old_vector + 2
    .endif
    movw $0x3524, %ax
    int $0x21
    movw %es, %ax
    cmpw %cx, %ax
    jne fail
    .ifdef CLIENT32
    cmpl %edx, %ebx
    .else
    cmpw %dx, %bx
    .endif
    jne fail
    movb $0x24, %bl
    movw %cs, %cx
    movl $handler, %edx
    movw $0x0205, %ax
    int $0x31
    jc fail

    movb $3, stage
    call device_call
    call expect_fail
    cmpw $1, calls
    jne fail

    movb $4, stage
    movb $0, action
    call device_call
    testw $1, rm_regs + 32
    jnz fail
    cmpw $1, rm_regs + 28
    jne fail
    cmpb $0xa5, read_buffer
    jne fail

    movb $5, stage
    movb $1, action
    movb $2, retries
    movw $0, calls
    call device_call
    call expect_fail
    cmpw $3, calls
    jne fail

    movb $6, stage
    movb $2, action /* Abort must fail the call, not terminate this client. */
    call device_call
    call expect_fail

    movb $7, stage
    movb $4, action /* Chain the default protected vector. */
    call device_call
    call expect_fail
    cmpw $0, real_calls
    jne fail

    movb $8, stage
    movb $5, action /* Invalid RETF must not damage the suspended RM stack. */
    call device_call
    call expect_fail

    movb $9, stage
    movb $6, action /* Nested unavailable AUX must not recurse. */
    movw $0, calls
    call device_call
    call expect_fail
    cmpw $1, calls
    jne fail

    movb $10, stage
    movb $0x7f, action /* DOS's unknown-response Abort is also Fail in PM. */
    call device_call
    call expect_fail

    movb $11, stage
    movb $3, action
    movb $1, expected_write
    movl $4, rm_regs + 16
    movl $0x4000, rm_regs + 28
    call device_call
    call expect_fail

    movb $12, stage
    /* Restore through the DOS translator instead of INT 31h. */
    pushw %ds
    .ifdef CLIENT32
    lds old_vector, %edx
    .else
    lds old_vector, %dx
    .endif
    movw $0x2524, %ax
    int $0x21
    popw %ds
    movw $0, calls
    call device_call
    call expect_fail
    cmpw $0, calls
    jne fail
    cmpw $0, real_calls
    jne fail
    movb $13, stage
    /* The DOS vector functions share DPMI state but leave the IVT alone. */
    movw $0x0200, %ax
    movb $0x24, %bl
    int $0x31
    cmpw real_segment, %cx
    jne fail
    cmpw $real_handler, %dx
    jne fail
    pushw %ds
    pushw %cs
    popw %ds
    movl $handler, %edx
    movw $0x2524, %ax
    int $0x21
    popw %ds
    movw $0x3524, %ax
    int $0x21
    movw %es, %ax
    movw %cs, %dx
    cmpw %dx, %ax
    jne fail
    .ifdef CLIENT32
    cmpl $handler, %ebx
    .else
    cmpw $handler, %bx
    .endif
    jne fail
    call device_call
    call expect_fail
    cmpw $1, calls
    jne fail

    movb $14, stage
    movw real_segment, %ax
    movw %ax, rm_regs + 48
    movw $0x4ff0, rm_regs + 46
    call device_call
    call expect_fail

    movb $15, stage
    movw $0x0301, call_function
    movw $real_io, rm_regs + 42
    movw real_segment, %ax
    movw %ax, rm_regs + 44
    movw $after_real_io, expected_return
    call device_call
    call expect_fail

    movb $16, stage
    movb $0, expected_virtual_if
    movw $0x0900, %ax
    int $0x31
    call device_call
    call expect_fail
    movw $0x0901, %ax
    int $0x31
    movb $17, stage
    movb $0x24, %bl
    movw %cs, %cx
    movl $raw_handler, %edx
    movw $0x0205, %ax
    int $0x31
    jc fail
    movw $real_critical, rm_regs + 42
    movw $0, calls
raw_again:
    movl %esp, saved_sp
    movw data_selector, %ax
    movw %ax, %es
    movw $0x0203, rm_regs + 32
    cmpb $0, raw_if
    jne raw_if_ready
    movw $3, rm_regs + 32
raw_if_ready:
    movw $0x0301, %ax
    xorw %cx, %cx
    cmpb $0, raw_simulated
    je raw_invoke
    /* 0300h must put these DOS words directly above its six-byte IRET. */
    .set WORD, 0x310b
    .rept 12
    pushw $WORD
    .set WORD, WORD - 1
    .endr
    movw $0x0300, %ax
    movw $12, %cx
raw_invoke:
    movw $0x24, %bx
    movl $rm_regs, %edi
    int $0x31
    jc fail
    cmpb $0, raw_simulated
    je raw_unwound
    .ifdef CLIENT32
    addl $24, %esp
    .else
    addw $24, %sp
    .endif
raw_unwound:
    cmpl saved_sp, %esp
    jne fail
    cmpb $0, handler_error
    jne fail
    cmpw $0, real_calls
    jne fail
    movb raw_action, %al
    cmpb $2, %al
    jb raw_expected
    movb $3, %al
raw_expected:
    cmpb $0, raw_retf
    je raw_compare
    movb $3, %al
raw_compare:
    cmpb %al, rm_regs + 28
    jne fail
    incb raw_action
    cmpb $4, raw_action
    jb raw_again
    movb $0, raw_action
    xorb $1, raw_if
    jnz raw_again
    xorb $1, raw_simulated
    jnz raw_again
    xorb $1, raw_retf
    jnz raw_again
    cmpw $32, calls
    jne fail
    movb $0, stage
fail:
    movl $pass, %edx
    cmpb $0, stage
    jne report_failed
    cmpb $0, handler_error
    je report
report_failed:
    movl $failure, %edx
report:
    movw $0x0900, %ax
    int $0x21
    movb stage, %al
    cmpb $0, handler_error
    je exit
    movb handler_error, %al
    addb $64, %al
exit:
    movb $0x4c, %ah
    int $0x21

device_call:
    movl %esp, saved_sp
    movw data_selector, %ax
    movw %ax, %es
    movw $0x3f00, rm_regs + 28
    cmpb $0, expected_write
    je read_call
    movw $0x4000, rm_regs + 28
read_call:
    movw $0x0202, rm_regs + 32
    movw call_function, %ax
    movw $0x21, %bx
    xorw %cx, %cx
    movl $rm_regs, %edi
    int $0x31
    jc fail
    cmpl saved_sp, %esp
    jne fail
    cmpl $0x12341357, rm_regs + 4
    jne fail
    cmpl $0x56782468, rm_regs + 8
    jne fail
    cmpl $0x9abc369c, rm_regs
    jne fail
    movw real_segment, %ax
    cmpw %ax, rm_regs + 34
    jne fail
    cmpw %ax, rm_regs + 36
    jne fail
    cmpb $0, handler_error
    jne fail
    movw indos_selector, %ax
    movw %ax, %es
    movzwl indos_offset, %ebx
    cmpw $0, %es:-1(%ebx)
    jne fail
    movw data_selector, %ax
    movw %ax, %es
    movw $0x0902, %ax
    int $0x31
    cmpb expected_virtual_if, %al
    jne fail
    ret
expect_fail:
    testw $1, rm_regs + 32
    jz fail
    cmpw $5, rm_regs + 28
    jne fail
    xorw %bx, %bx
    movw $0x5900, %ax
    int $0x21
    cmpw $83, %ax
    jne fail
    cmpw $0x0d04, %bx
    jne fail
    cmpb $1, %ch
    jne fail
    ret

raw_handler:
    cmpl %esp, %ebp
    jne raw_bad_frame
    pushal
    pushw %ds
    movw %cs:data_selector, %ax
    movw %ax, %ds
    incw calls
    xorl %esi, %esi
raw_tail:
    movw %si, %ax
    addw $0x3100, %ax
    cmpw %ax, %ss:IRET_BYTES(%ebp,%esi,2)
    jne raw_bad_tail
    incl %esi
    cmpl $12, %esi
    jb raw_tail
    movw $0x0902, %ax
    int $0x31
    cmpb raw_if, %al
    je raw_done
raw_bad_tail:
    movb $5, handler_error
raw_done:
    popw %ds
    popal
    movb %cs:raw_action, %al
    cmpb $0, %cs:raw_retf
    jne handler_bad_return
    iret
raw_bad_frame:
    pushw %ds
    movw %cs:data_selector, %ax
    movw %ax, %ds
    movb $6, handler_error
    popw %ds
    movb $3, %al
    iret

handler:
    pushal
    pushw %ds
    pushw %es
    movw %cs:data_selector, %dx
    movw %dx, %ds
    leal 36(%esp), %edx
    movl %edx, frame_sp
    incw calls
    movb $1, handler_error
    movw %ss, %dx
    cmpw client_ss, %dx
    je handler_done
    cmpl frame_sp, %ebp
    jne handler_done
    andb $0xb9, %ah
    subb $0xb8, %ah
    cmpb expected_write, %ah
    jne handler_done
    cmpw $2, %di
    jne handler_done
    movb $2, handler_error
    movw $0x3f00, %ax
    cmpb $0, expected_write
    je frame_read
    movw $0x4000, %ax
frame_read:
    cmpw %ax, %ss:IRET_BYTES(%ebp)
    jne handler_done
    movw rm_regs + 16, %ax
    cmpw %ax, %ss:IRET_BYTES+2(%ebp)
    jne handler_done
    cmpw $1, %ss:IRET_BYTES+4(%ebp)
    jne handler_done
    cmpw $read_buffer, %ss:IRET_BYTES+6(%ebp)
    jne handler_done
    cmpw $0x1357, %ss:IRET_BYTES+8(%ebp)
    jne handler_done
    cmpw $0x369c, %ss:IRET_BYTES+10(%ebp)
    jne handler_done
    cmpw $0x2468, %ss:IRET_BYTES+12(%ebp)
    jne handler_done
    movw real_segment, %ax
    cmpw %ax, %ss:IRET_BYTES+14(%ebp)
    jne handler_done
    cmpw %ax, %ss:IRET_BYTES+16(%ebp)
    jne handler_done
    cmpw $0x0202, %ss:IRET_BYTES+22(%ebp)
    jne handler_done
    movw expected_return, %ax
    testw %ax, %ax
    jz frame_return_ok
    cmpw %ax, %ss:IRET_BYTES+18(%ebp)
    jne handler_done
    movw real_segment, %ax
    cmpw %ax, %ss:IRET_BYTES+20(%ebp)
    jne handler_done
frame_return_ok:
    movb $3, handler_error
    movw indos_selector, %ax
    movw %ax, %es
    movzwl indos_offset, %ebx
    cmpw $1, %es:-1(%ebx)
    jne handler_done
    movw $0x0902, %ax
    int $0x31
    testb %al, %al
    jnz handler_done
    xorw %bx, %bx
    movw $0x5900, %ax
    int $0x21
    cmpw $21, %ax
    jne handler_done
    cmpw $0x0507, %bx
    jne handler_done
    cmpb $4, %ch
    jne handler_done
    cmpb $6, action
    jne handler_ok
    movb $4, handler_error
    movw $0x0400, %ax
    int $0x21
    jnc handler_done
    cmpw $21, %ax
    jne handler_done
    movw data_selector, %ax
    movw %ax, %es
    movw $0x0300, %ax
    movw $0x21, %bx
    xorw %cx, %cx
    movl $nested_regs, %edi
    int $0x31
    jc handler_done
    testw $1, nested_regs + 32
    jz handler_done
    cmpw $21, nested_regs + 28
    jne handler_done
handler_ok:
    movb $0, handler_error
handler_done:
    popw %es
    popw %ds
    popal
    cmpb $0, %cs:handler_error
    jne handler_fail
    movb %cs:action, %al
    cmpb $1, %al
    je handler_retry
    cmpb $4, %al
    je handler_chain
    cmpb $5, %al
    je handler_bad_return
    cmpb $6, %al
    je handler_fail
    iret
handler_retry:
    cmpb $0, %cs:retries
    je handler_fail
    pushw %ds
    pushw %ax
    movw %cs:data_selector, %ax
    movw %ax, %ds
    decb retries
    popw %ax
    popw %ds
    iret
handler_chain:
    .ifdef CLIENT32
    pushfl
    .else
    pushfw
    .endif
    lcall *%cs:old_vector
    iret
handler_bad_return:
    lret
handler_fail:
    movb $3, %al
    iret

stage: .byte 1
action: .byte 3
retries: .byte 0
expected_write: .byte 0
expected_virtual_if: .byte 1
handler_error: .byte 0
raw_action: .byte 0
raw_if: .byte 0
raw_simulated: .byte 0
raw_retf: .byte 0
call_function: .word 0x0300
expected_return: .word 0
real_segment: .word 0
data_selector: .word 0
client_ss: .word 0
indos_segment: .word 0
indos_offset: .word 0
indos_selector: .word 0
real_calls: .word 0
calls: .word 0
base_hi: .word 0
base_lo: .word 0
entry: .long 0
code_pointer: .word protected_entry, 0
old_vector: .space 6
saved_sp: .long 0
frame_sp: .long 0
rm_regs:
    .long 0x9abc369c, 0x12341357, 0x56782468, 0
    .long 3, read_buffer, 1, 0x3f00
    .word 0x0202, 0, 0, 0, 0, 0, 0, 0, 0
nested_regs:
    .long 0, 0, 0, 0, 0, 0x21, 0, 0x0400
    .word 0x0202, 0, 0, 0, 0, 0, 0, 0x3ff0, 0
read_buffer: .byte 0xa5
    .ifdef CLIENT32
pass: .ascii "DPMI32 CRITICAL CONTRACT PASS\r\n$"
failure: .ascii "DPMI32 CRITICAL CONTRACT FAIL\r\n$"
    .else
pass: .ascii "DPMI16 CRITICAL CONTRACT PASS\r\n$"
failure: .ascii "DPMI16 CRITICAL CONTRACT FAIL\r\n$"
    .endif
