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
extern void boot_diag_maybe_flush(const char *reason, uint64_t min_bytes,
                                  uint64_t min_ticks) __attribute__((weak));
extern void *kmalloc(uint64_t size);
extern void *krealloc(void *ptr, uint64_t new_size);
extern void  kfree(void *ptr);

/* Framebuffer */
extern void fb_puts(const char *s);
extern void fb_puts_color(const char *s, uint32_t color);
extern void fb_putchar(char c);

/* Keyboard */
extern char kb_getchar(void);
extern int kb_getchar_interruptible(bool (*cancel)(void *), void *context)
    __attribute__((weak));
extern char kb_trygetchar(void);
extern bool kb_has_input(void);

/* Serial output (single char) */
extern void serial_putchar(char c);

/* ── Terminal state ──────────────────────────────────────────── */

#define TERM_LINE_INITIAL_CAP  256
#define TERM_HISTORY           8

static char    *line_buf;
static uint64_t line_capacity;
static uint64_t line_pos;
static uint64_t line_len;

/* Command history */
static char *history[TERM_HISTORY];
static int hist_count;
static int hist_index;   /* Current browse position */

/* Signal flags */
static volatile bool term_sigint;  /* Ctrl+C pressed */

static bool term_line_reserve(uint64_t needed)
{
    if (needed <= line_capacity)
        return true;

    uint64_t new_capacity = line_capacity ? line_capacity
                                          : TERM_LINE_INITIAL_CAP;
    while (new_capacity < needed) {
        if (new_capacity > UINT64_MAX / 2) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2;
    }

    char *new_buf = (char *)krealloc(line_buf, new_capacity);
    if (!new_buf)
        return false;

    line_buf = new_buf;
    line_capacity = new_capacity;
    return true;
}

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
    for (uint64_t i = 0; i < line_pos; i++)
        term_putchar('\b');
    for (uint64_t i = 0; i < line_len; i++)
        term_putchar(' ');
    for (uint64_t i = 0; i < line_len; i++)
        term_putchar('\b');
}

/* ── Redraw line after edit ──────────────────────────────────── */

static void term_redraw_line(void)
{
    for (uint64_t i = 0; i < line_len; i++)
        term_putchar(line_buf[i]);
    /* Move cursor back to line_pos */
    for (uint64_t i = line_pos; i < line_len; i++)
        term_putchar('\b');
}

/* ── Add to history ──────────────────────────────────────────── */

static void hist_add(const char *cmd)
{
    if (cmd[0] == '\0') return;  /* Skip empty */

    /* Don't duplicate last entry */
    if (hist_count > 0) {
        const char *last = history[(hist_count - 1) % TERM_HISTORY];
        if (last && strcmp(last, cmd) == 0)
            return;
    }

    int idx = hist_count % TERM_HISTORY;
    uint64_t len = strlen(cmd);
    if (len == UINT64_MAX)
        return;

    char *copy = (char *)kmalloc(len + 1);
    if (!copy)
        return;
    memcpy(copy, cmd, len + 1);

    if (history[idx])
        kfree(history[idx]);
    history[idx] = copy;
    hist_count++;
}

/* ── Get history entry ───────────────────────────────────────── */

static const char *hist_get(int index)
{
    if (index < 0 || index >= hist_count) return NULL;
    if (hist_count - index > TERM_HISTORY) return NULL;
    return history[index % TERM_HISTORY];
}

/* Navigation sequences arrive from PS/2, HID and the serial terminal. */
static bool term_apply_csi(char final, unsigned param)
{
    if (final == 'A' || final == 'B') {
        int index = hist_index + (final == 'A' ? -1 : 1);
        const char *entry = index == hist_count ? "" : hist_get(index);
        if (!entry) return true;
        uint64_t len = strlen(entry);
        if (!term_line_reserve(len + 1)) return false;
        term_erase_line();
        memcpy(line_buf, entry, len + 1);
        line_len = line_pos = len;
        hist_index = index;
        term_redraw_line();
    } else if (final == 'D') {
        if (line_pos) { term_putchar('\b'); line_pos--; }
    } else if (final == 'C') {
        if (line_pos < line_len) term_putchar(line_buf[line_pos++]);
    } else if (final == 'H' || (final == '~' && (param == 1 || param == 7))) {
        while (line_pos) { term_putchar('\b'); line_pos--; }
    } else if (final == 'F' || (final == '~' && (param == 4 || param == 8))) {
        while (line_pos < line_len) term_putchar(line_buf[line_pos++]);
    } else if (final == '~' && param == 3 && line_pos < line_len) {
        for (uint64_t i = line_pos; i < line_len; i++)
            line_buf[i] = line_buf[i + 1];
        line_len--;
        for (uint64_t i = line_pos; i < line_len; i++) term_putchar(line_buf[i]);
        term_putchar(' ');
        for (uint64_t i = line_pos; i <= line_len; i++) term_putchar('\b');
    }
    return true;
}

