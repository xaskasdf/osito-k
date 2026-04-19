/*
 * OsitoK x86-64 Kernel Linker Script — higher-half kernel
 *
 * Kernel loaded by boot.efi at LMA 0x2000000 (32 MB phys), but linked
 * to run at VMA = KERNEL_VBASE + LMA so all symbol references resolve
 * to the upper half. boot.efi installs an upper-half direct map in its
 * PML4 before ExitBootServices so the high entry point is reachable
 * through the PE32 entry symbol; once paging_init runs, the kernel's
 * own kernel_pml4 (Fase 1 mirror) takes over.
 *
 * KERNEL_VBASE = 0xFFFF800000000000 (must match arch/x86/include/paging.h).
 */

OUTPUT_FORMAT("elf64-x86-64")
ENTRY(kernel_entry)

KERNEL_VBASE = 0xFFFF800000000000;
KERNEL_LMA   = 0x2000000;       /* 32MB — above OVMF reserved regions */

SECTIONS
{
    . = KERNEL_VBASE + KERNEL_LMA;

    .text : AT(ADDR(.text) - KERNEL_VBASE) ALIGN(4096) {
        __text_start = .;
        /* Hot path first — fits in L1i cache (32KB) */
        __text_hot_start = .;
        *(.text.hot)
        __text_hot_end = .;
        /* Normal text + any GCC-generated subsections */
        *(.text .text.unlikely .text.startup .text.exit)
        /* Cold path */
        *(.text.cold)
        /* Init code — page-aligned so it can be reclaimed after boot.
         * MUST appear before the .text.* catch-all below. */
        . = ALIGN(4096);
        __init_start = .;
        *(.text.init)
        . = ALIGN(4096);
        __init_end = .;
        /* Remaining .text.* subsections */
        *(.text.*)
        __text_end = .;
    }

    .rodata : AT(ADDR(.rodata) - KERNEL_VBASE) ALIGN(4096) {
        __rodata_start = .;
        *(.rodata .rodata.*)
        __rodata_end = .;
    }

    .data : AT(ADDR(.data) - KERNEL_VBASE) ALIGN(4096) {
        __data_start = .;
        *(.data .data.*)
        __data_end = .;
    }

    .bss : AT(ADDR(.bss) - KERNEL_VBASE) ALIGN(4096) {
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
