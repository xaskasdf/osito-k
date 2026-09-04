/* End-to-end VBE 2.0 BIOS contract test. */

    .section .text
    .code16
    .global _start

_start:
    pushw %cs
    popw %ds
    pushw %cs
    popw %es

    /* VBE2 controller information and advertised video memory. */
    movw $0x4256, controller_info
    movw $0x3245, controller_info + 2
    movw $0x4f00, %ax
    movw $controller_info, %di
    int $0x10
    cmpw $0x004f, %ax
    jne bad_controller
    cmpw $0x4556, controller_info
    jne bad_controller
    cmpw $0x4153, controller_info + 2
    jne bad_controller
    cmpw $0x0200, controller_info + 4
    jb bad_controller
    cmpw $64, controller_info + 18
    jne bad_controller

    /* A direct-color mode must describe its LFB and channel layout. */
    movw $0x4f01, %ax
    movw $0x0111, %cx
    movw $mode_info, %di
    int $0x10
    cmpw $0x004f, %ax
    jne bad_mode_info
    testw $0x0080, mode_info
    jz bad_mode_info
    cmpw $640, mode_info + 18
    jne bad_mode_info
    cmpw $480, mode_info + 20
    jne bad_mode_info
    cmpb $16, mode_info + 25
    jne bad_mode_info
    cmpb $6, mode_info + 27
    jne bad_mode_info
    cmpl $0x01000000, mode_info + 40
    jne bad_mode_info

    /* Banked 640x480x8 mode and its 64 KB window. */
    movw $0x4f02, %ax
    movw $0x0101, %bx
    int $0x10
    cmpw $0x004f, %ax
    jne bad_set_mode
    movw $0x4f03, %ax
    int $0x10
    cmpw $0x004f, %ax
    jne bad_set_mode
    cmpw $0x0101, %bx
    jne bad_set_mode

    /* Exercise interpreter dirty tracking and the periodic GOP presenter. */
    movw $0xa000, %ax
    movw %ax, %es
    xorw %di, %di
    movb $1, %al
    movw $64000, %cx
    rep stosb
    movw $0x9000, %cx
