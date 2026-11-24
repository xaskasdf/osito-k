/* INT 23h real-mode continuation, abort, redirection and EXEC contracts.
 * Send Ctrl-C after RAW READY, EXTENDED READY and EOF READY. */
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
    movw $0x3523, %ax
    int $0x21
    movw %bx, old_vector
    movw %es, old_vector + 2
    movb $1, vector_saved
    pushw %cs
    popw %es
    call install_handler
    xorw %bx, %bx
    movw $0x4500, %ax
    int $0x21
    jc fail
    movw %ax, saved_stdin
    movw $file_name, %dx
    xorw %cx, %cx
    movw $0x5b00, %ax
    int $0x21
    jc fail
    movw %ax, file_handle
    movb $1, file_owned
    movw %ax, %bx
    movw $input, %dx
    movw $input_end-input, %cx
    movw $0x4000, %ax
    int $0x21
    jc fail
    cmpw $input_end-input, %ax
    jne fail
    xorw %cx, %cx
    xorw %dx, %dx
    movw $0x4200, %ax
    int $0x21
    jc fail
    xorw %cx, %cx
    movw $0x4600, %ax
    int $0x21
    jc fail

    movb $2, stage
    movb $0xff, %dl
    movw $0x0600, %ax
    int $0x21
    jz fail
    cmpb $3, %al
    jne fail
    movw $0x0700, %ax
    int $0x21
    cmpb $3, %al
    jne fail
    cmpw $0, calls
    jne fail

    movb $3, stage
    movw %sp, saved_sp
    movw $0x1234, %bx
    movw $0x5678, %cx
    movw $0x9abc, %dx
    movw $0x0800, %ax
    int $0x21
    cmpb $'A', %al
    jne fail
    cmpw $1, calls
    jne fail
    cmpw saved_sp, %sp
    jne fail
    cmpw $0x1234, %bx
    jne fail
    cmpw $0x5678, %cx
    jne fail
    cmpw $0x9abc, %dx
    jne fail
    cmpb $0, handler_error
    jne fail

    movb $4, stage
    movb $1, mode
    movw $0x0800, %ax
    int $0x21
    cmpb $'B', %al
    jne fail
    cmpw $2, calls
    jne fail
    cmpw saved_sp, %sp
    jne fail

    movb $5, stage
    movb $2, mode
    movw $0x0800, %ax
    int $0x21
    cmpb $2, %al /* Handler selects AH=19h instead of repeating AH=08h. */
    jne fail
    movw $0x0700, %ax
    int $0x21
    cmpb $'C', %al
    jne fail

    movb $6, stage
    movb $0, mode
    movw $line, %dx
    movw $0x0a00, %ax
    int $0x21
    cmpb $1, line + 1
    jne fail
    cmpw $0x0d62, line + 2
    jne fail
    cmpw $4, calls
    jne fail
    movw $0x0b00, %ax
    int $0x21
    cmpb $0xff, %al
    jne fail
    cmpw $5, calls
    jne fail
    movw $0x0700, %ax
    int $0x21
    cmpb $'Z', %al
    jne fail

    /* Each child sees Ctrl-C before it can read X. Its exit must leave the
     * shared position and parent's suspended INT 21h frame intact. */
    movb $7, stage
    movw %cs, parameters + 4
    movb $3, mode
    call execute_child
    cmpw $0x0100, %ax
    jne fail
    call consume_x
    movb $4, mode
    call execute_child
    cmpw $0x0100, %ax
    jne fail
    call consume_x
    movb $5, mode
    call execute_child
    cmpw $7, %ax
    jne fail
    call consume_x
    call restore_vector
    call execute_child
    cmpw $0x0100, %ax
    jne fail
    call consume_x
    call install_handler

    movb $12, stage
    movb $5, mode
    call execute_loaded_child
    cmpw $7, %ax
    jne fail
    call consume_x
    movb $3, mode
    call execute_loaded_child
    cmpw $0x0100, %ax
    jne fail
    call consume_x

    movb $8, stage
    movw $raw_ready, %dx
    call announce
    call wait_control
    movw calls, %si
    movw $0x3000, %ax /* BREAK OFF does not poll ordinary disk services. */
    int $0x21
    cmpw calls, %si
    jne fail
    movw $0x0600, %ax
    movb $0xff, %dl /* stdin is still a file at EOF. */
    int $0x21
    jnz fail
    xorw %ax, %ax
    int $0x16 /* Raw file I/O must leave the physical Ctrl-C alone. */
    cmpb $3, %al
    jne fail

    movb $9, stage
    movb $0, mode
    movw $extended_ready, %dx
    call announce
    call wait_control
    movw $0x3301, %ax
    movb $1, %dl
    int $0x21
    movw calls, %si
    incw %si
    movw $0x3000, %ax
    int $0x21
    cmpw calls, %si
    jne fail
    movw $0x3301, %ax
    xorb %dl, %dl
    int $0x21

    movb $10, stage
    movb $2, mode
    movw $eof_ready, %dx
    call announce
    movw $0x0800, %ax /* Ctrl-C must cancel the redirected EOF wait. */
    int $0x21
    cmpb $2, %al
    jne fail
    cmpw saved_sp, %sp
    jne fail
    movb $0, stage