/* ── Public API ──────────────────────────────────────────────── */

enum {
    TERM_READ_OK = 0,
    TERM_READ_EVENT = 1,
    TERM_READ_EOF = -1,
    TERM_READ_NOMEM = -2,
};

static void term_clear_secret(char *buf, unsigned capacity)
{
    volatile char *p = buf;
    while (capacity--) *p++ = 0;
}

/* Caller-owned input, independent of the editor and command history. Errors
 * drain through Enter rather than leaving a pasted suffix as a shell command. */
int term_read_secret(const char *prompt, char *buf, unsigned capacity, bool echo)
{
    if (!buf || !capacity || capacity > 0x7fffffffU) return -1;
    term_clear_secret(buf, capacity);
    if (prompt) term_puts(prompt);
    unsigned n = 0, escape = 0;
    bool invalid = false;
    for (;;) {
        unsigned char c = (unsigned char)kb_getchar();
        if (c == 3 || c == 4 || c == '\r' || c == '\n') {
            term_putchar('\n');
            if (c == 3 || c == 4 || invalid) {
                term_clear_secret(buf, capacity);
                return -1;
            }
            return (int)n;
        }
        if (c == 27) { escape = 1; continue; }
        if (escape == 1) {
            escape = 0;
            if (c == '[' || c == 'O') { escape = 2; continue; }
        } else if (escape == 2) {
            if (c >= 0x20 && c <= 0x3f) continue;
            escape = 0;
            if (c >= 0x40 && c <= 0x7e) continue;
        }
        if (c == 21) {
            if (echo) for (unsigned i = 0; i < n; i++) term_puts("\b \b");
            term_clear_secret(buf, capacity);
            n = 0; invalid = false;
        } else if (c == '\b' || c == 0x7f) {
            if (n) {
                buf[--n] = 0;
                if (echo) term_puts("\b \b");
            }
        } else if (c >= 0x20 && c <= 0x7e) {
            if (n < capacity - 1 && !invalid) {
                buf[n++] = c;
                if (echo) term_putchar(c);
            } else invalid = true;
        } else if (c >= 0x80 || c == '\t') invalid = true;
    }
}

struct term_prompt_wait {
    bool (*poll)(bool empty);
    bool empty;
};

static bool term_prompt_poll(void *context)
{
    struct term_prompt_wait *wait = context;
    return wait->poll(wait->empty);
}

