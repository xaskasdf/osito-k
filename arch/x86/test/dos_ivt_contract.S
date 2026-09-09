/* Unassigned vectors share a callable IRET; installed services stay distinct. */
    .section .text
    .code16
    .global _start
_start:
    cld
    pushw %cs
    popw %ds
    movw $0x3580, %ax
    int $0x21
    movw %bx, default_vector
    movw %es, default_vector+2
    cmpb $0xcf, %es:(%bx)
    jne fail
    movw $0x81, %si
check_unused:
    movw %si, %ax
    movb $0x35, %ah
    int $0x21
    cmpw default_vector, %bx
    jne fail
    movw %es, %ax
    cmpw default_vector+2, %ax
    jne fail
    incw %si
    cmpw $0x100, %si
    jb check_unused

    stc
    pushfw
    lcall *default_vector
    jnc fail
    movw $0x1234, %ax
    int $0x80
    cmpw $0x1234, %ax
    jne fail
    movw $0x3533, %ax
    int $0x21
    cmpb $0xcf, %es:(%bx)
    je fail
    movw $0x3523, %ax
    int $0x21
    cmpw default_vector, %bx
    je fail
    movw $0x3524, %ax
    int $0x21
    cmpw default_vector, %bx
    je fail

    movw $0x3521, %ax
    int $0x21
    movw %bx, dos_vector
    movw %es, dos_vector+2
    movw $0x3000, %ax
    int $0x21
    movw %ax, version
    movw %sp, saved_sp
    movw $80, repeats
warm_service:
    movw $0x3000, %ax
    stc
    sti
    std
    pushfw
    cli
    cld
    lcall *dos_vector
    pushfw
    popw %dx
    cld
    cmpw version, %ax
    jne fail
    andw $0x0600, %dx
    cmpw $0x0600, %dx
    jne fail
    cmpw saved_sp, %sp
    jne fail
    decw repeats
    jnz warm_service

    movw $0x4400, %ax
    movw $0xffff, %bx
    clc
    pushfw
    lcall *dos_vector
    jnc fail
    cmpw $6, %ax
    jne fail
    movw $0x4400, %ax
    xorw %bx, %bx
    stc
    pushfw
    lcall *dos_vector
    jc fail

    movw $0x3515, %ax
    int $0x21
    movw %bx, bios_vector
    movw %es, bios_vector+2
    movw $0xbf00, %ax
    clc
    pushfw
    lcall *bios_vector
    jnc fail
    movw $0xe801, %ax
    stc
    pushfw
    lcall *bios_vector
    jc fail
    testw %ax, %ax
    jz fail

    /* A guest INT F8h outside the ROM entry must still use the guest IVT. */
    movw $private_hook, %dx
    movw $0x25f8, %ax
    int $0x21
    int $0xf8
    cmpb $1, hook_seen
    jne fail
    pushw %ds
    movw default_vector+2, %ax
    movw %ax, %ds
    xorw %dx, %dx
    movw $0x25f8, %ax
    int $0x21
    popw %ds

    /* A replaced DOS vector must run before the saved ROM service. */
    movw $dos_hook, %dx
    movw $0x2521, %ax
    int $0x21
    movw $0x3000, %ax
    int $0x21
    cmpw version, %ax
    jne fail
    cmpb $1, dos_hook_seen
    jne fail
    pushw %ds
    movw dos_vector, %dx
    movw dos_vector+2, %ax
    movw %ax, %ds
    movw $0x2521, %ax
    pushfw
    lcall *%cs:dos_vector
    popw %ds

    /* LIDT redirects CPU INTs, not DOS's AH=25/35 IVT services. */
    pushfw
    cli
    sidt saved_idtr
    pushw %es
    pushw %ds
    pushw %cs
    popw %es
    xorw %ax, %ax
    movw %ax, %ds
    xorw %si, %si
    movw $relocated_ivt, %di
    movw $512, %cx
    rep movsw
    popw %ds
    popw %es
    xorl %eax, %eax
    movw %cs, %ax
    shll $4, %eax
    addl $relocated_ivt, %eax
    movl %eax, new_idtr+2
    movw $relocated_hook, relocated_ivt+0x80*4
    movw %cs, relocated_ivt+0x80*4+2
    lidt new_idtr
    int $0x80
    .byte 0x66
    int $0x80
    lidt saved_idtr
    popfw
    cmpb $2, relocated_seen
    jne fail

    movw $pass_text, %dx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c00, %ax
    int $0x21
fail:
    movw $fail_text, %dx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c01, %ax
    int $0x21
private_hook:
    incb %cs:hook_seen
    iret
dos_hook:
    incb %cs:dos_hook_seen
    ljmp *%cs:dos_vector
relocated_hook:
    incb %cs:relocated_seen
    iret
default_vector: .long 0
dos_vector: .long 0
bios_vector: .long 0
version: .word 0
saved_sp: .word 0
repeats: .word 0
hook_seen: .byte 0
dos_hook_seen: .byte 0
relocated_seen: .byte 0
saved_idtr: .space 6
new_idtr: .word 0x3ff
    .long 0
pass_text: .asciz "DOS IVT PASS\r\n$"
fail_text: .asciz "DOS IVT FAIL\r\n$"
    .balign 4
relocated_ivt: .space 1024
