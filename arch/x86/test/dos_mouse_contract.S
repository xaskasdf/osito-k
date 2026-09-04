/* End-to-end Microsoft mouse INT 33h contract test. */

    .section .text
    .code16
    .global _start

_start:
    pushw %cs
    popw %ds

    /* Legacy installation probes inspect the IVT before calling function 0. */
    movw $0x3533, %ax
    int $0x21
    movw %es, %ax
    orw %bx, %ax
    jz no_driver
    cmpb $0xcf, %es:(%bx)
    je no_driver

    xorw %ax, %ax
    int $0x33
    cmpw $0xffff, %ax
    jne no_driver
    cmpw $2, %bx
    jb no_driver

    /* Driver capabilities use the absolute 640x200 virtual screen. */
    movw $0x0026, %ax
    int $0x33
    testw %bx, %bx
    jne bad_extent
    cmpw $639, %cx
    jne bad_extent
    cmpw $199, %dx
    jne bad_extent

    /* Reversed limits must be normalized. */
    movw $0x0007, %ax
    movw $500, %cx
    movw $100, %dx
    int $0x33
    movw $0x0008, %ax
    movw $160, %cx
    movw $40, %dx
    int $0x33

    movw $0x0031, %ax
    int $0x33
    cmpw $100, %ax
    jne bad_limits
    cmpw $40, %bx
    jne bad_limits
    cmpw $500, %cx
    jne bad_limits
    cmpw $160, %dx
    jne bad_limits

    /* Set position clamps to the limits and text-cell grid. */
    movw $0x0004, %ax
    xorw %cx, %cx
    movw $300, %dx
    int $0x33
    movw $0x0003, %ax
    int $0x33
    cmpw $100, %cx
    jb bad_position
    cmpw $500, %cx
    ja bad_position
    cmpw $40, %dx
    jb bad_position
    cmpw $160, %dx
    ja bad_position
    testw $7, %cx
    jne bad_position
    testw $7, %dx
    jne bad_position

    /* Show/hide nesting and query functions must remain callable. */
    movw $0x0001, %ax
    int $0x33
    int $0x33
    movw $0x0002, %ax
    int $0x33
    movw $0x000b, %ax
    int $0x33
    movw $0x0023, %ax
    int $0x33
    testw %bx, %bx
    jne bad_version
    movw $0x0024, %ax
    int $0x33
    cmpw $0x0705, %bx
    jb bad_version

pass:
    movw $pass_message, %dx
    movb $9, %ah
    int $0x21
    movw $0x4c00, %ax
    int $0x21

no_driver:
    movb $0x61, %al
    movw $driver_message, %dx
    jmp fail
bad_extent:
    movb $0x62, %al
    movw $extent_message, %dx
    jmp fail
bad_limits:
    movb $0x63, %al
    movw $limits_message, %dx
    jmp fail
bad_position:
    movb $0x64, %al
    movw $position_message, %dx
    jmp fail
bad_version:
    movb $0x65, %al
    movw $version_message, %dx

fail:
    pushw %ax
    movb $9, %ah
    int $0x21
    popw %ax
    movb $0x4c, %ah
    int $0x21

pass_message:
    .ascii "DOS INT 33h mouse contract: PASS\r\n$"
driver_message:
    .ascii "DOS INT 33h mouse contract: driver missing\r\n$"
extent_message:
    .ascii "DOS INT 33h mouse contract: wrong virtual extent\r\n$"
limits_message:
    .ascii "DOS INT 33h mouse contract: wrong limits\r\n$"
position_message:
    .ascii "DOS INT 33h mouse contract: wrong position\r\n$"
version_message:
    .ascii "DOS INT 33h mouse contract: wrong driver metadata\r\n$"
