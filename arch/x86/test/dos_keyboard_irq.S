/* IRQ1 through the 8042 output-buffer command. Build RM, DPMI16 and DPMI32. */
    .section .text
    .code16
    .global _start
_start:
    cld
    pushw %cs
    popw %ds
    pushw %cs
    popw %es
    movw $0x5ff0, %sp
    movw $0x600, %bx
    movw $0x4a00, %ax
    int $0x21
    jc initial_fail
    .ifndef REALMODE
    movw $0x1687, %ax
    int $0x2f
    testw %ax, %ax
    jnz initial_fail
    movw %di, entry
    movw %es, entry+2
    .ifdef CLIENT32
    movw $1, %ax
    .else
    xorw %ax, %ax
    .endif
    lcall *entry
    jc initial_fail
    .endif
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
    movw %ax, %bx
    movw %ax, code_pointer+2
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
    movw $0x4c01, %ax
    int $0x21

    .ifdef CLIENT32
    .code32
    .endif
protected_entry:
    inb $0x21, %al
    movb %al, old_mask
    .ifdef REALMODE
    movw $0x3509, %ax
    int $0x21
    movw %bx, old_vector
    movw %es, old_vector+2
    movw $handler, %dx
    movw $0x2509, %ax
    int $0x21
    .else
    movb $9, %bl
    movw $0x0204, %ax
    int $0x31
    jc fail
    movl %edx, old_vector
    movw %cx, old_vector+4
    movw %cs, %cx
    movl $handler, %edx
    movw $0x0205, %ax
    int $0x31
    jc fail
    .endif
    movb $1, installed
    cli
    movb $0xff, %al
    outb %al, $0x21
    movl $sequence, %esi
    movl $6, %ecx
send_next:
    movb $0xd2, %al
    outb %al, $0x64
    lodsb
    outb %al, $0x60
    loop send_next
    sti
    movl $100000, %ecx
masked_wait:
    decl %ecx
    jnz masked_wait
    cmpb $0, calls
    jne fail
    movb $0xfd, %al
    outb %al, $0x21
    movb $2, stage
    xorw %ax, %ax
    int $0x1a
    movw %dx, started
wait_irq:
    cmpb $6, calls
    je check
    xorw %ax, %ax
    int $0x1a
    subw started, %dx
    cmpw $90, %dx
    ja fail
    jmp wait_irq
check:
    cmpb $0, bad
    jne fail
    movl $sequence, %esi
    movl $received, %edi
    movw data_selector, %ax
    movw %ax, %es
    movl $6, %ecx
    repe cmpsb
    jne fail
    call restore
    movb $3, stage
    cli
    movb $0xd2, %al
    outb %al, $0x64
    movb $0x1e, %al
    outb %al, $0x60
    /* The blocking BIOS API must run the default IRQ1, not steal raw data. */
    xorw %ax, %ax
    int $0x16
    cmpw $0x1e61, %ax
    jne fail
    movl $pass_text, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c00, %ax
    int $0x21
fail:
    call restore
    movl $fail_text, %edx
    movw $0x0900, %ax
    int $0x21
    movb stage, %al
    movb $0x4c, %ah
    int $0x21
restore:
    cli
    cmpb $0, installed
    je restored
    .ifdef REALMODE
    pushw %ds
    movw old_vector, %dx
    movw old_vector+2, %ax
    movw %ax, %ds
    movw $0x2509, %ax
    int $0x21
    popw %ds
    .else
    movl old_vector, %edx
    movw old_vector+4, %cx
    movb $9, %bl
    movw $0x0205, %ax
    int $0x31
    .endif
    movb $0, installed
restored:
    movb old_mask, %al
    outb %al, $0x21
    sti
    ret
handler:
    pushl %eax
    pushl %ebx
    pushw %ds
    movw %cs:data_selector, %ax
    movw %ax, %ds
    movb $0x0b, %al
    outb %al, $0x20
    inb $0x20, %al
    testb $2, %al
    jnz in_service
    movb $1, bad
in_service:
    inb $0x60, %al
    movzbl calls, %ebx
    cmpb $6, %bl
    jae overflow
    movb %al, received(%bx)
    incb calls
    jmp ack
overflow:
    movb $1, bad
ack:
    movb $0x61, %al
    outb %al, $0x20
    popw %ds
    popl %ebx
    popl %eax
    sti
    iret
    .align 4
entry: .long 0
code_pointer: .word protected_entry, 0
base_hi: .word 0
base_lo: .word 0
data_selector: .word 0
old_vector: .long 0; .word 0
old_mask: .byte 0
installed: .byte 0
calls: .byte 0
bad: .byte 0
stage: .byte 1
started: .word 0
sequence: .byte 0x1e,0x9e,0xe0,0x48,0xe0,0xc8
received: .space 6
pass_text: .asciz "KEYBOARD IRQ PASS\r\n$"
fail_text: .asciz "KEYBOARD IRQ FAIL\r\n$"
