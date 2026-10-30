#ifndef OSITOK_SERIAL_UART_HOST_H
#define OSITOK_SERIAL_UART_HOST_H

#include "../kernel/serial.h"

uint8_t serial_test_inb(uint16_t port);
void serial_test_outb(uint16_t port, uint8_t value);
#define inb serial_test_inb
#define outb serial_test_outb

#endif
