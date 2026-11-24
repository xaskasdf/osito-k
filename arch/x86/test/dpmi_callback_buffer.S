/* Return an RM callback through a different, read-only register buffer.
 * Also relocate the 0301h caller's output selector inside that callback. */
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
    movw %cs, %bx
    movw $6, %ax
    int $0x31
    jc setup_fail
    movw %cx, base_hi
    movw %dx, base_lo
    .if PM32
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
    .equ FIELD, 4
    .else
    .equ FIELD, 2
    .endif
test_entry:
    movw $0x0900, %ax
    int $0x31
    movw %cs, %cx
    movl $descriptor_fault, %edx
    movw $13, %bx
    movw $0x0203, %ax
    int $0x31
    jc fail
    movw %ds, data_sel
    movw %ds, %bx
    movw $0x000a, %ax
    int $0x31
    jc fail
    movw %ax, output_sel
    movw %ds, %bx
    movw $0x000a, %ax
    int $0x31
    jc fail
    movw %ax, readonly_sel
    movw %ax, %bx
    movw $0x00f0, %cx
    movw $9, %ax
    int $0x31
    jc fail
    /* Descriptor records use the caller's buffer permissions, not BX's.
     * 000Ch must consume the complete read-only input before editing BX. */
    movw data_sel, %ax
    movw %ax, %es
    movw %ax, %bx
    movl $descriptor_record, %edi
    .if !PM32
    orl $0xa5a50000, %edi
    .endif
    movw $0x000b, %ax
    int $0x31
    jc fail
    movw readonly_sel, %ax
    movw %ax, %es
    movw output_sel, %bx
    movw $0x000c, %ax
    int $0x31
    jc fail
    movw $0x000b, %ax
    stc
    int $0x31
after_bad_buffer:
    jmp fail
buffer_rejected:
    jnc fail
    cmpw $0x000b, %ax
    jne fail
    cmpb $1, descriptor_faults
    jne fail
    movw output_sel, %ax
    movw %ax, %es
    movw $0x000c, %ax
    int $0x31
    jc fail
    movl $descriptor_copy, %edi
    .if !PM32
    orl $0x5a5a0000, %edi
    .endif
    movw $0x000b, %ax
    int $0x31
    jc fail
    movl descriptor_copy, %eax
    cmpl descriptor_record, %eax
    jne fail
    movl descriptor_copy + 4, %eax
    cmpl descriptor_record + 4, %eax
    jne fail
    movw %ds, %ax
    movw %ax, %es
    movw %cs, %ax
    movw %ax, %ds
    movl $callback, %esi
    movl $callback_regs, %edi
    movw $0x0303, %ax
    int $0x31
    jc fail
    movw %cs:data_sel, %ax
    movw %ax, %ds
    movw %cx, rm_call + 44
    movw %dx, rm_call + 42
    movw output_sel, %ax
    movw %ax, %es
    movl $rm_call, %edi
    .if !PM32
    orl $0xa5a50000, %edi
    .endif
    xorw %bx, %bx
    xorw %cx, %cx
    movw $0x0301, %ax
    int $0x31
    jc fail
    cmpl $0xcafebabe, relocated_call + 28
    jne fail
    cmpl $0, rm_call + 28
    jne fail
    cmpl $0xcafebabe, alternate_regs + 28
    jne fail
    cmpl $0, callback_regs + 28
    jne fail
    cmpb $1, called
    jne fail
    movw rm_call + 44, %cx
    movw rm_call + 42, %dx
    movw $0x0304, %ax
    int $0x31
    jc fail
    movw $0x4c2a, %ax
    int $0x21
fail:
    movw $0x4c80, %ax
    int $0x21

descriptor_fault:
    /* Reject the inaccessible output without retrying or changing its rights. */
    pushal
    movl %esp, %ebp
    movw readonly_sel, %ax
    andw $0xfffc, %ax
    cmpw %ax, 32 + 2 * FIELD(%ebp)
    jne fail
    .if PM32
    cmpl $after_bad_buffer, 32 + 3 * FIELD(%ebp)
    .else
    cmpw $after_bad_buffer, 32 + 3 * FIELD(%ebp)
    .endif
    jne fail
    movw %cs, %ax
    cmpw %ax, 32 + 4 * FIELD(%ebp)
    jne fail
    .if PM32
    movl $buffer_rejected, 32 + 3 * FIELD(%ebp)
    .else
    movw $buffer_rejected, 32 + 3 * FIELD(%ebp)
    .endif
    incb descriptor_faults
    popal
    lret

callback:
    /* DS:(E)SI points at the real FAR return address. */
    movw (%esi), %ax
    movw %ax, %es:42(%edi)
    movw 2(%esi), %ax
    movw %ax, %es:44(%edi)
    addw $4, %es:46(%edi)
    pushw %es
    popw %ds
    movl $callback_regs, %esi
    movl $alternate_regs, %edi
    movl $25, %ecx
    rep movsw
    movl $0xcafebabe, alternate_regs + 28
    incb called
    /* Legal descriptor relocation while a host RM call is suspended. */
    movzwl base_hi, %edx
    shll $16, %edx
    movw base_lo, %dx
    addl $(relocated_call - rm_call), %edx
    movl %edx, %ecx
    shrl $16, %ecx
    movw output_sel, %bx
    movw $7, %ax
    int $0x31
    jc fail
    movw readonly_sel, %ax
    movw %ax, %es
    movl $alternate_regs, %edi
    .if !PM32
    orl $0x5a5a0000, %edi
    .endif
    iret

data_sel: .word 0
output_sel: .word 0
readonly_sel: .word 0
descriptor_faults: .byte 0
called: .byte 0
    .balign 4
descriptor_record: .space 8
descriptor_copy: .space 8
rm_call: .space 50
relocated_call: .space 50
callback_regs: .space 50
alternate_regs: .space 50
