/* Relocatable MZ child used by the INT 21h/AH=4Bh contract test. */

    .section .text
    .code16
    .global _start

    /* One complete 512-byte MZ page, with a three-paragraph header. */
    .word 0x5a4d              /* e_magic */
    .word 0                   /* e_cblp: full final page */
    .word 1                   /* e_cp */
    .word 1                   /* e_crlc */
    .word 3                   /* e_cparhdr */
    .word 0x0040              /* e_minalloc */
    .word 0x0080              /* e_maxalloc */
    .word 0                   /* e_ss */
    .word 0x0400              /* e_sp */
    .word 0                   /* e_csum */
    .word _start - image_start /* e_ip */
    .word 0                   /* e_cs */
    .word 0x001c              /* e_lfarlc */
    .word 0                   /* e_ovno */

    .word relocated_segment - image_start
    .word 0
    .org 0x30

image_start:
_start:
    pushw %cs
    popw %ds
    movw relocated_segment - image_start, %ax
    movw %cs, %dx
    cmpw %dx, %ax
    jne relocation_fail

    movw $(pass_message - image_start), %dx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c2b, %ax
    int $0x21

relocation_fail:
    movw $(fail_message - image_start), %dx
    movw $0x0900, %ax
    int $0x21
    movw $0x4c7b, %ax
    int $0x21

relocated_segment:
    .word 0
pass_message:
    .ascii "DOS EXEC MZ CHILD PASS\r\n$"
fail_message:
    .ascii "DOS EXEC MZ CHILD FAIL\r\n$"

    .org 0x1ff
    .byte 0
