/* Protected legacy console translation. An optional command tail enables
 * keyboard checks: send J after FLUSH READY, then XY CR after LINE READY.
 * CREATE_NEW fixtures are removed only when created by this probe. */
    .section .text
    .code16
    .global _start
    .ifdef CLIENT32
    .set BUFFER_OFF, 0x10ffd
    .set STRING_BYTES, 0x10015
    .set BUFFER_LIMIT, 0x3ffff
    .set IRET_BYTES, 12
    .else
    .set BUFFER_OFF, 0xffd
    .set STRING_BYTES, 0x1015
    .set BUFFER_LIMIT, 0xffff
    .set IRET_BYTES, 6
    .endif
_start:
    cld
    pushw %cs
    popw %ds
    pushw %cs
    popw %es
    movw %cs, real_segment
    movb 0x80, %al
    movb %al, interactive
    movw $0x5ff0, %sp
    movw $0x600, %bx
    movw $0x4a00, %ax
    int $0x21
    jc initial_fail
    xorw %bx, %bx
    movw $0x4500, %ax
    int $0x21
    jc initial_fail
    movw %ax, saved_in
    movw $1, %bx
    movw $0x4500, %ax
    int $0x21
    jc initial_fail
    movw %ax, saved_out
    movw $input_name, %dx
    xorw %cx, %cx
    movw $0x5b00, %ax
    int $0x21
    jc initial_fail
    movw %ax, input_handle
    movw %ax, %bx
    movw $input_data, %dx
    movw $input_end-input_data, %cx
    movw $0x4000, %ax
    int $0x21
    jc initial_fail
    xorw %cx, %cx
    xorw %dx, %dx
    movw $0x4200, %ax
    int $0x21
    jc initial_fail
    movw $output_name, %dx
    xorw %cx, %cx
    movw $0x5b00, %ax
    int $0x21
    jc initial_fail
    movw %ax, output_handle
    movw $0x1687, %ax
    int $0x2f
    testw %ax, %ax
    jnz initial_fail
    movw %es, entry+2
    movw %di, entry
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

real_console_hook:
    movb $0, %cs:hook_seen
    cmpb $9, %ah
    jne hook_result
    testw %dx, %dx
    jnz hook_result
    movw %ds, %ax
    cmpw %cs:buffer_selector, %ax
    je hook_result
    cmpw $0xf000, %ax
    jae hook_result
    movw %dx, %bx
    cmpw $0x2451, (%bx)
    jne hook_result
    movb $1, %cs:hook_seen
hook_result:
    pushw %bp
    movw %sp, %bp
    orw $0x40, 6(%bp)
    popw %bp
    pushw %cs
    popw %ds
    pushw %cs
    popw %es
    movl $0xc3c30924, %eax
    movl $0xa1a11234, %ebx
    movl $0xd2d25678, %ecx
    movl $0xb4b40000, %edx
    iret

    .ifdef CLIENT32
    .code32
    .endif
protected_entry:
    movb $2, stage
    movw $4, %bx
    xorw %cx, %cx
    movw $0x0501, %ax
    int $0x31
    jc fail
    movw %bx, base_hi
    movw %cx, base_lo
    movw %si, memory_handle
    movw %di, memory_handle+2
    xorw %ax, %ax
    movw $1, %cx
    int $0x31
    jc fail
    movw %ax, buffer_selector
    movw %ax, %bx
    movw base_hi, %cx
    movw base_lo, %dx
    movw $7, %ax
    int $0x31
    jc fail
    call full_limit
    movw buffer_selector, %ax
    movw %ax, %es
    call largest_free
    movw %bx, free_before
    movw input_handle, %bx
    xorw %cx, %cx
    call duplicate
    movw output_handle, %bx
    movw $1, %cx
    call duplicate

    /* A hot BIOS simulation must not poison subsequent DOS translations. */
    pushw %es
    pushw %ds
    popw %es
