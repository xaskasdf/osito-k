/*
 * OsitoK - UART0 driver header
 */
#ifndef OSITO_UART_H
#define OSITO_UART_H

#include "osito.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize UART0 with RX interrupts */
void uart_init(void);

/* UART ISR handler — called from os_exception_handler (IRAM) */
void uart_isr_handler(void);

/* Lock/unlock UART output for atomic multi-line printing.
 * uart_lock blocks (yields) until the lock is available. */
void uart_lock(void);
void uart_unlock(void);

/* Send a single byte (polled TX) */
void uart_putc(char c);

/* Send a null-terminated string */
void uart_puts(const char *s);

/* Print a decimal number */
void uart_put_dec(uint32_t val);

/* Print a hex number (0x prefix) */
void uart_put_hex(uint32_t val);

/* Read a byte from RX ring buffer. Returns -1 if empty. */
int uart_getc(void);

/* Check if there's data available in RX buffer */
bool uart_rx_available(void);

/* Bulk write raw bytes directly to UART FIFO.
 * No \n -> \r\n conversion, no mutex. For video bridge use. */
void uart_write_raw(const uint8_t *buf, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* OSITO_UART_H */
