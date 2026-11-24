/* DPMI 0302h must execute a saved DOS ROM vector, even after it is hooked. */
    .section .text
    .code16
    .global _start
_start:
    cld
    pushw %cs
    popw %ds
    pushw %cs
    popw %es
    movw %cs, real_segment
    movw $0x5ff0, %sp
    movw $0x600, %bx
    movw $0x4a00, %ax
    int $0x21
    jc initial_fail
    movw $0x3521, %ax
    int $0x21
    movw %bx, regs+42
    movw %es, regs+44
    movw $0x3000, %ax
    int $0x21
    movw %ax, version
    movw $0x1687, %ax
    int $0x2f
    testw %ax, %ax
    jnz initial_fail
    movw %di, entry
    movw %es, entry+2
    .ifdef CLIENT32
    movw $1, %ax
    .else
    xorw %ax, %ax
    .endif
    lcall *entry
    jc initial_fail
    .ifdef CLIENT32
    movw %cs, %bx
    movw $6, %ax
    int $0x31
    jc initial_fail
    movw %cx, base_hi
    movw %dx, base_lo
    movw $1, %cx
    xorw %ax, %ax
    int $0x31
    jc initial_fail
    movw %ax, %bx
    movw %ax, code_pointer+2
    movw base_hi, %cx
    movw base_lo, %dx
    movw $7, %ax
    int $0x31
    jc initial_fail
    xorw %cx, %cx
    movw $0xffff, %dx
    movw $8, %ax
    int $0x31
    jc initial_fail
    movw $0x40fa, %cx
    movw $9, %ax
    int $0x31
    jc initial_fail
    ljmp *code_pointer
    .else
    jmp protected_entry
    .endif
initial_fail:
    movw $0x4c01, %ax
    int $0x21

real_hook:
    incb %cs:hook_seen
    iret

    .ifdef CLIENT32
    .code32
    .endif
protected_entry:
    pushw %ds
    popw %es
    movw $0x21, %bx
    movw real_segment, %cx
    movl $real_hook, %edx
    movw $0x0201, %ax
    int $0x31
    jc fail
    movl $0x3000, regs+28
    movw $0x0603, regs+32
    call simulate
    movw regs+28, %ax
    cmpw version, %ax
    jne fail
    movw regs+32, %ax
    andw $0x0600, %ax
    cmpw $0x0600, %ax
    jne fail
    cmpb $0, hook_seen
    jne fail

    /* DOS allocation and release must have real side effects. */
    movl $0x4800, regs+28
    movl $0x20, regs+16
    call simulate
    testw $1, regs+32
    jnz fail
    movw regs+28, %ax
    testw %ax, %ax
    jz fail
    movw %ax, regs+34
    movl $0x4900, regs+28
    orw $1, regs+32
    call simulate
    testw $1, regs+32
    jnz fail

    /* Invalid handle failure must cross both nested interrupt frames. */
    movl $0x4400, regs+28
    movl $0xffff, regs+16
    andw $0xfffe, regs+32
    call simulate
    testw $1, regs+32
    jz fail
    cmpw $6, regs+28
    jne fail

    /* Exercise pointer translation and byte counts through the old vector. */
    movw real_segment, %ax
    movw %ax, regs+36
    movl $nul_name, regs+20
    movl $0x3d00, regs+28
    call simulate
    testw $1, regs+32
    jnz fail
    movzwl regs+28, %eax
    movl %eax, regs+16
    movl $0x3f00, regs+28
    movl $1, regs+24
    movl $canary, regs+20
    call simulate
    testw $1, regs+32
    jnz fail
    cmpw $0, regs+28
    jne fail
    cmpb $0x5a, canary
    jne fail
    movl $0x3e00, regs+28
    call simulate
    testw $1, regs+32
    jnz fail

    call restore_vector
    movl $pass_text, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c00, %ax
    int $0x21
fail:
    call restore_vector
    movl $fail_text, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c01, %ax
    int $0x21
simulate:
    movl $regs, %edi
    xorw %bx, %bx
    xorw %cx, %cx
    movw $0x0302, %ax
    int $0x31
    jc fail
    ret
restore_vector:
    movw $0x21, %bx
    movw regs+44, %cx
    movzwl regs+42, %edx
    movw $0x0201, %ax
    int $0x31
    ret

    .align 4
regs: .space 50
entry: .long 0
code_pointer: .word protected_entry, 0
real_segment: .word 0
base_hi: .word 0
base_lo: .word 0
version: .word 0
hook_seen: .byte 0
canary: .byte 0x5a
nul_name: .asciz "NUL"
    .ifdef CLIENT32
pass_text: .asciz "DPMI32 ROM CHAIN PASS\r\n$"
fail_text: .asciz "DPMI32 ROM CHAIN FAIL\r\n$"
    .else
pass_text: .asciz "DPMI16 ROM CHAIN PASS\r\n$"
fail_text: .asciz "DPMI16 ROM CHAIN FAIL\r\n$"
    .endif