warm_bios:
    movl $warm_regs, %edi
    movw $0x11, %bx
    xorw %cx, %cx
    movw $0x0300, %ax
    int $0x31
    jc fail
    decw warm_left
    jnz warm_bios
    popw %es

    /* The first byte straddles a page; USE32 also uses EDX above 64 KiB. */
    movb $3, stage
    call init_line
    movl $0xa5a50a00, %eax
    movl $0xc7c71234, %ecx
    call console_buffer
    jc fail
    cmpl $0xa5a50a00, %eax
    jne fail
    cmpl $0xc7c71234, %ecx
    jne fail
    .ifdef CLIENT32
    cmpl $BUFFER_OFF, %edx
    .else
    cmpl $(0xd8d80000+BUFFER_OFF), %edx
    .endif
    jne fail
    cmpb $8, %es:BUFFER_OFF
    jne fail
    cmpb $2, %es:BUFFER_OFF+1
    jne fail
    cmpw $0x4341, %es:BUFFER_OFF+2
    jne fail
    cmpb $13, %es:BUFFER_OFF+4
    jne fail
    cmpb $0xa5, %es:BUFFER_OFF+5
    jne fail
    cmpb $0xa5, %es:BUFFER_OFF+10
    jne fail
    call check_free

    movb $4, stage
    movb $2, %es:BUFFER_OFF
    movw $0x0c0a, %ax
    call console_buffer
    jc fail
    cmpb $1, %es:BUFFER_OFF+1
    jne fail
    cmpw $0x0d45, %es:BUFFER_OFF+2
    jne fail
    movb $0, %es:BUFFER_OFF
    movb $0x5a, %es:BUFFER_OFF+1
    movw $0x0a00, %ax
    call console_buffer
    jc fail
    cmpb $0x5a, %es:BUFFER_OFF+1
    jne fail
    /* A zero maximum only reads one byte, even at a read-only limit. */
    movw buffer_selector, %bx
    movw $(BUFFER_OFF >> 16), %cx
    movw $(BUFFER_OFF & 0xffff), %dx
    movw $8, %ax
    int $0x31
    jc fail
    movw $0xf0, %cx
    movw $9, %ax
    int $0x31
    jc fail
    movw $0x0a00, %ax
    clc
    call console_buffer
    jc fail
    movw $0x0c0a, %ax
    clc
    call console_buffer
    jc fail
    movw buffer_selector, %bx
    movw $0xf2, %cx
    movw $9, %ax
    int $0x31
    jc fail
    call full_limit
    /* Invalid flush subfunctions have no pointer argument. */
    pushw %ds
    xorw %ax, %ax
    movw %ax, %ds
    movl $0xffffffff, %edx
    movw $0x0c55, %ax
    int $0x21
    popw %ds
    testb %al, %al
    jnz fail
    movw $0x0b00, %ax
    int $0x21
    cmpb $0xff, %al
    jne fail
    movb $0xff, %dl
    movw $0x0600, %ax
    int $0x21
    jz fail
    cmpb $'K', %al
    jne fail
    movw $0x0700, %ax
    int $0x21
    testb %al, %al
    jnz fail
    movw $0x0800, %ax
    int $0x21
    cmpb $'L', %al
    jne fail
    movw $0x0100, %ax
    int $0x21
    cmpb $0x1a, %al
    jne fail
    movw $0x0c07, %ax
    int $0x21
    cmpb $'D', %al
    jne fail
    movw $0x0b00, %ax
    int $0x21
    testb %al, %al
    jnz fail
    movb $0xff, %dl
    movw $0x0600, %ax
    int $0x21
    jnz fail
    testb %al, %al
    jnz fail
    call check_free

    /* CF is unspecified/preserved on legacy success, not a chunk counter. */
    movb $5, stage
    call truncate_output
    movl $BUFFER_OFF, %edi
    movl $STRING_BYTES, %ecx
    movb $'Q', %al
    rep stosb
    movb $'$', %es:(%edi)
    movl $0xa5a50900, %eax
    movl $0xc7c71234, %ecx
    stc
    call console_buffer
    jnc fail
    cmpl $0xa5a50924, %eax
    jne fail
    cmpl $0xc7c71234, %ecx
    jne fail
    movw output_handle, %bx
    xorw %cx, %cx
    xorw %dx, %dx
    movw $0x4202, %ax
    int $0x21
    jc fail
    cmpw $(STRING_BYTES & 0xffff), %ax
    jne fail
    cmpw $(STRING_BYTES >> 16), %dx
    jne fail
    call rewind
    movl $STRING_BYTES, %ecx
    movw $0x3f00, %ax
    call console_buffer
    jc fail
    movl $BUFFER_OFF, %edi
    movl $STRING_BYTES, %ecx
    movb $'Q', %al
    repe scasb
    jne fail
    call check_free

    movb $6, stage
    movw %cs, %cx
    movl $buffer_fault_handler, %edx
    movw $13, %bx
    movw $0x0203, %ax
    int $0x31
    jc fail
    /* Read-only sources and a terminator at the descriptor limit work. */
    movb $'$', %es:BUFFER_OFF
    movw buffer_selector, %bx
    movw $(BUFFER_OFF >> 16), %cx
    movw $(BUFFER_OFF & 0xffff), %dx
    movw $8, %ax
    int $0x31
    jc fail
    movw $0x0900, %ax
    clc
    call console_buffer
    jc fail
    movb $'Q', %es:BUFFER_OFF
    movw $0x0900, %ax
    call console_buffer
    jnc fail
    cmpw $0x0900, %ax
    jne fail
    cmpb $1, buffer_faults
    jne fail
    call full_limit
    movb $'$', %es:BUFFER_OFF
    movw buffer_selector, %bx
    movw $0xf0, %cx
    movw $9, %ax
    int $0x31
    jc fail
    movw $0x0900, %ax
    clc
    call console_buffer
    jc fail
    movw $0x0a00, %ax
    call console_buffer
    jnc fail
    cmpw $0x0a00, %ax
    jne fail
    cmpb $2, buffer_faults
    jne fail
    movw buffer_selector, %bx
    movw $0xf2, %cx
    movw $9, %ax
    int $0x31
    jc fail
    call check_free

    movb $7, stage
    pushw %ds
    pushw %cs
    popw %ds
    movl $critical_handler, %edx
    movw $0x2524, %ax
    int $0x21
    popw %ds
    movw $0x0300, expected_ax
    movw $0x0300, %ax
    clc
    int $0x21
    jnc fail
    cmpw $21, %ax
    jne fail
    cmpw $1, calls
    jne fail
    xorw %bx, %bx
    movw $0x5900, %ax
    int $0x21
    cmpw $21, %ax
    jne fail
    movw buffer_selector, %ax
    movw %ax, %es
    movb $0, action
    movw $0x0400, expected_ax
    movb $'X', %dl
    movw $0x0400, %ax
    clc
    int $0x21
    jc fail
    cmpb $'X', %al
    jne fail
    movb $1, action
    movb $2, retries
    movw $0, calls
    movw $0x0500, expected_ax
    movw $0x0500, %ax
    int $0x21
    jnc fail
    cmpw $21, %ax
    jne fail
    cmpw $3, calls
    jne fail
    cmpb $0, handler_error
    jne fail

    /* Nested legacy output must have its own RM buffer and stack. */
    movb $8, stage
    movw $4, %bx
    movw $1, %cx
    call duplicate
    movw $0x2451, %es:BUFFER_OFF
    movb $4, action
    movw $0, calls
    movw $0x0900, expected_ax
    movw $0x0900, %ax
    call console_buffer
    jnc fail
    cmpw $21, %ax
    jne fail
    cmpw $1, calls
    jne fail
    cmpb $0, handler_error
    jne fail
    movw output_handle, %bx
    movw $1, %cx
    call duplicate
    call check_free

    movb $9, stage
    /* A redirected Ctrl-C must reach protected INT 23h and resume input. */
    pushw %ds
    pushw %cs
    popw %ds
    movl $break_handler, %edx
    movw $0x2523, %ax
    int $0x21
    popw %ds
    movw input_handle, %bx
    call rewind
    movl $break_data, %edx
    movl $3, %ecx
    movw $0x4000, %ax
    int $0x21
    jc fail
    call rewind
    call init_line
    movw $0x0a00, %ax
    call console_buffer
    jc fail
    cmpw $1, breaks
    jne fail
    cmpb $1, %es:BUFFER_OFF+1
    jne fail
    cmpw $0x0d52, %es:BUFFER_OFF+2
    jne fail
    call check_free

    movb $10, stage
    movw free_before, %bx
    subw $0x102, %bx
    movw $0x0100, %ax
    int $0x31
    jc fail
    movw %dx, reserve_selector
    movl $BUFFER_OFF, %edi
    movl $17, %ecx
    movb $'Q', %al
    rep stosb
    movb $'$', %es:(%edi)
    movw $0x0900, %ax
    clc
    call console_buffer
    jc fail
    cmpb $'$', %al
    jne fail
    movb $255, %es:BUFFER_OFF
    movw $0x0c0a, %ax
    call console_buffer
    jnc fail
    cmpw $8, %ax
    jne fail
    call largest_free
    cmpw $0x101, %bx
    jne fail
    movw $0x0100, %ax
    int $0x31
    jc fail
    movw %dx, last_selector
    movb $'Z', %dl
    movw $0x0200, %ax
    int $0x21
    jnc fail
    cmpw $8, %ax
    jne fail
    movw last_selector, %dx
    movw $0x0101, %ax
    int $0x31
    jc fail
    movw reserve_selector, %dx
    movw $0x0101, %ax
    int $0x31
    jc fail
    call check_free

    movb $12, stage
    /* Real-mode hooks can change high words. Restore only translated DX,
     * never erase those changes or expose returned real segment values. */
    movw $0x2451, %es:BUFFER_OFF
    movw $0x0200, %ax
    movb $0x21, %bl
    int $0x31
    jc fail
    movw %cx, old_vector+2
    movw %dx, old_vector
    movw real_segment, %cx
    movw $real_console_hook, %dx
    movw $0x0201, %ax
    int $0x31
    jc fail
    movw $0x0900, %ax
    clc
    call console_buffer
    pushfw
    popw hook_flags
    pushal
    movw old_vector+2, %cx
    movw old_vector, %dx
    movb $0x21, %bl
    movw $0x0201, %ax
    int $0x31
    popal
    cmpl $0xc3c30924, %eax
    jne fail
    cmpl $0xa1a11234, %ebx
    jne fail
    cmpl $0xd2d25678, %ecx
    jne fail
    cmpl $(0xb4b40000+(BUFFER_OFF & 0xffff)), %edx
    jne fail
    cmpb $1, hook_seen
    jne fail
    testw $0x40, hook_flags
    jz fail
    movw %es, %ax
    cmpw buffer_selector, %ax
    jne fail
    call check_free

    movb $11, stage
    call restore_handles
    cmpb $0, interactive
    je finish
    movl $flush_ready, %edx
    movw $0x0900, %ax
    int $0x21
