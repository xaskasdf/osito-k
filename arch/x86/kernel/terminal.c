/*
 * OsitoK x86-64 — Terminal Line Editor
 *
 * X-OS7: Line discipline with echo, backspace, Ctrl+C/D,
 * and basic VT100 cursor control. Bridges keyboard input
 * to a line-buffered interface for the shell.
 */

#include "../include/types.h"

/* ── External functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);

/* Framebuffer */
extern void fb_puts(const char *s);
extern void fb_puts_color(const char *s, uint32_t color);
extern void fb_putchar(char c);

/* Keyboard */
extern char kb_getchar(void);
extern char kb_trygetchar(void);
extern bool kb_has_input(void);

/* Serial output (single char) */
extern void serial_putchar(char c);

/* ── Terminal state ──────────────────────────────────────────── */

#define TERM_LINE_MAX  256
#define TERM_HISTORY   8

static char line_buf[TERM_LINE_MAX];
static uint32_t line_pos;
static uint32_t line_len;

/* Command history */
static char history[TERM_HISTORY][TERM_LINE_MAX];
static int hist_count;
static int hist_index;   /* Current browse position */

/* Signal flags */
static volatile bool term_sigint;  /* Ctrl+C pressed */

/* ── Terminal output (serial + framebuffer) ───────────────────── */

static void term_putchar(char c)
{
    char s[2] = { c, 0 };
    serial_puts(s);
    fb_putchar(c);
}

static void term_puts(const char *s)
{
    serial_puts(s);
    fb_puts(s);
}

static void term_puts_color(const char *s, uint32_t color)
{
    serial_puts(s);
    fb_puts_color(s, color);
}

/* ── Erase current line display ──────────────────────────────── */

static void term_erase_line(void)
{
    /* Move cursor to start of input, overwrite with spaces */
    for (uint32_t i = 0; i < line_pos; i++)
        term_putchar('\b');
    for (uint32_t i = 0; i < line_len; i++)
        term_putchar(' ');
    for (uint32_t i = 0; i < line_len; i++)
        term_putchar('\b');
}

/* ── Redraw line after edit ──────────────────────────────────── */

static void term_redraw_line(void)
{
    for (uint32_t i = 0; i < line_len; i++)
        term_putchar(line_buf[i]);
    /* Move cursor back to line_pos */
    for (uint32_t i = line_pos; i < line_len; i++)
        term_putchar('\b');
}

/* ── Add to history ──────────────────────────────────────────── */

static void hist_add(const char *cmd)
{
    if (cmd[0] == '\0') return;  /* Skip empty */

    /* Don't duplicate last entry */
    if (hist_count > 0 && strcmp(history[(hist_count - 1) % TERM_HISTORY], cmd) == 0)
        return;

    int idx = hist_count % TERM_HISTORY;
    uint32_t len = strlen(cmd);
    if (len >= TERM_LINE_MAX) len = TERM_LINE_MAX - 1;
    memcpy(history[idx], cmd, len);
    history[idx][len] = '\0';
    hist_count++;
}

/* ── Get history entry ───────────────────────────────────────── */

static const char *hist_get(int index)
{
    if (index < 0 || index >= hist_count) return NULL;
    if (hist_count - index > TERM_HISTORY) return NULL;
    return history[index % TERM_HISTORY];
}

/* ── Public API ──────────────────────────────────────────────── */

