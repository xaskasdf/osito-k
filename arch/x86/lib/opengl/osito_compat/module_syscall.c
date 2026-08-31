/* Ring-0 syscall bridge for dynamically loaded graphics modules. */

#include <stddef.h>
#include <stdint.h>

static void okgl_serial_putc(char c)
{
    const uint16_t data_port = 0x3F8;
    const uint16_t status_port = 0x3FD;

    for (unsigned spin = 0; spin < 4096; spin++) {
        uint8_t ready;
        __asm__ volatile ("inb %1, %0" : "=a"(ready) : "Nd"(status_port));
        if (ready & 0x20) {
            __asm__ volatile ("outb %0, %1" :: "a"((uint8_t)c),
                              "Nd"(data_port));
            return;
        }
        __asm__ volatile ("pause" ::: "memory");
    }
}

static void okgl_serial_puts(const char *message)
{
    if (!message)
        return;
    while (*message) {
        if (*message == '\n')
            okgl_serial_putc('\r');
        okgl_serial_putc(*message++);
    }
}

static void okgl_serial_puthex64(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4)
        okgl_serial_putc(digits[(value >> shift) & 0xf]);
}

void okgl_diag3(const char *message, uint64_t a, uint64_t b, uint64_t c)
{
    okgl_serial_puts(message);
    okgl_serial_puts(" a=0x");
    okgl_serial_puthex64(a);
    okgl_serial_puts(" b=0x");
    okgl_serial_puthex64(b);
    okgl_serial_puts(" c=0x");
    okgl_serial_puthex64(c);
    okgl_serial_putc('\r');
    okgl_serial_putc('\n');
}

void okgl_trace(const char *message)
{
#ifndef OKGL_SERIAL_TRACE
    (void)message;
#else
    okgl_serial_puts(message);
#endif
}

extern long syscall_dispatch(unsigned long nr, unsigned long a1,
                             unsigned long a2, unsigned long a3,
                             unsigned long a4, unsigned long a5,
                             unsigned long a6)
    __attribute__((visibility("default")));

long __syscall0(long nr)
{
    return syscall_dispatch((unsigned long)nr, 0, 0, 0, 0, 0, 0);
}

long __syscall1(long nr, long a1)
{
    return syscall_dispatch((unsigned long)nr, (unsigned long)a1,
                            0, 0, 0, 0, 0);
}

long __syscall2(long nr, long a1, long a2)
{
    return syscall_dispatch((unsigned long)nr, (unsigned long)a1,
                            (unsigned long)a2, 0, 0, 0, 0);
}

long __syscall3(long nr, long a1, long a2, long a3)
{
    return syscall_dispatch((unsigned long)nr, (unsigned long)a1,
                            (unsigned long)a2, (unsigned long)a3,
                            0, 0, 0);
}

long __syscall4(long nr, long a1, long a2, long a3, long a4)
{
    return syscall_dispatch((unsigned long)nr, (unsigned long)a1,
                            (unsigned long)a2, (unsigned long)a3,
                            (unsigned long)a4, 0, 0);
}

long __syscall5(long nr, long a1, long a2, long a3, long a4, long a5)
{
    return syscall_dispatch((unsigned long)nr, (unsigned long)a1,
                            (unsigned long)a2, (unsigned long)a3,
                            (unsigned long)a4, (unsigned long)a5, 0);
}

long __syscall6(long nr, long a1, long a2, long a3, long a4, long a5,
                long a6)
{
    return syscall_dispatch((unsigned long)nr, (unsigned long)a1,
                            (unsigned long)a2, (unsigned long)a3,
                            (unsigned long)a4, (unsigned long)a5,
                            (unsigned long)a6);
}
