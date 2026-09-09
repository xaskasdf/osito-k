/* Software-CPU DPMI FLAGS contract, 16- and 32-bit clients.
 * The native IOPL0 profile is intentionally not required to pass this probe.
 * Successful virtual IOPL3 execution returns 2Ah. */

    .section .text
    .code16
    .global _start
_start:
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
    jmp flags_entry
    .endif

setup_fail:
    movw $0x4c70, %ax
    int $0x21
dpmi_entry: .word 0, 0
pm_pointer: .word flags_entry, 0
base_hi: .word 0
base_lo: .word 0

    .macro vif expected, error
    movw $0x0902, %ax
    int $0x31
    cmpw $(0x0900 + \expected), %ax
    je 1f
    movw $(0x4c00 + \error), %ax
    int $0x21
1:
    .endm

    .macro flags_if expected, error
    pushfl
    popl %eax
    andw $0x0200, %ax
    cmpw $(0x0200 * \expected), %ax
    je 1f
    movw $(0x4c00 + \error), %ax
    int $0x21
1:
    .endm

    .if PM32
    .code32
    .endif
flags_entry:
    movl %esp, entry_sp
    sti
    stc
    std
    pushfl
    cli
    flags_if 0, 0x71
    pushfw
    cli
    popfw
    vif 0, 0x72
    flags_if 0, 0x73
    popfl
    pushfl
    popl %eax
    andw $0x3401, %ax
    cmpw $0x3401, %ax       /* IOPL3, DF and CF survived the outer restore. */
    jne flags_fail
    cld
    vif 1, 0x74
    flags_if 1, 0x75

    /* No CLI: POPFD alone must disable and then re-enable the virtual IF.
     * A CPL3 client cannot lower the software CPU's IOPL using POPFD. */
    pushfl
    popl %eax
    andl $~0x3200, %eax
    pushl %eax
    popfl
    vif 0, 0x76
    flags_if 0, 0x77
    pushfl
    popl %eax
    orl $0x200, %eax
    pushl %eax
    popfl
    vif 1, 0x78
    pushfw
    popw %ax
    andw $0x3000, %ax
    cmpw $0x3000, %ax
    jne flags_fail

    /* DPMI's enable/disable API and FLAGS must describe the same state. */
    movw $0x0900, %ax
    int $0x31
    flags_if 0, 0x79
    movw $0x0901, %ax
    int $0x31
    flags_if 1, 0x7a

    movw %cs, %cx
    movl $software_handler, %edx
    movb $0x60, %bl
    movw $0x0205, %ax
    int $0x31
    jc flags_fail
    cli
    int $0x60
    vif 0, 0x7b
    cmpb $1, software_seen
    jne flags_fail
    sti

    movw %cs, %cx
    movl $timer_handler, %edx
    movb $8, %bl
    movw $0x0205, %ax
    int $0x31
    jc flags_fail
    inb $0x21, %al
    andb $0xfe, %al
    outb %al, $0x21
    movl $2000000, %ecx
timer_wait:
    cmpb $0, handler_error
    jne flags_fail
    cmpb $2, timer_seen
    jae timer_done
    decl %ecx
    jnz timer_wait
    movw $0x4c7c, %ax
    int $0x21
timer_done:
    vif 1, 0x7d
    flags_if 1, 0x7e
    cmpl entry_sp, %esp
    jne flags_fail
    movw $0x4c2a, %ax
    int $0x21
flags_fail:
    movw $0x4c7f, %ax
    int $0x21

software_handler:
    pushl %eax
    pushfl
    popl %eax
    testw $0x200, %ax
    jz software_ok
    movb $1, handler_error
software_ok:
    incb software_seen
    popl %eax
    .if PM32
    iretl
    .else
    iretw
    .endif

timer_handler:
    pushal
    pushfl
    popl %eax
    testw $0x200, %ax
    jnz timer_bad
    movw $0x0902, %ax
    int $0x31
    cmpw $0x0900, %ax
    jne timer_bad
    int $0x60                 /* Nested software IRET must keep IRQs disabled. */
    pushfl
    popl %eax
    testw $0x200, %ax
    jnz timer_bad
    incb timer_seen
    jmp timer_eoi
timer_bad:
    movb $1, handler_error
timer_eoi:
    movb $0x60, %al
    outb %al, $0x20
    popal
    /* No STI: the interrupted FLAGS image restores IF, at virtual IOPL3. */
    .if PM32
    iretl
    .else
    iretw
    .endif

software_seen: .byte 0
timer_seen: .byte 0
handler_error: .byte 0
entry_sp: .long 0
