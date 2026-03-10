/*
 * OsitoK x86-64 Kernel Linker Script
 *
 * Kernel loaded at 2MB by boot.efi (ELF PT_LOAD segments).
 * No EFI restrictions — no PIE, no objcopy PE32+, jump tables OK.
 */

OUTPUT_FORMAT("elf64-x86-64")
ENTRY(kernel_entry)

SECTIONS
{
    . = 0x2000000;  /* 32MB — above OVMF reserved regions (16MB occupied in QEMU) */

    .text : ALIGN(4096) {
        __text_start = .;
        *(.text .text.*)
        __text_end = .;
    }

    .rodata : ALIGN(4096) {
        __rodata_start = .;
        *(.rodata .rodata.*)
        __rodata_end = .;
    }

    .data : ALIGN(4096) {
        __data_start = .;
        *(.data .data.*)
        __data_end = .;
    }

    .bss : ALIGN(4096) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    __kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note*)
    }
}
