/* Protected DOS I/O must not depend on free application MCB paragraphs. */
    .section .text
    .code16
    .global _start
_start:
    cld
    pushw %cs
    popw %ds
    pushw %cs
    popw %es
    movw $0xfff0, %sp
    .ifdef RELEASE_ARENA
    movw $0x1000, %bx
    movw $0x4a00, %ax
    int $0x21
    jc setup_fail
    .endif
    movw $real_message, %dx
    movw $0x0900, %ax
    int $0x21
    movw $0xffff, %bx
    movw $0x4800, %ax
    int $0x21
    jnc setup_fail
    cmpw $8, %ax
    jne setup_fail
    .ifdef RELEASE_ARENA
    cmpw $0x104, %bx
    jb setup_fail
    .else
    testw %bx, %bx
    jnz setup_fail
    .endif
    movw $nul_name, %dx
    movw $0x3d02, %ax
    int $0x21
    jc setup_fail
    movw %ax, handle
    movw $0x1687, %ax
    int $0x2f
    testw %ax, %ax
    jnz setup_fail
    movw %di, entry
    movw %es, entry+2
    .ifdef CLIENT32
    movw $1, %ax
    .else
    xorw %ax, %ax
    .endif
    lcall *entry
    jc setup_fail
    .ifdef CLIENT32
    movw %cs, %bx
    movw $6, %ax
    int $0x31
    jc setup_fail
    movw %cx, base_hi
    movw %dx, base_lo
    movw $1, %cx
    xorw %ax, %ax
    int $0x31
    jc setup_fail
    movw %ax, %bx
    movw %ax, code_pointer+2
    movw base_hi, %cx
    movw base_lo, %dx
    movw $7, %ax
    int $0x31
    jc setup_fail
    xorw %cx, %cx
    movw $0xffff, %dx
    movw $8, %ax
    int $0x31
    jc setup_fail
    movw $0x40fa, %cx
    movw $9, %ax
    int $0x31
    jc setup_fail
    ljmp *code_pointer
    .else
    jmp protected_entry
    .endif
setup_fail:
    movw $0x4c71, %ax
    int $0x21
    .align 2
entry: .word 0, 0
code_pointer: .word protected_entry, 0
base_hi: .word 0
base_lo: .word 0
handle: .word 0
nul_name: .asciz "NUL"
real_message: .ascii "DPMI file pressure: real mode ready\r\n$"
pass_message: .ascii "DPMI file pressure: PASS\r\n$"

    .ifdef CLIENT32
    .code32
    .endif
protected_entry:
    movzwl handle, %ebx
    movl $file_buffer, %edx
    movl $0x5000, %ecx
    movl $0x4000, %eax
    clc
    int $0x21
    jc io_error
    cmpl $0x5000, %eax
    jne result_fail
    movl $0x5000, %ecx
    movl $0x3f00, %eax
    int $0x21
    jc io_error
    testl %eax, %eax
    jnz result_fail
    movl $0x3e00, %eax
    int $0x21
    jc io_error
    movl $pass_message, %edx
    movl $0x0900, %eax
    clc
    int $0x21
    jc io_error
    cmpw $0x0924, %ax
    jne result_fail
    movl $0x4c2a, %eax
    int $0x21
io_error:
    movb $0x4c, %ah
    int $0x21
result_fail:
    movl $0x4c72, %eax
    int $0x21
    .align 16
file_buffer: .fill 0x5000, 1, 0xa5
