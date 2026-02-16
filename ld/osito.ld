/*
 * OsitoK v0.1 - Linker script for ESP8266 (Wemos D1)
 *
 * Memory map:
 *   IRAM:  0x40100000 - 0x40107FFF  (32KB) - ALL code goes here for v0.1
 *   DRAM:  0x3FFE8000 - 0x3FFFBFFF  (~80KB)
 *
 * v0.1: All code in IRAM (total ~4KB, fits easily in 32KB).
 * Flash-mapped irom0 will be added in v0.2 when code grows larger.
 */

MEMORY
{
    iram0  (rwx) : ORIGIN = 0x40100000, LENGTH = 32K
    dram0  (rw)  : ORIGIN = 0x3FFE8000, LENGTH = 0x13C00  /* ~79KB */
}

/* ROM functions */
INCLUDE rom_functions.ld

PHDRS
{
    iram0_phdr PT_LOAD;
    dram0_phdr PT_LOAD;
}

ENTRY(_start)

SECTIONS
{
    /* ---- IRAM: all code ---- */
    .vectors : ALIGN(256)
    {
        _vecbase = .;
        KEEP(*(.vectors))
        *(.vectors.*)
    } > iram0 :iram0_phdr

    .text : ALIGN(4)
    {
        *(.iram0.text)
        *(.iram0.text.*)
        *(.irom0.text)
        *(.irom0.text.*)
        *(.text .text.*)
        *(.literal .literal.*)
        . = ALIGN(4);
    } > iram0 :iram0_phdr

    /* ---- DRAM ---- */
    .data : ALIGN(4)
    {
        _data_start = ABSOLUTE(.);
        *(.data)
        *(.data.*)
        *(.rodata)
        *(.rodata.*)
        . = ALIGN(4);
        _data_end = ABSOLUTE(.);
    } > dram0 :dram0_phdr

    .bss : ALIGN(4)
    {
        _bss_start = ABSOLUTE(.);
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(4);
        _bss_end = ABSOLUTE(.);
    } > dram0 :dram0_phdr

    /* ISR stack (512 bytes) */
    .isr_stack : ALIGN(16)
    {
        _isr_stack_bottom = ABSOLUTE(.);
        . = . + 512;
        _isr_stack_top = ABSOLUTE(.);
    } > dram0 :dram0_phdr

    _heap_start = ALIGN(4);

    /* Initial stack pointer at top of DRAM */
    _stack_top = 0x3FFFBFF0;
}
