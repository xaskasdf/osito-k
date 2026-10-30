/* XMS far entry, hook chaining, memory ownership, UMBs and DPMI RM calls. */
.section .text
.code16
.global _start
_start:
    cld
    pushw %cs
    popw %ds
    pushw %cs
    popw %es
    movw %cs, real_cs
    movw $0x1000, %bx
    movw $0x4a00, %ax
    int $0x21
    jc fail_real
    movb $1, stage
    movw $0x4300, %ax
    int $0x2f
    cmpb $0x80, %al
    jne fail_real
    movw $0x4310, %ax
    int $0x2f
    movw %bx, entry
    movw %es, entry + 2
    movb $2, stage
    movl $0xabcd0000, %eax
    movl $0x11223344, %ebx
    movl $0x55667788, %ecx
    movl $0x99aabbcc, %edx
    movl $0x12345678, %esi
    movl $0x87654321, %edi
    stc
    std
    lcall *entry
    pushfw
    cmpl $0xabcd0300, %eax
    jne fail_real
    cmpl $0x11220100, %ebx
    jne fail_real
    cmpl $0x55667788, %ecx
    jne fail_real
    cmpl $0x99aa0001, %edx
    jne fail_real
    cmpl $0x12345678, %esi
    jne fail_real
    cmpl $0x87654321, %edi
    jne fail_real
    popw %ax
    andw $0x401, %ax
    cmpw $0x401, %ax
    jne fail_real
    cld
    movw $0x8800, %ax
    int $0x15
    testw %ax, %ax
    jz fail_real
    movb $3, stage
    movw entry, %bx
    cmpb $0xeb, %es:(%bx)
    jne fail_real
    movzbl %es:1(%bx), %eax
    addw %bx, %ax
    addw $2, %ax
    movw %ax, chain
    movw %es, chain + 2
    xorw %si, %si
1:  movb %es:(%bx,%si), %al
    movb %al, old_header(%si)
    incw %si
    cmpw $5, %si
    jne 1b
    movb $0xea, %es:(%bx)
    movw $hook, %es:1(%bx)
    movw %cs, %es:3(%bx)
    xorb %ah, %ah
    lcall *entry
    cmpw $1, hooked
    jne fail_real
    movw entry, %bx
    xorw %si, %si
1:  movb old_header(%si), %al
    movb %al, %es:(%bx,%si)
    incw %si
    cmpw $5, %si
    jne 1b
    movb $4, stage
    movw $0xffff, %dx
    movb $1, %ah
    lcall *entry
    cmpw $1, %ax
    jne fail_real
    movb $3, %ah
    lcall *entry
    cmpw $1, %ax
    jne fail_real
    movw $0xffff, %ax
    movw %ax, %es
    movw $0x1234, %es:0x8010
    cmpw $0x1234, %es:0x8010
    jne fail_real
    movb $4, %ah
    lcall *entry
    cmpw $1, %ax
    jne fail_real
    movb $7, %ah
    lcall *entry
    testw %ax, %ax
    jnz fail_real
    movb $2, %ah
    lcall *entry
    cmpw $1, %ax
    jne fail_real
    movw $0x8800, %ax
    int $0x15
    testw %ax, %ax
    jnz fail_real
    movb $5, stage
    movw $8, %dx
    movb $9, %ah
    lcall *entry
    cmpw $1, %ax
    jne fail_real
    movw %dx, handle
    movw %dx, move_args + 10
    movw %cs, move_args + 8
    movw $move_args, %si
    movb $0x0b, %ah
    lcall *entry
    cmpw $1, %ax
    jne fail_real
    movw handle, %dx
    movw $16, %bx
    movb $0x0f, %ah
    lcall *entry
    cmpw $1, %ax
    jne fail_real
    movw handle, %ax
    movw %ax, move_args + 4
    movl $0, move_args + 6
    movw $0, move_args + 10
    movw $output, move_args + 12
    movw %cs, move_args + 14
    movb $0x0b, %ah
    lcall *entry
    cmpw $1, %ax
    jne fail_real
    pushw %ds
    popw %es
    movw $source, %si
    movw $output, %di
    movw $4096, %cx
    repe cmpsb
    jne fail_real
    movw handle, %dx
    movb $0x0a, %ah
    lcall *entry
    cmpw $1, %ax
    jne fail_real
    movb $6, stage
    movw $64, %dx
    movb $0x10, %ah
    lcall *entry
    cmpw $1, %ax
    jne fail_real
    movw %bx, umb
    movw %bx, %es
    movw $0x5678, %es:0
    movw %bx, %dx
    movw $128, %bx
    movb $0x12, %ah
    lcall *entry
    cmpw $1, %ax
    jne fail_real
    cmpw $0x5678, %es:0
    jne fail_real
    movw umb, %dx
    movb $0x11, %ah
    lcall *entry
    cmpw $1, %ax
    jne fail_real
