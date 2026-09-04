/* End-to-end DOS create-new, temporary-file, and commit contract test. */

    .section .text
    .code16
    .global _start

_start:
    pushw %cs
    popw %ds

    /* Recover cleanly if an earlier run stopped before deletion. */
    movw $new_name, %dx
    movw $0x4100, %ax
    int $0x21

    xorw %cx, %cx
    movw $new_name, %dx
    movw $0x5b00, %ax
    int $0x21
    jc fail_create
    movw %ax, new_handle

    movw %ax, %bx
    movw $payload, %dx
    movw $payload_end - payload, %cx
    movw $0x4000, %ax
    int $0x21
    jc fail_create
    cmpw $payload_end - payload, %ax
    jne fail_create

    movw new_handle, %bx
    movw $0x6800, %ax
    int $0x21
    jc fail_commit

    /* DOS 4 dispatches AH=6Ah to the same commit implementation. */
    movw $0x6a00, %ax
    int $0x21
    jc fail_commit

    /* Create-new must fail while the directory entry already exists. */
    xorw %cx, %cx
    movw $new_name, %dx
    movw $0x5b00, %ax
    int $0x21
    jnc fail_exclusive
    cmpw $80, %ax
    jne fail_exclusive

    movw $0x5900, %ax
    int $0x21
    jc fail_exclusive
    cmpw $80, %ax
    jne fail_exclusive
    cmpb $3, %bl
    jne fail_exclusive
    cmpb $12, %bh
    jne fail_exclusive
    cmpb $2, %ch
    jne fail_exclusive

    movw new_handle, %bx
    movw $0x3e00, %ax
    int $0x21
    jc fail_create

    movw $new_name, %dx
    movw $0x4100, %ax
    int $0x21
    jc fail_create

    /* Commit validates the handle even though writes are already coherent. */
    movw $0xffff, %bx
    movw $0x6a00, %ax
    int $0x21
    jnc fail_commit
    cmpw $6, %ax
    jne fail_commit

    movw $0x5900, %ax
    int $0x21
    jc fail_commit
    cmpw $6, %ax
    jne fail_commit
    cmpb $4, %bl
    jne fail_commit
    cmpb $7, %bh
    jne fail_commit
    cmpb $1, %ch
    jne fail_commit

    /* DOS appends a separator and eight uppercase hexadecimal digits. */
    xorw %cx, %cx
    movw $temp_path, %dx
    movw $0x5a00, %ax
    int $0x21
    jc fail_temp
    movw %ax, temp_handle

    cmpb $'C', temp_path
    jne fail_temp
    cmpb $':', temp_path + 1
    jne fail_temp
    cmpb $'\\', temp_path + 2
    jne fail_temp
    cmpb $0, temp_path + 11
    jne fail_temp
    cmpw $0xa55a, temp_canary
    jne fail_temp

    movw $temp_path + 3, %si
    movw $8, %cx
check_hex:
    lodsb
    cmpb $'0', %al
    jb fail_temp
    cmpb $'9', %al
    jbe hex_ok
    cmpb $'A', %al
    jb fail_temp
    cmpb $'F', %al
    ja fail_temp
hex_ok:
    loop check_hex

    movw temp_handle, %bx
    movw $payload, %dx
    movw $payload_end - payload, %cx
    movw $0x4000, %ax
    int $0x21
    jc fail_temp

    movw temp_handle, %bx
    movw $0x6800, %ax
    int $0x21
    jc fail_commit
    movw $0x3e00, %ax
    int $0x21
    jc fail_temp

    movw $temp_path, %dx
    movw $0x4100, %ax
    int $0x21
    jc fail_temp

    /* Unsupported high attribute bits are rejected, not truncated away. */
    movw $0x0100, %cx
    movw $invalid_temp_path, %dx
    movw $0x5a00, %ax
    int $0x21
    jnc fail_attributes
    cmpw $5, %ax
    jne fail_attributes

    movw $0x5900, %ax
    int $0x21
    jc fail_attributes
    cmpw $5, %ax
    jne fail_attributes
    cmpb $3, %bl
    jne fail_attributes
    cmpb $3, %bh
    jne fail_attributes
    cmpb $2, %ch
    jne fail_attributes

    movw $pass_message, %dx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c00, %ax
    int $0x21

fail_create:
    movb $0x5b, %al
    jmp fail
fail_exclusive:
    movb $0x50, %al
    jmp fail
fail_commit:
    movb $0x68, %al
    jmp fail
fail_temp:
    movb $0x5a, %al
    jmp fail
fail_attributes:
    movb $0x51, %al

fail:
    pushw %ax
    movw $fail_message, %dx
    movw $0x0900, %ax
    int $0x21
    popw %ax
    movb $0x4c, %ah
    int $0x21

new_name:
    .asciz "QZXNEW5B.TMP"
payload:
    .ascii "dos-file-contract"
payload_end:
temp_path:
    .ascii "C:\0"
    .space 16
temp_canary:
    .word 0xa55a
invalid_temp_path:
    .ascii "C:\0"
    .space 16
pass_message:
    .ascii "DOS FILE CREATE CONTRACT PASS\r\n$"
fail_message:
    .ascii "DOS FILE CREATE CONTRACT FAIL\r\n$"

    .balign 2
new_handle:
    .word 0
temp_handle:
    .word 0
