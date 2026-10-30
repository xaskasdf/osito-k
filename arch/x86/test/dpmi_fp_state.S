/* Native floating-point state must survive host services and raw switches.
 * DIAGNOSTIC_STATE adds strict FOP/FIP/FDP/FCS/FDS checks, kept separate from
 * numerical state because some native backends lose these on VM exits. */
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
    movw %cs, exec_block + 4
    movw %cs, exec_block + 8
    movw %cs, exec_block + 12
    movw $0x1000, %bx
    movw $0x4a00, %ax
    int $0x21
    jc setup_fail
    movw $0x1687, %ax
    int $0x2f
    testw %ax, %ax
    jnz setup_fail
    movw %di, entry
    movw %es, entry + 2
    movw $1, %ax
    lcall *entry
    jc setup_fail
    movw %cs, %bx
    movw $0x000a, %ax
    int $0x31
    jc setup_fail
    movw %ax, pm_entry + 2
    movw %ax, %bx
    movw $0x40fa, %cx
    movw $9, %ax
    int $0x31
    jc setup_fail
    ljmp *pm_entry
setup_fail:
    movw $0x4c70, %ax
    int $0x21
entry: .word 0, 0
pm_entry: .word protected_entry, 0

    .code32
protected_entry:
    fxsave before
    cmpw $0x037f, before
    jne fail
    cmpw $0, before + 2
    jne fail
    cmpb $0, before + 4
    jne fail
    cmpl $0x1f80, before + 24
    jne fail
    movw %ss, %bx
    movw $(0xf2 | STACK32 * 0x4000), %cx
    movw $9, %ax
    int $0x31
    jc fail
    movw %bx, %ss
    movl $(0x8000 + !STACK32 * 0xbeef0000), %esp
    movl %esp, saved_sp
    movw %ss, saved_ss
    movw %cs, saved_cs
    movw %ds, saved_ds
    movw $0x0306, %ax
    int $0x31
    jc fail
    movw %cx, rm_switch
    movw %bx, rm_switch + 2
    movl %edi, pm_switch
    movw %si, pm_switch + 4
    movw %cs, %cx
    movl $software_handler, %edx
    movw $0x60, %bx
    movw $0x0205, %ax
    int $0x31
    jc fail
    movw %cs, %cx
    movl $mf_handler, %edx
    movw $16, %bx
    movw $0x0203, %ax
    int $0x31
    jc fail
    movl $1, %eax
    cpuid
    andl $0x18000000, %ecx
    cmpl $0x18000000, %ecx
    jne 1f
    xorl %ecx, %ecx
    xgetbv
    andl $7, %eax
    cmpl $7, %eax
    sete has_avx
1:
    .ifdef CHILD_ONLY
    call seed
    fldcw pending_control
    fldz
    fldz
    .byte 0xde, 0xf9
    movw $0x4c2b, %ax
    int $0x21
    .endif
    .if RAW_ONLY
    movb $4, scenario
    jmp raw_test
    .endif
    call seed
    movl $64, iterations
service_loop:
    movw $0x0400, %ax
    int $0x31
    jc fail
    call check
    decl iterations
    jnz service_loop

    incb scenario
    movw $0x3000, %ax
    int $0x21
    call check
    incb scenario
    movw $0x0f00, %ax
    int $0x10
    cmpw $0x5003, %ax
    jne fail
    call check
    incb scenario
    int $0x60
    cmpb $1, software_calls
    jne fail
    call check

    incb scenario
raw_test:
    call seed
    movl $4, iterations
raw_loop:
    movw real_seg, %ax
    movw %ax, %cx
    movw %ax, %dx
    movw %ax, %si
    movl $0x6000, %ebx
    movl $real_code, %edi
    ljmp *pm_switch
raw_return:
    cmpl saved_sp, %esp
    jne fail
    call check
    decl iterations
    jnz raw_loop
    cmpb $8, scenario
    je pass
    .if RAW_ONLY
    jmp pass
    .endif

    incb scenario
    fninit
    fldcw pending_control
    fldz
    fldz
    .byte 0xde, 0xf9 /* 0/0: unmasked invalid, delivered at the next FWAIT. */
    movw $0x0400, %ax
    int $0x31
    fnstsw %ax
    testw $1, %ax
    jz fail