static int term_readline_edit_poll(const char *prompt, bool (*poll)(bool empty))
{
    if (prompt) {
        term_puts_color(prompt, 0x0000FF88);
        if (boot_diag_maybe_flush)
            boot_diag_maybe_flush("readline-prompt", 1, 0);
    }

    if (!term_line_reserve(TERM_LINE_INITIAL_CAP)) {
        term_puts("terminal: out of memory\n");
        return TERM_READ_NOMEM;
    }

    line_pos = 0;
    line_len = 0;
    line_buf[0] = '\0';
    hist_index = hist_count;
    term_sigint = false;
    unsigned escape_state = 0, escape_param = 0;
    bool escape_invalid = false;

    for (;;) {
        char c;
        if (poll && kb_getchar_interruptible) {
            struct term_prompt_wait wait = {poll, !line_len && !escape_state};
            int key = kb_getchar_interruptible(term_prompt_poll, &wait);
            if (key < 0) return TERM_READ_EVENT;
            /* Revoke idle admission before applying a local keystroke. */
            poll(false);
            c = (char)key;
        } else c = kb_getchar();

        /* Incremental parsing never waits for a suffix after a lone Escape.
         * Consume a complete CSI/SS3 sequence, not printable fragments of it. */
        if (c == 27) {
            escape_state = 1;
            escape_param = 0;
            escape_invalid = false;
            continue;
        }
        if (escape_state == 1) {
            escape_state = 0;
            if (c == '[' || c == 'O') { escape_state = 2; continue; }
        } else if (escape_state == 2) {
            if (c >= '0' && c <= '9') {
                if (escape_param > 999) escape_invalid = true;
                else escape_param = escape_param * 10 + (unsigned)(c - '0');
                continue;
            }
            if (c >= 0x20 && c <= 0x3F) {
                escape_invalid = true;
                continue;
            }
            escape_state = 0;
            if (c >= 0x40 && c <= 0x7E) {
                if (!escape_invalid && !term_apply_csi(c, escape_param)) {
                    term_puts("\nterminal: out of memory\n");
                    return TERM_READ_NOMEM;
                }
                continue;
            }
            /* Control keys cancel incomplete sequences and retain their role. */
        }

        /* Ctrl+C — interrupt */
        if (c == 3) {
            term_puts("^C\n");
            line_pos = 0;
            line_len = 0;
            line_buf[0] = '\0';
            return TERM_READ_OK;
        }

        /* Ctrl+D — EOF (only on empty line) */
        if (c == 4) {
            if (line_len == 0) {
                term_puts("\n");
                return TERM_READ_EOF;
            }
            continue;
        }

        /* Enter — submit line */
        if (c == '\n' || c == '\r') {
            term_putchar('\n');
            line_buf[line_len] = '\0';

            /* Add to history */
            hist_add(line_buf);

            return TERM_READ_OK;
        }

        /* Backspace (0x08 or 0x7F) */
        if (c == '\b' || c == 0x7F) {
            if (line_pos > 0) {
                /* Shift chars left */
                for (uint64_t i = line_pos - 1; i < line_len - 1; i++)
                    line_buf[i] = line_buf[i + 1];
                line_pos--;
                line_len--;
                line_buf[line_len] = '\0';

                /* Visual: back, redraw rest, erase trailing char */
                term_putchar('\b');
                for (uint64_t i = line_pos; i < line_len; i++)
                    term_putchar(line_buf[i]);
                term_putchar(' ');
                /* Move cursor back */
                for (uint64_t i = line_pos; i <= line_len; i++)
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
                for (uint64_t i = line_pos; i < line_len; i++)
                    line_buf[i] = line_buf[i + 1];
            }
            while (line_pos > 0 && line_buf[line_pos - 1] != ' ') {
                term_putchar('\b');
                line_pos--;
                line_len--;
                for (uint64_t i = line_pos; i < line_len; i++)
                    line_buf[i] = line_buf[i + 1];
            }
            line_buf[line_len] = '\0';
            /* Redraw from cursor */
            for (uint64_t i = line_pos; i < line_len; i++)
                term_putchar(line_buf[i]);
            /* Erase trailing */
            term_putchar(' '); term_putchar(' ');
            uint64_t back = line_len - line_pos + 2;
            for (uint64_t i = 0; i < back; i++)
                term_putchar('\b');
            continue;
        }

        /* Printable character */
        if (c >= 32 && c < 127) {
            if (line_len == UINT64_MAX || !term_line_reserve(line_len + 2)) {
                term_puts("\nterminal: out of memory\n");
                return TERM_READ_NOMEM;
            }

            /* Insert at position */
            for (uint64_t i = line_len; i > line_pos; i--)
                line_buf[i] = line_buf[i - 1];
            line_buf[line_pos] = c;
            line_len++;
            line_buf[line_len] = '\0';

            /* Echo: print from cursor to end, then move back */
            for (uint64_t i = line_pos; i < line_len; i++)
                term_putchar(line_buf[i]);
            line_pos++;
            for (uint64_t i = line_pos; i < line_len; i++)
                term_putchar('\b');
        }
    }
}

static int term_readline_edit(const char *prompt)
{
    return term_readline_edit_poll(prompt, NULL);
}

/* Fixed-buffer compatibility API for bounded prompts. */
int term_readline(const char *prompt, char *buf, uint32_t buf_size)
{
    if (!buf || buf_size == 0)
        return TERM_READ_NOMEM;

    int status = term_readline_edit(prompt);
    if (status != TERM_READ_OK) {
        buf[0] = '\0';
        return status;
    }

    uint64_t copy_len = line_len;
    if (copy_len >= buf_size)
        copy_len = buf_size - 1;
    memcpy(buf, line_buf, copy_len);
    buf[copy_len] = '\0';
    return (int)copy_len;
}

/* Unbounded shell API. The caller owns *buf_out and must kfree() it. */
int term_readline_alloc_poll(const char *prompt, char **buf_out, uint64_t *len_out,
                            bool (*poll)(bool empty))
{
    if (!buf_out) {
        if (poll) poll(false);
        return TERM_READ_NOMEM;
    }

    *buf_out = NULL;
    if (len_out)
        *len_out = 0;

    int status = term_readline_edit_poll(prompt, poll);
    if (poll) poll(false);
    if (status != TERM_READ_OK)
        return status;

    *buf_out = line_buf;
    if (len_out)
        *len_out = line_len;

    line_buf = NULL;
    line_capacity = 0;
    line_pos = 0;
    line_len = 0;
    return TERM_READ_OK;
}

int term_readline_alloc(const char *prompt, char **buf_out, uint64_t *len_out)
{
    return term_readline_alloc_poll(prompt, buf_out, len_out, NULL);
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
    if (line_buf)
        kfree(line_buf);
    line_buf = NULL;
    line_capacity = 0;
    line_pos = 0;
    line_len = 0;
    for (int i = 0; i < TERM_HISTORY; i++) {
        if (history[i])
            kfree(history[i]);
        history[i] = NULL;
    }
    hist_count = 0;
    hist_index = 0;
    term_sigint = false;
    serial_puts("[TERM] Terminal line editor ready\n");
}
