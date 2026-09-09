/* Warm every path beyond the JIT threshold, including fallback-only blocks. */
    .section .text
    .code16
    .global _start
_start:
    cld
    pushw %cs
    popw %ds
    movw $0x3500, %ax
    int $0x21
    movw %bx, old_vector
    movw %es, old_vector+2
    movw $zero_handler, %dx
    movw $0x2500, %ax
    int $0x21

    movw $256, %cx
    xorw %si, %si
hot_loop:
    movb $1, stage
    movw $0x1234, %ax
    pushw %ax
    xorw %dx, %dx
    popw %dx
    cmpw %ax, %dx
    jne fail
    movw $data_word, %bx
    cmpw %ax, (%bx)
    jne fail
    call hot_call
    movl $0x56789abc, %eax
    incl %eax
    cmpl $0x56789abd, %eax
    jne fail

    movb $2, stage
    xorw %bx, %bx
    stc
    incw %bx
    jnc fail
    stc
    decw %bx
    jnc fail
    clc
    incw %bx
    jc fail
    clc
    decw %bx
    jc fail

    movb $3, stage
    cli
    std
    xorw %ax, %ax
    jz zero_taken
    jmp fail
zero_taken:
    jnz fail
    incw %ax
    jnz nonzero_taken
    jmp fail
nonzero_taken:
    jz fail
    pushfw
    popw %dx
    andw $0x0600, %dx
    cmpw $0x0400, %dx
    jne fail
    cld
    sti

    movb $4, stage
    int $0
    decw %cx
    jnz hot_loop
    cmpw $256, zero_count
    jne fail
    cmpw $256, %si
    jne fail
    movb $0, stage
    call restore_vector
    movw $pass_text, %dx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c00, %ax
    int $0x21

hot_call:
    incw %si
    ret

zero_handler:
    incw %cs:zero_count
    iret

fail:
    cld
    sti
    call restore_vector
    movw $fail_text, %dx
    movw $0x0900, %ax
    int $0x21
    movb stage, %al
    movb $0x4c, %ah
    int $0x21

restore_vector:
    pushw %ds
    movw old_vector, %dx
    movw old_vector+2, %ax
    movw %ax, %ds
    movw $0x2500, %ax
    int $0x21
    popw %ds
    ret

old_vector: .long 0
zero_count: .word 0
data_word: .word 0x1234
stage: .byte 0
pass_text: .asciz "DOS JIT PASS\r\n$"
fail_text: .asciz "DOS JIT FAIL\r\n$"
