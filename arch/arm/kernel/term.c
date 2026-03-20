/*
 * term.c -- Terminal line editor for ARM64 (serial-only)
 *
 * Port of x86 terminal.c with history, cursor movement, and control keys.
 * Uses UART for input/output (no framebuffer, no keyboard driver).
 */

#include "../include/hal.h"
#include "../include/types.h"

/* ── Terminal state ──────────────────────────────────────── */

#define TERM_LINE_MAX  256
#define TERM_HISTORY   8

static char line_buf[TERM_LINE_MAX];
static int  line_pos;
static int  line_len;

/* Command history */
static char history[TERM_HISTORY][TERM_LINE_MAX];
static int  hist_count;
static int  hist_index;

/* ── Output helpers ──────────────────────────────────────── */

static void term_putchar(char c)
{
    serial_putc(c);
}

/* ── Erase current line display ──────────────────────────── */

static void term_erase_line(void)
{
    for (int i = 0; i < line_pos; i++) term_putchar('\b');
    for (int i = 0; i < line_len; i++) term_putchar(' ');
    for (int i = 0; i < line_len; i++) term_putchar('\b');
}

/* ── Redraw line after edit ──────────────────────────────── */

static void term_redraw_line(void)
{
    for (int i = 0; i < line_len; i++) term_putchar(line_buf[i]);
    for (int i = line_pos; i < line_len; i++) term_putchar('\b');
}

/* ── History ─────────────────────────────────────────────── */

static void hist_add(const char *cmd)
{
    if (cmd[0] == '\0') return;
    if (hist_count > 0 && strcmp(history[(hist_count - 1) % TERM_HISTORY], cmd) == 0)
        return;
    int idx = hist_count % TERM_HISTORY;
    int len = strlen(cmd);
    if (len >= TERM_LINE_MAX) len = TERM_LINE_MAX - 1;
    memcpy(history[idx], cmd, len);
    history[idx][len] = '\0';
    hist_count++;
}

static const char *hist_get(int index)
{
    if (index < 0 || index >= hist_count) return (void *)0;
    if (hist_count - index > TERM_HISTORY) return (void *)0;
    return history[index % TERM_HISTORY];
}

/* ── Escape sequence parser (arrow keys) ─────────────────── */

static int read_escape_seq(void)
{
    /* After ESC (0x1B), expect '[' then letter */
    int c = uart_trygetc();
    if (c < 0) return -1;
    if (c != '[') return -1;
    c = uart_trygetc();
    if (c < 0) return -1;
    return c;  /* A=up, B=down, C=right, D=left */
}

/* ── History navigation ──────────────────────────────────── */

static void hist_navigate(int dir)
{
    int new_idx = hist_index + dir;
    const char *entry = (void *)0;

    if (dir < 0) {  /* up */
        if (new_idx < 0) return;
        entry = hist_get(new_idx);
        if (!entry) return;
    } else {  /* down */
        if (new_idx > hist_count) return;
        if (new_idx == hist_count) entry = "";  /* back to empty */
        else entry = hist_get(new_idx);
        if (!entry) return;
    }

    hist_index = new_idx;
    term_erase_line();
    line_len = strlen(entry);
    if (line_len >= TERM_LINE_MAX) line_len = TERM_LINE_MAX - 1;
    memcpy(line_buf, entry, line_len);
    line_buf[line_len] = '\0';
    line_pos = line_len;
    term_redraw_line();
}

/* ── Public API ──────────────────────────────────────────── */

void term_init(void)
{
    line_pos = 0;
    line_len = 0;
    hist_count = 0;
    hist_index = 0;
}

