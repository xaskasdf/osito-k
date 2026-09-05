/* Real-mode INT 24h device errors. No keyboard input or writable fixture. */
    .section .text
    .code16
    .global _start
_start:
    cld
    pushw %cs
    popw %ds
    pushw %cs
    popw %es
    movw $0x5ff0, %sp
    movw $0x600, %bx
    movw $0x4a00, %ax
    int $0x21
    jc fail
    movw $0x3524, %ax
    int $0x21
    movw %bx, old_vector
    movw %es, old_vector + 2
    movb $1, vector_saved
    movw $0x3400, %ax
    int $0x21
    movw %bx, indos_pointer
    movw %es, indos_pointer + 2
    pushw %cs
    popw %es
    call install_handler

    movb $2, stage
    movw $3, %bx
    movw $0x20, %dx
    call set_mode
    movw $3, %cx
    movw $read_buffer, %dx
    movw $0x3f00, %ax
    call device_call
    jnc fail
    cmpw $1, calls
    jne fail
    cmpw $5, %ax
    jne fail
    call check_failed_error
    call check_idle
    cmpw $0xa5a5, read_buffer
    jne fail

    movb $3, stage
    movb $1, action
    movb $2, retries
    movw $0, calls
    movw $3, %bx
    movw $3, %cx
    movw $read_buffer, %dx
    movw $0x3f00, %ax
    call device_call
    jnc fail
    cmpw $5, %ax
    jne fail
    cmpw $3, calls
    jne fail
    call check_failed_error

    movb $4, stage
    movb $0, action
    movw $0, calls
    movw $3, %bx
    movw $3, %cx
    movw $read_buffer, %dx
    movw $0x3f00, %ax
    call device_call
    jc fail
    cmpw $3, %ax
    jne fail
    cmpw $1, calls
    jne fail
    cmpw $0xa5a5, read_buffer
    jne fail
    cmpb $0xa5, read_buffer + 2
    jne fail
    movw $0, calls
    xorw %dx, %dx
    call set_mode
    movw $read_buffer, %dx
    movw $0x3f00, %ax
    call device_call
    jc fail
    cmpw $3, %ax
    jne fail
    cmpw $3, calls
    jne fail

    movb $5, stage
    movw $0x5250, expected_device /* PRN */
    movb $1, expected_write
    movw $0, calls
    movw $4, %bx
    movw $3, %cx
    movw $write_buffer, %dx
    movw $0x4000, %ax
    call device_call
    jc fail
    cmpw $1, %ax /* Cooked output stops before Ctrl-Z. */
    jne fail
    cmpw $1, calls
    jne fail
    movw $0, calls
    movw $0x20, %dx
    call set_mode
    movw $write_buffer, %dx
    movw $0x4000, %ax
    call device_call
    jc fail
    cmpw $3, %ax
    jne fail
    cmpw $1, calls
    jne fail

    /* A completed first byte must not be replayed by later retries. */
    movb $9, action
    movw $0, calls
    xorw %dx, %dx
    call set_mode
    movw $three_bytes, %dx
    movw $0x4000, %ax
    call device_call
    jc fail
    cmpw $3, %ax
    jne fail
    cmpw $5, calls
    jne fail

    movb $6, stage
    movb $0, action
    movw $0x5541, expected_device /* AUX */
    movb $0, expected_write
    movw $0, calls
    movw $3, %bx
    movw $0x0300, %ax
    call device_call
    jc fail
    cmpb $0, %al /* Explicit ignore may leave the character unspecified. */
    jne fail
    cmpw $1, calls
    jne fail
    movb $1, expected_write
    movw $0x0400, %ax
    call device_call
    jc fail
    cmpw $2, calls
    jne fail
    movw $0x5250, expected_device
    movw $0x0500, %ax
    call device_call
    jc fail
    cmpw $3, calls
    jne fail

    movb $7, stage
    movb $4, action /* The handler's own AUX failure must not recurse. */
    movw $0, calls
    movw $4, %bx
    movw $1, %cx
    movw $write_buffer, %dx
    movw $0x4000, %ax
    call device_call
    jnc fail
    cmpw $5, %ax
    jne fail
    cmpw $1, calls
    jne fail
    call check_failed_error
    call check_idle

    movb $8, stage
    movb $5, action /* Chain through the default saved vector. */
    movw $0, calls
    movw $4, %bx
    movw $0x4000, %ax
    call device_call
    jnc fail
    cmpw $5, %ax
    jne fail
    cmpw $1, calls
    jne fail
    call check_failed_error

    movb $9, stage
    movb $6, action /* RETF is invalid here; only the request should fail. */
    movw $0, calls
    movw $4, %bx
    movw $0x4000, %ax
    call device_call
    jnc fail
    cmpw $5, %ax
    jne fail
    call check_idle

    movb $10, stage
    movb $3, action
    movw $0, calls
    movw $0xffff, %bx
    movw $1, %cx
    movw $0x3f00, %ax
    int $0x21
    jnc fail
    cmpw $6, %ax
    jne fail
    movw $4, %bx /* The standard printer handle is write-only. */
    movw $0x3f00, %ax
    int $0x21
    jnc fail
    cmpw $5, %ax
    jne fail
    movw $3, %bx
    xorw %cx, %cx
    movw $0x3f00, %ax
    int $0x21
    jc fail
    testw %ax, %ax
    jnz fail
    movw $0x4000, %ax
    int $0x21
    jc fail
    testw %ax, %ax
    jnz fail
    movw $0x4406, %ax
    int $0x21
    jc fail
    testb %al, %al
    jnz fail
    cmpw $0, calls
    jne fail

    movb $11, stage
    movb $0, check_frame
    movb $2, action
    movw %cs, parameters + 4
    call execute_child
    cmpw $0x0200, %ax
    jne fail
    call check_idle
    movb $8, action /* An unknown response has DOS's abort policy. */
    call execute_child
    cmpw $0x0200, %ax
    jne fail
    movb $7, action /* Explicit AH=4Ch keeps a normal exit status. */
    call execute_child
    cmpw $7, %ax
    jne fail

    movb $12, stage
    movb $2, action
    call execute_loaded_child
    cmpw $0x0200, %ax
    jne fail
    call check_idle

    movb $13, stage
    call restore_vector
    movw $0, calls
    movw $4, %bx
    movw $1, %cx
    movw $write_buffer, %dx
    movw $0x4000, %ax
    int $0x21
    jnc fail
    cmpw $5, %ax
    jne fail
    cmpw $0, calls
    jne fail
    call check_failed_error
    call check_idle
    movb $0, stage
