/* VGA memory operations through native CPL3 DPMI, including restarted REP. */
    .section .text
    .code16
    .global _start
_start:
    cld
    pushw %cs
    popw %ds
    pushw %cs
    popw %es
    movw $0x5ff0, %sp
    movw $0x600, %bx
    movw $0x4a00, %ax
    int $0x21
    jc initial_fail
    movw $0x1687, %ax
    int $0x2f
    testw %ax, %ax
    jnz initial_fail
    movw %di, entry
    movw %es, entry+2
    movw $1, %ax
    lcall *entry
    jc initial_fail
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
initial_fail:
    movw $0x4c01, %ax
    int $0x21

    .code32
protected_entry:
    movw $1, %cx
    xorw %ax, %ax
    int $0x31
    jc fail
    movw %ax, %bx
    xorw %cx, %cx
    xorw %dx, %dx
    movw $7, %ax
    int $0x31
    jc fail
    movw $0x000f, %cx
    movw $0xffff, %dx
    movw $8, %ax
    int $0x31
    jc fail
    movw $0x40f2, %cx
    movw $9, %ax
    int $0x31
    jc fail
    movw %bx, %es
    movw %bx, %fs
    movw %bx, %gs
    movw $0x0013, %ax
    int $0x10

    movl $0x76543210, %eax
    movl %eax, %es:0xa0000
    movl %es:0xa0000, %ebx
    cmpl %eax, %ebx
    jne fail
    movw $0xabcd, %es:0xa0003
    movw %es:0xa0003, %bx
    cmpw $0xabcd, %bx
    jne fail
    addl $1, %es:0xa0010
    cmpl $1, %es:0xa0010
    jne fail

    movl $source, %esi
    movl $0xa0100, %edi
    movl $513, %ecx
    rep movsl
    testl %ecx, %ecx
    jnz fail
    cmpl $0xa0904, %edi
    jne fail
    movl $source, %esi
    movl $0xa0100, %edi
    movl $513, %ecx
    repe cmpsl
    jne fail
    testl %ecx, %ecx
    jnz fail

    /* A match/mismatch on either side of the chunk boundary must stop REP. */
    movl $0xdeadbeef, %gs:0xa0500
    movl $source, %esi
    movl $0xa0100, %edi
    movl $513, %ecx
    repe cmpsl
    je fail
    cmpl $256, %ecx
    jne fail
    movl $0xdeadbeef, %eax
    movl $0xa0100, %edi
    movl $513, %ecx
    repne scasl
    jne fail
    cmpl $256, %ecx
    jne fail
    movl $0xdeadbeef, %fs:0xa04fc
    movl $source, %esi
    movl $0xa0100, %edi
    movl $513, %ecx
    repe cmpsl
    je fail
    cmpl $257, %ecx
    jne fail

    std
    movl $0x13579bdf, %eax
    movl $0xa1800, %edi
    movl $513, %ecx
    rep stosl
    cld
    testl %ecx, %ecx
    jnz fail
    cmpl $0xa0ffc, %edi
    jne fail
    cmpl $0x13579bdf, %es:0xa1000
    jne fail

    /* Leave Chain-4, enable one plane, and validate read-map isolation. */
    movw $0x3c4, %dx
    movw $0x0604, %ax
    outw %ax, %dx
    movw $0x0402, %ax
    outw %ax, %dx
    movb $0x5a, %es:0xa4000
    movw $0x3ce, %dx
    movw $0x0204, %ax
    outw %ax, %dx
    cmpb $0x5a, %es:0xa4000
    jne fail
    movw $0x0104, %ax
    outw %ax, %dx
    cmpb $0, %es:0xa4000
    jne fail

    movw $3, %ax
    int $0x10
    movw %es:0xb8000, %bx
    movw %bx, %es:0xb8000
    movl $pass_text, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c00, %ax
    int $0x21
fail:
    cld
    movw $3, %ax
    int $0x10
    movl $fail_text, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c01, %ax
    int $0x21
    .align 4
entry: .long 0
code_pointer: .word protected_entry, 0
base_hi: .word 0
base_lo: .word 0
source:
    .rept 513
    .long 0x12345678
    .endr
pass_text: .asciz "DPMI VGA MEMORY PASS\r\n$"
fail_text: .asciz "DPMI VGA MEMORY FAIL\r\n$"
