/*
 * linker.ld -- Osito-K AArch64 linker script for SM8350
 *
 * The kernel is a PIE linked at 0. ABL (through the sm8350-boot shim) loads
 * it at ~0xA0200000 and `kload` runs it wherever it lands, so start.S adds
 * the load address to every R_AARCH64_RELATIVE entry in .rela.dyn before
 * any C code runs. Stored pointers (string tables, function pointers, GCC's
 * switch tables) work as in any other kernel.
 */

ENTRY(_start)

SECTIONS
{
    . = 0;

    .text.boot : {
        *(.text.boot)       /* ARM64 header + entry point FIRST */
    }

    .text : {
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

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

    . = ALIGN(16);
    __bss_start = .;
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }
    . = ALIGN(16);
    __bss_end = .;

    _image_end = .;

    /* Stack: 64 KB above image end (grows down), wherever we were loaded.
     * Not a fixed address: 0x85000000 sits inside cdsp_secure_heap
     * (0x80C00000-0x85200000 in the device's reserved-memory). Defined in
     * a section so _stack_top is section-relative: as an absolute symbol
     * its GOT entry (memory.c reads it) would get no relocation. */
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
