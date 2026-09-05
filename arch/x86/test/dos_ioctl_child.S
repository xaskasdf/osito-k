/* Change a shared inherited CON SFT, not a private copy of its mode. */
    .section .text
    .code16
    .global _start
_start:
    xorw %bx, %bx
    movw $0x4400, %ax
    int $0x21
    jc failed
    xorw %dx, %dx
    movb $0xf3, %dl
    movw $0x4401, %ax
    int $0x21
    jc failed
    movw $0x4c00, %ax
    int $0x21
failed:
    movw $0x4c01, %ax
    int $0x21
