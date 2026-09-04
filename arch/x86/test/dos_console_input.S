/*
 * End-to-end blocking DOS console input contract test.
 *
 * Feed "ABCDE\rFG\r" through the guest serial console after this program
 * prints its ready message. The sequence exercises character, buffered, and
 * standard-handle reads without depending on graphical keyboard injection.
 */

    .section .text
    .code16
    .global _start

_start:
    pushw %cs
    popw %ds

    movw $ready_message, %dx
    movw $0x0900, %ax
    int $0x21

    movw $0x0800, %ax
    int $0x21
    cmpb $'A', %al
    jne fail_08

    movw $0x0700, %ax
    int $0x21
    cmpb $'B', %al
    jne fail_07

    movw $0x0100, %ax
    int $0x21
    cmpb $'C', %al
    jne fail_01

    movw $line_buffer, %dx
    movw $0x0a00, %ax
    int $0x21
    cmpb $2, line_buffer + 1
    jne fail_0a
    cmpb $'D', line_buffer + 2
    jne fail_0a
    cmpb $'E', line_buffer + 3
    jne fail_0a
    cmpb $0x0d, line_buffer + 4
    jne fail_0a

    xorw %bx, %bx
    movw $3, %cx
    movw $read_buffer, %dx
    movw $0x3f00, %ax
    int $0x21
    jc fail_3f
    cmpw $3, %ax
    jne fail_3f
    cmpb $'F', read_buffer
    jne fail_3f
    cmpb $'G', read_buffer + 1
    jne fail_3f
    cmpb $0x0d, read_buffer + 2
    jne fail_3f

    movw $pass_message, %dx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c00, %ax
    int $0x21

fail_08:
    movb $0x68, %al
    jmp fail
fail_07:
    movb $0x67, %al
    jmp fail
fail_01:
    movb $0x61, %al
    jmp fail
fail_0a:
    movb $0x6a, %al
    jmp fail
fail_3f:
    movb $0x6f, %al

fail:
    pushw %ax
    movw $fail_message, %dx
    movw $0x0900, %ax
    int $0x21
    popw %ax
    movb $0x4c, %ah
    int $0x21

ready_message:
    .ascii "DOS CONSOLE INPUT READY: send ABCDE<Enter>FG<Enter>\r\n$"
pass_message:
    .ascii "DOS CONSOLE INPUT CONTRACT PASS\r\n$"
fail_message:
    .ascii "DOS CONSOLE INPUT CONTRACT FAIL\r\n$"

line_buffer:
    .byte 8, 0
    .space 9
read_buffer:
    .space 3
