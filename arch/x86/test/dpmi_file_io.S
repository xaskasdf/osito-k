/* Direct protected INT 21h handle I/O, including reflected INT 24h. */
    .section .text
    .code16
    .global _start
    .ifdef CLIENT32
    .set BUFFER_OFF, 0x10023
    .set IO_BYTES, 0x20023
    .set IRET_BYTES, 12
    .else
    .set BUFFER_OFF, 0x100
    .set IO_BYTES, 0xfe00
    .set IRET_BYTES, 6
    .endif
_start:
    cld
    pushw %cs
    popw %ds
    pushw %cs
    popw %es
    movb 0x80, %al
    movb %al, console_test
    movw $0x5ff0, %sp
    movw $0x600, %bx
    movw $0x4a00, %ax
    int $0x21
    jc initial_fail
    movw $filename, %dx
    xorw %cx, %cx
    movw $0x3c00, %ax
    int $0x21
    jc initial_fail
    movw %ax, file_handle
    movw $0x1687, %ax
    int $0x2f
    testw %ax, %ax
    jnz initial_fail
    movw %es, entry + 2
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
    movw $0x4c01, %ax
    int $0x21

    .ifdef CLIENT32
    .code32
    .equ EXCEPTION_FIELD, 4
    .else
    .equ EXCEPTION_FIELD, 2
    .endif
protected_entry:
    movb $2, stage
    movl $0x11223344, %eax
    cmpl $0x11223344, %eax
    jne fail
    cmpl $0x55663344, %eax
    je fail
    jnc fail
    cmpw $0x3344, %ax
    jne fail
    cmpl $0x11223344, %eax
    jne fail
    movw $4, %bx
    xorw %cx, %cx
    movw $0x0501, %ax
    int $0x31
    jc fail
    movw %bx, base_hi
    movw %cx, base_lo
    movw %si, memory_handle
    movw %di, memory_handle + 2
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
    .ifdef CLIENT32
    movw $3, %cx
    .else
    xorw %cx, %cx
    .endif
    movw $0xffff, %dx
    movw $8, %ax
    int $0x31
    jc fail
    call largest_free
    movw %bx, free_before

    movb $3, stage
    movw buffer_selector, %ax
    movw %ax, %es
    call check_strings
    movl $BUFFER_OFF, %edi
    movl $IO_BYTES, %ecx
    movb $0x21, %al
fill_pattern:
    stosb
    incb %al
    decl %ecx
    jnz fill_pattern
    movb $0x5a, %es:(%edi)
    movl $IO_BYTES, %ecx
    movl $BUFFER_OFF, %edx
    movl $0xb6b60000, %ebx
    movw file_handle, %bx
    movl $0xa5a54000, %eax
    .ifndef CLIENT32
    orl $0xc7c70000, %ecx
    orl $0xd8d80000, %edx
    .endif
    movb $21, stage
    call file_io
    jc fail
    movb $22, stage
    .ifdef CLIENT32
    cmpl $IO_BYTES, %eax
    jne fail
    cmpl $BUFFER_OFF, %edx
    jne fail
    cmpl $IO_BYTES, %ecx
    .else
    cmpl $(0xa5a50000 + IO_BYTES), %eax
    jne fail
    movb $23, stage
    cmpl $(0xd8d80000 + BUFFER_OFF), %edx
    jne fail
    movb $24, stage
    cmpl $(0xc7c70000 + IO_BYTES), %ecx
    .endif
    jne fail
    movb $25, stage
    movl %ebx, %eax
    shrl $16, %eax
    cmpl $0xb6b6, %eax
    jne fail
    movb $26, stage
    call check_free

    movb $4, stage
    call seek_start
    movl $BUFFER_OFF, %edi
    movl $(IO_BYTES + 1), %ecx
    movb $0xa5, %al
clear_buffer:
    stosb
    decl %ecx
    jnz clear_buffer
    movl $BUFFER_OFF, %edx
    movl $IO_BYTES, %ecx
    movl $0xa5a53f00, %eax
    movw file_handle, %bx
    call file_io
    jc fail
    .ifdef CLIENT32
    cmpl $IO_BYTES, %eax
    .else
    cmpl $(0xa5a50000 + IO_BYTES), %eax
    .endif
    jne fail
    movl $BUFFER_OFF, %edi
    movl $IO_BYTES, %ecx
    movb $0x21, %al
