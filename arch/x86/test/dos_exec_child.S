/* Child side of the DOS EXEC contract fixture. */

    .section .text
    .code16
    .global _start

_start:
    pushw %cs
    popw %ds
    movw $0x1ff0, %sp

    movw $0x5100, %ax
    int $0x21
    movw %cs, %ax
    cmpw %ax, %bx
    jne fail_psp
    cmpw %ax, 0x16
    je fail_psp
    cmpw $0, 0x16
    je fail_psp

    cmpb $7, 0x80
    jne fail_tail
    movw $expected_tail, %si
    movw $0x81, %di
    movw $7, %cx
tail_loop:
    movb (%si), %al
    cmpb %al, (%di)
    jne fail_tail
    incw %si
    incw %di
    loop tail_loop
    cmpb $0x0d, 0x88
    jne fail_tail

    movw 0x2c, %ax
    testw %ax, %ax
    jz fail_environment
    movw %ax, %es
    xorw %di, %di
    movw $blaster_prefix, %si
    movw $8, %cx
    repe cmpsb
    jne fail_environment

    xorw %di, %di
environment_end:
    cmpw $0, %es:(%di)
    je environment_trailer
    incw %di
    cmpw $0x7ffe, %di
    jb environment_end
    jmp fail_environment
environment_trailer:
    addw $2, %di
    cmpw $1, %es:(%di)
    jne fail_environment
    addw $2, %di
    movw $expected_program, %si
program_name_loop:
    movb (%si), %al
    cmpb %al, %es:(%di)
    jne fail_environment
    incw %si
    incw %di
    testb %al, %al
    jne program_name_loop

    movw $1, %bx
    xorw %dx, %dx
    movw $0x4400, %ax
    int $0x21
    jc fail_inherit
    testw $0x8000, %dx
    jz fail_inherit

    movw $5, %bx
    movw $0x4400, %ax
    int $0x21
    jnc fail_noinherit
    cmpw $6, %ax
    jne fail_noinherit

    /* Bypass DOS duplication deliberately: closing this forged alias drives
     * the inherited SFT below its legitimate parent reference count. */
    movb 0x1e, %al
    cmpb $0xff, %al
    je fail_inherit
    movb %al, 0x1f
    movw $6, %bx
    movw $0x3e00, %ax
    int $0x21
    jc fail_inherit
    movw $7, %bx
    movw $0x3e00, %ax
    int $0x21
    jc fail_inherit

    /* Leave an external JFT, an SFT and two MCBs for process cleanup. */
    pushw %cs
    popw %es
    movw $0x0200, %bx
    movw $0x4a00, %ax
    int $0x21
    jc fail_memory
    movw $32, %bx
    movw $0x6700, %ax
    int $0x21
    jc fail_memory
    movw $1, %bx
    movw $31, %cx
    movw $0x4600, %ax
    int $0x21
    jc fail_inherit
    movw $0x20, %bx
    movw $0x4800, %ax
    int $0x21
    jc fail_memory

    pushw %cs
    popw %ds
    movw $self_name, %dx
    movw $0x3d00, %ax
    int $0x21
    jc fail_handle

    movw $child_dta, %dx
    movw $0x1a00, %ax
    int $0x21
    movw $dummy_handler, %dx
    movw $0x2522, %ax
    int $0x21
    movw $0x2523, %ax
    int $0x21
    movw $0x2524, %ax
    int $0x21

    movw $child_message, %dx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c2a, %ax
    int $0x21

dummy_handler:
    iret

fail_psp:
    movb $0x51, %al
    jmp fail
fail_tail:
    movb $0x80, %al
    jmp fail
fail_environment:
    movb $0x2c, %al
    jmp fail
fail_inherit:
    movb $0x44, %al
    jmp fail
fail_noinherit:
    movb $0x85, %al
    jmp fail
fail_memory:
    movb $0x48, %al
    jmp fail
fail_handle:
    movb $0x3d, %al

fail:
    movb $0x4c, %ah
    int $0x21

expected_tail:
    .ascii " /CHILD"
blaster_prefix:
    .ascii "BLASTER="
expected_program:
    .asciz "C:\\DEXCHILD.COM"
self_name:
    .asciz "DEXCHILD.COM"
child_message:
    .ascii "DOS EXEC CHILD PASS\r\n$"
child_dta:
    .space 128
