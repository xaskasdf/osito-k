/* Exact x87 environments across services, raw switches, waits and EXEC.
 * No FXSAVE/SIMD dependency: the same COM can test native, JIT and interpreter.
 * A rejected seed is a failure, never missing coverage relabelled as PASS. */
    .section .text
    .code16
    .global _start
_start:
    .ifdef CHILD_ONLY
    fninit
    fldz
    fld1
    movw $0x4c2b, %ax
    int $0x21
    .else
    cld
    pushw %cs
    popw %ds
    pushw %cs
    popw %es
    movw %cs, real_seg
    movw %cs, exec_block + 4
    movw %cs, exec_block + 8
    movw %cs, exec_block + 12
    movw $0x1000, %bx
    movw $0x4a00, %ax
    int $0x21
    jc setup_fail
    movw $0x1687, %ax
    int $0x2f
    testw %ax, %ax
    jnz setup_fail
    movw %di, entry
    movw %es, entry + 2
    movw $1, %ax
    lcall *entry
    jc setup_fail
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
setup_fail:
    movw $0x4c70, %ax
    int $0x21
entry: .word 0, 0
pm_entry: .word protected_entry, 0

    .code32
protected_entry:
    movw %ss, %bx
    movw $(0xf2 | STACK32 * 0x4000), %cx
    movw $9, %ax
    int $0x31
    jc fail
    movw %bx, %ss
    movl $(0x8000 + !STACK32 * 0xbeef0000), %esp
    movl %esp, saved_sp
    movw %ss, saved_ss
    movw %cs, saved_cs
    movw %ds, saved_ds
    movw $0x0306, %ax
    int $0x31
    jc fail
    movw %cx, rm_switch
    movw %bx, rm_switch + 2
    movl %edi, pm_switch
    movw %si, pm_switch + 4
    fninit
    fld1
    fldpi
    fldl number
    fnsave before
    movw $(0x0b7f - PENDING), before
    .if PENDING
    orw $0x8081, before + 4
    .endif
    movl $0x12345678, before + 12
    movw %cs, before + 16
    movw $0x345, before + 18
    movl $0x23456789, before + 20
    movw %ds, before + 24
    frstor before
    call check

    movb $1, scenario
    movl $64, iterations
service_loop:
    movw $0x0400, %ax
    int $0x31
    jc fail
    call check
    movw $0x3000, %ax
    int $0x21
    call check
    decl iterations
    jnz service_loop

    movb $2, scenario
    movl $4, iterations
raw_loop:
    movw real_seg, %ax
    movw %ax, %cx
    movw %ax, %dx
    movw %ax, %si
    movl $0x6000, %ebx
    movl $real_code, %edi
    ljmp *pm_switch
raw_return:
    cmpl saved_sp, %esp
    jne fail
    call check
    decl iterations
    jnz raw_loop
    cmpb $4, scenario
    je pass

    movb $3, scenario
    movl $100000, %esi
wait_loop:
    xorl %eax, %eax
    cpuid
    decl %esi
    jnz wait_loop
    call check
    movb $4, scenario
    movl $1, iterations
    jmp raw_loop

check:
    pushal
    push %es
    movw %ds, %ax
    movw %ax, %es
    cld
    fnsave after
    movb $'E', detail
    /* Reserved words are not state. Compare only defined environment fields. */
    .irp offset,0,4,8,16,18,24
    movw before + \offset, %ax
    cmpw %ax, after + \offset
    jne fail
    .endr
    movb $'P', detail
    .irp offset,12,20
    movl before + \offset, %eax
    cmpl %eax, after + \offset
    jne fail
    .endr
    movb $'R', detail
    movl $before + 28, %esi
    movl $after + 28, %edi
    movl $20, %ecx
    repe cmpsl
    jne fail
    frstor after
    incw checks
    pop %es
    popal
    ret
pass:
    cmpw $135, checks
    jne fail
    fninit
    movl $passed, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c2a, %ax
    int $0x21
fail:
    fninit
    movl $failed, %edx
    movw $0x0900, %ax
    int $0x21
    movb scenario, %dl
    addb $'0', %dl
    movb $2, %ah
    int $0x21
    movb detail, %dl
    movb $2, %ah
    int $0x21
    .irp offset,4,12,16,20,24
    movl before + \offset, %eax
    call print_hex
    movl after + \offset, %eax
    call print_hex
    .endr
    movw $0x4c71, %ax
    int $0x21
print_hex:
    pushal
    movl %eax, %ebx
    movb $' ', %dl
    movb $2, %ah
    int $0x21
    movl $8, %ecx
1:  roll $4, %ebx
    movb %bl, %dl
    andb $15, %dl
    addb $'0', %dl
    cmpb $'9', %dl
    jbe 2f
    addb $7, %dl
2:  movb $2, %ah
    int $0x21
    loop 1b
    popal
    ret

    .code16
real_code:
    cmpb $4, scenario
    jne 1f
    pushw %ds
    popw %es
    movw $child_name, %dx
    movw $exec_block, %bx
    movw $0x4b00, %ax
    int $0x21
    jc real_fail
    movw $0x4d00, %ax
    int $0x21
    cmpw $0x002b, %ax
    jne real_fail
1:  movw $0x3000, %ax
    int $0x21
    movw saved_ds, %ax
    movw %ax, %cx
    movw saved_ss, %dx
    movl saved_sp, %ebx
    movw saved_cs, %si
    movl $raw_return, %edi
    ljmp *rm_switch
real_fail:
    movw $0x4c72, %ax
    int $0x21

    .balign 16
before: .space 108
after: .space 108
number: .double -123.456
saved_sp: .long 0
saved_ss: .word 0
saved_cs: .word 0
saved_ds: .word 0
real_seg: .word 0
pm_switch: .long 0; .word 0
rm_switch: .word 0, 0
iterations: .long 0
checks: .word 0
scenario: .byte 0
detail: .byte '0'
child_name: .asciz "dpxdc.com"
exec_block: .word 0, command_tail, 0, 0x5c, 0, 0x6c, 0
command_tail: .byte 0, 13
passed: .ascii "DPMI X87 DIAGNOSTIC PASS checks=135\r\n$"
failed: .ascii "DPMI X87 DIAGNOSTIC FAIL case=$"
    .endif
