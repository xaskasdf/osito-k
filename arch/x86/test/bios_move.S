/* INT 15h/87h: 64 KiB round trip, validation and an aliased descriptor table.
 * MODE=0 real mode; MODE=1 DPMI16; MODE=2 DPMI32, SS.B selected separately. */
.section .text
.code16
.global _start
_start:
    cld
    pushw %cs
    popw %ds
    pushw %cs
    popw %es
    xorl %eax, %eax
    movw %cs, %ax
    shll $4, %eax
    movl %eax, program_base
    movw $0x1000, %bx
    movw $0x4a00, %ax
    int $0x21
    jc setup_fail
    movw $0x1000, %bx
    movb $0x48, %ah
    int $0x21
    jc setup_fail
    movw %ax, buffer_segment
    movw %ax, buffer_selector
    movw %ax, %es
    movzwl %ax, %eax
    shll $4, %eax
    movl %eax, buffer_base
    xorw %di, %di
    movw $0x8000, %cx
    movw $0x1357, %ax
    rep stosw
.if MODE
    movw $0x1687, %ax
    int $0x2f
    testw %ax, %ax
    jnz setup_fail
    movw %di, entry
    movw %es, entry + 2
    movw $(MODE == 2), %ax
    andw $1, %ax
    lcall *entry
    jc setup_fail
    movw buffer_segment, %bx
    movw $2, %ax
    int $0x31
    jc setup_fail
    movw %ax, buffer_selector
.endif
.if MODE == 2
    movw %cs, %bx
    movw $0x000a, %ax
    int $0x31
    jc setup_fail
    movw %ax, pm_entry + 2
    movw %ax, %bx
    movw $0x40fa, %cx
    movw $9, %ax
    int $0x31
    jc setup_fail
    ljmp *pm_entry
.else
    jmp move_entry
.endif
setup_fail:
    movw $0x4c70, %ax
    int $0x21
entry: .word 0, 0
pm_entry: .word move_entry, 0
buffer_segment: .word 0
buffer_selector: .word 0
buffer_base: .long 0
program_base: .long 0

.macro address descriptor, value
    movl \value, %eax
    movw %ax, \descriptor + 2
    shrl $16, %eax
    movb %al, \descriptor + 4
.endm

.macro move words, status
    pushw %ds
    popw %es
    movl $0xa5a5875b, %eax
    movl $0xb6b61234, %ebx
    movl $(0xc7c70000 + \words), %ecx
    movl $0xd8d85678, %edx
    movl $(gdt + (MODE < 2) * -0xaaaa0000), %esi
    movl $0xe9e99abc, %edi
    movl $0xfafadef0, %ebp
    cmpl %eax, %eax          /* ZF=1: old no-op must reach the data check. */
    sti
    stc
    std
    int $0x15
    pushfl
    popl returned_flags
    cld
    cmpl $(0xa5a5005b + (\status << 8)), %eax
    jne fail
    cmpl $0xb6b61234, %ebx
    jne fail
    cmpl $(0xc7c70000 + \words), %ecx
    jne fail
    cmpl $0xd8d85678, %edx
    jne fail
    cmpl $(gdt + (MODE < 2) * -0xaaaa0000), %esi
    jne fail
    cmpl $0xe9e99abc, %edi
    jne fail
    cmpl $0xfafadef0, %ebp
    jne fail
    movl returned_flags, %eax
    andl $0x641, %eax
    cmpl $(0x600 + (\status == 0) * -0x40 + (\status != 0) * -1), %eax
    jne fail
    cmpl saved_sp, %esp
    jne fail
.endm

.macro verify_buffer
    movw buffer_selector, %ax
    movw %ax, %es
    xorl %edi, %edi
    movl $0x8000, %ecx
    movw $0x1357, %ax
    repe scasw
    jne fail
.endm

.if MODE == 2
.code32
.endif
move_entry:
.if MODE == 2
    movw %ss, %bx
    movw $(0xf2 | STACK32 * 0x4000), %cx
    movw $9, %ax
    int $0x31
    jc fail
    movw %bx, %ss
.endif
    movl $(0x8000 + !STACK32 * 0xbeef0000), %esp
    movl %esp, saved_sp
    movb $1, stage
    address source, buffer_base
    address target, $0x200000
    move 0x8000, 0

    movw buffer_selector, %ax
    movw %ax, %es
    xorl %edi, %edi
    movl $0x8000, %ecx
    xorw %ax, %ax
    rep stosw
    movb $2, stage
    address source, $0x200000
    address target, buffer_base
    move 0x8000, 0
    movb $3, stage
    verify_buffer

    movb $4, stage
    movw $0xfffc, source
    move 0x8000, 2
    verify_buffer
    movw $0xffff, source
    movb $5, stage
    move 0x8001, 2
    verify_buffer
    movb $6, stage
    movb $0x91, target + 5
    move 0x8000, 2
    verify_buffer
    movb $0x93, target + 5
    movb $7, stage
    movb $1, source + 7
    move 0x8000, 2
    verify_buffer
    movb $0, source + 7
    movb $8, stage
    move 0, 0
    movb $0, source + 5
    move 0, 2
    verify_buffer
    movb $0x93, source + 5

    movb $9, stage
    address source, buffer_base
    movl program_base, %eax
    addl $gdt, %eax
    movw %ax, target + 2
    shrl $16, %eax
    movb %al, target + 4
    move 16, 0
    pushw %ds
    popw %es
    movl $gdt, %edi
    movl $16, %ecx
    movw $0x1357, %ax
    repe scasw
    jne fail
    movl $pass_text, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c2a, %ax
    int $0x21
fail:
    cld
    movl $fail_text, %edx
    movw $0x0900, %ax
    int $0x21
    movb stage, %dl
    addb $'0', %dl
    movb $2, %ah
    int $0x21
    movb stage, %al
    addb $0x70, %al
    movb $0x4c, %ah
    int $0x21

saved_sp: .long 0
returned_flags: .long 0
stage: .byte 0
.balign 8
gdt: .space 16
source: .word 0xffff, 0
    .byte 0, 0x93
    .word 0
target: .word 0xffff, 0
    .byte 0, 0x93
    .word 0
    .space 16
pass_text: .asciz "BIOS MOVE PASS\r\n$"
fail_text: .asciz "BIOS MOVE FAIL stage=$"
