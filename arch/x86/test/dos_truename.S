/* End-to-end DOS INT 21h/AH=60h TRUENAME contract test. */

    .section .text
    .code16
    .global _start

_start:
    pushw %cs
    popw %ds
    pushw %cs
    popw %es
    cld

    movw $canonical_source, %si
    movw $canonical_result, %di
    movw $0x6000, %ax
    stc
    int $0x21
    jc fail_canonical
    testw %ax, %ax
    jne fail_canonical
    cmpw $canonical_result + 22, %di
    jne fail_canonical
    movw $canonical_expected, %si
    movw $canonical_result, %di
    call compare_asciiz
    jc fail_canonical

    /* Source and destination may overlap; DOS stages the result internally. */
    movw $overlap_source, %si
    movw $overlap_result, %di
    movw $0x6000, %ax
    int $0x21
    jc fail_overlap
    cmpw $overlap_result + 11, %di
    jne fail_overlap
    movw $overlap_expected, %si
    movw $overlap_result, %di
    call compare_asciiz
    jc fail_overlap

    movw $wildcard_source, %si
    movw $wildcard_result, %di
    movw $0x6000, %ax
    int $0x21
    jc fail_wildcard
    cmpw $wildcard_result + 16, %di
    jne fail_wildcard
    movw $wildcard_expected, %si
    movw $wildcard_result, %di
    call compare_asciiz
    jc fail_wildcard

    /* A bare character device uses DOS's forward-slash device marker. */
    movw $device_source, %si
    movw $device_result, %di
    movw $0x6000, %ax
    int $0x21
    jc fail_device
    cmpw $device_result + 11, %di
    jne fail_device
    movw $device_expected, %si
    movw $device_result, %di
    call compare_asciiz
    jc fail_device

    /* Backing above the drive root fails without modifying ES:DI. */
    movw $parent_source, %si
    movw $error_result, %di
    movw $0x6000, %ax
    int $0x21
    jnc fail_parent
    cmpw $3, %ax
    jne fail_parent
    cmpw $error_result, %di
    jne fail_parent
    cmpw $0xa5a5, error_result
    jne fail_parent
    cmpw $0xa5a5, error_result + 30
    jne fail_parent

    movw $0x5900, %ax
    int $0x21
    jc fail_parent
    cmpw $3, %ax
    jne fail_parent
    cmpb $3, %bl
    jne fail_parent
    cmpb $8, %bh
    jne fail_parent
    cmpb $2, %ch
    jne fail_parent

    pushw %cs
    popw %es
    movw $invalid_name_source, %si
    movw $error_result, %di
    movw $0x6000, %ax
    int $0x21
    jnc fail_invalid_name
    cmpw $2, %ax
    jne fail_invalid_name
    cmpw $error_result, %di
    jne fail_invalid_name
    cmpw $0xa5a5, error_result
    jne fail_invalid_name

    movw $invalid_drive_source, %si
    movw $error_result, %di
    movw $0x6000, %ax
    int $0x21
    jnc fail_invalid_drive
    cmpw $3, %ax
    jne fail_invalid_drive
    cmpw $error_result, %di
    jne fail_invalid_drive

    movw $empty_source, %si
    movw $error_result, %di
    movw $0x6000, %ax
    int $0x21
    jnc fail_empty
    cmpw $2, %ax
    jne fail_empty
    cmpw $error_result, %di
    jne fail_empty

    movw $pass_message, %dx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c00, %ax
    int $0x21

compare_asciiz:
    lodsb
    scasb
    jne compare_failed
    testb %al, %al
    jne compare_asciiz
    clc
    ret
compare_failed:
    stc
    ret

fail_canonical:
    movb $0x60, %al
    jmp fail
fail_overlap:
    movb $0x61, %al
    jmp fail
fail_wildcard:
    movb $0x62, %al
    jmp fail
fail_device:
    movb $0x63, %al
    jmp fail
fail_parent:
    movb $0x64, %al
    jmp fail
fail_invalid_name:
    movb $0x65, %al
    jmp fail
fail_invalid_drive:
    movb $0x66, %al
    jmp fail
fail_empty:
    movb $0x67, %al

fail:
    pushw %ax
    movw $fail_message, %dx
    movw $0x0900, %ax
    int $0x21
    popw %ax
    movb $0x4c, %ah
    int $0x21

canonical_source:
    .asciz "c:/games//./doom/../longfilename.extension"
canonical_expected:
    .asciz "C:\\GAMES\\LONGFILE.EXT"
overlap_expected:
    .asciz "C:\\BAR.TXT"
wildcard_source:
    .asciz "*.t*"
wildcard_expected:
    .asciz "C:\\????????.T??"
device_source:
    .asciz "nul.txt"
device_expected:
    .asciz "C:/NUL.TXT"
parent_source:
    .asciz "..\\BAD"
invalid_name_source:
    .asciz "BAD|NAME"
invalid_drive_source:
    .asciz "D:\\BAD"
empty_source:
    .asciz ""
pass_message:
    .ascii "DOS TRUENAME CONTRACT PASS\r\n$"
fail_message:
    .ascii "DOS TRUENAME CONTRACT FAIL\r\n$"

    .balign 2
overlap_result:
    .fill 4, 1, 0xa5
overlap_source:
    .asciz "foo\\..\\bar.txt"
    .space 16
canonical_result:
    .space 128
wildcard_result:
    .space 128
device_result:
    .space 128
error_result:
    .fill 32, 1, 0xa5
