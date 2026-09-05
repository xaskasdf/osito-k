/* IOCTL/CON integration probe. Feed bytes after each READY marker:
 * RAW: A CR 1A 03 00 Z; COOKED: A B BS C CR; EOF: 1A CR.
 * The scratch file is CREATE_NEW and is deleted only when owned by this run.
 */
    .section .text
    .code16
    .global _start

_start:
    pushw %cs
    popw %ds
    pushw %cs
    popw %es
    movw $0x3ff0, %sp
    movw $0x0400, %bx
    movw $0x4a00, %ax
    int $0x21
    jc fail
    movb $2, failure_code

    /* Disk status is position-sensitive, but a query must not advance it. */
    movw $file_name, %dx
    xorw %cx, %cx
    movw $0x5b00, %ax
    int $0x21
    jc fail
    movw %ax, file_handle
    movb $1, file_owned
    movw %ax, %bx
    movw $0x4400, %ax
    int $0x21
    jc fail
    andw $0xff, %dx
    cmpw $0x42, %dx
    jne fail
    movw $0x4406, %ax
    int $0x21
    jc fail
    testb %al, %al
    jnz fail
    movw $file_data, %dx
    movw $4, %cx
    movw $0x4000, %ax
    int $0x21
    jc fail
    cmpw $4, %ax
    jne fail
    movw $0x4400, %ax
    int $0x21
    jc fail
    cmpb $2, %dl
    jne fail
    xorw %cx, %cx
    xorw %dx, %dx
    movw $0x4200, %ax
    int $0x21
    jc fail
    movw $0x4406, %ax
    int $0x21
    jc fail
    cmpb $0xff, %al
    jne fail
    movw $read_buffer, %dx
    movw $4, %cx
    movw $0x3f00, %ax
    int $0x21
    jc fail
    cmpw $4, %ax
    jne fail
    cmpw $0x1a41, read_buffer
    jne fail
    cmpw $0x4342, read_buffer + 2
    jne fail
    movw $0x4406, %ax
    int $0x21
    jc fail
    testb %al, %al
    jnz fail
    movw $0x4407, %ax
    int $0x21
    jc fail
    cmpb $0xff, %al
    jne fail
    movw $0x440a, %ax
    int $0x21
    jc fail
    testw $0x8000, %dx
    jnz fail

    /* Device control strings are unsupported, not silently accepted. */
    movb $3, failure_code
    movw $0xffff, %bx
    movw $0x4406, %ax
    int $0x21
    jnc fail
    cmpw $6, %ax
    jne fail
    movw $1, %bx
    movw $0x4402, %ax
    int $0x21
    jnc fail
    cmpw $1, %ax
    jne fail

    /* NUL is ready because its EOF read and discarded write never block. */
    movb $4, failure_code
    movw $nul_name, %dx
    movw $0x3d02, %ax
    int $0x21
    jc fail
    movw %ax, %bx
    movw $0x4406, %ax
    int $0x21
    jc fail
    cmpb $0xff, %al
    jne fail
    movw $0x4407, %ax
    int $0x21
    jc fail
    cmpb $0xff, %al
    jne fail
    movw $0x3e00, %ax
    int $0x21
    jc fail

    /* No host UART/printer backend is exposed by DOS. */
    movb $5, failure_code
    movw $aux_name, %dx
    movw $0x3d02, %ax
    int $0x21
    jc fail
    movw %ax, %bx
    movw $0x4406, %ax
    int $0x21
    jc fail
    testb %al, %al
    jnz fail
    movw $0x4407, %ax
    int $0x21
    jc fail
    testb %al, %al
    jnz fail
    movw $read_buffer, %dx
    movw $1, %cx
    movw $0x3f00, %ax
    int $0x21
    jnc fail
    cmpw $21, %ax
    jne fail
    movw %bx, %si
    xorw %bx, %bx
    movw $0x5900, %ax
    int $0x21
    cmpw $21, %ax
    jne fail
    cmpb $4, %ch
    jne fail
    movw %si, %bx
    movw $0x3e00, %ax
    int $0x21
    jc fail
    movw $4, %bx
    movw $read_buffer, %dx
    movw $1, %cx
    movw $0x4000, %ax
    int $0x21
    jnc fail
    cmpw $21, %ax
    jne fail

    /* Independent opens have independent mode, duplicates share it. */
    movb $6, failure_code
    movw $con_name, %dx
    movw $0x3d02, %ax
    int $0x21
    jc fail
    movw %ax, con_handle
    movw %ax, %bx
    movw $0x4400, %ax
    int $0x21
    jc fail
    cmpw $0x80d3, %dx
    jne fail
    movw $0x4500, %ax
    int $0x21
    jc fail
    movw %ax, duplicate_handle
    movw %ax, %bx
    movw $0xf3, %dx
    movw $0x4401, %ax
    int $0x21
    jc fail
    movw con_handle, %bx
    movw $0x4400, %ax
    int $0x21
    jc fail
    cmpw $0x80f3, %dx
    jne fail
    movw $0x80d3, %dx
    movw $0x4401, %ax
    int $0x21
    jnc fail
    cmpw $13, %ax
    jne fail
    movw $0x4400, %ax
    int $0x21
    cmpw $0x80f3, %dx
    jne fail
    movw $1, %bx
    movw $0x4400, %ax
    int $0x21
    cmpw $0x80d3, %dx
    jne fail
    movw con_handle, %bx
    movw $0xd3, %dx
    movw $0x4401, %ax
    int $0x21
    jc fail
    movw $control_output, %dx
    movw $4, %cx
    movw $0x4000, %ax
    int $0x21
    jc fail
    cmpw $1, %ax
    jne fail

    /* The child's change must survive restoring the parent execution state. */
    movb $7, failure_code
    movw con_handle, %bx
    xorw %cx, %cx
    movw $0x4600, %ax
    int $0x21
    jc fail
    movw %cs, %ax
    movw %ax, exec_parameters + 4
    movw $exec_parameters, %bx
    movw $child_name, %dx
    movw $0x4b00, %ax
    int $0x21
    jc fail
    movw $0x4d00, %ax
    int $0x21
    testw %ax, %ax
    jnz fail
    movw con_handle, %bx
    movw $0x4400, %ax
    int $0x21
    jc fail
    cmpw $0x80f3, %dx
    jne fail

    movb $8, failure_code
    movw $raw_ready, %dx
    movw $0x0900, %ax
    int $0x21
