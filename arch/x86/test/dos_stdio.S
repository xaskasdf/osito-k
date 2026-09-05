/* Legacy DOS standard-handle routing, byte-exact output, and EXEC probe.
 * Feed K, J, R, and G CR after the respective KEYBOARD/FLUSH/ECHO/CON READY
 * markers. Fixtures use CREATE_NEW; cleanup touches only owned files.
 */
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
    movw $0x0600, %bx
    movw $0x4a00, %ax
    int $0x21
    jc fail
    xorw %si, %si
save_handles:
    movw %si, %bx
    shrw $1, %bx
    movw $0x4500, %ax
    int $0x21
    jc fail
    movw %ax, saved_handles(%si)
    addw $2, %si
    cmpw $10, %si
    jb save_handles

    movw $input_name, %dx
    xorw %cx, %cx
    movw $0x5b00, %ax
    int $0x21
    jc fail
    movw %ax, input_handle
    movb $1, input_owned
    movw %ax, %bx
    movw $input_data, %dx
    movw $input_end - input_data, %cx
    movw $0x4000, %ax
    int $0x21
    jc fail
    cmpw $input_end - input_data, %ax
    jne fail
    call rewind_input
    jc fail
    movw $output_name, %dx
    xorw %cx, %cx
    movw $0x5b00, %ax
    int $0x21
    jc fail
    movw %ax, output_handle
    movb $1, output_owned
    movw %ax, %bx
    movw $1, %cx
    movw $0x4600, %ax
    int $0x21
    jc fail

    /* Check output first so an uncorrected kernel fails without blocking. */
    movb $2, failure_code
    movw $initial_text, %dx
    movw $0x0900, %ax
    int $0x21
    cmpb $'$', %al
    jne fail
    movb $'!', %dl
    movw $0x0200, %ax
    int $0x21
    cmpb $'!', %al
    jne fail
    movb $9, %dl
    movw $0x0600, %ax
    int $0x21
    cmpb $9, %al
    jne fail
    movw output_handle, %bx
    call rewind
    jc fail
    movw $capture, %dx
    movw $5, %cx
    movw $0x3f00, %ax
    int $0x21
    jc fail
    cmpw $5, %ax
    jne fail
    movw $capture, %si
    movw $expected_output, %di
    movw $5, %cx
    repe cmpsb
    jne fail

    movb $3, failure_code
    movw input_handle, %bx
    xorw %cx, %cx
    movw $0x4600, %ax
    int $0x21
    jc fail
    movw $0x0b00, %ax
    int $0x21
    cmpb $0xff, %al
    jne fail
    movw $0x0b00, %ax
    int $0x21
    cmpb $0xff, %al
    jne fail
    movb $0xff, %dl
    movw $0x0600, %ax
    int $0x21
    jz fail
    cmpb $'A', %al
    jne fail
    movw $0x0700, %ax
    int $0x21
    testb %al, %al
    jnz fail
    movw $0x0800, %ax
    int $0x21
    cmpb $'B', %al
    jne fail
    movw $0x0100, %ax
    int $0x21
    cmpb $0x1a, %al
    jne fail
    movw $0x0c07, %ax
    int $0x21
    cmpb $'D', %al
    jne fail

    movb $4, failure_code
    movw $line_buffer, %dx
    movw $0x0a00, %ax
    int $0x21
    cmpb $2, line_buffer + 1
    jne fail
    cmpw $0x4361, line_buffer + 2
    jne fail
    cmpb $13, line_buffer + 4
    jne fail
    movb $2, line_buffer
    movw $0x0a00, %ax
    int $0x21
    cmpb $1, line_buffer + 1
    jne fail
    cmpw $0x0d45, line_buffer + 2
    jne fail
    cmpb $0xa5, line_guard
    jne fail
    movw $0x0b00, %ax
    int $0x21
    testb %al, %al
    jnz fail
    movb $0xff, %dl
    movw $0x0600, %ax
    int $0x21
    jnz fail
    testb %al, %al
    jnz fail
    movw input_handle, %bx
    xorw %cx, %cx
    xorw %dx, %dx
    movw $0x4201, %ax
    int $0x21
    jc fail
    cmpw $input_end - input_data, %ax
    jne fail

    /* AUX and PRN are standard handles too, not fixed physical routes. */
    movb $5, failure_code
    call rewind_input
    jc fail
    movw input_handle, %bx
    movw $3, %cx
    movw $0x4600, %ax
    int $0x21
    jc fail
    movw $0x0300, %ax
    int $0x21
    cmpb $'A', %al
    jne fail
    movw output_handle, %bx
    movw $3, %cx
    movw $0x4600, %ax
    int $0x21
    jc fail
    movw $4, %cx
    movw $0x4600, %ax
    int $0x21
    jc fail
    movb $9, %dl
    movw $0x0400, %ax
    int $0x21
    movb $'#', %dl
    movw $0x0500, %ax
    int $0x21
    movb $13, %dl
    movw $0x0200, %ax
    int $0x21
    movb $'T', %dl
    movw $0x0200, %ax
    int $0x21
    movb $9, %dl
    movw $0x0200, %ax
    int $0x21
    cmpb $' ', %al
    jne fail
    movb $'.', %dl
    movw $0x0200, %ax
    int $0x21

    movb $6, failure_code
    call rewind_input
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
    movw $0x0800, %ax
    int $0x21
    cmpb $'B', %al
    jne fail

    /* A file flush must leave the physical and VM keyboard queues alone. */
    movb $7, failure_code
    movw $keyboard_ready, %dx
    movw $keyboard_end - keyboard_ready, %cx
    call announce
