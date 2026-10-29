/*
 * uart.c -- Qualcomm GENI UART driver for SM8350
 *
 * Tested on ASUS ROG Phone 5, 2026-03-03/04.
 * ABL pre-configures clocks, pinmux, baud rate (115200).
 * We only need to read/write the FIFO.
 *
 * CRITICAL: TX FIFO polling can hang if ABL didn't leave TX functional.
 * Always use timeout in uart_putc. See sm8350-boot session notes.
 *
 * There is no serial cable, so the console also runs over the USB CDC-ACM
 * gadget (dwc3_acm.c): every byte is queued for the host, and reads poll the
 * gadget before the GENI RX FIFO. The shell's read loop is what services USB.
 */

#include "../../include/hal.h"
#include "sm8350.h"
#include "usb.h"

#define UART_BASE   GENI_UART_BASE
#define UART_REG(off)   (UART_BASE + (off))

/* TX timeout: ~100k iterations prevents hang if FIFO doesn't drain.
 * Discovered on real hardware -- infinite loop caused black screen. */
#define TX_TIMEOUT  100000

/* Set on the first TX timeout. Once the FIFO stops draining, paying the full
 * timeout on every character stalled boot for minutes on the device
 * (2026-10-28), so all further output is dropped. */
static int tx_stuck;

void uart_init(void) {
    /* ABL pre-configures GENI UART -- nothing to do */
}

void uart_putc(char c) {
    acm_putc(c);
    if (tx_stuck)
        return;
    for (int i = 0; i < TX_TIMEOUT; i++) {
        if ((mmio_read32(UART_REG(SE_GENI_TX_FIFO_STATUS)) & TX_FIFO_WC_MASK) < TX_FIFO_DEPTH) {
            mmio_write32(UART_REG(SE_GENI_TX_FIFOn), (uint32_t)c);
            return;
        }
    }
    /* Timeout -- drop this and every later character rather than hang */
    tx_stuck = 1;
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
    int c;
    while ((c = uart_trygetc()) < 0)
        ;
    return (char)c;
}

/* Under the console lock: polling the gadget can also send queued output */
int uart_trygetc(void) {
    uint64_t flags = console_lock();
    acm_poll();
    int c = acm_getc();
    if (c < 0 && uart_rx_ready())
        c = (int)(mmio_read32(UART_REG(SE_GENI_RX_FIFOn)) & 0xFF);
    console_unlock(flags);
    return c;
}
