/* EXEC 4B00/4B01: TSR lifetime, nested EXEC, shared SFT and normal cleanup. */
    .section .text
    .code16
    .global _start
_start:
    pushw %cs
    popw %ds
    pushw %cs
    popw %es
    movw $0x3ff0, %sp
    movw $0x400, %bx
    movw $0x4a00, %ax
    int $0x21
    jc fail
    movw %cs, block+4
    movw %cs, block+8
    movw %cs, block+12
    movw $data_name, %dx
    xorw %cx, %cx
    movw $0x3c00, %ax
    int $0x21
    jc fail
    cmpw $5, %ax
    jne fail
    movw %ax, %bx
    movw $data, %dx
    movw $4, %cx
    movw $0x4000, %ax
    int $0x21
    jc fail
    cmpw $4, %ax
    jne fail
    movw $5, %bx
    movw $0x3e00, %ax
    int $0x21
    jc fail
    movw $data_name, %dx
    movw $0x3d42, %ax
    int $0x21
    jc fail
    cmpw $5, %ax
    jne fail
    movw $names, next_name
    movw $4, remaining
next:
    movb $'1', stage
    movw $5, %bx
    xorw %cx, %cx
    xorw %dx, %dx
    movw $0x4200, %ax
    int $0x21
    jc fail
    movw next_name, %si
    movw (%si), %dx
    movw 2(%si), %ax
    movw %ax, expected_status
    addw $4, next_name
    call exec_child
    jc fail
    movb $'2', stage
    movw $0x4d00, %ax
    int $0x21
    cmpw expected_status, %ax
    jne fail
    movw $0x4d00, %ax
    int $0x21
    testw %ax, %ax
    jne fail
    movw $0x5100, %ax
    int $0x21
    movw %cs, %ax
    cmpw %ax, %bx
    jne fail
    movb $'3', stage
    xorw %ax, %ax
    int $0x60
    cmpw $0x5453, %ax
    jne fail
    movw %bx, resident
    movw %bx, %es
    movw %bx, %ax
    addw $0x200, %ax
    cmpw %ax, %es:2
    jne fail
    movw %bx, %ax
    decw %ax
    movw %ax, %es
    cmpw %bx, %es:1
    jne fail
    cmpw $0x200, %es:3
    jne fail
    movw %si, %es
    cmpw $0x1234, %es:0
    jne fail
    decw %si
    movw %si, %es
    cmpw %bx, %es:1
    jne fail
    cmpw $16, %es:3
    jne fail
    decw %di
    movw %di, %es
    cmpw %bx, %es:1
    jne fail
    movb $'4', stage
    movw $1, %bx
    movw $0x4400, %ax
    int $0x21
    jc fail
    movw $5, %bx
    movw $1, %cx
    movw $buffer, %dx
    movw $0x3f00, %ax
    int $0x21
    jc fail
    cmpw $1, %ax
    jne fail
    cmpb $'B', buffer
    jne fail
    movw $0x7100, %ax
    int $0x60
    cmpw $'A', %ax
    jne fail
    movb $'5', stage
    /* A later normal child must not close any resident's SFT. */
    movw $normal_name, %dx
    call exec_child
    jc fail
    movw $0x4d00, %ax
    int $0x21
    cmpw $0x002b, %ax
    jne fail
    movw $0x7100, %ax
    int $0x60
    cmpw $'B', %ax
    jne fail
    decw remaining
    jnz next
    movw $pass_text, %dx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c2a, %ax
    int $0x21
fail:
    pushw %cs
    popw %ds
    movw $fail_text, %dx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c71, %ax
    int $0x21

exec_child:
    pushw %cs
    popw %es
    movw $block, %bx
    .if LOAD_ONLY
    movw $0x4b01, %ax
    int $0x21
    jc exec_return
    movw $0x5100, %ax
    int $0x21
    movw %bx, %es
    movw $exec_resumed, %es:0x0a
    movw %cs, %es:0x0c
    movw block+16, %ax
    movw block+14, %si
    cli
    movw %ax, %ss
    movw %si, %sp
    movw %bx, %ds
    popw %ax
    sti
    ljmp *%cs:block+18
exec_resumed:
    clc
    .else
    movw $0x4b00, %ax
    int $0x21
    .endif
exec_return:
    ret

remaining: .word 0
next_name: .word 0
expected_status: .word 0
resident: .word 0
buffer: .byte 0
data: .ascii "ABCD"
data_name: .asciz "TSRDATA.BIN"
normal_name: .asciz "DEXMZ.EXE"
name31: .asciz "TSRC31.COM"
name27: .asciz "TSRC27.COM"
name_rom: .asciz "TSRROM.COM"
name_mid: .asciz "TSRMID.COM"
names: .word name31, 0x035a, name27, 0x0300, name_rom, 0x0300, name_mid, 0x007a
block:
    .word 0, tail, 0, 0xffff, 0, 0xffff, 0, 0, 0, 0, 0
tail: .byte 0, 13
    .space 126
pass_text: .ascii "DOS TSR PASS\r\n$"
fail_text: .ascii "DOS TSR FAIL stage="
stage: .ascii "0\r\n$"