wait_invalid:
    fwait
    cmpb $1, exceptions
    jne fail

    incb scenario
    fninit
    .irp n,0,1,2,3,4,5,6,7
    movq pattern + (\n * 4), %mm\n
    .endr
    fxsave before
    .ifdef DIAGNOSTIC_STATE
    fnstenv before_env
    fldcw before
    .endif
    movw $0x0400, %ax
    int $0x31
    call check
    emms
    incb scenario
    call seed
    movl $10000000, %ecx
1:  pause
    loop 1b
    call check
    incb scenario
    call seed
    movl $1, iterations
    jmp raw_loop
pass:
    fninit
    ldmxcsr default_mxcsr
    .ifdef DIAGNOSTIC_STATE
    movl $selectors_hidden, %edx
    cmpb $0, has_selectors
    je 3f
    movl $selectors_seen, %edx
3:  movw $0x0900, %ax
    int $0x21
    .endif
    movl $pass_msg, %edx
    cmpb $0, has_avx
    je 1f
    movl $pass_avx, %edx
1:  movw $0x0900, %ax
    int $0x21
    movw $0x4c2a, %ax
    int $0x21
fail:
    fninit
    ldmxcsr default_mxcsr
    movl $fail_msg, %edx
    movw $0x0900, %ax
    int $0x21
    movb scenario, %dl
    addb $'0', %dl
    movb $2, %ah
    int $0x21
    movb detail, %dl
    movb $2, %ah
    int $0x21
    .irp offset,12,16,20,24
    movl before_env + \offset, %eax
    call diagnostic_hex
    movl after_env + \offset, %eax
    call diagnostic_hex
    .endr
    movw $0x4c71, %ax
    int $0x21

diagnostic_hex:
    pushal
    movl %eax, %ebx
    movb $' ', %dl
    movb $2, %ah
    int $0x21
    movl $8, %ecx
1:  roll $4, %ebx
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

seed:
    fninit
    fldcw control
    fld1
    fldpi
    fldl number
    ldmxcsr client_mxcsr
    .irp n,0,1,2,3,4,5,6,7
    movdqu pattern + (\n * 4), %xmm\n
    .endr
    cmpb $0, has_avx
    je 1f
    .irp n,0,1,2,3,4,5,6,7
    vmovdqu pattern + (\n * 4), %ymm\n
    vextractf128 $1, %ymm\n, before_ymmh + (\n * 16)
    .endr
1:
    .ifdef DIAGNOSTIC_STATE
    fnstenv diagnostic_env
    /* Seed the legacy selectors explicitly: modern native arithmetic may
     * leave them zero even on CPUs which restore them from an environment. */
    movw %cs, %ax
    movw %ax, diagnostic_env + 16
    movw %ds, %ax
    movw %ax, diagnostic_env + 24
    movl $0x12345678, diagnostic_env + 12
    movl $0x23456789, diagnostic_env + 20
    fldenv diagnostic_env
    .endif
    fxsave before
    .ifdef DIAGNOSTIC_STATE
    fnstenv before_env
    fldcw before
    movw %cs, %ax
    cmpw %ax, before_env + 16
    jne 2f
    movw %ds, %ax
    cmpw %ax, before_env + 24
    jne 2f
    movb $1, has_selectors
2:
    .endif
    ret
check:
    pushal
    push %es
    movw %ds, %ax
    movw %ax, %es
    cld
    fxsave after
    .ifdef DIAGNOSTIC_STATE
    fnstenv after_env
    fldcw after
    movb $'S', detail
    .irp offset,16,24
    movw before_env + \offset, %ax
    cmpw %ax, after_env + \offset
    jne fail
    .endr
    movb $'P', detail
    .irp offset,12,20
    movl before_env + \offset, %eax
    cmpl %eax, after_env + \offset
    jne fail
    .endr
    .endif
    movb $'C', detail
    movl $before, %esi
    movl $after, %edi
    movl $5, %ecx /* FCW, FSW, abridged FTW; reserved/FIP fields excluded. */
    repe cmpsb
    jne fail
    .ifdef DIAGNOSTIC_STATE
    /* The host must preserve the diagnostic opcode/instruction/data pointers. */
    movb $'F', detail
    .irp offset,6,12,20
    movw before + \offset, %ax
    cmpw %ax, after + \offset
    jne fail
    .endr
    movb $'I', detail
    .irp offset,8,16
    movl before + \offset, %eax
    cmpl %eax, after + \offset
    jne fail
    .endr
    .endif
    movb $'M', detail
    movl before + 24, %eax
    cmpl %eax, after + 24
    jne fail
    movb $'R', detail
    movl $before + 32, %esi
    movl $after + 32, %edi
    movl $8, %ebx
