/*
 * term.c -- Terminal line editor for ARM64 (serial-only)
 *
 * Port of x86 terminal.c with history, cursor movement, and control keys.
 * Uses UART for input/output (no framebuffer, no keyboard driver). It only
 * moves the cursor with '\b' and rewrites text, which the screen console
 * follows too (it knows no escape sequences).
 *
 * Keys: Left/Right, Home/End (or Ctrl+A/E), Backspace, Delete (or Ctrl+D on
 * a line), Ctrl+U/K (kill to the start/end), Ctrl+W (word), Up/Down
 * (history), Tab (shell_complete), Ctrl+C (drop the line).
 */

#include "../include/hal.h"
#include "../include/types.h"

/* ── Terminal state ──────────────────────────────────────── */

#define TERM_LINE_MAX  256
#define TERM_HISTORY   32

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

/* From the cursor on, after the text there changed: the rest of the line,
 * `gone` blanks over what it no longer covers, the cursor back */
static void term_redraw_tail(int gone)
{
    for (int i = line_pos; i < line_len; i++) term_putchar(line_buf[i]);
    for (int i = 0; i < gone; i++) term_putchar(' ');
    for (int i = line_pos; i < line_len + gone; i++) term_putchar('\b');
}

static void term_insert(char c)
{
    if (line_len >= TERM_LINE_MAX - 1)
        return;
    for (int i = line_len; i > line_pos; i--)
        line_buf[i] = line_buf[i - 1];
    line_buf[line_pos] = c;
    line_len++;
    line_buf[line_len] = '\0';
    term_putchar(c);
    line_pos++;
    term_redraw_tail(0);
}

/* `n` characters at the cursor out of the line */
static void term_delete(int n)
{
    if (n > line_len - line_pos)
        n = line_len - line_pos;
    if (n <= 0)
        return;
    for (int i = line_pos; i + n <= line_len; i++)
        line_buf[i] = line_buf[i + n];
    line_len -= n;
    term_redraw_tail(n);
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

void term_history_print(void)
{
    int first = hist_count > TERM_HISTORY ? hist_count - TERM_HISTORY : 0;
    for (int i = first; i < hist_count; i++) {
        serial_putdec((uint64_t)i + 1);
        serial_puts("  ");
        serial_puts(history[i % TERM_HISTORY]);
        serial_puts("\n");
    }
}

void term_history_clear(void)
{
    hist_count = 0;
    hist_index = 0;
}

/* ── Escape sequences ────────────────────────────────────── */

/* The next byte of a sequence: a terminal sends them together, but give it
 * ~100 ms (a script that types a byte at a time) */
static int esc_byte(void)
{
    for (int i = 0; i < 10000; i++) {
        int c = uart_trygetc();
        if (c >= 0)
            return c;
        udelay(10);
    }
    return -1;
}

/* After ESC: "[A".."[D" (arrows), "[H"/"[F" or "OH"/"OF" (Home/End), and
 * "[n~" (1 or 7 Home, 4 or 8 End, 3 Delete, returned as 'X') */
static int read_escape_seq(void)
{
    int c = esc_byte();
    if (c != '[' && c != 'O')
        return -1;
    int n = 0;
    c = esc_byte();
    while (c >= '0' && c <= '9') {
        n = n * 10 + (c - '0');
        c = esc_byte();
    }
    if (c == '~')
        return n == 1 || n == 7 ? 'H' : n == 4 || n == 8 ? 'F' : n == 3 ? 'X' : -1;
    return c;
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

/* ── Completion ──────────────────────────────────────────── */

/* Tab: the word before the cursor, a command name if it's the first of
 * the line or of a pipe stage */
static void term_complete(const char *prompt)
{
    int ws = line_pos;
    while (ws > 0 && line_buf[ws - 1] != ' ')
        ws--;
    int first = 1;
    for (int i = ws - 1; i >= 0 && line_buf[i] != '|'; i--)
        if (line_buf[i] != ' ')
            first = 0;
    char add[TERM_LINE_MAX];
    int n = shell_complete(line_buf + ws, line_pos - ws, first, add, sizeof(add));
    if (add[0]) {
        for (const char *p = add; *p; p++)
            term_insert(*p);
    } else if (n > 1) {
        /* The shell listed them under the line: the line again */
        serial_puts(prompt);
        term_redraw_line();
    }
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

        /* ESC — start of escape sequence (arrows, Home/End, Delete) */
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
            if (seq == 'H') c = 1;         /* Home: as Ctrl+A */
            else if (seq == 'F') c = 5;    /* End: as Ctrl+E */
            else if (seq == 'X') {         /* Delete */
                term_delete(1);
                continue;
            } else {
                continue;
            }
        }

        /* Ctrl+C */
        if (c == 3) {
            serial_puts("^C\n");
            buf[0] = '\0';
            return 0;
        }

        /* Ctrl+D: EOF on an empty line, else Delete */
        if (c == 4) {
            if (line_len == 0) { serial_puts("\n"); return -1; }
            term_delete(1);
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
                term_putchar('\b');
                line_pos--;
                term_delete(1);
            }
            continue;
        }

        /* Tab — complete */
        if (c == '\t') {
            term_complete(prompt);
            continue;
        }

        /* Ctrl+U — kill to the start of the line */
        if (c == 21) {
            int n = line_pos;
            while (line_pos > 0) { term_putchar('\b'); line_pos--; }
            term_delete(n);
            continue;
        }

        /* Ctrl+K — kill to the end of the line */
        if (c == 11) {
            term_delete(line_len - line_pos);
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
            int n = 0;
            while (line_pos > 0 && line_buf[line_pos - 1] == ' ') {
                term_putchar('\b'); line_pos--; n++;
            }
            while (line_pos > 0 && line_buf[line_pos - 1] != ' ') {
                term_putchar('\b'); line_pos--; n++;
            }
            term_delete(n);
            continue;
        }

        /* Printable character — insert at cursor */
        if (c >= 32 && c < 127)
            term_insert(c);
    }
}
