/* Inherit redirected stdin/stdout and advance the parent's shared SFTs. */
    .section .text
    .code16
    .global _start
_start:
    pushw %cs
    popw %ds
    movw $0x0700, %ax
    int $0x21
    cmpb $'A', %al
    jne failed
    movw $message, %dx
    movw $0x0900, %ax
    int $0x21
    cmpb $'$', %al
    jne failed
    movb $':', %dl
    movw $0x0200, %ax
    int $0x21
    movw $0x0100, %ax
    int $0x21
    testb %al, %al
    jnz failed
    movw $0x4c00, %ax
    int $0x21
failed:
    movw $0x4c01, %ax
    int $0x21
message: .ascii "child$"
