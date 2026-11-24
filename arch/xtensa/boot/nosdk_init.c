/*
 * OsitoK - Bare-metal hardware initialization (no SDK)
 *
 * Runs very early in boot, before flash cache is enabled.
 * All this code MUST be in IRAM (not flash).
 *
 * Tasks:
 *   1. Disable watchdog timer
 *   2. Configure PLL for 80 MHz
 *   3. Set up IOMUX for UART0 pins
 *   4. Configure UART0 baud rate to 115200
 */

#include "osito.h"

/* This entire file is placed in IRAM via the linker script */

/*
 * nosdk_init - called from crt0.S before Cache_Read_Enable
 *
 * At this point only IRAM code is executable (no flash mapping yet).
 */
void nosdk_init(void)
{
    /*
     * 1. Disable hardware watchdog
     *
     * Without SDK there's no WDT feed mechanism, so the WDT would
     * reset us. We disable it entirely.
     */
    WDT_CTRL = 0;

    /* Also try the ROM function for good measure */
    ets_wdt_disable();

    /*
     * 2. Set CPU to 80 MHz
     *
     * The ROM bootloader already sets up the PLL, but we make sure
     * CPU_CLK bit 0 is clear (80 MHz, not 160 MHz).
     */
    DPORT_CPU_CLK &= ~1;

    /*
     * 3. Configure IOMUX for UART0 pins
     *
     * GPIO1 = U0TXD (function 0)
     * GPIO3 = U0RXD (function 0)
     *
     * Clear function bits and set to function 0 (UART).
     */
    IOMUX_GPIO1 = IOMUX_FUNC0;          /* GPIO1 -> U0TXD */
    IOMUX_GPIO3 = IOMUX_FUNC0;          /* GPIO3 -> U0RXD */

    /*
     * 4. Configure UART0: 115200 baud, 8N1
     *
     * Baud divisor = CPU_FREQ / baud_rate = 80000000 / 115200 = 694
     *
     * Reset TX and RX FIFOs first.
     */
    /* Reset FIFOs */
    UART0_CONF0 |= UART_TXFIFO_RST | UART_RXFIFO_RST;
    UART0_CONF0 &= ~(UART_TXFIFO_RST | UART_RXFIFO_RST);

    /* Set 8N1 */
    UART0_CONF0 = UART_CONF0_8N1;

    /* Set baud rate: use ROM function for reliability */
    uart_div_modify(0, CPU_FREQ_HZ / UART_BAUD);

    /* Disable all UART interrupts for now (enabled later by driver) */
    UART0_INT_ENA = 0;
    UART0_INT_CLR = 0xFFFFFFFF;

    /* Print a sign-of-life character */
    /* Wait for TX FIFO space */
    while ((UART0_STATUS >> UART_TXFIFO_CNT_SHIFT) & UART_TXFIFO_CNT_MASK)
        ;
    UART0_FIFO = '\n';
}
