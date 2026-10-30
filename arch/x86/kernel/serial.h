#ifndef OSITOK_SERIAL_H
#define OSITOK_SERIAL_H

#include "serial_tx.h"

void serial_init(void);
void serial_putc(char c);
void serial_putchar(char c);
void serial_puts(const char *s);
void serial_write(const char *data, uint64_t length);
void serial_puthex(uint64_t value, int digits);
void serial_putdec(uint64_t value);
int serial_getc(void);
void serial_poll(void);
void serial_poll_irq(void);
bool serial_flush(void);
void serial_get_stats(serial_stats_t *stats);
void serial_stats_dump(void);
int serial_selftest(void);

#endif