wait_key:
    movw $0x0100, %ax
    int $0x16
    jz wait_key
    cmpb $'K', %al
    jne fail
    movb $0xff, %dl
    movw $0x0c06, %ax
    int $0x21
    jz fail
    cmpb $0x1a, %al
    jne fail
    movw $0x0c55, %ax
    int $0x21
    testb %al, %al
    jnz fail
    movw $0x0100, %ax
    int $0x16
    jz fail
    cmpb $'K', %al
    jne fail
    xorw %ax, %ax
    int $0x16
    cmpb $'K', %al
    jne fail

    /* Flush does empty the VM keyboard queue when stdin is CON. */
    movb $8, failure_code
    movw saved_handles, %bx
    xorw %cx, %cx
    movw $0x4600, %ax
    int $0x21
    jc fail
    movw $flush_ready, %dx
    movw $flush_end - flush_ready, %cx
    call announce
wait_flush:
    movw $0x0100, %ax
    int $0x16
    jz wait_flush
    cmpb $'J', %al
    jne fail
    movw $0x0c55, %ax
    int $0x21
    testb %al, %al
    jnz fail
    movw $0x0100, %ax
    int $0x16
    jnz fail
    movw $echo_ready, %dx
    movw $echo_end - echo_ready, %cx
    call announce
    movw $0x0100, %ax
    int $0x21
    cmpb $'R', %al
    jne fail

    /* An explicit cooked CON read uses that device for input AND echo. */
    movb $9, failure_code
    movw input_handle, %bx
    xorw %cx, %cx
    movw $0x4600, %ax
    int $0x21
    jc fail
    movw $con_name, %dx
    movw $0x3d00, %ax
    int $0x21
    jc fail
    movw %ax, con_handle
    movw $con_ready, %dx
    movw $con_end - con_ready, %cx
    call announce
    movw con_handle, %bx
    movw $capture, %dx
    movw $2, %cx
    movw $0x3f00, %ax
    int $0x21
    jc fail
    cmpw $2, %ax
    jne fail
    cmpw $0x0d47, capture
    jne fail
    movw $1, %cx
    movw $0x3f00, %ax
    int $0x21
    jc fail
    cmpw $1, %ax
    jne fail
    cmpb $10, capture
    jne fail
    movw $0x3e00, %ax
    int $0x21
    jc fail
    movw $0xffff, con_handle

    movb $10, failure_code
    movw output_handle, %bx
    call rewind
    jc fail
    movw $capture, %dx
    movw $128, %cx
    movw $0x3f00, %ax
    int $0x21
    jc fail
    cmpw $expected_end - expected_output, %ax
    jne bad_capture
    movw %ax, captured_count
    movw %ax, %cx
    movw $capture, %si
    movw $expected_output, %di
    repe cmpsb
    jne dump_capture

    movb $12, failure_code
    movw $nul_name, %dx
    movw $0x3d02, %ax
    int $0x21
    jc fail
    movw %ax, con_handle
    movw %ax, %bx
    xorw %cx, %cx
    movw $0x4600, %ax
    int $0x21
    jc fail
    movw $1, %cx
    movw $0x4600, %ax
    int $0x21
    jc fail
    movw $0x0b00, %ax
    int $0x21
    cmpb $0xff, %al
    jne fail
    movb $0xff, %dl
    movw $0x0600, %ax
    int $0x21
    jz fail /* NUL is ready; the returned character is not specified. */
    movw $initial_text, %dx
    movw $0x0900, %ax
    int $0x21
    movb $'!', %dl
    movw $0x0200, %ax
    int $0x21
    movw output_handle, %bx
    xorw %cx, %cx
    xorw %dx, %dx
    movw $0x4202, %ax
    int $0x21
    jc fail
    cmpw $expected_end - expected_output, %ax
    jne fail

    /* OsitoK reports unavailable legacy I/O through CF/AX and AH=59h;
     * this is not a test of DOS's still-missing INT 24h callback protocol. */
    movb $13, failure_code
    movw $1, %bx
    movw $0x3e00, %ax
    int $0x21
    jc fail
    movb $'!', %dl
    movw $0x0200, %ax
    clc
    int $0x21
    jnc fail
    cmpw $6, %ax
    jne fail
    xorw %bx, %bx
    movw $0x5900, %ax
    int $0x21
    cmpw $6, %ax
    jne fail
    movw output_handle, %bx
    movw $1, %cx
    movw $0x4600, %ax
    int $0x21
    jc fail
    movw $input_name, %dx
    movw $0x3d00, %ax
    int $0x21
    jc fail
    movw %ax, readonly_handle
    movw %ax, %bx
    movw $1, %cx
    movw $0x4600, %ax
    int $0x21
    jc fail
    movb $'!', %dl
    movw $0x0200, %ax
    clc
    int $0x21
    jnc fail
    cmpw $5, %ax
    jne fail
    xorw %bx, %bx
    movw $0x3e00, %ax
    int $0x21
    jc fail
    movw $0x0700, %ax
    clc
    int $0x21
    jnc fail
    cmpw $6, %ax
    jne fail
    movw saved_handles + 6, %bx
    movw $3, %cx
    movw $0x4600, %ax
    int $0x21
    jc fail
    movw saved_handles + 8, %bx
    movw $4, %cx
    movw $0x4600, %ax
    int $0x21
    jc fail
    movw $0x0300, %ax
    clc
    int $0x21
    jnc fail
    cmpw $21, %ax
    jne fail
    movb $'!', %dl
    movw $0x0400, %ax
    clc
    int $0x21
    jnc fail
    cmpw $21, %ax
    jne fail
    movw $0x0500, %ax
    clc
    int $0x21
    jnc fail
    cmpw $21, %ax
    jne fail
    xorw %bx, %bx
    movw $0x5900, %ax
    int $0x21
    cmpw $21, %ax
    jne fail
    cmpb $4, %ch
    jne fail
    movb $0, failure_code
    jmp fail
