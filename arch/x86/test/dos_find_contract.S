/* End-to-end DOS path, attribute, and DTA contract test. */

    .section .text
    .code16
    .global _start

_start:
    pushw %cs
    popw %ds
    pushw %cs
    popw %es

    /* Recover cleanly if an earlier test run stopped before cleanup. */
    movw $extended_name, %dx
    movw $0x0020, %cx
    movw $0x4301, %ax
    int $0x21
    movw $0x4100, %ax
    int $0x21

    movw $long_name, %dx
    xorw %cx, %cx
    movw $0x3c00, %ax
    int $0x21
    jc fail
    movw %ax, %bx

    movw $payload, %dx
    movw $payload_end - payload, %cx
    movw $0x4000, %ax
    int $0x21
    jc fail
    cmpw $payload_end - payload, %ax
    jne fail

    movw $0x3e00, %ax
    int $0x21
    jc fail

    movw $long_name, %dx
    movw $0x0007, %cx
    movw $0x4301, %ax
    int $0x21
    jc fail

    xorw %cx, %cx
    movw $0x4300, %ax
    int $0x21
    jc fail
    cmpw $0x0007, %cx
    jne fail

    movw $dta, %dx
    movw $0x1a00, %ax
    int $0x21

    movw $search_pattern, %dx
    movw $0x0006, %cx
    movw $0x4e00, %ax
    int $0x21
    jc fail

    cmpb $'Q', dta + 30
    jne fail
    cmpb $'Z', dta + 31
    jne fail
    cmpb $'X', dta + 32
    jne fail
    cmpb $'~', dta + 33
    jne fail
    cmpb $'.', dta + 38
    jne fail
    cmpb $'D', dta + 39
    jne fail
    cmpb $'A', dta + 40
    jne fail
    cmpb $'T', dta + 41
    jne fail
    cmpb $0, dta + 42
    jne fail
    cmpb $0x07, dta + 21
    jne fail
    cmpl $payload_end - payload, dta + 26
    jne fail

    movw $dta + 30, %si
    movw $alias_name, %di
    movw $13, %cx
    rep movsb

    movw $0x4f00, %ax
    int $0x21
    jnc fail
    cmpw $18, %ax
    jne fail

    movw $alias_name, %dx
    movw $0x3d00, %ax
    int $0x21
    jc fail
    movw %ax, %bx
    movw $0x3e00, %ax
    int $0x21
    jc fail

    movw $alias_name, %dx
    movw $0x3d01, %ax
    int $0x21
    jnc fail
    cmpw $5, %ax
    jne fail

    movw $alias_name, %dx
    movw $0x0020, %cx
    movw $0x4301, %ax
    int $0x21
    jc fail

    movw $alias_name, %dx
    movw $renamed_name, %di
    movw $0x5600, %ax
    int $0x21
    jc fail

    movw $renamed_name, %dx
    movw $0x4100, %ax
    int $0x21
    jc fail

    /* AX=6C00h: missing file with no create action. */
    movw $extended_name, %si
    movw $0x0042, %bx
    movw $0x0006, %cx
    movw $0x0001, %dx
    movw $0x6c00, %ax
    int $0x21
    jnc fail
    cmpw $2, %ax
    jne fail

    /* Create read-only+hidden+system; the new handle remains writable. */
    movw $extended_name, %si
    movw $0x0042, %bx
    movw $0x0007, %cx
    movw $0x0010, %dx
    movw $0x6c00, %ax
    int $0x21
    jc fail
    cmpw $2, %cx
    jne fail
    movw %ax, %bx

    movw $payload, %dx
    movw $payload_end - payload, %cx
    movw $0x4000, %ax
    int $0x21
    jc fail
    cmpw $payload_end - payload, %ax
    jne fail

    movw $0x3e00, %ax
    int $0x21
    jc fail

    movw $extended_name, %dx
    xorw %cx, %cx
    movw $0x4300, %ax
    int $0x21
    jc fail
    cmpw $0x0027, %cx
    jne fail

    /* Existing file with fail/create must report file exists. */
    movw $extended_name, %si
    movw $0x0042, %bx
    movw $0x0006, %cx
    movw $0x0010, %dx
    movw $0x6c00, %ax
    int $0x21
    jnc fail
    cmpw $80, %ax
    jne fail

    /* Once closed, the read-only attribute rejects a writable open. */
    movw $extended_name, %si
    movw $0x0042, %bx
    xorw %cx, %cx
    movw $0x0001, %dx
    movw $0x6c00, %ax
    int $0x21
    jnc fail
    cmpw $5, %ax
    jne fail

    /* Open accepts extended flags and reports the opened action. */
    movw $extended_name, %si
    movw $0x70c0, %bx
    xorw %cx, %cx
    movw $0x0001, %dx
    movw $0x6c00, %ax
    int $0x21
    jc fail
    cmpw $1, %cx
    jne fail
    movw %ax, %bx
    movw $0x3e00, %ax
    int $0x21
    jc fail

    movw $extended_name, %dx
    movw $0x0006, %cx
    movw $0x4301, %ax
    int $0x21
    jc fail

    /* Replace truncates the file and reports the replaced action. */
    movw $extended_name, %si
    movw $0x0042, %bx
    movw $0x0006, %cx
    movw $0x0002, %dx
    movw $0x6c00, %ax
    int $0x21
    jc fail
    cmpw $3, %cx
    jne fail
    movw %ax, %bx

    xorw %cx, %cx
    xorw %dx, %dx
    movw $0x4202, %ax
    int $0x21
    jc fail
    testw %ax, %ax
    jne fail
    testw %dx, %dx
    jne fail
    movw $0x3e00, %ax
    int $0x21
    jc fail

    /* A deny-write reader must reject a second writer. */
    movw $extended_name, %si
    movw $0x0020, %bx
    xorw %cx, %cx
    movw $0x0001, %dx
    movw $0x6c00, %ax
    int $0x21
    jc fail
    movw %ax, extended_handle

    movw $extended_name, %si
    movw $0x0041, %bx
    xorw %cx, %cx
    movw $0x0001, %dx
    movw $0x6c00, %ax
    int $0x21
    jnc fail
    cmpw $32, %ax
    jne fail

    movw extended_handle, %bx
    movw $0x3e00, %ax
    int $0x21
    jc fail

    movw $extended_name, %si
    movw $0x0042, %bx
    xorw %cx, %cx
    movw $0x0003, %dx
    movw $0x6c00, %ax
    int $0x21
    jnc fail
    cmpw $1, %ax
    jne fail

    movw $extended_name, %si
    movw $0x0050, %bx
    xorw %cx, %cx
    movw $0x0001, %dx
    movw $0x6c00, %ax
    int $0x21
    jnc fail
    cmpw $12, %ax
    jne fail

    movw $extended_name, %dx
    movw $0x0020, %cx
    movw $0x4301, %ax
    int $0x21
    jc fail
    movw $0x4100, %ax
    int $0x21
    jc fail

    movw $pass_message, %dx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c00, %ax
    int $0x21

fail:
    movw $fail_message, %dx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c7f, %ax
    int $0x21

long_name:
    .asciz "Qzx Guest Contract Alpha.data"
search_pattern:
    .asciz "QZX~????.DAT"
renamed_name:
    .asciz "QZXGUEST.TMP"
extended_name:
    .asciz "QZXOPEN6.TMP"
payload:
    .ascii "osito-dos"
payload_end:
pass_message:
    .ascii "DOS FIND CONTRACT PASS\r\n$"
fail_message:
    .ascii "DOS FIND CONTRACT FAIL\r\n$"

    .balign 2
alias_name:
    .space 13
extended_handle:
    .word 0
dta:
    .space 43
