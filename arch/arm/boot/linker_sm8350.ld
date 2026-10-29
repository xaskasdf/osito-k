/*
 * linker.ld -- Osito-K AArch64 linker script for SM8350
 *
 * IMPORTANT: The nominal base (0x80080000) does NOT match the actual
 * load address (~0xA0080000). ABL loads us higher than expected.
 *
 * This means:
 *   - All code/data must be position-independent (ADRP/ADR)
 *   - NEVER use stored pointers (char *p = "str") — use arrays (char p[] = "str")
 *   - GCC -O2 generates ADRP for direct references = safe
 *   - Pointers in .data/.rodata contain link-time addresses = BROKEN
 *
 * See arch/arm/README.md for full explanation.
 */

ENTRY(_start)

SECTIONS
{
    /* Nominal base — actual load address differs! */
    . = 0x80080000;

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

    . = ALIGN(16);
    __bss_start = .;
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }
    . = ALIGN(16);
    __bss_end = .;

    _image_end = .;

    /* Stack: 64 KB above image end (grows down), wherever ABL loaded us.
     * Not a fixed address: 0x85000000 sits inside cdsp_secure_heap
     * (0x80C00000-0x85200000 in the device's reserved-memory). */
    _stack_top = _image_end + 0x10000;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