1:
    nop
    loop 1b
    pushw %cs
    popw %es

    movw $0x4f05, %ax
    xorw %bx, %bx
    movw $1, %dx
    int $0x10
    cmpw $0x004f, %ax
    jne bad_bank
    movw $0xa000, %ax
    movw %ax, %es
    movb $0x5a, %es:7
    movw $0x4f05, %ax
    movw $0x0100, %bx
    xorw %dx, %dx
    int $0x10
    cmpw $0x004f, %ax
    jne bad_bank
    cmpw $1, %dx
    jne bad_bank
    pushw %cs
    popw %es

    /* Logical scan line and display-start round trip. */
    movw $0x4f06, %ax
    xorw %bx, %bx
    movw $672, %cx
    int $0x10
    cmpw $0x004f, %ax
    jne bad_geometry
    cmpw $672, %bx
    jne bad_geometry
    cmpw $672, %cx
    jne bad_geometry

    movw $0x4f07, %ax
    xorw %bx, %bx
    movw $16, %cx
    movw $1, %dx
    int $0x10
    cmpw $0x004f, %ax
    jne bad_geometry
    movw $0x4f07, %ax
    movw $0x0001, %bx
    xorw %cx, %cx
    xorw %dx, %dx
    int $0x10
    cmpw $0x004f, %ax
    jne bad_geometry
    cmpw $16, %cx
    jne bad_geometry
    cmpw $1, %dx
    jne bad_geometry

    /* Fixed six-bit DAC and one palette entry. */
    movw $0x4f08, %ax
    movw $0x0800, %bx
    int $0x10
    cmpw $0x004f, %ax
    jne bad_palette
    cmpb $6, %bh
    jne bad_palette
    movw $0x4f09, %ax
    xorw %bx, %bx
    movw $1, %cx
    movw $7, %dx
    movw $palette_in, %di
    int $0x10
    cmpw $0x004f, %ax
    jne bad_palette
    movl $0, palette_out
    movw $0x4f09, %ax
    movw $1, %bx
    movw $1, %cx
    movw $7, %dx
    movw $palette_out, %di
    int $0x10
    cmpw $0x004f, %ax
    jne bad_palette
    cmpl $0x03020100, palette_out
    jne bad_palette

    /* Save enough state to recover bank, pitch, palette, and viewport. */
    movw $0x4f04, %ax
    movw $0x000f, %cx
    xorw %dx, %dx
    int $0x10
    cmpw $0x004f, %ax
    jne bad_state
    cmpw $13, %bx
    jb bad_state
    movw $0x4f04, %ax
    movw $0x000f, %cx
    movw $1, %dx
    movw $state_buffer, %bx
    int $0x10
    cmpw $0x004f, %ax
    jne bad_state

    /* Linear mode selection is reflected by Function 03h. */
    movw $0x4f02, %ax
    movw $0x4111, %bx
    int $0x10
    cmpw $0x004f, %ax
    jne bad_linear
    movw $0x4f03, %ax
    int $0x10
    cmpw $0x004f, %ax
    jne bad_linear
    cmpw $0x4111, %bx
    jne bad_linear

    movw $0x4f04, %ax
    movw $0x000f, %cx
    movw $2, %dx
    movw $state_buffer, %bx
    int $0x10
    cmpw $0x004f, %ax
    jne bad_state
    movw $0x4f03, %ax
    int $0x10
    cmpw $0x0101, %bx
    jne bad_state
    movw $0x4f06, %ax
    movw $1, %bx
    int $0x10
    cmpw $672, %bx
    jne bad_state

    /* Mouse extents follow the active 640x480 VBE viewport. */
    xorw %ax, %ax
    int $0x33
    cmpw $0xffff, %ax
    jne bad_mouse
    movw $0x0026, %ax
    int $0x33
    cmpw $639, %cx
    jne bad_mouse
    cmpw $479, %dx
    jne bad_mouse

    /* The VBE 2.0 protected-mode table must be discoverable. */
    movw $0x4f0a, %ax
    xorw %bx, %bx
    int $0x10
    cmpw $0x004f, %ax
    jne bad_pm
    cmpw $23, %cx
    jne bad_pm
    cmpw $8, %es:(%di)
    jne bad_pm

    movw $0x0003, %ax
    int $0x10
    movw $pass_message, %dx
    movb $9, %ah
    int $0x21
    movw $0x4c00, %ax
    int $0x21

bad_controller:
    movb $0x71, %al
    jmp fail
bad_mode_info:
    movb $0x72, %al
    jmp fail
bad_set_mode:
    movb $0x73, %al
    jmp fail
bad_bank:
    movb $0x74, %al
    jmp fail
bad_geometry:
    movb $0x75, %al
    jmp fail
bad_palette:
    movb $0x76, %al
    jmp fail
bad_state:
    movb $0x77, %al
    jmp fail
bad_linear:
    movb $0x78, %al
    jmp fail
bad_mouse:
    movb $0x79, %al
    jmp fail
bad_pm:
    movb $0x7a, %al

fail:
    pushw %ax
    pushw %cs
    popw %ds
    movw $fail_message, %dx
    movb $9, %ah
    int $0x21
    popw %ax
    movb $0x4c, %ah
    int $0x21

    .align 4
palette_in:
    .byte 0, 1, 2, 3
palette_out:
    .space 4
pass_message:
    .ascii "DOS VBE 2.0 contract: PASS\r\n$"
fail_message:
    .ascii "DOS VBE 2.0 contract: FAIL\r\n$"
    .align 16
controller_info:
    .space 512
mode_info:
    .space 256
state_buffer:
    .space 832
