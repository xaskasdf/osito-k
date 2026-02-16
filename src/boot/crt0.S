/*
 * OsitoK - C runtime startup (crt0)
 *
 * Runs before any C code. Sets up:
 *   1. VECBASE to our vector table
 *   2. Stack pointer (a1) to top of DRAM
 *   3. Clears .bss section
 *   4. Calls nosdk_init() for hardware setup
 *   5. Jumps to kernel_main()
 *
 * v0.1: All code in IRAM — no flash cache needed.
 *
 * Xtensa LX106, CALL0 ABI: a0=return addr, a1=SP, a2-a7=args/temps
 */

    .section .iram0.text, "ax"
    .global  _start
    .type    _start, @function
    .literal_position
    .align   4

_start:
    /*
     * Step 1: Set VECBASE to our vector table
     */
    movi    a0, _vecbase
    wsr     a0, vecbase
    isync

    /*
     * Step 2: Set stack pointer to top of DRAM
     * Must be 16-byte aligned (CALL0 ABI requirement)
     */
    movi    a1, 0x3FFFBFF0

    /*
     * Step 3: Clear .bss
     */
    movi    a2, _bss_start
    movi    a3, _bss_end
    movi    a4, 0
.Lbss_loop:
    bge     a2, a3, .Lbss_done
    s32i    a4, a2, 0
    addi    a2, a2, 4
    j       .Lbss_loop
.Lbss_done:

    /*
     * Step 4: Disable interrupts during init
     */
    rsil    a0, 15

    /*
     * Step 5: Hardware init (WDT, PLL, UART)
     */
    call0   nosdk_init

    /*
     * Step 6: Jump to kernel_main (C++)
     * This should never return.
     */
    call0   kernel_main

    /* If kernel_main returns, spin forever */
.Lhalt:
    waiti   0
    j       .Lhalt

    .size _start, . - _start