wait_raw:
    movw con_handle, %bx
    movw $0x4406, %ax
    int $0x21
    jc fail
    testb %al, %al
    jz wait_raw
    movw $0x4406, %ax
    int $0x21
    jc fail
    cmpb $0xff, %al
    jne fail
    movw $0x0800, %ax
    int $0x21
    cmpb $'A', %al
    jne fail
    movw con_handle, %bx
    movw $read_buffer, %dx
    movw $5, %cx
    movw $0x3f00, %ax
    int $0x21
    jc fail
    cmpw $5, %ax
    jne fail
    cmpw $0x1a0d, read_buffer
    jne fail
    cmpw $0x0003, read_buffer + 2
    jne fail
    cmpb $'Z', read_buffer + 4
    jne fail
    movw $control_output, %dx
    movw $4, %cx
    movw $0x4000, %ax
    int $0x21
    jc fail
    cmpw $4, %ax
    jne fail

    movb $9, failure_code
    movw con_handle, %bx
    movw $0xd3, %dx
    movw $0x4401, %ax
    int $0x21
    jc fail
    movw $cooked_ready, %dx
    movw $0x0900, %ax
    int $0x21
    movw con_handle, %bx
    movw $read_buffer, %dx
    movw $1, %cx
    movw $0x3f00, %ax
    int $0x21
    jc fail
    cmpw $1, %ax
    jne fail
    cmpb $'A', read_buffer
    jne fail
    movw duplicate_handle, %bx
    movw $read_buffer, %dx
    movw $2, %cx
    movw $0x3f00, %ax
    int $0x21
    jc fail
    cmpw $2, %ax
    jne fail
    cmpw $0x0d43, read_buffer
    jne fail
    movw $1, %cx
    movw $0x3f00, %ax
    int $0x21
    jc fail
    cmpw $1, %ax
    jne fail
    cmpb $0x0a, read_buffer
    jne fail

    movb $10, failure_code
    movw $eof_ready, %dx
    movw $0x0900, %ax
    int $0x21
    movw con_handle, %bx
    movw $read_buffer, %dx
    movw $4, %cx
    movw $0x3f00, %ax
    int $0x21
    jc fail
    testw %ax, %ax
    jnz fail
    movw duplicate_handle, %bx
    movw $0x4400, %ax
    int $0x21
    jc fail
    testb $0x40, %dl
    jnz fail
    movw $read_buffer, %dx
    movw $4, %cx
    movw $0x3f00, %ax
    int $0x21
    jc fail
    testw %ax, %ax
    jnz fail
    xorw %cx, %cx
    movw $0x4000, %ax
    int $0x21
    jc fail
    movw con_handle, %bx
    movw $0x4400, %ax
    int $0x21
    jc fail
    testb $0x40, %dl
    jz fail

    movw $pass_message, %dx
    movw $0x0900, %ax
    int $0x21
    movb $0, failure_code
    jmp cleanup
fail:
    movw $fail_message, %dx
    movw $0x0900, %ax
    int $0x21
cleanup:
    cmpb $0, file_owned
    je exit
    movw file_handle, %bx
    movw $0x3e00, %ax
    int $0x21
    movw $file_name, %dx
    movw $0x4100, %ax
    int $0x21
exit:
    movb failure_code, %al
    movb $0x4c, %ah
    int $0x21

failure_code: .byte 1
file_owned: .byte 0
file_handle: .word 0
con_handle: .word 0
duplicate_handle: .word 0
exec_parameters:
    .word 0, command_tail, 0, 0, 0, 0, 0
command_tail: .byte 0, 13
file_name: .asciz "IOCTEST.TMP"
child_name: .asciz "IOCHILD.COM"
con_name: .asciz "CON:"
nul_name: .asciz "NUL"
aux_name: .asciz "COM1"
file_data: .byte 'A', 0x1a, 'B', 'C'
control_output: .byte 'O', 0x1a, '!', '\n'
read_buffer: .space 16, 0x5a
raw_ready: .ascii "DOS IOCTL RAW READY\r\n$"
cooked_ready: .ascii "DOS IOCTL COOKED READY\r\n$"
eof_ready: .ascii "DOS IOCTL EOF READY\r\n$"
pass_message: .ascii "DOS IOCTL CONTRACT PASS\r\n$"
fail_message: .ascii "DOS IOCTL CONTRACT FAIL\r\n$"
