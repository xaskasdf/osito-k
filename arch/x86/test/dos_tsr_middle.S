/* A resident grandchild must outlive its normally exiting EXEC parent. */
    .section .text
    .code16
    .global _start
_start:
    pushw %cs
    popw %ds
    pushw %cs
    popw %es
    movw $0x1ff0, %sp
    movw $0x200, %bx
    movw $0x4a00, %ax
    int $0x21
    jc fail
    movw %cs, block+4
    movw %cs, block+8
    movw %cs, block+12
    movw $name, %dx
    movw $block, %bx
    movw $0x4b00, %ax
    int $0x21
    jc fail
    movw $0x4d00, %ax
    int $0x21
    cmpw $0x035a, %ax
    jne fail
    movw $0x4c7a, %ax
    int $0x21
fail:
    movw $0x4cf1, %ax
    int $0x21
name: .asciz "TSRC31.COM"
block:
    .word 0, tail, 0, 0xffff, 0, 0xffff, 0
tail: .byte 0, 13
    .space 126
