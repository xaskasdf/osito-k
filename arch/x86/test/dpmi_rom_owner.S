/* Nested DPMI clients must not allocate stacks over VBE-owned ROM data.
 * Discover the ROM interval through VBE, then retain the root's byte copy
 * while 70 EXEC children enter their own DPMI clients and unwind. */
    .set DEPTH, 70
    .section .text
    .code16
    .global _start
_start:
    cld
    pushw %cs
    popw %ds
    pushw %cs
    popw %es
    movw %cs, real_seg
    movw $0x0ffe, %sp
    movw $0x100, %bx
    movw $0x4a00, %ax
    int $0x21
    jc setup_fail
    cmpb $3, 0x80
    je child
    cmpb $0, 0x80
    jne setup_fail
    movw %cs, root_seg
    movb $DEPTH, remaining
    movw $0x4f01, %ax
    movw $0x101, %cx
    movw $mode_info, %di
    int $0x10
    cmpw $0x004f, %ax
    jne setup_fail
    movw mode_info + 12, %ax
    movw %ax, rom_offset
    movw mode_info + 14, %ax
    movw %ax, rom_segment
    movw $0x4f0a, %ax
    xorw %bx, %bx
    int $0x10
    cmpw $0x004f, %ax
    jne setup_fail
    movw %es, %ax
    cmpw rom_segment, %ax
    jne setup_fail
    testw %cx, %cx
    jz setup_fail
    addw %di, %cx
    jc setup_fail
    subw rom_offset, %cx
    jc setup_fail
    cmpw $1024, %cx
    ja setup_fail
    movw %cx, rom_bytes
    movw rom_offset, %si
    movw $original, %di
    movw rom_segment, %ax
    pushw %ds
    popw %es
    movw %ax, %ds
    rep movsb
    pushw %cs
    popw %ds
    jmp start_dpmi
child:
    movw 0x81, %ax
    movw %ax, root_seg
    movb 0x83, %al
    movb %al, remaining
start_dpmi:
    pushw %cs
    popw %es
    movw root_seg, %ax
    movw %ax, child_tail + 1
    movb remaining, %al
    decb %al
    movb %al, child_tail + 3
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
    movw $0x000a, %ax
    int $0x31
    jc setup_fail
    movw %ax, protected_pointer + 2
    movw %ax, %bx
    movw $0x40fa, %cx
    movw $9, %ax
    int $0x31
    jc setup_fail
    ljmp *protected_pointer
    .else
    jmp protected_entry
    .endif
setup_fail:
    movw $setup_message, %dx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c70, %ax
    int $0x21
dpmi_entry: .word 0, 0
protected_pointer: .word protected_entry, 0

    .if PM32
    .code32
    .endif
protected_entry:
    movw %ds, parameters + 4
    movw %ds, parameters + 8
    movw %ds, parameters + 12
    movw root_seg, %bx
    movw $0x0002, %ax
    int $0x31
    jc failed
    movw %ax, %fs
    movw %fs:rom_segment, %bx
    movw $0x0002, %ax
    int $0x31
    jc failed
    movw %ax, %gs
    incw %fs:entered
    call check_rom
    cmpb $0, remaining
    je done_child
    movw %ds, %ax
    movw %ax, %es
    movl $child_name, %edx
    movl $parameters, %ebx
    movw $0x4b00, %ax
    int $0x21
    jc failed
    movw $0x4d00, %ax
    int $0x21
    cmpw $0x002a, %ax
    jne propagate
done_child:
    call check_rom
    movw real_seg, %ax
    cmpw root_seg, %ax
    jne success
    cmpw $(DEPTH + 1), %fs:entered
    jne failed
    cmpw $(2 * (DEPTH + 1)), %fs:snapshots
    jne failed
    movl $pass_message, %edx
    movw $0x0900, %ax
    int $0x21
success:
    movw $0x4c2a, %ax
    int $0x21
check_rom:
    pushal
    movzwl %fs:rom_offset, %edi
    movzwl %fs:rom_bytes, %ecx
    movl $original, %esi
1:  movb %fs:(%esi), %al
    cmpb %gs:(%edi), %al
    jne corrupted
    incl %esi
    incl %edi
    loop 1b
    incw %fs:snapshots
    popal
    ret
corrupted:
    movw %di, %ax
    subw %fs:rom_offset, %ax
    movw %ax, bad_offset
    movl $corrupt_message, %edx
    movw $0x0900, %ax
    int $0x21
    movw %fs:entered, %ax
    call print_hex
    movw bad_offset, %ax
    call print_hex
    jmp propagate
failed:
    movw %ax, service_error
    movl $fail_message, %edx
    movw $0x0900, %ax
    int $0x21
    movw service_error, %ax
    call print_hex
propagate:
    movw $0x4c71, %ax
    int $0x21
print_hex:
    pushal
    movw %ax, %bx
    movb $' ', %dl
    movb $2, %ah
    int $0x21
    movl $4, %ecx
1:  rolw $4, %bx
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

root_seg: .word 0
real_seg: .word 0
remaining: .byte 0
rom_segment: .word 0
rom_offset: .word 0
rom_bytes: .word 0
entered: .word 0
snapshots: .word 0
bad_offset: .word 0xffff
service_error: .word 0
parameters: .word 0, child_tail, 0, 0x5c, 0, 0x6c, 0
child_tail: .byte 3; .word 0; .byte 0, 13
    .if PM32
child_name: .asciz "dprom32o.com"
    .else
child_name: .asciz "dprom16o.com"
    .endif
setup_message: .ascii "DPMI ROM OWNER SETUP FAIL\r\n$"
fail_message: .ascii "DPMI ROM OWNER SERVICE FAIL\r\n$"
corrupt_message: .ascii "DPMI ROM OWNER CORRUPTED client/offset:$"
pass_message: .ascii "DPMI ROM OWNER PASS clients=71 snapshots=142\r\n$"
mode_info: .space 256
original: .space 1024