1:  movl $10, %ecx
    repe cmpsb
    jne fail
    addl $6, %esi
    addl $6, %edi
    decl %ebx
    jnz 1b
    movb $'X', detail
    movl $32, %ecx /* eight 128-bit XMM registers */
    repe cmpsl
    jne fail
    cmpb $0, has_avx
    je 2f
    movb $'Y', detail
    .irp n,0,1,2,3,4,5,6,7
    vextractf128 $1, %ymm\n, after_ymmh + (\n * 16)
    .endr
    movl $before_ymmh, %esi
    movl $after_ymmh, %edi
    movl $32, %ecx
    repe cmpsl
    jne fail
2:  pop %es
    popal
    ret
software_handler:
    pushal
    incb software_calls
    call check
    movw $0x0400, %ax
    int $0x31
    call check
    popal
    iretl
mf_handler:
    fnclex
    incb exceptions
    pushl %ebp
    movl %esp, %ebp
    cmpl $wait_invalid, %ss:16(%ebp)
    jne fail
    popl %ebp
    lret

    .code16
real_code:
    cmpb $8, scenario
    jne 1f
    pushw %ds
    popw %es
    movw $child_name, %dx
    movw $exec_block, %bx
    movw $0x4b00, %ax
    int $0x21
    jc real_fail
    movw $0x4d00, %ax
    int $0x21
    cmpw $0x002b, %ax
    jne real_fail
1:
    movw $0x3000, %ax
    int $0x21
    movw saved_ds, %ax
    movw %ax, %cx
    movw saved_ss, %dx
    movl saved_sp, %ebx
    movw saved_cs, %si
    movl $raw_return, %edi
    ljmp *rm_switch
real_fail:
    movw $0x4c72, %ax
    int $0x21

    .balign 64
before: .space 512
after: .space 512
before_ymmh: .space 128
after_ymmh: .space 128
diagnostic_env: .space 28
before_env: .space 28
after_env: .space 28
pattern:
    .long 0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00
    .long 0x01234567, 0x89abcdef, 0x76543210, 0xfedcba98
    .long 0x24681357, 0xf0e1d2c3, 0xaabb9876, 0x22224444
    .long 0x2468abcd, 0x33331111, 0x5a5aa5a5, 0xbabecafe
number: .double -123.456
client_mxcsr: .long 0x3f80
default_mxcsr: .long 0x1f80
control: .word 0x0b7f
pending_control: .word 0x037e
saved_sp: .long 0
saved_ss: .word 0
saved_cs: .word 0
saved_ds: .word 0
real_seg: .word 0
pm_switch: .long 0; .word 0
rm_switch: .word 0, 0
iterations: .long 0
has_avx: .byte 0
has_selectors: .byte 0
scenario: .byte 0
detail: .byte '0'
software_calls: .byte 0
exceptions: .byte 0
child_name: .asciz "dpfpc32.com"
exec_block: .word 0, command_tail, 0, 0x5c, 0, 0x6c, 0
command_tail: .byte 0, 13
pass_msg: .asciz "DPMI FP STATE PASS SSE\r\n$"
pass_avx: .asciz "DPMI FP STATE PASS AVX\r\n$"
fail_msg: .asciz "DPMI FP STATE FAIL case=$"
selectors_seen: .asciz "DPMI FP SELECTORS OBSERVED\r\n$"
selectors_hidden: .asciz "DPMI FP SELECTORS NOT EXPOSED\r\n$"
