/*
 * linker_virt.ld -- Osito-K AArch64 linker script for QEMU virt
 *
 * Linked at 0 like SM8350: the kernel is a PIE that relocates itself in
 * start.S. QEMU -kernel loads the Image at RAM base + 0x80000 (0x40080000;
 * the header's image_size is 0), which keeps page 0 of RAM clear.
 */

ENTRY(_start)

SECTIONS
{
    . = 0;

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

    .got : {
        *(.got .got.plt)
    }

    .rela.dyn : ALIGN(8) {
        __rela_start = .;
        *(.rela .rela.*)
        __rela_end = .;
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

    /* Stack: 64 KB above image end (grows down). In a section so that
     * _stack_top is section-relative and gets relocated (see sm8350). */
    .stack (NOLOAD) : {
        . = . + 0x10000;
        _stack_top = .;
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.interp .dynamic)
        *(.dynsym .dynstr .hash .gnu.hash)
    }
}
