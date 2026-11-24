.intel_syntax noprefix
.code16
.section .text
.global _start
_start:
    mov bp, sp
    mov ax, cs
    cmp ax, 0xD000
    jae child
    mov byte ptr [stage], '1'
    mov ax, 0x5803
    mov bx, 1
    int 0x21
    jc failed
    mov ax, 0x5801
    mov bx, 0x40
    int 0x21
    jc failed
    mov ah, 0x48
    mov bx, 0x20
    int 0x21
    jc failed
    cmp ax, 0xD001
    jb failed
    mov [owned], ax
    mov es, ax
    mov byte ptr es:[0], 0xA7
    mov ah, 0x48
    mov bx, 0xFFFF
    int 0x21
    jnc failed
    cmp ax, 8
    jne failed
    mov [available], bx
    mov ax, cs
    mov word ptr [parameters + 4], ax
    mov byte ptr [iterations], 3
again:
    mov byte ptr [stage], '2'
    mov ax, cs
    mov es, ax
    mov bx, offset parameters
    mov dx, offset filename
    mov ax, 0x4B00
    int 0x21
    jc failed
    mov ax, 0x4D00
    int 0x21
    cmp ax, 0x2A
    jne failed
    mov byte ptr [stage], '3'
    mov ax, 0x5802
    int 0x21
    jc failed
    test al, al
    jnz failed
    mov ax, [owned]
    mov es, ax
    cmp byte ptr es:[0], 0xA7
    jne failed
    mov ax, 0x5803
    mov bx, 1
    int 0x21
    jc failed
    mov ah, 0x48
    mov bx, 0xFFFF
    int 0x21
    jnc failed
    cmp ax, 8
    jne failed
    cmp bx, [available]
    jne failed
    dec byte ptr [iterations]
    jnz again
    mov byte ptr [stage], '4'
    mov ax, 0x5803
    xor bx, bx
    int 0x21
    jc failed
    mov ax, [owned]
    mov es, ax
    mov ah, 0x49
    int 0x21
    jc failed
    mov ax, 0x5803
    mov bx, 1
    int 0x21
    jc failed
    mov ah, 0x48
    mov bx, 0xFFFF
    int 0x21
    jnc failed
    cmp bx, 0xFFF
    jne failed
    mov ax, 0x5803
    xor bx, bx
    int 0x21
    jc failed
    mov ax, 0x5801
    xor bx, bx
    int 0x21
    jc failed
    mov dx, offset pass
    mov ah, 9
    int 0x21
    mov ax, 0x4C2A
    int 0x21
child:
    mov byte ptr [stage], '5'
    mov dx, ss
    cmp ax, dx
    jne failed
    dec ax
    mov es, ax
    mov ax, word ptr es:[3]
    shl ax, 4
    sub ax, 2
    cmp bp, ax
    jne failed
    mov ax, cs
    mov es, ax
    mov bx, sp
    cmp word ptr [bx], 0
    jne failed
    mov sp, 0x1000
    mov bx, 0x100
    mov ah, 0x4A
    int 0x21
    jc failed
    mov byte ptr [stage], '6'
    mov bx, 0x20
    mov ah, 0x48
    int 0x21
    jc failed
    cmp ax, 0xD001
    jb failed
    mov ax, 0x5803
    xor bx, bx
    int 0x21
    jc failed
    mov ax, 0x4C2A
    int 0x21
failed:
    mov dx, offset fail
    mov ah, 9
    int 0x21
    mov ax, 0x4C71
    int 0x21
pass: .ascii "DOS UMB PASS\r\n$"
fail: .ascii "DOS UMB FAIL stage="
stage: .ascii "0\r\n$"
filename: .asciz "umblink.com"
tail: .byte 0, 13
parameters: .word 0, tail, 0, 0, 0, 0, 0
owned: .word 0
available: .word 0
iterations: .byte 0
