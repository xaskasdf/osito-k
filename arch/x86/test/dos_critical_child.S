    .section .text
    .code16
    .global _start
_start:
    movw $4, %bx
    movw $1, %cx
    movw $byte, %dx
    movw $0x4000, %ax
    int $0x21
    movw $0x4cff, %ax
    int $0x21
byte: .byte '!'
