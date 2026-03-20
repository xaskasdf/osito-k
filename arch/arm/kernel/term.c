/*
 * term.c -- Terminal line editor over UART
 */
#include "../include/hal.h"

void term_init(void)
{
    /* Nothing to init for now */
}

int term_readline(const char *prompt, char *buf, int bufsize)
{
    serial_puts(prompt);
    int pos = 0;
    buf[0] = 0;

    for (;;) {
        char c = uart_getc();

        if (c == '\r' || c == '\n') {
            serial_puts("\n");
            buf[pos] = 0;
            return pos;
        } else if (c == 0x7F || c == '\b') {
            if (pos > 0) {
                pos--;
                serial_puts("\b \b");
            }
        } else if (c == 0x03) {  /* Ctrl-C */
            serial_puts("^C\n");
            buf[0] = 0;
            return 0;
        } else if (c == 0x15) {  /* Ctrl-U: kill line */
            while (pos > 0) {
                serial_puts("\b \b");
                pos--;
            }
        } else if (c >= 0x20 && c < 0x7F && pos < bufsize - 1) {
            buf[pos++] = c;
            serial_putc(c);
        }
    }
}