/* Read a line with editing. Returns line length, -1 on EOF (Ctrl+D) */
int term_readline(const char *prompt, char *buf, uint32_t buf_size)
{
    if (prompt) term_puts_color(prompt, 0x0000FF88);

    line_pos = 0;
    line_len = 0;
    line_buf[0] = '\0';
    hist_index = hist_count;
    term_sigint = false;

    for (;;) {
        char c = kb_getchar();

        /* Ctrl+C — interrupt */
        if (c == 3) {
            term_puts("^C\n");
            buf[0] = '\0';
            return 0;
        }

        /* Ctrl+D — EOF (only on empty line) */
        if (c == 4) {
            if (line_len == 0) {
                term_puts("\n");
                return -1;
            }
            continue;
        }

        /* Enter — submit line */
        if (c == '\n' || c == '\r') {
            term_putchar('\n');
            line_buf[line_len] = '\0';

            /* Copy to output buffer */
            uint32_t copy_len = line_len;
            if (copy_len >= buf_size) copy_len = buf_size - 1;
            memcpy(buf, line_buf, copy_len);
            buf[copy_len] = '\0';

            /* Add to history */
            hist_add(line_buf);

            return (int)copy_len;
        }

        /* Backspace (0x08 or 0x7F) */
        if (c == '\b' || c == 0x7F) {
            if (line_pos > 0) {
                /* Shift chars left */
                for (uint32_t i = line_pos - 1; i < line_len - 1; i++)
                    line_buf[i] = line_buf[i + 1];
                line_pos--;
                line_len--;
                line_buf[line_len] = '\0';

                /* Visual: back, redraw rest, erase trailing char */
                term_putchar('\b');
                for (uint32_t i = line_pos; i < line_len; i++)
                    term_putchar(line_buf[i]);
                term_putchar(' ');
                /* Move cursor back */
                for (uint32_t i = line_pos; i <= line_len; i++)
                    term_putchar('\b');
            }
            continue;
        }

        /* Tab — ignore for now */
        if (c == '\t') continue;

        /* Ctrl+U — kill line */
        if (c == 21) {
            term_erase_line();
            line_pos = 0;
            line_len = 0;
            line_buf[0] = '\0';
            continue;
        }

        /* Ctrl+A — move to start */
        if (c == 1) {
            while (line_pos > 0) {
                term_putchar('\b');
                line_pos--;
            }
            continue;
        }

        /* Ctrl+E — move to end */
        if (c == 5) {
            while (line_pos < line_len) {
                term_putchar(line_buf[line_pos]);
                line_pos++;
            }
            continue;
        }

        /* Ctrl+W — delete word backward */
        if (c == 23) {
            while (line_pos > 0 && line_buf[line_pos - 1] == ' ') {
                term_putchar('\b');
                line_pos--;
                line_len--;
                for (uint32_t i = line_pos; i < line_len; i++)
                    line_buf[i] = line_buf[i + 1];
            }
            while (line_pos > 0 && line_buf[line_pos - 1] != ' ') {
                term_putchar('\b');
                line_pos--;
                line_len--;
                for (uint32_t i = line_pos; i < line_len; i++)
                    line_buf[i] = line_buf[i + 1];
            }
            line_buf[line_len] = '\0';
            /* Redraw from cursor */
            for (uint32_t i = line_pos; i < line_len; i++)
                term_putchar(line_buf[i]);
            /* Erase trailing */
            term_putchar(' '); term_putchar(' ');
            uint32_t back = line_len - line_pos + 2;
            for (uint32_t i = 0; i < back; i++)
                term_putchar('\b');
            continue;
        }

        /* Printable character */
        if (c >= 32 && c < 127 && line_len < TERM_LINE_MAX - 1) {
            /* Insert at position */
            for (uint32_t i = line_len; i > line_pos; i--)
                line_buf[i] = line_buf[i - 1];
            line_buf[line_pos] = c;
            line_len++;
            line_buf[line_len] = '\0';

            /* Echo: print from cursor to end, then move back */
            for (uint32_t i = line_pos; i < line_len; i++)
                term_putchar(line_buf[i]);
            line_pos++;
            for (uint32_t i = line_pos; i < line_len; i++)
                term_putchar('\b');
        }
    }
}

/* Check if Ctrl+C was pressed */
bool term_interrupted(void)
{
    bool v = term_sigint;
    term_sigint = false;
    return v;
}

/* Init terminal */
void term_init(void)
{
    line_pos = 0;
    line_len = 0;
    hist_count = 0;
    hist_index = 0;
    term_sigint = false;
    serial_puts("[TERM] Terminal line editor ready\n");
}
