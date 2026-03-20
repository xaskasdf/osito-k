/*
 * linker_virt.ld -- Osito-K AArch64 linker script for QEMU virt
 *
 * Base at 0x40080000 (64 KB into RAM at 0x40000000).
 * QEMU -kernel loads the binary at the start of RAM, but the ARM64
 * image header text_offset field can adjust. We use a small offset
 * to keep page 0 clear.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x40080000;

    __text_start = .;
    .text.boot : {
        *(.text.boot)       /* ARM64 header + entry point FIRST */
    }

    .text : ALIGN(4096) {
        *(.text .text.*)
    }
    __text_end = .;

    . = ALIGN(4096);
    __rodata_start = .;
    .rodata : {
        *(.rodata .rodata.*)
    }
    __rodata_end = .;

    . = ALIGN(4096);
    .data : {
        *(.data .data.*)
    }

    . = ALIGN(4096);
    __bss_start = .;
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }
    . = ALIGN(4096);
    __bss_end = .;

    _image_end = .;

    /* Stack: 64 KB above image end (grows down) */
    _stack_top = _image_end + 0x10000;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
