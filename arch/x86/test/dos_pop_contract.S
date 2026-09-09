/* Warm memory POP through the JIT's interpreter fallback. */
    .section .text
    .code16
    .global _start
_start:
    pushw %cs
    popw %ds
    movl $0x6000, %esp
    movw $128, repeats
warm:
    movl $0x12345678, %eax
    movl $0xcccccccc, canary
    stc
    pushl %eax
    popl slot
    jnc fail
    cmpl $0x12345678, slot
    jne fail
    cmpl $0xcccccccc, canary
    jne fail
    cmpl $0x6000, %esp
    jne fail
    movl $0xcccccccc, 8(%esp)
    pushl %eax
    popl 8(%esp)
    cmpl $0x12345678, 8(%esp)
    jne fail
    pushl %eax
    .byte 0x66, 0x8f, 0xc3 /* POP EBX using the ModR/M encoding. */
    cmpl %eax, %ebx
    jne fail
    decw repeats
    jnz warm
    movw $pass_text, %dx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c00, %ax
    int $0x21
fail:
    movw $fail_text, %dx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c01, %ax
    int $0x21
slot: .long 0
canary: .long 0
repeats: .word 0
pass_text: .asciz "DOS POP PASS\r\n$"
fail_text: .asciz "DOS POP FAIL\r\n$"