fail:
    cmpb $0, vector_saved
    je report
    call restore_vector
report:
    movw $pass, %dx
    cmpb $0, stage
    jne report_failed
    cmpb $0, handler_error
    je report_now
report_failed:
    movw $failure, %dx
report_now:
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
    movw $0x1357, %si
    movw $0x2468, %di
    movw $0x369c, %bp
    movw %ax, expected_regs
    movw %bx, expected_regs + 2
    movw %cx, expected_regs + 4
    movw %dx, expected_regs + 6
    movw %si, expected_regs + 8
    movw %di, expected_regs + 10
    movw %bp, expected_regs + 12
    movw %ds, expected_regs + 14
    movw %es, expected_regs + 16
    movw %sp, expected_sp
    clc
    pushfw
    popw expected_flags
    int $0x21
after_device_int:
    pushfw
    pushw %ax
    cmpb $0, handler_error
    jne fail
    cmpw expected_regs + 2, %bx
    jne fail
    cmpw expected_regs + 4, %cx
    jne fail
    cmpw expected_regs + 6, %dx
    jne fail
    cmpw $0x1357, %si
    jne fail
    cmpw $0x2468, %di
    jne fail
    cmpw $0x369c, %bp
    jne fail
    movw %ds, %ax
    cmpw expected_regs + 14, %ax
    jne fail
    movw %es, %ax
    cmpw expected_regs + 16, %ax
    jne fail
    movw %sp, %ax
    addw $4, %ax
    cmpw expected_sp, %ax
    jne fail
    popw %ax
    popfw
    ret
set_mode:
    movw $0x4401, %ax
    int $0x21
    jc fail
    ret
check_failed_error:
    pushaw
    pushw %es
    xorw %bx, %bx
    movw $0x5900, %ax
    int $0x21
    cmpw $83, %ax
    jne fail
    cmpw $0x0d04, %bx
    jne fail
    cmpb $1, %ch
    jne fail
    popw %es
    popaw
    ret
check_idle:
    pushaw
    pushw %es
    les indos_pointer, %bx
    cmpw $0, %es:-1(%bx)
    jne fail
    popw %es
    popaw
    ret
install_handler:
    movw $handler, %dx
    movw $0x2524, %ax
    int $0x21
    ret
restore_vector:
    pushw %ds
    lds old_vector, %dx
    movw $0x2524, %ax
    int $0x21
    popw %ds
    ret
execute_child:
    pushw %cs
    popw %es
    movw $child, %dx
    movw $parameters, %bx
    movw $0x4b00, %ax
    int $0x21
    jc fail
    movw $0x4d00, %ax
    int $0x21
    ret