.if !MODE
    jmp pass_real
.else
    movb $7, stage
    movw $0x1687, %ax
    int $0x2f
    testw %ax, %ax
    jnz fail_real
    movw %di, dpmi_entry
    movw %es, dpmi_entry + 2
    movw $(MODE == 2), %ax
    andw $1, %ax
    lcall *dpmi_entry
    jc fail_real
.if MODE == 2
    movw %cs, %bx
    movw $0x000a, %ax
    int $0x31
    jc fail_real
    movw %ax, pm_entry + 2
    movw %ax, %bx
    movw $0x40fa, %cx
    movw $9, %ax
    int $0x31
    jc fail_real
    ljmp *pm_entry
.else
    jmp protected_entry
.endif
.endif

hook:
    incw %cs:hooked
    ljmp *%cs:chain
pass_real:
    movw $pass_text, %dx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c2a, %ax
    int $0x21
fail_real:
    cld
    movw $fail_text, %dx
    movw $0x0900, %ax
    int $0x21
    movb stage, %dl
    addb $'0', %dl
    movb $2, %ah
    int $0x21
    movw $0x4c71, %ax
    int $0x21

.if MODE == 2
.code32
.endif
.if MODE
protected_entry:
    movw %ss, %bx
    movw $(0xf2 | STACK32 * 0x4000), %cx
    movw $9, %ax
    int $0x31
    jc fail_pm
    movw %bx, %ss
    movl $(0xe000 + !STACK32 * 0xbeef0000), %esp
    movl %esp, saved_sp
    pushw %ds
    popw %es
    movw entry, %ax
    movw %ax, rm_regs + 42
    movw entry + 2, %ax
    movw %ax, rm_regs + 44
    movl $0x0900, rm_regs + 28
    movl $32, rm_regs + 20
    call xms_pm
    cmpw $1, rm_regs + 28
    jne fail_pm
    movw rm_regs + 20, %ax
    movw %ax, handle
    movw $0x0c00, rm_regs + 28
    call xms_pm
    cmpw $1, rm_regs + 28
    jne fail_pm
    movzwl rm_regs + 20, %eax
    shll $16, %eax
    movw rm_regs + 16, %ax
    movl %eax, physical
    movb $8, stage
    xorw %bx, %bx
    movw $0x8000, %cx
    movw $0x0501, %ax
    int $0x31
    jc fail_pm
    movw %si, dpmi_handle
    movw %di, dpmi_handle + 2
    movzwl %bx, %eax
    shll $16, %eax
    movw %cx, %ax
    movl physical, %edx
    addl $32768, %edx
    cmpl %edx, %eax
    jae 1f
    addl $32768, %eax
    cmpl physical, %eax
    ja fail_pm
1:  movw dpmi_handle, %si
    movw dpmi_handle + 2, %di
    movw $0x0502, %ax
    int $0x31
    jc fail_pm
    movw handle, %ax
    movw %ax, rm_regs + 20
    movw $0x0d00, rm_regs + 28
    call xms_pm
    cmpw $1, rm_regs + 28
    jne fail_pm
    movw $0x0a00, rm_regs + 28
    call xms_pm
    cmpw $1, rm_regs + 28
    jne fail_pm
    cmpl saved_sp, %esp
    jne fail_pm
    movl $pass_text, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c2a, %ax
    int $0x21
xms_pm:
    movw $0x202, rm_regs + 32
    movl $rm_regs, %edi
    xorw %cx, %cx
    xorw %bx, %bx
    movw $0x0301, %ax
    int $0x31
    jc fail_pm
    ret
fail_pm:
    movl $fail_text, %edx
    movw $0x0900, %ax
    int $0x21
    movb stage, %dl
    addb $'0', %dl
    movb $2, %ah
    int $0x21
    movw $0x4c71, %ax
    int $0x21
.endif

entry: .word 0, 0
chain: .word 0, 0
hooked: .word 0
old_header: .space 5
dpmi_entry: .word 0, 0
.if MODE
pm_entry: .word protected_entry, 0
.endif
real_cs: .word 0
handle: .word 0
umb: .word 0
physical: .long 0
saved_sp: .long 0
dpmi_handle: .word 0, 0
stage: .byte 0
rm_regs: .space 50
move_args: .long 4096
    .word 0
    .word source, 0
    .word 0
    .long 0
pass_text: .asciz "DOS XMS PASS\r\n$"
fail_text: .asciz "DOS XMS FAIL stage=$"
source: .fill 4096, 1, 0x5A
output: .space 4096
