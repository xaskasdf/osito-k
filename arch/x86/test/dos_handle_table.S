/* End-to-end DOS INT 21h/AH=67h dynamic JFT contract test. */

    .section .text
    .code16
    .global _start

_start:
    pushw %cs
    popw %ds
    movw $0x1ff0, %sp

    /* COM programs initially own the remaining arena. Release it while
     * keeping this image and its relocated stack inside the PSP block. */
    movw %cs, %ax
    movw %ax, %es
    movw $0x0200, %bx
    movw $0x4a00, %ax
    int $0x21
    jc fail_memory

    movw $0x5100, %ax
    int $0x21
    movw %bx, psp_segment
    movw %bx, %es
    cmpw $20, %es:0x32
    jne fail_psp
    cmpw $0x18, %es:0x34
    jne fail_psp
    cmpw %bx, %es:0x36
    jne fail_psp

    /* Capture the largest free MCB span so a leaked external JFT is visible. */
    movw $0xffff, %bx
    movw $0x4800, %ax
    int $0x21
    jnc fail_memory
    cmpw $8, %ax
    jne fail_memory
    movw %bx, largest_before

    movw $32, %bx
    movw $0x6700, %ax
    int $0x21
    jc fail_grow

    movw psp_segment, %ax
    movw %ax, %es
    cmpw $32, %es:0x32
    jne fail_grow
    cmpw $0, %es:0x34
    jne fail_grow
    movw %es:0x36, %ax
    testw %ax, %ax
    jz fail_grow
    cmpw psp_segment, %ax
    je fail_grow
    movw %ax, external_segment
    movw %ax, %es

    xorw %si, %si
check_standard:
    cmpb $0xff, %es:(%si)
    je fail_table
    incw %si
    cmpw $5, %si
    jb check_standard
    movw $20, %si
check_new:
    cmpb $0xff, %es:(%si)
    jne fail_table
    incw %si
    cmpw $32, %si
    jb check_new

    /* Force-duplicate reaches an explicitly selected extended JFT slot. */
    movw $1, %bx
    movw $31, %cx
    movw $0x4600, %ax
    int $0x21
    jc fail_duplicate
    movw $31, %bx
    xorw %dx, %dx
    movw $0x4400, %ax
    int $0x21
    jc fail_duplicate
    testw $0x8000, %dx
    jz fail_duplicate

    /* Occupy the old 5..19 range, then verify allocation pops into slot 20. */
    movw $5, %si
fill_inline_range:
    movw $1, %bx
    movw %si, %cx
    movw $0x4600, %ax
    int $0x21
    jc fail_duplicate
    incw %si
    cmpw $20, %si
    jb fill_inline_range

    movw $nul_name, %dx
    movw $0x3d02, %ax
    int $0x21
    jc fail_open
    cmpw $20, %ax
    jne fail_open
    movw %ax, %bx
    movw $0x3e00, %ax
    int $0x21
    jc fail_open

    movw $5, %si
close_inline_range:
    movw %si, %bx
    movw $0x3e00, %ax
    int $0x21
    jc fail_duplicate
    incw %si
    cmpw $20, %si
    jb close_inline_range

    /* Slot 31 is still live, so shrinking past it must fail atomically. */
    movw $20, %bx
    movw $0x6700, %ax
    int $0x21
    jnc fail_shrink_guard
    cmpw $4, %ax
    jne fail_shrink_guard
    movw psp_segment, %ax
    movw %ax, %es
    cmpw $32, %es:0x32
    jne fail_shrink_guard

    movw $0x5900, %ax
    int $0x21
    jc fail_shrink_guard
    cmpw $4, %ax
    jne fail_shrink_guard
    cmpb $4, %bl
    jne fail_shrink_guard
    cmpb $1, %bh
    jne fail_shrink_guard

    movw $31, %bx
    movw $0x3e00, %ax
    int $0x21
    jc fail_duplicate

    /* Requests below 20 clamp to the embedded PSP table. */
    xorw %bx, %bx
    movw $0x6700, %ax
    int $0x21
    jc fail_shrink
    movw psp_segment, %ax
    movw %ax, %es
    cmpw $20, %es:0x32
    jne fail_shrink
    cmpw $0x18, %es:0x34
    jne fail_shrink
    movw %es:0x36, %ax
    cmpw psp_segment, %ax
    jne fail_shrink

    movw $1, %bx
    xorw %dx, %dx
    movw $0x4400, %ax
    int $0x21
    jc fail_shrink
    testw $0x8000, %dx
    jz fail_shrink

    movw $0xffff, %bx
    movw $0x4800, %ax
    int $0x21
    jnc fail_reclaim
    cmpw $8, %ax
    jne fail_reclaim
    cmpw largest_before, %bx
    jne fail_reclaim

    movw $0xffff, %bx
    movw $0x6700, %ax
    int $0x21
    jnc fail_invalid
    cmpw $1, %ax
    jne fail_invalid

    movw $pass_message, %dx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c00, %ax
    int $0x21

fail_memory:
    movb $0x48, %al
    jmp fail
fail_psp:
    movb $0x32, %al
    jmp fail
fail_grow:
    movb $0x67, %al
    jmp fail
fail_table:
    movb $0x20, %al
    jmp fail
fail_duplicate:
    movb $0x46, %al
    jmp fail
fail_open:
    movb $0x3d, %al
    jmp fail
fail_shrink_guard:
    movb $0x04, %al
    jmp fail
fail_shrink:
    movb $0x14, %al
    jmp fail
fail_reclaim:
    movb $0x49, %al
    jmp fail
fail_invalid:
    movb $0xff, %al

fail:
    pushw %ax
    movw $fail_message, %dx
    movw $0x0900, %ax
    int $0x21
    popw %ax
    movb $0x4c, %ah
    int $0x21

nul_name:
    .asciz "NUL"
pass_message:
    .ascii "DOS DYNAMIC JFT CONTRACT PASS\r\n$"
fail_message:
    .ascii "DOS DYNAMIC JFT CONTRACT FAIL\r\n$"

    .balign 2
psp_segment:
    .word 0
external_segment:
    .word 0
largest_before:
    .word 0