bad_capture:
    movw %ax, captured_count
dump_capture:
    movw $capture, %si
    movw $hex_capture, %di
    movw captured_count, %cx
    cmpw $128, %cx
    jbe dump_next
    movw $128, %cx
dump_next:
    jcxz dump_done
    lodsb
    movb %al, %dl
    shrb $4, %al
    xorb %ah, %ah
    movw %ax, %bx
    movb hex_digits(%bx), %al
    stosb
    movb %dl, %al
    andb $15, %al
    movw %ax, %bx
    movb hex_digits(%bx), %al
    stosb
    movb $' ', %al
    stosb
    loop dump_next
dump_done:
    movb $13, %al
    stosb
    movb $10, %al
    stosb
    movw %di, %cx
    subw $hex_capture, %cx
    movw $hex_capture, %dx
    call announce
fail:
    /* Restore every standard handle before releasing/deleting fixtures. */
    xorw %si, %si
restore_handles:
    movw saved_handles(%si), %bx
    cmpw $0xffff, %bx
    je next_restore
    movw %si, %cx
    shrw $1, %cx
    movw $0x4600, %ax
    int $0x21
    call cleanup_error
    movw $0x3e00, %ax
    int $0x21
    call cleanup_error
next_restore:
    addw $2, %si
    cmpw $10, %si
    jb restore_handles
    movw con_handle, %bx
    call close_fixture
    movw input_handle, %bx
    call close_fixture
    movw output_handle, %bx
    call close_fixture
    movw readonly_handle, %bx
    call close_fixture
    cmpb $0, input_owned
    je skip_input
    movw $input_name, %dx
    movw $0x4100, %ax
    int $0x21
    call cleanup_error