int term_readline(const char *prompt, char *buf, int bufsize)
{
    serial_puts(prompt);

    line_pos = 0;
    line_len = 0;
    line_buf[0] = '\0';
    hist_index = hist_count;

    for (;;) {
        char c = uart_getc();

        /* ESC — start of escape sequence (arrow keys) */
        if (c == 0x1B) {
            int seq = read_escape_seq();
            if (seq == 'A') { hist_navigate(-1); continue; }  /* Up */
            if (seq == 'B') { hist_navigate(1);  continue; }  /* Down */
            if (seq == 'C') {  /* Right */
                if (line_pos < line_len) {
                    term_putchar(line_buf[line_pos]);
                    line_pos++;
                }
                continue;
            }
            if (seq == 'D') {  /* Left */
                if (line_pos > 0) {
                    term_putchar('\b');
                    line_pos--;
                }
                continue;
            }
            continue;
        }

        /* Ctrl+C */
        if (c == 3) {
            serial_puts("^C\n");
            buf[0] = '\0';
            return 0;
        }

        /* Ctrl+D (EOF on empty line) */
        if (c == 4) {
            if (line_len == 0) { serial_puts("\n"); return -1; }
            continue;
        }

        /* Enter */
        if (c == '\n' || c == '\r') {
            serial_puts("\n");
            line_buf[line_len] = '\0';
            int copy_len = line_len;
            if (copy_len >= bufsize) copy_len = bufsize - 1;
            memcpy(buf, line_buf, copy_len);
            buf[copy_len] = '\0';
            hist_add(line_buf);
            return copy_len;
        }

        /* Backspace */
        if (c == '\b' || c == 0x7F) {
            if (line_pos > 0) {
                for (int i = line_pos - 1; i < line_len - 1; i++)
                    line_buf[i] = line_buf[i + 1];
                line_pos--;
                line_len--;
                line_buf[line_len] = '\0';
                term_putchar('\b');
                for (int i = line_pos; i < line_len; i++)
                    term_putchar(line_buf[i]);
                term_putchar(' ');
                for (int i = line_pos; i <= line_len; i++)
                    term_putchar('\b');
            }
            continue;
        }

        /* Tab — ignore */
        if (c == '\t') continue;

        /* Ctrl+U — kill line */
        if (c == 21) {
            term_erase_line();
            line_pos = 0;
            line_len = 0;
            line_buf[0] = '\0';
            continue;
        }

        /* Ctrl+A — home */
        if (c == 1) {
            while (line_pos > 0) { term_putchar('\b'); line_pos--; }
            continue;
        }

        /* Ctrl+E — end */
        if (c == 5) {
            while (line_pos < line_len) { term_putchar(line_buf[line_pos]); line_pos++; }
            continue;
        }

        /* Ctrl+W — delete word backward */
        if (c == 23) {
            while (line_pos > 0 && line_buf[line_pos - 1] == ' ') {
                term_putchar('\b'); line_pos--; line_len--;
                for (int i = line_pos; i < line_len; i++) line_buf[i] = line_buf[i + 1];
            }
            while (line_pos > 0 && line_buf[line_pos - 1] != ' ') {
                term_putchar('\b'); line_pos--; line_len--;
                for (int i = line_pos; i < line_len; i++) line_buf[i] = line_buf[i + 1];
            }
            line_buf[line_len] = '\0';
            for (int i = line_pos; i < line_len; i++) term_putchar(line_buf[i]);
            term_putchar(' '); term_putchar(' ');
            int back = line_len - line_pos + 2;
            for (int i = 0; i < back; i++) term_putchar('\b');
            continue;
        }

        /* Printable character — insert at cursor */
        if (c >= 32 && c < 127 && line_len < TERM_LINE_MAX - 1) {
            for (int i = line_len; i > line_pos; i--)
                line_buf[i] = line_buf[i - 1];
            line_buf[line_pos] = c;
            line_len++;
            line_buf[line_len] = '\0';
            for (int i = line_pos; i < line_len; i++)
                term_putchar(line_buf[i]);
            line_pos++;
            for (int i = line_pos; i < line_len; i++)
                term_putchar('\b');
        }
    }
}