wait_flush:
    movw $0x0100, %ax
    int $0x16
    jz wait_flush
    cmpb $'J', %al
    jne fail
    movb $0xff, %dl
    movw $0x0c06, %ax
    int $0x21
    jnz fail
    testb %al, %al
    jnz fail
    movw $0x0100, %ax
    int $0x16
    jnz fail
    movl $line_ready, %edx
    movw $0x0900, %ax
    int $0x21
    call init_line
    movw $0x0c0a, %ax
    call console_buffer
    jc fail
    cmpb $2, %es:BUFFER_OFF+1
    jne fail
    cmpw $0x5958, %es:BUFFER_OFF+2
    jne fail
    cmpb $13, %es:BUFFER_OFF+4
    jne fail
    call check_free
finish:
    call cleanup
    movw memory_handle, %si
    movw memory_handle+2, %di
    movw $0x0502, %ax
    int $0x31
    jc fail
    movl $pass, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c00, %ax
    int $0x21
fail:
    movw %cs:data_selector, %ax
    movw %ax, %ds
    call restore_handles
    call cleanup
    movl $failure, %edx
    movw $0x0900, %ax
    int $0x21
    movb stage, %al
    movb $0x4c, %ah
    int $0x21
console_buffer:
    .ifdef CLIENT32
    movl $BUFFER_OFF, %edx
    .else
    movl $(0xd8d80000+BUFFER_OFF), %edx
    .endif
    pushw %ds
    movw %cs:buffer_selector, %ds
    int $0x21
