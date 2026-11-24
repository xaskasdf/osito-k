/* DPMI stack arguments through real INT 31h gates, with a nested callback. */
    .section .text
    .code16
    .global _start
    .set WORDS, 2304
_start:
    cld
    pushw %cs
    popw %ds
    pushw %cs
    popw %es
    movw %cs, real_segment
    movw $0xe000, %sp
    movw $0x1000, %bx
    movw $0x4a00, %ax
    int $0x21
    jc initial_fail
    movw $0x1687, %ax
    int $0x2f
    testw %ax, %ax
    jnz initial_fail
    movw %di, entry
    movw %es, entry + 2
    .ifdef CLIENT32
    movw $1, %ax
    .else
    xorw %ax, %ax
    .endif
    lcall *entry
    jc initial_fail
    movw %ds, data_selector
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
    movw %ax, code_pointer + 2
    movw %ax, %bx
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
    movw $0x4c70, %ax
    int $0x21

entry: .word 0, 0
code_pointer: .word protected_entry, 0
base_hi: .word 0
base_lo: .word 0
real_segment: .word 0
data_selector: .word 0
which: .word 0
called: .word 0
saved_sp: .long 0

    .ifdef CLIENT32
    .code32
    .endif
protected_entry:
    movw $0x0900, %ax
    int $0x31
    movw data_selector, %ax
    movw %ax, %ds
    movw %ax, %es
    movw real_segment, %ax
    movw %ax, rm_call + 44
    movw %ax, rm_call + 48
    movw %ax, inner_call + 44
    movw %ax, inner_call + 48
    movw %ax, %cx
    movw $real_procedure, %dx
    movw $0x60, %bx
    movw $0x0201, %ax
    int $0x31
    jc fail
    call all_calls
    .ifdef CLIENT32
    /* The client width and SS.B are independent. Exercise both stack sizes. */
    movw %ss, %bx
    movw $0x40f2, %cx
    movw $9, %ax
    int $0x31
    jc fail
    movzwl %sp, %esp
    call all_calls
    .endif
    movw $0x4c2a, %ax
    int $0x21

all_calls:
    movw $0, which
    movw real_segment, %ax
    movw %ax, rm_call + 44
    movw $real_procedure, rm_call + 42
1:
    call with_arguments
    incw which
    cmpw $4, which
    jb 1b

    /* Register a PM callback, then call another real procedure inside it. */
    movw %cs, %ax
    movw %ax, %ds
    movl $callback, %esi
    movl $callback_regs, %edi
    movw $0x0303, %ax
    int $0x31
    jc fail
    pushw %es
    popw %ds
    movw %cx, rm_call + 44
    movw %dx, rm_call + 42
    movw $1, which
    call with_arguments
    movw rm_call + 44, %cx
    movw rm_call + 42, %dx
    movw $0x0304, %ax
    int $0x31
    jc fail

    /* Excessive CX and an unbalanced return must report errors to the caller. */
    movl $rm_call, %edi
    xorw %bx, %bx
    movw $0xffff, %cx
    movw $0x0301, %ax
    int $0x31
    jnc fail
    cmpw $0x8021, %ax
    jne fail
    movw real_segment, %ax
    movw %ax, rm_call + 44
    movw $bad_return, rm_call + 42
    xorw %cx, %cx
    movw $0x0301, %ax
    int $0x31
    jnc fail
    cmpw $0x8021, %ax
    jne fail
    ret

with_arguments:
    movl %esp, saved_sp
    movl $WORDS, %ecx
1:
    pushw %cx
    loop 1b
    movw $6, rm_call + 8
    movw which, %ax
    cmpw $1, %ax
    jne 2f
    movw $4, rm_call + 8
2:
    cmpw $3, %ax
    jne 3f
    decw %ax
3:
    addw $0x0300, %ax
    movl $WORDS, rm_call + 24
    movl $0, rm_call + 28
    movw $2, rm_call + 32
    movl $rm_call, %edi
    movw $0x60, %bx
    movw $WORDS, %cx
    int $0x31
    jc fail
    cmpl $0xcafebabe, rm_call + 28
    jne fail
    movl $1, %ecx
4:
    popw %ax
    cmpw %cx, %ax
    jne fail
    incw %cx
    cmpw $WORDS, %cx
    jbe 4b
    cmpl saved_sp, %esp
    jne fail
    ret

callback:
    cmpw $1, 4(%esi)
    jne fail
    cmpw $WORDS, (4 + (WORDS-1)*2)(%esi)
    jne fail
    movw (%esi), %ax
    movw %ax, %es:42(%edi)
    movw 2(%esi), %ax
    movw %ax, %es:44(%edi)
    addw $4, %es:46(%edi)
    pushw %es
    popw %ds
    pushw $0xbeef
    movl $inner_call, %edi
    xorw %bx, %bx
    movw $1, %cx
    movw $0x0301, %ax
    int $0x31
    jc fail
    popw %ax
    cmpw $0xbeef, %ax
    jne fail
    cmpl $0xbeef, inner_call + 28
    jne fail
    movl $0xcafebabe, callback_regs + 28
    movl $callback_regs, %edi
    iret
fail:
    movw $0x4c80, %ax
    int $0x21

    .code16
real_procedure:
    movw %sp, %si
    addw %bp, %si
    movw $1, %bx
1:
    cmpw %bx, %ss:(%si)
    jne real_fail
    addw $2, %si
    incw %bx
    loop 1b
    movl $0xcafebabe, %eax
2:
    cmpw $1, %cs:which
    je 3f
    cmpw $3, %cs:which
    je 4f
    iret
3:
    lret
4:
    lret $2
real_fail:
    xorw %ax, %ax
    jmp 2b
inner_procedure:
    movw %sp, %bx
    movzwl %ss:4(%bx), %eax
    lret
bad_return:
    lret $2

    .balign 4
rm_call:
    .space 42
    .word real_procedure, 0, 0xb000, 0
inner_call:
    .space 32
    .word 2, 0, 0, 0, 0, inner_procedure, 0, 0x8000, 0
callback_regs: .space 50