fail:
    movw $0x3301, %ax
    xorb %dl, %dl
    int $0x21
    movw saved_stdin, %bx
    cmpw $0xffff, %bx
    je close_input
    xorw %cx, %cx
    movw $0x4600, %ax
    int $0x21
    call cleanup_error
    movw $0x3e00, %ax
    int $0x21
    call cleanup_error
close_input:
    movw file_handle, %bx
    cmpw $0xffff, %bx
    je remove_input
    movw $0x3e00, %ax
    int $0x21
    call cleanup_error
remove_input:
    cmpb $0, file_owned
    je restore_handler
    movw $file_name, %dx
    movw $0x4100, %ax
    int $0x21
    call cleanup_error
restore_handler:
    cmpb $0, vector_saved
    je report
    call restore_vector
report:
    movw $pass, %dx
    cmpb $0, stage
    je report_now
    movw $failure, %dx
report_now:
    call announce
    movb stage, %al
    movb $0x4c, %ah
    int $0x21

handler:
    pushaw
    pushw %ds
    pushw %es
    pushw %cs
    popw %ds
    incw calls
    movw $0x3400, %ax
    int $0x21
    cmpb $0, %es:(%bx)
    je handler_indos_ok
    movb $1, handler_error
handler_indos_ok:
    movw $0x3000, %ax
    int $0x21
    popw %es
    popw %ds
    popaw
    cmpb $1, %cs:mode
    je ret_clear
    cmpb $2, %cs:mode
    je new_service
    cmpb $3, %cs:mode
    je ret_abort
    cmpb $4, %cs:mode
    je chain
    cmpb $5, %cs:mode
    je exit_handler
    iret
ret_clear:
    clc
    lret
ret_abort:
    stc
    lret
new_service:
    movw $0x1900, %ax
    iret
chain:
    ljmp *%cs:old_vector
exit_handler:
    movw $0x4c07, %ax
    int $0x21
install_handler:
    movw $handler, %dx
    movw $0x2523, %ax
    int $0x21
    ret
restore_vector:
    pushw %ds
    lds old_vector, %dx
    movw $0x2523, %ax
    int $0x21
    popw %ds
    ret
execute_child:
    movw $child, %dx
    movw $parameters, %bx
    movw $0x4b00, %ax
    int $0x21
    jc child_failed
    movw $0x4d00, %ax
    int $0x21
    ret
child_failed:
    popw %ax
    jmp fail
consume_x:
    movw $0x0700, %ax
    int $0x21
    cmpb $'X', %al
    jne child_failed
    ret
execute_loaded_child:
    movb $0, loaded_started
    pushw %cs
    popw %es
    movw $child, %dx
    movw $parameters, %bx
    movw $0x4b01, %ax
    int $0x21
loaded_return:
    jc child_failed
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
announce:
    movw $0x0900, %ax
    int $0x21
    ret
wait_control:
    movw $0x0100, %ax
    int $0x16
    jz wait_control
    cmpb $3, %al
    jne child_failed
    ret
cleanup_error:
    jnc cleanup_done
    cmpb $0, stage
    jne cleanup_done
    movb $11, stage
cleanup_done:
    ret
old_vector: .long 0
vector_saved: .byte 0
file_owned: .byte 0
stage: .byte 1
mode: .byte 0
handler_error: .byte 0
loaded_started: .byte 0
calls: .word 0
saved_sp: .word 0
saved_stdin: .word 0xffff
file_handle: .word 0xffff
parameters: .word 0, tail, 0, 0, 0, 0, 0, 0, 0, 0, 0
tail: .byte 0, 13
file_name: .asciz "DBREAK.TMP"
child: .asciz "DBCHILD.COM"
input: .byte 3, 3, 3, 'A', 3, 'B', 3, 'C', 'a', 3, 'b', 13, 3, 'Z'
    .byte 3, 'X', 3, 'X', 3, 'X', 3, 'X'
    .byte 3, 'X', 3, 'X'
input_end:
line: .byte 8, 0
    .space 8
raw_ready: .ascii "DOS BREAK RAW READY\r\n$"
extended_ready: .ascii "DOS BREAK EXTENDED READY\r\n$"
eof_ready: .ascii "DOS BREAK EOF READY\r\n$"
pass: .ascii "DOS BREAK CONTRACT PASS\r\n$"
failure: .ascii "DOS BREAK CONTRACT FAIL\r\n$"