check_pattern:
    cmpb %al, %es:(%edi)
    jne fail
    incl %edi
    incb %al
    decl %ecx
    jnz check_pattern
    cmpb $0xa5, %es:(%edi)
    jne fail
    call check_free

    movb $5, stage
    movl $BUFFER_OFF, %edx
    movl $17, %ecx
    movw $0x3f00, %ax
    movw file_handle, %bx
    call file_io
    jc fail
    testw %ax, %ax
    jnz fail
    /* Seek seven bytes before EOF, then verify a short read and its tail. */
    movw $0xffff, %cx
    movw $0xfff9, %dx
    movw $0x4202, %ax
    int $0x21
    jc fail
    movl $BUFFER_OFF, %edi
    movl $17, %ecx
    movb $0xa5, %al
short_clear:
    stosb
    decl %ecx
    jnz short_clear
    movl $BUFFER_OFF, %edx
    movl $17, %ecx
    movw $0x3f00, %ax
    call file_io
    jc fail
    cmpw $7, %ax
    jne fail
    movl $(BUFFER_OFF + 7), %edi
    cmpb $0xa5, %es:(%edi)
    jne fail

    movb $6, stage
    call seek_start
    /* Zero-byte writes must truncate, without touching even a null DS. */
    pushw %ds
    xorw %ax, %ax
    movw %ax, %ds
    movl $0xffffffff, %edx
    xorl %ecx, %ecx
    movw $0x4000, %ax
    int $0x21
    popw %ds
    jc fail
    testw %ax, %ax
    jnz fail
    xorw %cx, %cx
    xorw %dx, %dx
    movw $0x4202, %ax
    int $0x21
    jc fail
    orw %dx, %ax
    jnz fail
    call check_free

    movb $7, stage
    movw %cs, %cx
    movl $buffer_fault_handler, %edx
    movw $13, %bx
    movw $0x0203, %ax
    int $0x31
    jc fail
    /* Read-only data can be a write source, but not a read destination. */
    movw buffer_selector, %bx
    movw $0xf0, %cx
    movw $9, %ax
    int $0x31
    jc fail
    movw file_handle, %bx
    movl $BUFFER_OFF, %edx
    movl $17, %ecx
    movw $0x4000, %ax
    call file_io
    jc fail
    cmpw $17, %ax
    jne fail
    call seek_start
    movl $BUFFER_OFF, %edx
    movl $17, %ecx
    movw $0x3f00, %ax
    call file_io
    jnc fail
    cmpw $0x3f00, %ax
    jne fail
    cmpb $1, buffer_faults
    jne fail
    movw buffer_selector, %bx
    movw $0xf2, %cx
    movw $9, %ax
    int $0x31
    jc fail
    movw file_handle, %bx
    xorw %cx, %cx
    xorw %dx, %dx
    movw $0x4201, %ax
    int $0x21
    jc fail
    orw %dx, %ax
    jnz fail

    movb $8, stage
    .ifdef CLIENT32
    movl $0x3fff0, %edx
    .else
    movl $0xfff0, %edx
    .endif
    movl $32, %ecx
    movw $0x4000, %ax
    call file_io
    jnc fail
    cmpw $0x4000, %ax
    jne fail
    cmpb $2, buffer_faults
    jne fail
    movw file_handle, %bx
    xorw %cx, %cx
    xorw %dx, %dx
    movw $0x4201, %ax
    int $0x21
    jc fail
    orw %dx, %ax
    jne fail
    movb $9, stage
    /* Direct protected I/O must reach the protected INT 24h handler. */
    movw $0x2524, %ax
    movl $handler, %edx
    pushw %ds
    pushw %cs
    popw %ds
    int $0x21
    popw %ds
    movw $3, %bx
    movw $0x20, %dx
    movw $0x4401, %ax
    int $0x21
    jc fail
    call device_read
    jnc fail
    cmpw $5, %ax
    jne fail
    cmpw $1, calls
    jne fail
    cmpb $0, handler_error
    jne fail
    xorw %bx, %bx
    movw $0x5900, %ax
    int $0x21
    cmpw $83, %ax
    jne fail
    cmpw $0x0d04, %bx
    jne fail
    cmpb $1, %ch
    jne fail

    movb $10, stage
    movb $0, action
    movw $0, calls
    movw buffer_selector, %ax
    movw %ax, %es
    movl $BUFFER_OFF, %edi
    movl $0xa5a5a5a5, %es:(%edi)
    call device_read
    jc fail
    cmpw $3, %ax
    jne fail
    cmpw $1, calls
    jne fail
    movl $BUFFER_OFF, %edi
    cmpl $0xa5a5a5a5, %es:(%edi)
    jne fail
    movb $11, stage
    movb $1, action
    movb $2, retries
    movw $0, calls
    call device_read
    jnc fail
    cmpw $5, %ax
    jne fail
    cmpw $3, calls
    jne fail

    movb $12, stage
    /* Nested direct I/O needs independent real buffers and a real stack. */
    movb $4, action
    movw $0, calls
    call device_read
    jnc fail
    cmpw $1, calls
    jne fail
    cmpb $0, handler_error
    jne fail
    call check_free

    movb $13, stage
    /* Reduce the conventional arena to a 16-byte bounce buffer plus stack. */
    movw free_before, %bx
    subw $0x102, %bx
    movw $0x0100, %ax
    int $0x31
    jc fail
    movw %dx, reserve_selector
    cmpb $0, console_test
    je console_done
    movl $console_ready, %edx
    movw $0x0900, %ax
    int $0x21
    xorw %bx, %bx
    movl $32, %ecx
    movl $BUFFER_OFF, %edx
    movw $0x3f00, %ax
    call file_io
    jc fail
    cmpw $16, %ax
    jne fail
    movw buffer_selector, %ax
    movw %ax, %es
    movl $(BUFFER_OFF + 14), %edi
    cmpw $0x0a0d, %es:(%edi)
    jne fail