console_after_int:
    popw %ds
    ret
console_fault_return:
    popw %ds
    ret
buffer_fault_handler:
    .ifdef CLIENT32
    .equ EXCEPTION_FIELD, 4
    .else
    .equ EXCEPTION_FIELD, 2
    .endif
    pushal
    movl %esp, %ebp
    pushw %ds
    movw %cs:data_selector, %ax
    movw %ax, %ds
    cmpb $6, stage
    jne fail
    movw %ss:32+3*EXCEPTION_FIELD(%ebp), %ax
    cmpw $console_after_int, %ax
    jne fail
    movw %ss:32+2*EXCEPTION_FIELD(%ebp), %ax
    cmpb $0, buffer_faults
    jne buffer_access_fault
    testw %ax, %ax
    jnz fail
    jmp buffer_fault_cancel
buffer_access_fault:
    cmpb $1, buffer_faults
    jne fail
    movw buffer_selector, %bx
    andw $0xfffc, %bx
    cmpw %bx, %ax
    jne fail
buffer_fault_cancel:
    incb buffer_faults
    .ifdef CLIENT32
    movl $console_fault_return, %ss:32+3*EXCEPTION_FIELD(%ebp)
    .else
    movw $console_fault_return, %ss:32+3*EXCEPTION_FIELD(%ebp)
    .endif
    orw $1, %ss:32+5*EXCEPTION_FIELD(%ebp)
    popw %ds
    popal
    lret
