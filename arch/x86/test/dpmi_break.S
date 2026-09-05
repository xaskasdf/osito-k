/* Build twice: USE16 and native USE32. No keyboard input required. */
    .section .text
    .code16
    .global _start
_start:
    cld
    pushw %cs
    popw %ds
    pushw %cs
    popw %es
    movw %cs, real_segment
    movw $0x5ff0, %sp
    movw $0x600, %bx
    movw $0x4a00, %ax
    int $0x21
    jc initial_fail
    xorw %bx, %bx
    movw $0x4500, %ax
    int $0x21
    jc initial_fail
    movw %ax, saved_stdin
    movw $file_name, %dx
    xorw %cx, %cx
    movw $0x5b00, %ax
    int $0x21
    jc initial_fail
    movw %ax, file_handle
    movb $1, owned
    movw %ax, %bx
    movw $input, %dx
    movw $input_end-input, %cx
    movw $0x4000, %ax
    int $0x21
    jc initial_fail
    xorw %cx, %cx
    xorw %dx, %dx
    movw $0x4200, %ax
    int $0x21
    jc initial_fail
    xorw %cx, %cx
    movw $0x4600, %ax
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
    movw $0x0006, %ax
    int $0x31
    jc initial_fail
    movw %cx, base_hi
    movw %dx, base_lo
    movw $1, %cx
    xorw %ax, %ax
    int $0x31
    jc initial_fail
    movw %ax, %bx
    movw %ax, code_pointer + 2
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
    call cleanup
    movw $0x4c01, %ax
    int $0x21

/* Same cleanup instructions work in either mode; assembler supplies the
 * appropriate operand-size prefixes for the two copies. */
    .macro CLEANUP
    movw saved_stdin, %bx
    cmpw $0xffff, %bx
    je 1f
    xorw %cx, %cx
    movw $0x4600, %ax
    int $0x21
    movw $0x3e00, %ax
    int $0x21
1:
    movw file_handle, %bx
    cmpw $0xffff, %bx
    je 2f
    movw $0x3e00, %ax
    int $0x21
2:
    cmpb $0, owned
    je 3f
    movw $file_name, %dx
    movw $0x4100, %ax
    int $0x21
    jnc 3f
    movb $20, stage
3:
    ret
    .endm
cleanup:
    CLEANUP

    .ifdef CLIENT32
    .code32
    .endif
protected_entry:
    movb $2, stage
    movw data_selector, %ax
    movw %ax, %es
    pushw %es
    movw $0x3400, %ax
    int $0x21
    cmpb $0, %es:(%bx)
    popw %es
    jne fail
    movw $0x0800, %ax /* Unhooked protected INT 23h must not abort. */
    int $0x21
    cmpb $'A', %al
    jne fail
    movb $3, stage
    movw $0x0204, %ax
    movb $0x23, %bl
    int $0x31
    jc fail
    .ifdef CLIENT32
    movl %edx, old_vector
    movw %cx, old_vector + 4
    .else
    movw %dx, old_vector
    movw %cx, old_vector + 2
    .endif
    movw %cs, %cx
    movl $handler, %edx
    movw $0x0205, %ax
    int $0x31
    jc fail
    movl %esp, saved_sp
    movl $0x12345678, %ebx
    movl $0x87654321, %ecx
    movl $0xabcdef98, %edx
    movw $0x0800, %ax
    int $0x21
    cmpb $'B', %al
    jne fail
    cmpl $1, calls
    jne fail
    cmpl $0x12345678, %ebx
    jne fail
    cmpl $0x87654321, %ecx
    jne fail
    cmpl $0xabcdef98, %edx
    jne fail
    cmpl saved_sp, %esp
    jne fail

    movb $4, stage
    movb $1, chaining
    movw $0x0800, %ax
    int $0x21
    cmpb $'C', %al
    jne fail
    cmpl $2, calls
    jne fail

    movb $5, stage
    movb $0, chaining
    movl $0x0800, rm_regs + 28
    movw $0x0202, rm_regs + 32
    movw $0x0300, %ax
    movw $0x21, %bx
    xorw %cx, %cx
    movl $rm_regs, %edi
    int $0x31
    jc fail
    movb $6, stage
    cmpb $'D', rm_regs + 28
    jne fail
    movb $7, stage
    cmpl $3, calls
    jne fail
    movb $8, stage
    cmpl saved_sp, %esp
    jne fail
    movb $9, stage
    cmpb $0, handler_error
    jne fail
    movb $0, stage
fail:
    /* Restore input before any reporting or cleanup call can check it. */
    call protected_cleanup
    movw $pass, %dx
    cmpb $0, stage
    je report
    movw $failure, %dx
report:
    movw $0x0900, %ax
    int $0x21
    movb stage, %al
    movb $0x4c, %ah
    int $0x21
protected_cleanup:
    CLEANUP

handler:
    pushal
    pushw %ds
    pushw %es
    movw %cs:data_selector, %ax
    movw %ax, %ds
    movw %ax, %es
    incl calls
    movw $0x3400, %ax
    int $0x21
    cmpb $0, %es:(%bx)
    je indos_ok
    movb $1, handler_error
indos_ok:
    cmpb $0, chaining
    je handler_done
    .ifdef CLIENT32
    pushfl
    .else
    pushfw
    .endif
    lcall *old_vector
handler_done:
    popw %es
    popw %ds
    popal
    /* Even a set carry in the returned IRET frame must not request abort. */
    .ifdef CLIENT32
    pushl %ebp
    movl %esp, %ebp
    orl $1, 12(%ebp)
    popl %ebp
    .else
    pushw %bp
    movw %sp, %bp
    orw $1, 6(%bp)
    popw %bp
    .endif
    iret

stage: .byte 1
owned: .byte 0
chaining: .byte 0
handler_error: .byte 0
saved_stdin: .word 0xffff
file_handle: .word 0xffff
real_segment: .word 0
data_selector: .word 0
base_hi: .word 0
base_lo: .word 0
entry: .long 0
code_pointer: .word protected_entry, 0
old_vector: .space 6
calls: .long 0
saved_sp: .long 0
rm_regs: .space 50
input: .byte 3, 'A', 3, 'B', 3, 'C', 3, 'D'
input_end:
    .ifdef CLIENT32
file_name: .asciz "DBPM32.TMP"
pass: .ascii "DPMI32 BREAK CONTRACT PASS\r\n$"
failure: .ascii "DPMI32 BREAK CONTRACT FAIL\r\n$"
    .else
file_name: .asciz "DBPM16.TMP"
pass: .ascii "DPMI16 BREAK CONTRACT PASS\r\n$"
failure: .ascii "DPMI16 BREAK CONTRACT FAIL\r\n$"
    .endif