console_done:
    call seek_start
    movl $BUFFER_OFF, %edx
    movl $17, %ecx
    movw $0x4000, %ax
    call file_io
    jc fail
    cmpw $17, %ax
    jne fail
    call largest_free
    cmpw $0x101, %bx
    jne fail
    movw $0x0100, %ax
    int $0x31
    jc fail
    movw %dx, last_selector
    movb $14, stage
    movw file_handle, %bx
    movl $BUFFER_OFF, %edx
    movl $1, %ecx
    movw $0x4000, %ax
    call file_io
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

    movb $15, stage
    movw memory_handle, %si
    movw memory_handle + 2, %di
    movw $0x0502, %ax
    int $0x31
    jc fail
    movw file_handle, %bx
    movw $0x3e00, %ax
    int $0x21
    jc fail
    movl $filename, %edx
    movw $0x4100, %ax
    int $0x21
    jc fail
    movl $pass, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c00, %ax
    int $0x21
fail:
    pushl %eax
    movw %cs:data_selector, %ax
    movw %ax, %ds
    popl %eax
    movl %eax, failure_value
    movl $failure, %edx
    movw $0x0900, %ax
    int $0x21
    movl failure_value, %esi
    movl $8, %ecx
failure_hex:
    roll $4, %esi
    movl %esi, %edx
    andb $15, %dl
    addb $48, %dl
    cmpb $57, %dl
    jbe failure_digit
    addb $7, %dl
failure_digit:
    movb $2, %ah
    int $0x21
    decl %ecx
    jnz failure_hex
    movb $13, %dl
    movb $2, %ah
    int $0x21
    movb $10, %dl
    movb $2, %ah
    int $0x21
    movb stage, %al
    movb $0x4c, %ah
    int $0x21
file_io:
    pushw %ds
    movw %cs:buffer_selector, %ds
    int $0x21
file_io_after_int:
    popw %ds
    ret
file_io_fault_return:
    popw %ds
    ret
buffer_fault_handler:
    pushal
    movl %esp, %ebp
    pushw %ds
    movw %cs:data_selector, %ax
    movw %ax, %ds
    movw %ss:32+3*EXCEPTION_FIELD(%ebp), %ax
    cmpw $file_io_after_int, %ax
    jne fail
    movw %ss:32+2*EXCEPTION_FIELD(%ebp), %ax
    cmpb $7, stage
    jne buffer_limit_fault
    movw buffer_selector, %bx
    andw $0xfffc, %bx
    cmpw %bx, %ax
    jne fail
    jmp buffer_fault_cancel