init_line:
    movl $BUFFER_OFF, %edi
    movl $11, %ecx
    movb $0xa5, %al
    rep stosb
    movb $8, %es:BUFFER_OFF
    ret
full_limit:
    movw buffer_selector, %bx
    movw $(BUFFER_LIMIT >> 16), %cx
    movw $0xffff, %dx
    movw $8, %ax
    int $0x31
    jc fail
    ret
duplicate:
    movw $0x4600, %ax
    int $0x21
    jc fail
    ret
rewind:
    xorw %cx, %cx
    xorw %dx, %dx
    movw $0x4200, %ax
    int $0x21
    jc fail
    ret
truncate_output:
    movw output_handle, %bx
    call rewind
    xorl %ecx, %ecx
    movw $0x4000, %ax
    int $0x21
    jc fail
    ret
largest_free:
    movw $0xffff, %bx
    movw $0x0100, %ax
    int $0x31
    jnc fail
    cmpw $8, %ax
    jne fail
    ret
check_free:
    pushal
    call largest_free
    cmpw free_before, %bx
    jne fail
    popal
    ret
restore_handles:
    movw saved_in, %bx
    xorw %cx, %cx
    movw $0x4600, %ax
    int $0x21
    movw saved_out, %bx
    movw $1, %cx
    movw $0x4600, %ax
    int $0x21
    ret
