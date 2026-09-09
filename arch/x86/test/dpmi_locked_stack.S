/* INT 1Ch -> software handler -> RM callback -> #UD, on one locked stack. */
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
    jmp test_entry
    .endif
setup_fail:
    movw $0x4c70, %ax
    int $0x21
dpmi_entry: .word 0, 0
pm_pointer: .word test_entry, 0
base_hi: .word 0
base_lo: .word 0

    .if PM32
    .code32
    .endif
test_entry:
    movw $0x0900, %ax
    int $0x31
    movw %ss, main_ss
    movl %esp, main_sp
    movw %ds, data_ss
    movw %ds, %ax
    movw %ax, %es
    movw %cs, %ax
    movw %ax, %ds
    movl $callback, %esi
    movl $callback_regs, %edi
    movw $0x0303, %ax
    int $0x31
    jc fail
    movw %cs:data_ss, %ax
    movw %ax, %ds
    movw %cx, rm_call + 44
    movw %dx, rm_call + 42
    movw %cs, %cx
    movl $irq_handler, %edx
    movb $0x1c, %bl
    movw $0x0205, %ax
    int $0x31
    jc fail
    movw %cs, %cx
    movl $software_handler, %edx
    movb $0x60, %bl
    movw $0x0205, %ax
    int $0x31
    jc fail
    movw %cs, %cx
    movl $exception_handler, %edx
    movb $6, %bl
    movw $0x0203, %ax
    int $0x31
    jc fail
    movl $2, %esp
    int $0x1c
    cmpl $2, %esp
    jne fail
    movw %ss, %ax
    cmpw %cs:main_ss, %ax
    jne fail
    int $0x1c
    cmpl $2, %esp
    jne fail
    movl %cs:main_sp, %esp
    cmpb $2, %cs:irq_count
    jne fail
    cmpb $2, %cs:software_count
    jne fail
    cmpb $2, %cs:callback_count
    jne fail
    cmpb $2, %cs:exception_count
    jne fail

    /* A handler may redirect execution and replace the interrupted stack.
     * Both aliases retain the original storage, but have distinct selectors. */
    movw %cs, %bx
    movw $0x000a, %ax
    int $0x31
    jc fail
    movw %ax, return_cs
    movw %ax, %bx
    .if PM32
    movw $0x40fa, %cx
    .else
    movw $0x00fa, %cx
    .endif
    movw $9, %ax
    int $0x31
    jc fail
    movw %ss, %bx
    movw $0x000a, %ax
    int $0x31
    jc fail
    movw %ax, return_ss
    movw %cs, %cx
    movl $redirect_handler, %edx
    movb $6, %bl
    movw $0x0203, %ax
    int $0x31
    jc fail
    .if PM32
    orl $0xbeef0000, %esp
    movl %esp, main_sp
    .endif
    ud2
    jmp fail
redirected:
    movw %cs, %ax
    cmpw return_cs, %ax
    jne fail
    movw %ss, %ax
    cmpw return_ss, %ax
    jne fail
    cmpl main_sp, %esp
    jne fail
    /* Returning an out-of-limit ESP is legal; using it for the next INT
     * is not. Keep the high-word assertion above, then restore backing. */
    andl $0xffff, %esp
    movw $0x4c2a, %ax
    int $0x21
fail:
    movl %cs:main_sp, %esp
    andl $0xffff, %esp
    movw $0x4c80, %ax
    int $0x21

irq_handler:
    movw %ss, %ax
    cmpw %cs:main_ss, %ax
    je fail
    movw %ax, irq_ss
    movl %esp, irq_sp
    incb irq_count
    int $0x60
    movw %cs:data_ss, %ax
    movw %ax, %es
    movl $rm_call, %edi
    xorw %bx, %bx
    xorw %cx, %cx
    movw $0x0301, %ax
    int $0x31
    jc fail
    cmpl %cs:irq_sp, %esp
    jne fail
    iret

software_handler:
    movw %ss, %ax
    cmpw %cs:irq_ss, %ax
    jne fail
    cmpl %cs:irq_sp, %esp
    jae fail
    incb software_count
    iret

callback:
    movw %ss, %ax
    cmpw %cs:irq_ss, %ax
    jne fail
    cmpl %cs:irq_sp, %esp
    jae fail
    movl %esp, %es:callback_sp
    incb %es:callback_count
    ud2
    cmpl %cs:callback_sp, %esp
    jne fail
    /* The RM caller used FAR CALL, so return IP/CS live at DS:(E)SI. */
    .if PM32
    movw (%esi), %ax
    movw %ax, %es:42(%edi)
    movw 2(%esi), %ax
    movw %ax, %es:44(%edi)
    addw $4, %es:46(%edi)
    .else
    movw (%si), %ax
    movw %ax, %es:42(%di)
    movw 2(%si), %ax
    movw %ax, %es:44(%di)
    addw $4, %es:46(%di)
    .endif
    iret

exception_handler:
    pushl %eax
    pushl %ebp
    movw %ss, %ax
    cmpw %cs:irq_ss, %ax
    jne fail
    cmpl %cs:callback_sp, %esp
    jae fail
    incb %es:exception_count
    .if PM32
    movl %esp, %ebp
    addl $2, %ss:20(%ebp)
    .else
    movw %sp, %bp
    addw $2, %ss:14(%bp)
    .endif
    popl %ebp
    popl %eax
    lret

redirect_handler:
    pushl %eax
    pushl %ebp
    .if PM32
    movl %esp, %ebp
    movl $redirected, %ss:20(%ebp)
    movw %cs:return_cs, %ax
    movw %ax, %ss:24(%ebp)
    movl %cs:main_sp, %eax
    movl %eax, %ss:32(%ebp)
    movw %cs:return_ss, %ax
    movw %ax, %ss:36(%ebp)
    .else
    movw %sp, %bp
    movw $redirected, %ss:14(%bp)
    movw %cs:return_cs, %ax
    movw %ax, %ss:16(%bp)
    movw %cs:main_sp, %ax
    movw %ax, %ss:20(%bp)
    movw %cs:return_ss, %ax
    movw %ax, %ss:22(%bp)
    .endif
    popl %ebp
    popl %eax
    lret

main_sp: .long 0
main_ss: .word 0
return_cs: .word 0
return_ss: .word 0
data_ss: .word 0
irq_sp: .long 0
irq_ss: .word 0
callback_sp: .long 0
irq_count: .byte 0
software_count: .byte 0
callback_count: .byte 0
exception_count: .byte 0
    .balign 4
rm_call: .space 50
callback_regs: .space 50
