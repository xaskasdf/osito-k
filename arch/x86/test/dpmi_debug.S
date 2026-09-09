/* ICEBP must deliver #DB as a trap, with the unmodified next IP. */
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
    jmp debug_entry
    .endif
setup_fail:
    movw $0x4c70, %ax
    int $0x21
dpmi_entry: .word 0, 0
pm_pointer: .word debug_entry, 0
base_hi: .word 0
base_lo: .word 0

    .if PM32
    .code32
    .endif
debug_entry:
    movw %cs, %cx
    movl $debug_handler, %edx
    movb $1, %bl
    movw $0x0203, %ax
    int $0x31
    jc fail
    movl %esp, saved_sp
    movl $after_icebp, expected_ip
    .byte 0xf1
after_icebp:
    cmpl $1, calls
    jne fail
    cmpl saved_sp, %esp
    jne fail
    movl $pass, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c2a, %ax
    int $0x21
fail:
    movl $failure, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c72, %ax
    int $0x21

debug_handler:
    .if PM32
    pushl %ebp
    movl %esp, %ebp
    pushl %eax
    movl %ss:16(%ebp), %eax
    .else
    pushw %bp
    movw %sp, %bp
    pushl %eax
    movzwl %ss:8(%bp), %eax
    .endif
    movl %eax, observed_ip
    cmpl expected_ip, %eax
    jne return_mismatch
    incl calls
    popl %eax
    .if PM32
    popl %ebp
    .else
    popw %bp
    .endif
    lret

return_mismatch:
    /* Do not LRET to an incorrect IP or hide the mismatch by adjusting it. */
    movl $observed_hex, %edi
    call hex32
    movl expected_ip, %eax
    movl $expected_hex, %edi
    call hex32
    movl $mismatch, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c71, %ax
    int $0x21
hex32:
    movw $8, %cx
hex_digit:
    roll $4, %eax
    movb %al, %dl
    andb $15, %dl
    addb $'0', %dl
    cmpb $'9', %dl
    jbe hex_store
    addb $('A'-'9'-1), %dl
hex_store:
    movb %dl, (%edi)
    incl %edi
    decw %cx
    jnz hex_digit
    ret

saved_sp: .long 0
expected_ip: .long 0
observed_ip: .long 0
calls: .long 0
    .if PM32
pass: .ascii "DPMI32 ICEBP CONTRACT PASS\r\n$"
failure: .ascii "DPMI32 ICEBP CONTRACT FAIL\r\n$"
mismatch: .ascii "DPMI32 ICEBP RETURN MISMATCH observed="
    .else
pass: .ascii "DPMI16 ICEBP CONTRACT PASS\r\n$"
failure: .ascii "DPMI16 ICEBP CONTRACT FAIL\r\n$"
mismatch: .ascii "DPMI16 ICEBP RETURN MISMATCH observed="
    .endif
observed_hex: .ascii "00000000 expected="
expected_hex: .ascii "00000000\r\n$"
