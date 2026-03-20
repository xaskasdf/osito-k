/*
 * uart.c -- Qualcomm GENI UART driver for SM8350
 *
 * Tested on ASUS ROG Phone 5, 2026-03-03/04.
 * ABL pre-configures clocks, pinmux, baud rate (115200).
 * We only need to read/write the FIFO.
 *
 * CRITICAL: TX FIFO polling can hang if ABL didn't leave TX functional.
 * Always use timeout in uart_putc. See sm8350-boot session notes.
 */

#include "sm8350.h"

#define UART_BASE   GENI_UART_BASE
#define UART_REG(off)   (UART_BASE + (off))

/* TX timeout: ~100k iterations prevents hang if FIFO doesn't drain.
 * Discovered on real hardware -- infinite loop caused black screen. */
#define TX_TIMEOUT  100000

void uart_init(void) {
    /* ABL pre-configures GENI UART -- nothing to do */
}

void uart_putc(char c) {
    for (int i = 0; i < TX_TIMEOUT; i++) {
        if ((mmio_read32(UART_REG(SE_GENI_TX_FIFO_STATUS)) & TX_FIFO_WC_MASK) < TX_FIFO_DEPTH) {
            mmio_write32(UART_REG(SE_GENI_TX_FIFOn), (uint32_t)c);
            return;
        }
    }
    /* Timeout -- silently drop character rather than hang */
}

void uart_puts(const char *s) {
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s++);
    }
}

int uart_rx_ready(void) {
    return mmio_read32(UART_REG(SE_GENI_RX_FIFO_STATUS)) & RX_FIFO_WC_MASK;
}

char uart_getc(void) {
    while (!uart_rx_ready())
        ;
    return (char)(mmio_read32(UART_REG(SE_GENI_RX_FIFOn)) & 0xFF);
}

int uart_trygetc(void) {
    if (!uart_rx_ready())
        return -1;
    return (int)(mmio_read32(UART_REG(SE_GENI_RX_FIFOn)) & 0xFF);
}
