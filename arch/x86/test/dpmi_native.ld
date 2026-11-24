OUTPUT_FORMAT(binary)
ENTRY(_start)

SECTIONS
{
    . = 0x100;
    .text : {
        *(.text*)
        *(.rodata*)
        *(.data*)
    }
    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
