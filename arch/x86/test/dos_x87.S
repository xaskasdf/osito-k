/* A real DOS program, including hot interpreter/JIT boundaries and RM->PM FP. */
    .section .text
    .code16
    .global _start
_start:
    cld
    pushw %cs
    popw %ds
    pushw %cs
    popw %es
    movw $no_fpu, %dx
    movw $0x2507, %ax
    int $0x21
    int $0x11
    testw $2, %ax
    jz fail16
    fninit
    fld1
    fld1
    faddp
    .if !REAL
    movw $0x1000, %bx
    movw $0x4a00, %ax
    int $0x21
    jc fail16
    movw $0x1687, %ax
    int $0x2f
    testw %ax, %ax
    jnz fail16
    movw %di, entry
    movw %es, entry + 2
    movw $PM32, %ax
    lcall *entry
    jc fail16
    movw %cs, body_entry + 4
    movw %cs, %bx
    movw $0x000a, %ax
    int $0x31
    jc fail16
    movw %ax, pm_entry + 2
    movw %ax, %bx
    movw $0x40fa, %cx
    movw $9, %ax
    int $0x31
    jc fail16
    ljmp *pm_entry
    .endif
    jmp body
no_fpu:
    movw $unavailable, %dx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c71, %ax
    int $0x21
fail16:
    movw $failed, %dx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c71, %ax
    int $0x21
entry: .word 0, 0
pm_entry: .word protected_entry, 0
body_entry: .long body
    .word 0
    .if !REAL
    .code32
protected_entry:
    .if !PM32
    /* Enter the native backend through a 32-bit alias, then execute the
     * actual 16-bit client code with its original CS and SS contracts. */
    ljmp *body_entry
    .code16
    .endif
    .else
protected_entry:
    .endif
body:
    fistpl result
    cmpl $2, result
    jne fail
    fninit
    fldz
    xorl %ecx, %ecx
hot_loop:
    fld1
    faddp
    incl %ecx
    cmpl $256, %ecx
    jne hot_loop
    fistpl result
    cmpl $256, result
    jne fail
    fldpi
    fnsave saved
    movw $0x3000, %ax
    int $0x21
    frstor saved
    fstpl result
    cmpl $0x54442d18, result
    jne fail
    cmpl $0x400921fb, result + 4
    jne fail
    .if REAL
    /* The BIOS IRQ13 handler must chain the installed NMI math handler. */
    movw $math_error, %dx
    movw $0x2502, %ax
    int $0x21
    fldcw invalid_control
    fldz
    fldz
    .byte 0xde, 0xf9
    sti
    fwait
    cmpw $1, error_calls
    jne fail
    .endif
    fninit
    movl $passed, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c2a, %ax
    int $0x21
fail:
    movl $failed, %edx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c71, %ax
    int $0x21
    .code16
math_error:
    fnclex
    incw %cs:error_calls
    iret
invalid_control: .word 0x037e
error_calls: .word 0
result: .quad 0
saved: .fill 108, 1, 0
passed: .ascii "DOS X87 PROGRAM PASS\r\n$"
failed: .ascii "DOS X87 PROGRAM FAIL\r\n$"
unavailable: .ascii "DOS X87 UNAVAILABLE\r\n$"
