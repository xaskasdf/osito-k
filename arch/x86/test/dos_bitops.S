/* Exercise bit operations repeatedly through the real-mode JIT fallback. */
    .section .text
    .code16
    .global _start
_start:
    pushw %cs
    popw %ds
    movw $256, %di
hot_loop:
    movb $1, stage
    movl $0, %edx
    btcl $21, %edx
    jc fail
    cmpl $0x00200000, %edx
    jne fail
    btcl $21, %edx
    jnc fail
    btsl $18, %edx
    jc fail
    btl $18, %edx
    jnc fail
    btrl $18, %edx
    jnc fail
    cmpl $0, %edx
    jne fail

    movb $2, stage
    movl $0xA5A50000, %edx
    movw $-1, %cx
    btsw %cx, %dx
    jc fail
    cmpl $0xA5A58000, %edx
    jne fail
    btw $31, %dx
    jnc fail
    btrw %cx, %dx
    jnc fail
    cmpl $0xA5A50000, %edx
    jne fail
    movl $21, %edx
    btcl %edx, %edx
    jc fail
    cmpl $0x00200015, %edx
    jne fail

    movb $3, stage
    movw $data+4, %bx
    movl $0, data
    movl $0, data+4
    movl $-1, %ecx
    lock btsl %ecx, (%bx)
    jc fail
    cmpl $0x80000000, data
    jne fail
    lock btcl %ecx, (%bx)
    jnc fail
    cmpl $0, data
    jne fail
    movl $33, %ecx
    btsw %cx, data
    jc fail
    cmpw $2, data+4
    jne fail
    btrw %cx, data
    jnc fail
    cmpw $0, data+4
    jne fail
    decw %di
    jnz hot_loop
    movw $pass_text, %dx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c00, %ax
    int $0x21

fail:
    movw $fail_text, %dx
    movw $0x0900, %ax
    int $0x21
    movb stage, %al
    movb $0x4c, %ah
    int $0x21

data: .long 0, 0, 0
stage: .byte 0
pass_text: .asciz "DOS BITOPS PASS\r\n$"
fail_text: .asciz "DOS BITOPS FAIL\r\n$"