execute_loaded_child:
    movb $0, loaded_started
    pushw %cs
    popw %es
    movw $child, %dx
    movw $parameters, %bx
    movw $0x4b01, %ax
    int $0x21
    jc fail
    cmpb $0, loaded_started
    jne loaded_done
    movb $1, loaded_started
    movw parameters + 14, %di
    movw parameters + 16, %bp
    cli
    movw %bp, %ss
    movw %di, %sp
    sti
    ljmp *parameters + 18
loaded_done:
    movw $0x4d00, %ax
    int $0x21
    ret

handler:
    movw %sp, %cs:frame_sp
    pushaw
    pushw %ds
    pushw %es
    pushw %cs
    popw %ds
    incw calls
    movb $1, handler_error
    andb $0xb9, %ah
    subb $0xb8, %ah
    cmpb expected_write, %ah
    jne handler_failed
    cmpw $2, %di
    jne handler_failed
    pushfw
    popw %ax
    testw $0x0200, %ax
    jnz handler_failed
    movb $2, handler_error
    movw %bp, %es
    testw $0x8000, %es:4(%si)
    jz handler_failed
    movw %es:10(%si), %ax
    cmpw expected_device, %ax
    jne handler_failed
    cmpb $0, check_frame
    je frame_ok
    movb $3, handler_error
    movw %ss, %ax
    movw %ax, %es
    movw frame_sp, %bx
    movw %bx, %ax
    addw $30, %ax
    cmpw expected_sp, %ax
    jne handler_failed
    leaw 6(%bx), %di
    movw $expected_regs, %si
    movw $9, %cx
    cld
    repe cmpsw
    jne handler_failed
    cmpw $after_device_int, %ss:24(%bx)
    jne handler_failed
    movw %cs, %ax
    cmpw %ax, %ss:26(%bx)
    jne handler_failed
    movw expected_flags, %ax
    cmpw %ax, %ss:28(%bx)
    jne handler_failed
frame_ok:
    movb $4, handler_error
    les indos_pointer, %bx
    cmpb $0, %es:(%bx)
    jne handler_failed
    cmpb $1, %es:-1(%bx)
    jne handler_failed
    movb $5, handler_error
    xorw %bx, %bx
    movw $0x5900, %ax
    int $0x21
    cmpw $21, %ax
    jne handler_failed
    cmpw $0x0507, %bx
    jne handler_failed
    cmpb $4, %ch
    jne handler_failed
    cmpb $4, action
    jne no_nested
    movb $6, handler_error
    movw calls, %si
    movb $'!', %dl
    movw $0x0400, %ax
    int $0x21
    jnc handler_failed
    cmpw $21, %ax
    jne handler_failed
    cmpw calls, %si
    jne handler_failed
no_nested:
    movb $0, handler_error
handler_failed:
    popw %es
    popw %ds
    popaw
    cmpb $0, %cs:handler_error
    jne handler_fail
    movb %cs:action, %al
    cmpb $1, %al
    je handler_retry
    cmpb $4, %al
    je handler_fail
    cmpb $5, %al
    je handler_chain
    cmpb $6, %al
    je handler_bad_return
    cmpb $7, %al
    je handler_exit
    cmpb $8, %al
    je handler_unknown
    cmpb $9, %al
    je handler_partial_retry
    iret
handler_retry:
    cmpb $0, %cs:retries
    je handler_fail
    decb %cs:retries
    iret
handler_fail:
    movb $3, %al
    iret
handler_chain:
    pushfw
    lcall *%cs:old_vector
    iret
handler_bad_return:
    movb $3, %al
    lret
handler_exit:
    movw $0x4c07, %ax
    int $0x21
handler_unknown:
    movb $0x7f, %al
    iret
handler_partial_retry:
    movb $0, %al
    cmpw $2, %cs:calls
    jb partial_done
    cmpw $3, %cs:calls
    ja partial_done
    movb $1, %al
partial_done:
    iret

stage: .byte 1
action: .byte 3
retries: .byte 0
vector_saved: .byte 0
handler_error: .byte 0
loaded_started: .byte 0
check_frame: .byte 1
expected_write: .byte 0
expected_device: .word 0x5541
calls: .word 0
expected_sp: .word 0
expected_flags: .word 0
expected_regs: .space 18
frame_sp: .word 0
indos_pointer: .long 0
old_vector: .long 0
parameters: .word 0, tail, 0, 0, 0, 0, 0, 0, 0, 0, 0
tail: .byte 0, 13
child: .asciz "DCCHILD.COM"
read_buffer: .byte 0xa5, 0xa5, 0xa5
write_buffer: .byte 'A', 0x1a, 'Z'
three_bytes: .ascii "ABC"
pass: .ascii "DOS CRITICAL CONTRACT PASS\r\n$"
failure: .ascii "DOS CRITICAL CONTRACT FAIL\r\n$"
