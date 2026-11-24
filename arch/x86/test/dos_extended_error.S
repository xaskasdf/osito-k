/* End-to-end DOS INT 21h/AH=59h extended-error contract test. */

    .section .text
    .code16
    .global _start

_start:
    pushw %cs
    popw %ds

    /* No prior failure: AX must report no extended error. */
    stc
    movw $0x5900, %ax
    int $0x21
    jc fail_initial
    testw %ax, %ax
    jne fail_initial

    /* A successful service must not erase the preceding handle error. */
    movw $0xffff, %bx
    movw $0x3e00, %ax
    int $0x21
    jnc fail_handle
    cmpw $6, %ax
    jne fail_handle

    movw $0x0d00, %ax
    int $0x21

    movw $0xa55a, %cx
    movw $0xffff, %ax
    movw %ax, %es
    movw $0xffff, %di
    stc
    movw $0x5900, %ax
    int $0x21
    jc fail_handle
    cmpw $6, %ax
    jne fail_handle
    cmpb $4, %bl
    jne fail_handle
    cmpb $7, %bh
    jne fail_handle
    cmpb $1, %ch
    jne fail_handle
    cmpb $0x5a, %cl
    jne fail_handle
    movw %es, %ax
    testw %ax, %ax
    jne fail_handle
    testw %di, %di
    jne fail_handle

    /* File lookup errors carry the disk locus and user action. */
    movw $missing_name, %si
    movw $0x0040, %bx
    xorw %cx, %cx
    movw $0x0001, %dx
    movw $0x6c00, %ax
    int $0x21
    jnc fail_file
    cmpw $2, %ax
    jne fail_file

    movw $0x5900, %ax
    int $0x21
    jc fail_file
    cmpw $2, %ax
    jne fail_file
    cmpb $3, %bl
    jne fail_file
    cmpb $8, %bh
    jne fail_file
    cmpb $2, %ch
    jne fail_file

    /* Allocation policy errors use the memory locus. */
    movw $0x0043, %bx
    movw $0x5801, %ax
    int $0x21
    jnc fail_strategy
    cmpw $1, %ax
    jne fail_strategy

    movw $0x5900, %ax
    int $0x21
    jc fail_strategy
    cmpw $1, %ax
    jne fail_strategy
    cmpb $4, %bl
    jne fail_strategy
    cmpb $7, %bh
    jne fail_strategy
    cmpb $5, %ch
    jne fail_strategy

    /* An impossible allocation reports resource exhaustion in memory. */
    movw $0xffff, %bx
    movw $0x4800, %ax
    int $0x21
    jnc fail_memory
    cmpw $8, %ax
    jne fail_memory

    movw $0x5900, %ax
    int $0x21
    jc fail_memory
    cmpw $8, %ax
    jne fail_memory
    cmpb $4, %bl
    jne fail_memory
    cmpb $1, %bh
    jne fail_memory
    cmpb $5, %ch
    jne fail_memory

    /* Unknown services remain application errors with unknown locus. */
    movw $0xe100, %ax
    int $0x21
    jnc fail_unknown
    cmpw $1, %ax
    jne fail_unknown

    movw $0x5900, %ax
    int $0x21
    jc fail_unknown
    cmpw $1, %ax
    jne fail_unknown
    cmpb $4, %bl
    jne fail_unknown
    cmpb $7, %bh
    jne fail_unknown
    cmpb $1, %ch
    jne fail_unknown

    movw $pass_message, %dx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c00, %ax
    int $0x21

fail_initial:
    movb $0x59, %al
    jmp fail
fail_handle:
    movb $0x06, %al
    jmp fail
fail_file:
    movb $0x02, %al
    jmp fail
fail_strategy:
    movb $0x58, %al
    jmp fail
fail_memory:
    movb $0x08, %al
    jmp fail
fail_unknown:
    movb $0xe1, %al

fail:
    pushw %ax
    movw $fail_message, %dx
    movw $0x0900, %ax
    int $0x21
    popw %ax
    movb $0x4c, %ah
    int $0x21

missing_name:
    .asciz "QZXERR59.MISSING"
pass_message:
    .ascii "DOS EXTENDED ERROR CONTRACT PASS\r\n$"
fail_message:
    .ascii "DOS EXTENDED ERROR CONTRACT FAIL\r\n$"
