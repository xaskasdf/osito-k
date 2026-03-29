/*
 * OsitoK WASM — Serial driver
 *
 * Replaces arch/x86/kernel/serial.c (COM1 port I/O).
 * Output is appended to the browser terminal via EM_ASM.
 */

#include <stdint.h>
#include <emscripten.h>

void serial_init(void)
{
    /* JS terminal already set up in HTML */
}

void serial_putc(char c)
{
    EM_ASM({ wasm_putchar($0); }, (unsigned char)c);
}

void serial_putchar(char c)
{
    if (c == '\r') return;   /* skip CR — HTML terminal handles line endings */
    serial_putc(c);
}

void serial_puts(const char *s)
{
    while (*s) {
        if (*s != '\r') serial_putc(*s);
        s++;
    }
}

void serial_puthex(uint64_t val, int digits)
{
    static const char hex[] = "0123456789ABCDEF";
    serial_puts("0x");
    for (int i = (digits - 1) * 4; i >= 0; i -= 4)
        serial_putc(hex[(val >> i) & 0xF]);
}

void serial_putdec(uint64_t val)
{
    char buf[21];
    int  i = 20;
    buf[i] = '\0';
    if (val == 0) { serial_putc('0'); return; }
    while (val > 0) { buf[--i] = '0' + (val % 10); val /= 10; }
    serial_puts(buf + i);
}
