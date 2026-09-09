/* DPMI string port I/O in USE16/USE32, software CPU and native mediation.
 * The virtual VGA DAC supplies an observable, deterministic port stream. */
    .section .text
    .code16
    .global _start
_start:
    cld
    pushw %cs
    popw %ds
    movw $0x1687, %ax
    int $0x2f
    testw %ax, %ax
    jnz setup_fail
    movw %di, dpmi_entry
    movw %es, dpmi_entry + 2
    movw $PM32, %ax
    lcall *dpmi_entry
    jc setup_fail
    .if PM32
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
    movw %ax, pm_pointer + 2
    movw %ax, %bx
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
    ljmp *pm_pointer
    .else
    jmp port_entry
    .endif
setup_fail:
    movw $0x4c70, %ax
    int $0x21
dpmi_entry: .word 0, 0
pm_pointer: .word port_entry, 0
base_hi: .word 0
base_lo: .word 0

    .macro port_string input, width, address32
    .if (\width == 4 && PM32 == 0) || (\width == 2 && PM32 == 1)
    .byte 0x66
    .endif
    .if \address32 != PM32
    .byte 0x67
    .endif
    .if \input
    .byte 0x2e             /* INS must ignore CS: and still write ES. */
    .endif
    .byte 0xf3
    .if \width == 1
    .byte 0x6e - 2 * \input
    .else
    .byte 0x6f - 2 * \input
    .endif
    .endm

    .macro port_case width, address32, backwards
    cld
    movw %ds, %ax
    movw %ax, %es
    movl $buffer, %edi
    movl $1032, %ecx
    movb $0x2a, %al
    rep stosb
    movw $0x3c8, %dx
    xorb %al, %al
    outb %al, %dx
    movw $0x3da, %dx
    movb $0x35, %al
    outb %al, %dx
    movw $0x3c2, %dx
    movb $0x6b, %al
    outb %al, %dx
    .set start_index, buffer + 256 * \width * \backwards
    .set end_index, buffer + (257 - 258 * \backwards) * \width
    .set upper_index, 0xabcd0000 * (1 - \address32)
    .set upper_count, 0x12340000 * (1 - \address32)
    movl $(upper_index + start_index), %esi
    movl $(upper_count + 257), %ecx
    movw $0x3c9, %dx
    .if \backwards
    std
    .endif
    stc
    port_string 0, \width, \address32
    jnc fail
    cmpl $(upper_index + end_index), %esi
    jne fail
    cmpl $upper_count, %ecx
    jne fail
    movw $0x3c7, %dx
    xorb %al, %al
    outb %al, %dx
    movw $0x3c9, %dx
    movl $(upper_index + start_index), %edi
    movl $(upper_count + 257), %ecx
    stc
    port_string 1, \width, \address32
    jnc fail
    cmpl $(upper_index + end_index), %edi
    jne fail
    cmpl $upper_count, %ecx
    jne fail
    pushfl
    popl %eax
    andl $0x400, %eax
    cmpl $(0x400 * \backwards), %eax
    jne fail
    cld
    movl $buffer, %esi
    movl $257, %ecx
1:
    .if \width == 1
    cmpb $0x2a, (%esi)
    .elseif \width == 2
    cmpw $0x352a, (%esi)
    .else
    cmpl $0x6bff352a, (%esi)
    .endif
    jne fail
    addl $\width, %esi
    decl %ecx
    jnz 1b
    cmpb $0x2a, (%esi)
    jne fail
    .endm

    .if PM32
    .code32
    .endif
port_entry:
    movl %esp, saved_sp
    .irp width,1,2,4
    .irp address32,0,1
    .irp backwards,0,1
    port_case \width, \address32, \backwards
    .endr
    .endr
    .endr

    /* A zero REP count must neither dereference null ES nor consume the DAC. */
    xorw %ax, %ax
    movw %ax, %es
    xorl %ecx, %ecx
    port_string 1, 4, 1
    movw %ds, %ax
    movw %ax, %es

    .if PM32
    /* Two dwords fit; #GP on the third must retain ECX=1 and the REP EIP.
     * The handler enlarges the segment and returns without patching EIP. */
    movw %cs, %cx
    movl $gp_handler, %edx
    movb $13, %bl
    movw $0x0203, %ax
    int $0x31
    jc fail
    movw %es, %bx
    xorw %cx, %cx
    movw $(buffer + 7), %dx
    movw $8, %ax
    int $0x31
    jc fail
    movw %bx, %es
    movw $0x3c7, %dx
    xorb %al, %al
    outb %al, %dx
    movw $0x3c9, %dx
    movl $buffer, %edi
    movl $3, %ecx
partial_rep:
    rep insl
    testl %ecx, %ecx
    jnz fail
    cmpl $(buffer + 12), %edi
    jne fail
    cmpl $1, fault_count
    jne fail
    .endif
    cmpl saved_sp, %esp
    jne fail
    movl $pass, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c2a, %ax
    int $0x21
fail:
    cld
    movw $0x4c71, %ax
    int $0x21

    .if PM32
gp_handler:
    pushl %ebp
    movl %esp, %ebp
    pushal
    cmpl $0, %ss:12(%ebp)
    jne fail
    cmpl $partial_rep, %ss:16(%ebp)
    jne fail
    cmpl $1, %ecx
    jne fail
    cmpl $(buffer + 8), %edi
    jne fail
    incl fault_count
    movw %es, %bx
    xorw %cx, %cx
    movw $0xffff, %dx
    movw $8, %ax
    int $0x31
    jc fail
    movw %bx, %es
    popal
    popl %ebp
    lretl
    .endif
pass: .asciz "[DPMI-STRINGIO] PASS\r\n$"
saved_sp: .long 0
fault_count: .long 0
    .balign 4
buffer: .fill 1032, 1, 0x2a