skip_input:
    cmpb $0, output_owned
    je report
    movw $output_name, %dx
    movw $0x4100, %ax
    int $0x21
    call cleanup_error
report:
    movw $pass_message, %dx
    movw $pass_end - pass_message, %cx
    cmpb $0, failure_code
    je show_report
    movw $fail_message, %dx
    movw $fail_end - fail_message, %cx
show_report:
    call announce
    movb failure_code, %al
    movb $0x4c, %ah
    int $0x21

announce:
    movw $2, %bx
    movw $0x4000, %ax
    int $0x21
    ret
rewind_input:
    movw input_handle, %bx
rewind:
    xorw %cx, %cx
    xorw %dx, %dx
    movw $0x4200, %ax
    int $0x21
    ret
close_fixture:
    cmpw $0xffff, %bx
    je close_done
    movw $0x3e00, %ax
    int $0x21
    call cleanup_error
close_done:
    ret
cleanup_error:
    jnc cleanup_done
    cmpb $0, failure_code
    jne cleanup_done
    movb $11, failure_code
cleanup_done:
    ret

failure_code: .byte 1
input_owned: .byte 0
output_owned: .byte 0
input_handle: .word 0xffff
output_handle: .word 0xffff
con_handle: .word 0xffff
readonly_handle: .word 0xffff
saved_handles: .word 0xffff, 0xffff, 0xffff, 0xffff, 0xffff
exec_parameters: .word 0, command_tail, 0, 0, 0, 0, 0
command_tail: .byte 0, 13
input_name: .asciz "DSTIN.TMP"
output_name: .asciz "DSTOUT.TMP"
child_name: .asciz "DSCCHILD.COM"
con_name: .asciz "CON"
nul_name: .asciz "NUL"
input_data: .byte 'A', 0, 'B', 0x1a, 'D', 'a', 'b', 8, 'C', 13, 10, 'E', 'F', 13
input_end:
initial_text: .byte 'H', 0x1a, 'i', '$'
expected_output:
    .byte 'H', 0x1a, 'i', '!', 9, 0x1a
    .ascii "ab\b \bC\rE"
    .byte 7, 13
    .byte 9, '#', 13
    .ascii "T       .child:"
    .byte 0, 'R'
expected_end:
line_buffer: .byte 8, 0
    .space 8, 0x5a
line_guard: .byte 0xa5
capture: .space 128, 0x5a
captured_count: .word 0
hex_digits: .ascii "0123456789abcdef"
hex_capture: .space 386
keyboard_ready: .ascii "DOS STDIO KEYBOARD READY\r\n"
keyboard_end:
flush_ready: .ascii "DOS STDIO FLUSH READY\r\n"
flush_end:
echo_ready: .ascii "DOS STDIO ECHO READY\r\n"
echo_end:
con_ready: .ascii "DOS STDIO CON READY\r\n"
con_end:
pass_message: .ascii "DOS STDIO CONTRACT PASS\r\n"
pass_end:
fail_message: .ascii "DOS STDIO CONTRACT FAIL\r\n"
fail_end:
