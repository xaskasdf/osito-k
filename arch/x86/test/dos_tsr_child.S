/* Real-mode resident: open files, external JFT, environment and extra MCB. */
    .section .text
    .code16
    .global _start
_start:
    pushw %cs
    popw %ds
    pushw %cs
    popw %es
    movw $0x2ff0, %sp
    movw $0x300, %bx
    movw $0x4a00, %ax
    int $0x21
    jc fail
    movw $16, %bx
    movw $0x4800, %ax
    int $0x21
    jc fail
    movw %ax, extra
    movw %ax, %es
    movw $0x1234, %es:0
    movw $32, %bx
    movw $0x6700, %ax
    int $0x21
    jc fail
    movw $data_name, %dx
    movw $0x3d40, %ax
    int $0x21
    jc fail
    movw %ax, %bx
    movw $31, %cx
    movw $0x4600, %ax
    int $0x21
    jc fail
    movw $0x3e00, %ax
    int $0x21
    jc fail
    /* This read must advance the parent's shared file position. */
    movw $5, %bx
    movw $buffer, %dx
    movw $1, %cx
    movw $0x3f00, %ax
    int $0x21
    jc fail
    cmpw $1, %ax
    jne fail
    cmpb $'A', buffer
    jne fail
    movw $handler, %dx
    movw $0x2560, %ax
    int $0x21
    movw $ready, %dx
    movw $0x0900, %ax
    int $0x21
    movw $1, %bx
    movw $0x3e00, %ax
    int $0x21
    jc fail
    .if TSR_KIND == 31
    movw $0x200, %dx
    movw $0x315a, %ax
    int $0x21
    .else
    movw $0x1ffd, %dx
    .if TSR_KIND == 28
    movw $0x3527, %ax
    int $0x21
    movw %bx, rom_vector
    movw %es, rom_vector+2
    pushf
    lcall *rom_vector
    .else
    int $0x27
    .endif
    .endif
fail:
    movw $0x4cf0, %ax
    int $0x21

handler:
    cmpw $0x7100, %ax
    je read_file
    movw $0x5453, %ax
    movw %cs, %bx
    movw %cs:extra, %si
    movw %cs:0x2c, %di
    iret
read_file:
    pushw %ds
    pushw %bx
    pushw %cx
    pushw %dx
    movw $0x5100, %ax
    int $0x21
    pushw %bx
    movw %cs, %bx
    movw $0x5000, %ax
    int $0x21
    pushw %cs
    popw %ds
    movw $31, %bx
    movw $1, %cx
    movw $buffer, %dx
    movw $0x3f00, %ax
    int $0x21
    jc read_failed
    cmpw $1, %ax
    jne read_failed
    movzbw buffer, %cx
    jmp read_done
read_failed:
    movw $0xffff, %cx
read_done:
    popw %bx
    movw $0x5000, %ax
    int $0x21
    movw %cx, %ax
    popw %dx
    popw %cx
    popw %bx
    popw %ds
    iret
extra: .word 0
rom_vector: .long 0
buffer: .byte 0
data_name: .asciz "TSRDATA.BIN"
ready: .ascii "DOS TSR CHILD READY\r\n$"
