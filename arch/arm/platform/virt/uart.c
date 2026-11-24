/*
 * uart.c -- PL011 UART driver for QEMU virt
 *
 * QEMU pre-configures PL011 (baud, clocks, FIFOs), so uart_init()
 * just ensures TX/RX are enabled. All we need is read/write DR.
 */

#include "virt.h"

void uart_init(void)
{
    /* QEMU already configures PL011. Just ensure UART + TX + RX enabled. */
    uint32_t cr = mmio_read32(PL011_BASE + PL011_CR);
    cr |= PL011_CR_UARTEN | PL011_CR_TXE | PL011_CR_RXE;
    mmio_write32(PL011_BASE + PL011_CR, cr);
}

void uart_putc(char c)
{
    /* Wait until TX FIFO is not full */
    while (mmio_read32(PL011_BASE + PL011_FR) & PL011_FR_TXFF)
        ;
    mmio_write32(PL011_BASE + PL011_DR, (uint32_t)c);
}

void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

int uart_rx_ready(void)
{
    return !(mmio_read32(PL011_BASE + PL011_FR) & PL011_FR_RXFE);
}

char uart_getc(void)
{
    /* Wait until RX FIFO has data */
    while (mmio_read32(PL011_BASE + PL011_FR) & PL011_FR_RXFE)
        ;
    return (char)(mmio_read32(PL011_BASE + PL011_DR) & 0xFF);
}

int uart_trygetc(void)
{
    if (mmio_read32(PL011_BASE + PL011_FR) & PL011_FR_RXFE)
        return -1;
    return (int)(mmio_read32(PL011_BASE + PL011_DR) & 0xFF);
}
