/*
 * Visual BIOS/VGA contract probe for the DOS interpreter.
 *
 * It renders on text page 1 through both INT 10h and direct B800 writes,
 * switches to mode 13h on a key, and remains there until Escape. Build with:
 *
 *     make -C arch/x86 dos-vga-test
 */

    .section .text
    .code16
    .global _start

_start:
    pushw %cs
    popw %ds

    /* Clear 80x25 color text mode. */
    movw $0x0003, %ax
    int $0x10

    /* Position page 1 before making it visible. */
    movb $0x02, %ah
    movb $0x01, %bh
    movw $0x0408, %dx
    int $0x10
    movw $0x0501, %ax
    int $0x10

    movw $bios_message, %si
1:
    lodsb
    testb %al, %al
    jz 2f
    movb $0x0e, %ah
    movb $0x01, %bh
    int $0x10
    jmp 1b

    /* Write colored rows directly into the active B800 text page. */
2:
    movw $0xb800, %ax
    movw %ax, %es

    movw $0x1540, %di           /* page 1, row 8, column 16 */
    movw $0x1f3d, %ax           /* '=': bright white on blue */
    movw $48, %cx
    rep stosw

    movw $0x15e0, %di           /* page 1, row 9, column 16 */
    movw $direct_message, %si
    movb $0x2e, %ah             /* yellow on green */
3:
    lodsb
    testb %al, %al
    jz 4f
    stosw
    jmp 3b

4:
    movw $0x1680, %di           /* page 1, row 10, column 16 */
    movw $0x4f3d, %ax           /* '=': bright white on red */
    movw $48, %cx
    rep stosw

    /* Moving the active-page cursor forces the direct writes to present. */
    movb $0x02, %ah
    movb $0x01, %bh
    movw $0x0d10, %dx
    int $0x10

    /* Disable the cursor so the expected image is stable. */
    movb $0x01, %ah
    movb $0x20, %ch
    xorb %cl, %cl
    int $0x10

    xorb %ah, %ah
    int $0x16

    /* Second stage: 320x200x8 mode with one palette index per row. */
    movw $0x0013, %ax
    int $0x10
    movw $0xa000, %ax
    movw %ax, %es
    xorw %di, %di
    xorw %dx, %dx
5:
    movb %dl, %al
    movw $320, %cx
    rep stosb
    incw %dx
    cmpw $200, %dx
    jne 5b

6:
    movb $0x01, %ah
    int $0x16
    jz 6b
    xorb %ah, %ah
    int $0x16
    cmpb $0x1b, %al
    jne 6b
    movw $0x0003, %ax
    int $0x10
    movw $0x4c00, %ax
    int $0x21

bios_message:
    .asciz "OsitoK DOS BIOS text page 1"
direct_message:
    .asciz "Direct B800 writes and dirty-cell presentation"
