/* End-to-end DOS PSP environment contract test. */

    .section .text
    .code16
    .global _start

_start:
    pushw %cs
    popw %ds

    /* The command tail selects the expected state: present or absent. */
    xorw %cx, %cx
    movb 0x80, %cl
    movw $0x81, %si
argument_next:
    testw %cx, %cx
    jz bad_argument
    lodsb
    decw %cx
    cmpb $' ', %al
    je argument_next
    andb $0xdf, %al
    cmpb $'P', %al
    je expect_present
    cmpb $'A', %al
    jne bad_argument
    xorw %bp, %bp
    jmp scan_environment
expect_present:
    movw $1, %bp

scan_environment:
    movw 0x2c, %ax
    testw %ax, %ax
    jz missing_environment
    movw %ax, %es
    xorw %di, %di

next_variable:
    cmpb $0, %es:(%di)
    je variable_not_found
    movw %di, %bx
    movw $blaster_value, %si
    movw $8, %cx
    cld
    repe cmpsb
    movw %bx, %di
    je blaster_found

skip_variable:
    cmpb $0, %es:(%di)
    je variable_end
    incw %di
    jmp skip_variable
variable_end:
    incw %di
    jmp next_variable

blaster_found:
    testw %bp, %bp
    jz unexpected_blaster
    movw %bx, %di
    movw $blaster_value, %si
    movw $blaster_value_end - blaster_value, %cx
    repe cmpsb
    jne wrong_blaster
    jmp pass

variable_not_found:
    testw %bp, %bp
    jnz missing_blaster

pass:
    movw $pass_message, %dx
    movb $9, %ah
    int $0x21
    movw $0x4c00, %ax
    int $0x21

bad_argument:
    movb $0x70, %al
    movw $argument_message, %dx
    jmp fail
missing_environment:
    movb $0x71, %al
    movw $environment_message, %dx
    jmp fail
missing_blaster:
    movb $0x72, %al
    movw $missing_message, %dx
    jmp fail
wrong_blaster:
    movb $0x73, %al
    movw $wrong_message, %dx
    jmp fail
unexpected_blaster:
    movb $0x74, %al
    movw $unexpected_message, %dx

fail:
    pushw %ax
    movb $9, %ah
    int $0x21
    popw %ax
    movb $0x4c, %ah
    int $0x21

blaster_value:
    .asciz "BLASTER=A220 I5 D1 H5 T6"
blaster_value_end:
pass_message:
    .ascii "DOS BLASTER environment contract: PASS\r\n$"
argument_message:
    .ascii "DOS BLASTER environment contract: invalid expectation\r\n$"
environment_message:
    .ascii "DOS BLASTER environment contract: missing environment\r\n$"
missing_message:
    .ascii "DOS BLASTER environment contract: BLASTER missing\r\n$"
wrong_message:
    .ascii "DOS BLASTER environment contract: BLASTER value mismatch\r\n$"
unexpected_message:
    .ascii "DOS BLASTER environment contract: unexpected BLASTER\r\n$"