cleanup:
    cmpw $0, input_handle
    je cleanup_output
    movw input_handle, %bx
    movw $0x3e00, %ax
    int $0x21
    movl $input_name, %edx
    movw $0x4100, %ax
    int $0x21
    movw $0, input_handle
cleanup_output:
    cmpw $0, output_handle
    je cleanup_done
    movw output_handle, %bx
    movw $0x3e00, %ax
    int $0x21
    movl $output_name, %edx
    movw $0x4100, %ax
    int $0x21
    movw $0, output_handle
cleanup_done:
    ret
critical_handler:
    pushal
    pushw %ds
    pushw %es
    movw %cs:data_selector, %ax
    movw %ax, %ds
    incw calls
    movw expected_ax, %ax
    cmpw %ax, %ss:IRET_BYTES(%ebp)
    jne handler_bad
    cmpb $4, action
    jne handler_done
    cmpw $0, %ss:IRET_BYTES+6(%ebp)
    jne handler_bad
    movw %ss:IRET_BYTES+14(%ebp), %ax
    cmpw buffer_selector, %ax
    je handler_bad
    cmpw $0xf000, %ax
    jae handler_bad
    movw $0x0900, %ax
    call console_buffer
    jnc handler_bad
    cmpw $21, %ax
    jne handler_bad
    movw $0x0300, %ax
    int $0x21
    jnc handler_bad
    cmpw $21, %ax
    jne handler_bad
    jmp handler_done
handler_bad:
    movb $1, handler_error
handler_done:
    popw %es
    popw %ds
    popal
    movb %cs:action, %al
    cmpb $4, %al
    je handler_fail
    cmpb $1, %al
    jne handler_return
    cmpb $0, %cs:retries
    je handler_fail
    pushw %ds
    pushw %ax
    movw %cs:data_selector, %ax
    movw %ax, %ds
    decb retries
    popw %ax
    popw %ds
handler_return:
    iret
handler_fail:
    movb $3, %al
    iret
break_handler:
    pushal
    pushw %ds
    movw %cs:data_selector, %ax
    movw %ax, %ds
    incw breaks
    movl $nested_text, %edx
    movw $0x0900, %ax
    int $0x21
    popw %ds
    popal
    iret
stage: .byte 1
buffer_faults: .byte 0
warm_left: .word 64
warm_regs: .space 50
hook_seen: .byte 0
hook_flags: .word 0
real_segment: .word 0
old_vector: .long 0
interactive: .byte 0
action: .byte 3
retries: .byte 0
handler_error: .byte 0
calls: .word 0
breaks: .word 0
expected_ax: .word 0
data_selector: .word 0
buffer_selector: .word 0
base_hi: .word 0
base_lo: .word 0
saved_in: .word 0
saved_out: .word 0
input_handle: .word 0
output_handle: .word 0
free_before: .word 0
reserve_selector: .word 0
last_selector: .word 0
memory_handle: .long 0
entry: .long 0
code_pointer: .word protected_entry, 0
input_data: .ascii "Ab\bC\r\nEF\rK\0L\032D"
input_end:
break_data: .byte 3, 'R', 13
nested_text: .ascii "nested$"
flush_ready: .ascii "DPMI CONSOLE FLUSH READY\r\n$"
line_ready: .ascii "DPMI CONSOLE LINE READY\r\n$"
    .ifdef CLIENT32
input_name: .asciz "dpcon32.in"
output_name: .asciz "dpcon32.out"
pass: .ascii "DPMI32 CONSOLE CONTRACT PASS\r\n$"
failure: .ascii "DPMI32 CONSOLE CONTRACT FAIL\r\n$"
    .else
input_name: .asciz "dpcon16.in"
output_name: .asciz "dpcon16.out"
pass: .ascii "DPMI16 CONSOLE CONTRACT PASS\r\n$"
failure: .ascii "DPMI16 CONSOLE CONTRACT FAIL\r\n$"
    .endif
