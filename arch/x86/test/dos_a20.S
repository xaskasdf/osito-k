/* A20 controls, wrapped DOS file buffers, DPMI's PM guarantee and RM calls. */
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
    jc setup_fail
    movb $1, stage
    movw $0x2403, %ax
    stc
    int $0x15
    jc fail_real
    testb %ah, %ah
    jnz fail_real
    cmpw $3, %bx
    jne fail_real
    movb $2, stage
    movw $0x2401, %ax
    int $0x15
    jc fail_real
    movw $0x0800, %ax
    movw %ax, %es
    movw %es:0, %ax
    movw %ax, low_saved
    movw $0x1234, %es:0
    movw $0xffff, %ax
    movw %ax, %es
    movw %es:0x8010, %ax
    movw %ax, high_saved
    movw $0x5678, %es:0x8010
    movw $0x2400, %ax
    int $0x15
    jc fail_real
    cmpw $0x1234, %es:0x8010
    jne fail_real
    movb $2, %al
    outb %al, $0x92
    cmpw $0x5678, %es:0x8010
    jne fail_real
    xorb %al, %al
    outb %al, $0x92
    cmpw $0x1234, %es:0x8010
    jne fail_real
    movb $0xd1, %al
    outb %al, $0x64
    movb $0xdf, %al
    outb %al, $0x60
    cmpw $0x5678, %es:0x8010
    jne fail_real
    movb $0xdd, %al
    outb %al, $0x64
    cmpw $0x1234, %es:0x8010
    jne fail_real

    /* Only vectors 0-3 overlap this 32-byte buffer; restore before leaving. */
    movb $3, stage
    xorw %cx, %cx
    movw $filename, %dx
    movw $0x3c00, %ax
    int $0x21
    jc fail_real
    movw %ax, file_handle
    movb $1, buffer_saved
    movw $0xffff, %ax
    movw %ax, %es
    xorw %di, %di
save_bytes:
    movb %es:(%di), %al
    movb %al, saved_bytes(%di)
    incw %di
    cmpw $32, %di
    jne save_bytes
    xorw %di, %di
fill_bytes:
    movw %di, %ax
    addb $0x20, %al
    movb %al, %es:(%di)
    incw %di
    cmpw $32, %di
    jne fill_bytes
    pushw %ds
    pushw %es
    popw %ds
    movw %cs:file_handle, %bx
    xorw %dx, %dx
    movw $32, %cx
    movw $0x4000, %ax
    int $0x21
    popw %ds
    jc fail_real
    cmpw $32, %ax
    jne fail_real
    movb $4, stage
    xorw %cx, %cx
    xorw %dx, %dx
    movw $0x4200, %ax
    int $0x21
    jc fail_real
    xorw %di, %di
    movw $32, %cx
    movb $0xaa, %al
    rep stosb
    pushw %ds
    pushw %es
    popw %ds
    movw $32, %cx
    xorw %dx, %dx
    movw $0x3f00, %ax
    int $0x21
    popw %ds
    jc fail_real
    cmpw $32, %ax
    jne fail_real
    xorw %di, %di
check_bytes:
    movw %di, %ax
    addb $0x20, %al
    cmpb %al, %es:(%di)
    jne fail_real
    incw %di
    cmpw $32, %di
    jne check_bytes
    call restore_buffer
    movw file_handle, %bx
    movw $0x3e00, %ax
    int $0x21
    jc fail_real
    movw $filename, %dx
    movw $0x4100, %ax
    int $0x21
    jc fail_real
.if !MODE
    jmp pass_real
.else
    movb $5, stage
    movw $0x1687, %ax
    int $0x2f
    testw %ax, %ax
    jnz fail_real
    movw %di, entry
    movw %es, entry + 2
    movw $(MODE == 2), %ax
    andw $1, %ax
    lcall *entry
    jc setup_fail
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
    jmp protected_entry
.endif
.endif

restore_buffer:
    cmpb $0, buffer_saved
    je 2f
    movw $0x2400, %ax
    int $0x15
    movw $0xffff, %ax
    movw %ax, %es
    xorw %di, %di
1:  movb saved_bytes(%di), %al
    movb %al, %es:(%di)
    incw %di
    cmpw $32, %di
    jne 1b
    movb $0, buffer_saved
2:  ret

pass_real:
    movw $pass_text, %dx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c2a, %ax
    int $0x21
fail_real:
    call restore_buffer
    movw $fail_text, %dx
    movw $0x0900, %ax
    int $0x21
    movb stage, %dl
    addb $'0', %dl
    movb $2, %ah
    int $0x21
    movw $0x4c71, %ax
    int $0x21
setup_fail:
    movw $0x4c70, %ax
    int $0x21

real_alias:
    movw $0xffff, %ax
    movw %ax, %es
    movw %es:0x8010, %ax
    lret

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
    movl $(0x8000 + !STACK32 * 0xbeef0000), %esp
    movl %esp, saved_sp
    xorw %ax, %ax
    movw $1, %cx
    int $0x31
    jc fail_pm
    movw %ax, %bx
    xorw %cx, %cx
    xorw %dx, %dx
    movw $7, %ax
    int $0x31
    jc fail_pm
    movw $0xffff, %cx
    movw %cx, %dx
    movw $8, %ax
    int $0x31
    jc fail_pm
    movw $0xc0f3, %cx
    movw $9, %ax
    int $0x31
    jc fail_pm
    movw %bx, %fs
    movb $6, stage
    movl $0x108000, %edi
    cmpw $0x5678, %fs:(%edi)
    jne fail_pm
    movw $0x2402, %ax
    int $0x15
    jc fail_pm
    cmpw $1, %ax
    jne fail_pm
    movb $7, stage
    movw real_cs, %ax
    movw %ax, rm_regs + 44
    movw $real_alias, rm_regs + 42
    pushw %ds
    popw %es
    movl $rm_regs, %edi
    xorw %cx, %cx
    xorw %bx, %bx
    movw $0x0301, %ax
    int $0x31
    jc fail_pm
    cmpw $0x1234, rm_regs + 28
    jne fail_pm
    movb $2, %al
    outb %al, $0x92
    movl $rm_regs, %edi
    xorw %cx, %cx
    xorw %bx, %bx
    movw $0x0301, %ax
    int $0x31
    jc fail_pm
    cmpw $0x5678, rm_regs + 28
    jne fail_pm
    xorb %al, %al
    outb %al, $0x92
    movl $0x108000, %edi
    cmpw $0x5678, %fs:(%edi)
    jne fail_pm
    cmpl saved_sp, %esp
    jne fail_pm
    movl $pass_text, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c2a, %ax
    int $0x21
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
.if MODE
pm_entry: .word protected_entry, 0
.endif
real_cs: .word 0
low_saved: .word 0
high_saved: .word 0
file_handle: .word 0
saved_sp: .long 0
stage: .byte 0
buffer_saved: .byte 0
saved_bytes: .space 32
rm_regs: .space 50
filename: .asciz "A20TEST.TMP"
pass_text: .asciz "DOS A20 PASS\r\n$"
fail_text: .asciz "DOS A20 FAIL stage=$"