buffer_limit_fault:
    cmpb $8, stage
    jne fail
    testw %ax, %ax
    jnz fail
buffer_fault_cancel:
    incb buffer_faults
    .ifdef CLIENT32
    movl $file_io_fault_return, %ss:32+3*EXCEPTION_FIELD(%ebp)
    .else
    movw $file_io_fault_return, %ss:32+3*EXCEPTION_FIELD(%ebp)
    .endif
    orw $1, %ss:32+5*EXCEPTION_FIELD(%ebp)
    popw %ds
    popal
    lret
check_strings:
    pushw %ds
    pushw %es
    popw %ds
    movl $0x40, %edi
    movl $0x11223344, %es:(%edi)
    movl $0x40, %esi
    movl $0x60, %edi
    movl $4, %ecx
    rep movsb
    movl $0x40, %esi
    movl $0x60, %edi
    movl $4, %ecx
    repe cmpsb
    jne fail
    movl $0x60, %esi
    lodsw
    cmpw $0x3344, %ax
    jne fail
    movl $0x60, %edi
    scasw
    jne fail
    std
    movl $0x43, %esi
    movl $0x83, %edi
    movl $4, %ecx
    rep movsb
    cld
    movl $0x80, %edi
    cmpl $0x11223344, %es:(%edi)
    jne fail
    popw %ds
    ret
seek_start:
    movw file_handle, %bx
    xorw %cx, %cx
    xorw %dx, %dx
    movw $0x4200, %ax
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
device_read:
    movw $3, %bx
    movl $3, %ecx
    movl $BUFFER_OFF, %edx
    movw $0x3f00, %ax
    call file_io
    ret
handler:
    pushal
    pushw %ds
    pushw %es
    movw %cs:data_selector, %ax
    movw %ax, %ds
    incw calls
    movb $1, handler_error
    cmpw $0x3f00, %ss:IRET_BYTES(%ebp)
    jne handler_done
    cmpw $3, %ss:IRET_BYTES+4(%ebp)
    jne handler_done
    cmpw $0, %ss:IRET_BYTES+6(%ebp)
    jne handler_done
    movw %ss:IRET_BYTES+14(%ebp), %ax
    cmpw buffer_selector, %ax
    je handler_done
    cmpw $0xf000, %ax
    jae handler_done
    cmpb $4, action
    jne handler_ok
    /* The nested error must not call INT 24h recursively. */
    call device_read
    jnc handler_done
    cmpw $5, %ax
    jne handler_done
    movw $4, %bx
    movl $BUFFER_OFF, %edx
    movl $1, %ecx
    movw $0x4000, %ax
    call file_io
    jnc handler_done
    cmpw $5, %ax
    jne handler_done
handler_ok:
    movb $0, handler_error
handler_done:
    popw %es
    popw %ds
    popal
    movb %cs:action, %al
    cmpb $0, %cs:handler_error
    jne handler_fail
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

stage: .byte 1
buffer_faults: .byte 0
action: .byte 3
retries: .byte 0
handler_error: .byte 0
console_test: .byte 0
calls: .word 0
file_handle: .word 0
data_selector: .word 0
buffer_selector: .word 0
base_hi: .word 0
base_lo: .word 0
free_before: .word 0
reserve_selector: .word 0
last_selector: .word 0
memory_handle: .long 0
failure_value: .long 0
console_ready: .ascii "DPMI FILE IO CONSOLE READY\r\n$"
entry: .long 0
code_pointer: .word protected_entry, 0
    .ifdef CLIENT32
filename: .asciz "dpio32.tmp"
pass: .ascii "DPMI32 FILE IO CONTRACT PASS\r\n$"
failure: .ascii "DPMI32 FILE IO CONTRACT FAIL\r\n$"
    .else
filename: .asciz "dpio16.tmp"
pass: .ascii "DPMI16 FILE IO CONTRACT PASS\r\n$"
failure: .ascii "DPMI16 FILE IO CONTRACT FAIL\r\n$"
    .endif
