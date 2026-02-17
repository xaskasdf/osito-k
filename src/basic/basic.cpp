/*
 * OsitoK - Tiny BASIC interpreter
 *
 * Native interpreter (not compiled to VM bytecode).
 * Runs inside the shell task context.
 *
 * Memory budget:
 *   Static state: ~270 bytes
 *   Program buffer: 3072 bytes (heap, allocated on entry)
 *   Token buffer: 128 bytes (stack)
 *   Total: ~3.4 KB
 */

#include "basic/basic.h"
#include "drivers/uart.h"
#include "drivers/video.h"
#include "fs/ositofs.h"
#include "mem/heap.h"
#include "kernel/task.h"

extern "C" {

/* ====== Token definitions ====== */

enum {
    TOK_NUM     = 0x80,  /* followed by 4-byte int32 LE */
    TOK_STR     = 0x81,  /* followed by 1-byte len, then chars */
    TOK_VAR     = 0x82,  /* followed by 1-byte index (0-25) */
    TOK_EOL     = 0x83,  /* end of tokenized line */

    /* Keywords (statements) */
    TOK_PRINT   = 0x90,
    TOK_INPUT   = 0x91,
    TOK_LET     = 0x92,
    TOK_IF      = 0x93,
    TOK_THEN    = 0x94,
    TOK_GOTO    = 0x95,
    TOK_GOSUB   = 0x96,
    TOK_RETURN  = 0x97,
    TOK_FOR     = 0x98,
    TOK_TO      = 0x99,
    TOK_STEP    = 0x9A,
    TOK_NEXT    = 0x9B,
    TOK_REM     = 0x9C,
    TOK_END     = 0x9D,

    /* Commands */
    TOK_NEW     = 0xA0,
    TOK_LIST    = 0xA1,
    TOK_RUN     = 0xA2,
    TOK_SAVE    = 0xA3,
    TOK_LOAD    = 0xA4,
    TOK_BYE     = 0xA5,

    /* Graphics */
    TOK_CLS     = 0xB0,
    TOK_PSET    = 0xB1,
    TOK_LINE    = 0xB2,
    TOK_DRAW    = 0xB3,

    /* Operators that map to keywords */
    TOK_MOD     = 0xC0,
    TOK_AND     = 0xC1,
    TOK_OR      = 0xC2,
    TOK_NOT     = 0xC3,

    /* Functions */
    TOK_ABS     = 0xD0,
    TOK_RND     = 0xD1,
    TOK_PEEK    = 0xD2,
};

/* Keyword table for tokenizer */
struct kw_entry {
    const char *name;
    uint8_t     token;
};

static const kw_entry keywords[] = {
    { "PRINT",  TOK_PRINT  },
    { "INPUT",  TOK_INPUT  },
    { "LET",    TOK_LET    },
    { "IF",     TOK_IF     },
    { "THEN",   TOK_THEN   },
    { "GOTO",   TOK_GOTO   },
    { "GOSUB",  TOK_GOSUB  },
    { "RETURN", TOK_RETURN },
    { "FOR",    TOK_FOR    },
    { "TO",     TOK_TO     },
    { "STEP",   TOK_STEP   },
    { "NEXT",   TOK_NEXT   },
    { "REM",    TOK_REM    },
    { "END",    TOK_END    },
    { "NEW",    TOK_NEW    },
    { "LIST",   TOK_LIST   },
    { "RUN",    TOK_RUN    },
    { "SAVE",   TOK_SAVE   },
    { "LOAD",   TOK_LOAD   },
    { "BYE",    TOK_BYE    },
    { "CLS",    TOK_CLS    },
    { "PSET",   TOK_PSET   },
    { "LINE",   TOK_LINE   },
    { "DRAW",   TOK_DRAW   },
    { "MOD",    TOK_MOD    },
    { "AND",    TOK_AND    },
    { "OR",     TOK_OR     },
    { "NOT",    TOK_NOT    },
    { "ABS",    TOK_ABS    },
    { "RND",    TOK_RND    },
    { "PEEK",   TOK_PEEK   },
    { nullptr,  0          }
};

/* ====== Interpreter state ====== */

#define PROG_SIZE    3072
#define LINE_BUF_LEN 80
#define TOK_BUF_LEN  128
#define GOSUB_DEPTH  8
#define FOR_DEPTH    4

struct for_entry {
    uint8_t  var;       /* variable index (0-25) */
    int32_t  limit;
    int32_t  step;
    uint8_t *loop_ptr;  /* pointer into prog after FOR line */
};

static struct {
    int32_t  vars[26];
    uint8_t *prog;             /* heap-allocated program buffer */
    uint16_t prog_used;
    uint8_t *exec_ptr;         /* current execution pointer in prog */
    uint8_t *tp;               /* token pointer (current position) */
    uint8_t *tp_end;           /* end of tokens for current line */
    uint8_t *gosub_stack[GOSUB_DEPTH];
    uint8_t  gosub_sp;
    for_entry for_stack[FOR_DEPTH];
    uint8_t  for_sp;
    char     line_buf[LINE_BUF_LEN];
    uint8_t  tok_buf[TOK_BUF_LEN];
    uint8_t  running;          /* program is executing */
    uint8_t  quit;             /* exit BASIC */
    uint32_t rnd_state;        /* simple PRNG state */
} bas;

/* ====== Helpers ====== */

static inline bool is_alpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static inline bool is_digit(char c) { return c >= '0' && c <= '9'; }
static inline bool is_space(char c) { return c == ' ' || c == '\t'; }
static inline char to_upper(char c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

static void bas_error(const char *msg)
{
    uart_puts("? ");
    uart_puts(msg);
    uart_puts("\n");
    bas.running = 0;
}

static int32_t bas_rnd(int32_t n)
{
    bas.rnd_state = bas.rnd_state * 1103515245 + 12345;
    if (n <= 0) return 0;
    return (int32_t)((bas.rnd_state >> 16) % (uint32_t)n);
}

/* ====== Tokenizer ====== */

/* Returns bytes written to tok_buf, or -1 on error.
 * If line starts with a number, *line_num is set and text advances past it.
 * Otherwise *line_num = 0 (immediate mode). */
static int tokenize(const char *src, uint8_t *out, int max_out, uint16_t *line_num)
{
    uint8_t *op = out;
    uint8_t *op_end = out + max_out - 6;  /* safety margin */
    const char *p = src;

    *line_num = 0;

    /* Skip leading spaces */
    while (is_space(*p)) p++;

    /* Check for line number */
    if (is_digit(*p)) {
        uint32_t num = 0;
        while (is_digit(*p)) {
            num = num * 10 + (*p - '0');
            p++;
        }
        if (num > 9999) { bas_error("LINE > 9999"); return -1; }
        *line_num = (uint16_t)num;
        while (is_space(*p)) p++;
    }

    /* Tokenize the rest */
    while (*p && op < op_end) {
        /* Skip spaces */
        if (is_space(*p)) { p++; continue; }

        /* String literal */
        if (*p == '"') {
            p++;
            const char *start = p;
            while (*p && *p != '"') p++;
            int len = (int)(p - start);
            if (*p == '"') p++;
            if (len > 255) len = 255;
            if (op + 2 + len > op_end) break;
            *op++ = TOK_STR;
            *op++ = (uint8_t)len;
            for (int i = 0; i < len; i++)
                *op++ = (uint8_t)start[i];
            continue;
        }

        /* REM — rest of line is comment */
        if (to_upper(p[0]) == 'R' && to_upper(p[1]) == 'E' && to_upper(p[2]) == 'M' &&
            (!p[3] || is_space(p[3]))) {
            *op++ = TOK_REM;
            break;  /* ignore rest of line */
        }

        /* Try keywords */
        if (is_alpha(*p)) {
            /* Collect word */
            const char *ws = p;
            while (is_alpha(*p) || *p == '$') p++;
            int wlen = (int)(p - ws);

            /* Match against keyword table */
            bool found = false;
            for (const kw_entry *kw = keywords; kw->name; kw++) {
                int klen = 0;
                const char *kn = kw->name;
                while (kn[klen]) klen++;
                if (klen == wlen) {
                    bool match = true;
                    for (int i = 0; i < klen; i++) {
                        if (to_upper(ws[i]) != kw->name[i]) { match = false; break; }
                    }
                    if (match) {
                        *op++ = kw->token;
                        found = true;
                        break;
                    }
                }
            }
            if (found) continue;

            /* Single letter = variable (A-Z) */
            if (wlen == 1 && is_alpha(ws[0])) {
                *op++ = TOK_VAR;
                *op++ = (uint8_t)(to_upper(ws[0]) - 'A');
                continue;
            }

            /* Unknown identifier */
            bas_error("UNKNOWN WORD");
            return -1;
        }

        /* Number */
        if (is_digit(*p)) {
            int32_t val = 0;
            bool neg = false;
            while (is_digit(*p)) {
                val = val * 10 + (*p - '0');
                p++;
            }
            if (neg) val = -val;
            if (op + 5 > op_end) break;
            *op++ = TOK_NUM;
            *op++ = (uint8_t)(val & 0xFF);
            *op++ = (uint8_t)((val >> 8) & 0xFF);
            *op++ = (uint8_t)((val >> 16) & 0xFF);
            *op++ = (uint8_t)((val >> 24) & 0xFF);
            continue;
        }

        /* Operators: <=, >=, <>, <, >, =, +, -, *, /, (, ), ,, ; */
        if (p[0] == '<' && p[1] == '=') { *op++ = 'L'; p += 2; continue; }  /* LE */
        if (p[0] == '>' && p[1] == '=') { *op++ = 'G'; p += 2; continue; }  /* GE */
        if (p[0] == '<' && p[1] == '>') { *op++ = 'N'; p += 2; continue; }  /* NE */

        /* Single-char tokens (kept as ASCII) */
        char c = *p++;
        *op++ = (uint8_t)c;
    }

    *op++ = TOK_EOL;
    return (int)(op - out);
}

/* ====== Program storage ====== */
/* Format: [line_hi][line_lo][len][tokens...(len bytes)]
 * Lines are stored sorted by line number. */

/* Find a line in the program. Returns pointer to its header, or null. */
static uint8_t *prog_find(uint16_t num)
{
    uint8_t *p = bas.prog;
    uint8_t *end = bas.prog + bas.prog_used;
    while (p < end) {
        uint16_t ln = ((uint16_t)p[0] << 8) | p[1];
        uint8_t len = p[2];
        if (ln == num) return p;
        p += 3 + len;
    }
    return nullptr;
}

/* Delete a line from the program. */
static void prog_delete(uint16_t num)
{
    uint8_t *p = prog_find(num);
    if (!p) return;
    uint8_t len = p[2];
    int total = 3 + len;
    uint8_t *rest = p + total;
    int remain = (int)((bas.prog + bas.prog_used) - rest);
    for (int i = 0; i < remain; i++)
        p[i] = rest[i];
    bas.prog_used -= (uint16_t)total;
}

/* Insert a tokenized line into the program (sorted). */
static void prog_insert(uint16_t num, const uint8_t *tokens, int tok_len)
{
    /* Delete existing line with same number first */
    prog_delete(num);

    int needed = 3 + tok_len;
    if (bas.prog_used + needed > PROG_SIZE) {
        bas_error("PROGRAM FULL");
        return;
    }

    /* Find insertion point (keep sorted) */
    uint8_t *p = bas.prog;
    uint8_t *end = bas.prog + bas.prog_used;
    while (p < end) {
        uint16_t ln = ((uint16_t)p[0] << 8) | p[1];
        if (ln > num) break;
        p += 3 + p[2];
    }

    /* Shift everything after insertion point */
    int shift = (int)(end - p);
    for (int i = shift - 1; i >= 0; i--)
        p[needed + i] = p[i];

    /* Write header + tokens */
    p[0] = (uint8_t)(num >> 8);
    p[1] = (uint8_t)(num & 0xFF);
    p[2] = (uint8_t)tok_len;
    for (int i = 0; i < tok_len; i++)
        p[3 + i] = tokens[i];

    bas.prog_used += (uint16_t)needed;
}

/* ====== Token reading helpers ====== */

static inline uint8_t tok_peek(void)
{
    if (bas.tp >= bas.tp_end) return TOK_EOL;
    return *bas.tp;
}

static inline uint8_t tok_next(void)
{
    if (bas.tp >= bas.tp_end) return TOK_EOL;
    return *bas.tp++;
}

static inline void tok_skip(void)
{
    if (bas.tp < bas.tp_end) bas.tp++;
}

/* Read an inline number value from token stream (after TOK_NUM was consumed) */
static int32_t tok_get_num(void)
{
    int32_t val = (int32_t)(
        (uint32_t)bas.tp[0] |
        ((uint32_t)bas.tp[1] << 8) |
        ((uint32_t)bas.tp[2] << 16) |
        ((uint32_t)bas.tp[3] << 24)
    );
    bas.tp += 4;
    return val;
}

/* ====== Expression parser (recursive descent) ====== */

static int32_t expr(void);  /* forward */

/* Primary: number, variable, (expr), -expr, NOT expr, ABS(...), RND(...), PEEK(...) */
static int32_t expr_primary(void)
{
    uint8_t t = tok_peek();

    if (t == TOK_NUM) {
        bas.tp++;  /* skip TOK_NUM */
        return tok_get_num();
    }

    if (t == TOK_VAR) {
        bas.tp++;  /* skip TOK_VAR */
        uint8_t idx = *bas.tp++;
        return bas.vars[idx];
    }

    if (t == '(') {
        bas.tp++;
        int32_t val = expr();
        if (tok_peek() == ')') bas.tp++;
        return val;
    }

    if (t == '-') {
        bas.tp++;
        return -expr_primary();
    }

    if (t == TOK_NOT) {
        bas.tp++;
        return expr_primary() ? 0 : 1;
    }

    if (t == TOK_ABS) {
        bas.tp++;
        if (tok_peek() == '(') bas.tp++;
        int32_t val = expr();
        if (tok_peek() == ')') bas.tp++;
        return val < 0 ? -val : val;
    }

    if (t == TOK_RND) {
        bas.tp++;
        if (tok_peek() == '(') bas.tp++;
        int32_t val = expr();
        if (tok_peek() == ')') bas.tp++;
        return bas_rnd(val);
    }

    if (t == TOK_PEEK) {
        bas.tp++;
        if (tok_peek() == '(') bas.tp++;
        int32_t val = expr();
        if (tok_peek() == ')') bas.tp++;
        volatile uint8_t *addr = (volatile uint8_t *)(uint32_t)val;
        return (int32_t)*addr;
    }

    bas_error("SYNTAX");
    return 0;
}

/* Multiplicative: *, /, MOD */
static int32_t expr_mul(void)
{
    int32_t val = expr_primary();
    for (;;) {
        uint8_t t = tok_peek();
        if (t == '*') { bas.tp++; val *= expr_primary(); }
        else if (t == '/') {
            bas.tp++;
            int32_t d = expr_primary();
            if (d == 0) { bas_error("DIV BY ZERO"); return 0; }
            val /= d;
        }
        else if (t == TOK_MOD) {
            bas.tp++;
            int32_t d = expr_primary();
            if (d == 0) { bas_error("DIV BY ZERO"); return 0; }
            val %= d;
        }
        else break;
    }
    return val;
}

/* Additive: +, - */
static int32_t expr_add(void)
{
    int32_t val = expr_mul();
    for (;;) {
        uint8_t t = tok_peek();
        if (t == '+')      { bas.tp++; val += expr_mul(); }
        else if (t == '-') { bas.tp++; val -= expr_mul(); }
        else break;
    }
    return val;
}

/* Comparison: =, <, >, <=, >=, <> */
static int32_t expr_cmp(void)
{
    int32_t val = expr_add();
    uint8_t t = tok_peek();
    if (t == '=') { bas.tp++; return val == expr_add() ? 1 : 0; }
    if (t == '<') { bas.tp++; return val <  expr_add() ? 1 : 0; }
    if (t == '>') { bas.tp++; return val >  expr_add() ? 1 : 0; }
    if (t == 'L') { bas.tp++; return val <= expr_add() ? 1 : 0; }  /* <= */
    if (t == 'G') { bas.tp++; return val >= expr_add() ? 1 : 0; }  /* >= */
    if (t == 'N') { bas.tp++; return val != expr_add() ? 1 : 0; }  /* <> */
    return val;
}

/* Logical AND/OR */
static int32_t expr(void)
{
    int32_t val = expr_cmp();
    for (;;) {
        uint8_t t = tok_peek();
        if (t == TOK_AND)      { bas.tp++; val = (val && expr_cmp()) ? 1 : 0; }
        else if (t == TOK_OR)  { bas.tp++; val = (val || expr_cmp()) ? 1 : 0; }
        else break;
    }
    return val;
}

/* ====== Statement executor ====== */

static void exec_statement(void);  /* forward */

/* Find a program line by number. Returns pointer to its token data,
 * and sets exec_ptr to the next line. Returns null if not found. */
static uint8_t *find_line_for_exec(uint16_t num)
{
    uint8_t *p = bas.prog;
    uint8_t *end = bas.prog + bas.prog_used;
    while (p < end) {
        uint16_t ln = ((uint16_t)p[0] << 8) | p[1];
        uint8_t len = p[2];
        if (ln == num) {
            return p;
        }
        p += 3 + len;
    }
    return nullptr;
}

/* Print an int32 to UART */
static void print_int(int32_t val)
{
    if (val < 0) {
        uart_putc('-');
        val = -val;
    }
    uart_put_dec((uint32_t)val);
}

static void stmt_print(void)
{
    bool need_newline = true;

    for (;;) {
        uint8_t t = tok_peek();
        if (t == TOK_EOL) break;

        if (t == ';') {
            bas.tp++;
            need_newline = false;
            if (tok_peek() == TOK_EOL) break;
            need_newline = true;
            continue;
        }
        if (t == ',') {
            bas.tp++;
            uart_putc('\t');
            need_newline = true;
            continue;
        }

        /* String literal */
        if (t == TOK_STR) {
            bas.tp++;
            uint8_t len = *bas.tp++;
            for (int i = 0; i < len; i++)
                uart_putc((char)*bas.tp++);
            need_newline = true;
            continue;
        }

        /* Expression */
        int32_t val = expr();
        if (!bas.running && !bas.quit) return;  /* error in expr */
        print_int(val);
        need_newline = true;
    }

    if (need_newline)
        uart_puts("\n");
}

static void stmt_input(void)
{
    /* Optional prompt string */
    if (tok_peek() == TOK_STR) {
        bas.tp++;
        uint8_t len = *bas.tp++;
        for (int i = 0; i < len; i++)
            uart_putc((char)*bas.tp++);
        if (tok_peek() == ',') bas.tp++;
        else if (tok_peek() == ';') bas.tp++;
    }

    /* Variable */
    if (tok_peek() != TOK_VAR) { bas_error("EXPECTED VAR"); return; }
    bas.tp++;
    uint8_t idx = *bas.tp++;

    uart_puts("? ");

    /* Read a line from UART */
    char ibuf[16];
    int ipos = 0;
    for (;;) {
        int c = uart_getc();
        if (c < 0) { task_yield(); continue; }
        if (c == 0x03) { bas.running = 0; bas.quit = 1; return; }  /* Ctrl+C */
        if (c == '\r' || c == '\n') { uart_puts("\n"); break; }
        if ((c == '\b' || c == 127) && ipos > 0) {
            ipos--;
            uart_puts("\b \b");
            continue;
        }
        if (ipos < 15) {
            ibuf[ipos++] = (char)c;
            uart_putc((char)c);
        }
    }
    ibuf[ipos] = '\0';

    /* Parse number */
    int32_t val = 0;
    bool neg = false;
    const char *ip = ibuf;
    if (*ip == '-') { neg = true; ip++; }
    while (is_digit(*ip)) {
        val = val * 10 + (*ip - '0');
        ip++;
    }
    if (neg) val = -val;
    bas.vars[idx] = val;
}

static void stmt_let(void)
{
    /* LET is optional: "LET A=5" or just "A=5" handled by caller */
    if (tok_peek() != TOK_VAR) { bas_error("EXPECTED VAR"); return; }
    bas.tp++;
    uint8_t idx = *bas.tp++;
    if (tok_peek() != '=') { bas_error("EXPECTED ="); return; }
    bas.tp++;
    bas.vars[idx] = expr();
}

static void stmt_goto(void)
{
    int32_t num = expr();
    uint8_t *target = find_line_for_exec((uint16_t)num);
    if (!target) { bas_error("LINE NOT FOUND"); return; }
    bas.exec_ptr = target;
}

static void stmt_gosub(void)
{
    if (bas.gosub_sp >= GOSUB_DEPTH) { bas_error("GOSUB OVERFLOW"); return; }
    int32_t num = expr();
    /* Save return point (next line after current) */
    /* exec_ptr already points to the next line */
    bas.gosub_stack[bas.gosub_sp++] = bas.exec_ptr;
    uint8_t *target = find_line_for_exec((uint16_t)num);
    if (!target) { bas_error("LINE NOT FOUND"); return; }
    bas.exec_ptr = target;
}

static void stmt_return(void)
{
    if (bas.gosub_sp == 0) { bas_error("RETURN WITHOUT GOSUB"); return; }
    bas.exec_ptr = bas.gosub_stack[--bas.gosub_sp];
}

static void stmt_for(void)
{
    if (bas.for_sp >= FOR_DEPTH) { bas_error("FOR OVERFLOW"); return; }
    if (tok_peek() != TOK_VAR) { bas_error("EXPECTED VAR"); return; }
    bas.tp++;
    uint8_t idx = *bas.tp++;
    if (tok_peek() != '=') { bas_error("EXPECTED ="); return; }
    bas.tp++;
    int32_t start = expr();
    if (tok_peek() != TOK_TO) { bas_error("EXPECTED TO"); return; }
    bas.tp++;
    int32_t limit = expr();
    int32_t step = 1;
    if (tok_peek() == TOK_STEP) {
        bas.tp++;
        step = expr();
    }

    bas.vars[idx] = start;

    for_entry *fe = &bas.for_stack[bas.for_sp++];
    fe->var = idx;
    fe->limit = limit;
    fe->step = step;
    fe->loop_ptr = bas.exec_ptr;  /* points to next line (after FOR) */
}

static void stmt_next(void)
{
    uint8_t idx = 255;
    if (tok_peek() == TOK_VAR) {
        bas.tp++;
        idx = *bas.tp++;
    }

    if (bas.for_sp == 0) { bas_error("NEXT WITHOUT FOR"); return; }

    /* Find matching FOR (topmost, or by variable) */
    int fi = -1;
    for (int i = bas.for_sp - 1; i >= 0; i--) {
        if (idx == 255 || bas.for_stack[i].var == idx) {
            fi = i;
            break;
        }
    }
    if (fi < 0) { bas_error("NEXT WITHOUT FOR"); return; }

    for_entry *fe = &bas.for_stack[fi];
    bas.vars[fe->var] += fe->step;

    bool done;
    if (fe->step > 0)
        done = bas.vars[fe->var] > fe->limit;
    else
        done = bas.vars[fe->var] < fe->limit;

    if (done) {
        /* Pop FOR stack down to (and including) this entry */
        bas.for_sp = (uint8_t)fi;
    } else {
        /* Loop back */
        bas.exec_ptr = fe->loop_ptr;
    }
}

static void stmt_if(void)
{
    int32_t cond = expr();
    if (tok_peek() != TOK_THEN) { bas_error("EXPECTED THEN"); return; }
    bas.tp++;

    if (cond) {
        /* Check if what follows THEN is a line number (implicit GOTO) */
        if (tok_peek() == TOK_NUM) {
            bas.tp++;
            int32_t num = tok_get_num();
            uint8_t *target = find_line_for_exec((uint16_t)num);
            if (!target) { bas_error("LINE NOT FOUND"); return; }
            bas.exec_ptr = target;
        } else {
            exec_statement();
        }
    }
    /* If false, skip the rest of the line (do nothing) */
}

static void stmt_cls(void)
{
    fb_clear();
}

static void stmt_pset(void)
{
    int32_t x = expr();
    if (tok_peek() == ',') bas.tp++;
    int32_t y = expr();
    fb_set_pixel((int)x, (int)y);
}

static void stmt_line(void)
{
    int32_t x0 = expr();
    if (tok_peek() == ',') bas.tp++;
    int32_t y0 = expr();
    if (tok_peek() == ',') bas.tp++;
    int32_t x1 = expr();
    if (tok_peek() == ',') bas.tp++;
    int32_t y1 = expr();
    fb_line((int)x0, (int)y0, (int)x1, (int)y1);
}

static void stmt_draw(void)
{
    fb_flush();
}

/* Execute one statement starting at bas.tp */
static void exec_statement(void)
{
    uint8_t t = tok_peek();

    switch (t) {
    case TOK_PRINT:  bas.tp++; stmt_print();  break;
    case TOK_INPUT:  bas.tp++; stmt_input();  break;
    case TOK_LET:    bas.tp++; stmt_let();    break;
    case TOK_IF:     bas.tp++; stmt_if();     break;
    case TOK_GOTO:   bas.tp++; stmt_goto();   break;
    case TOK_GOSUB:  bas.tp++; stmt_gosub();  break;
    case TOK_RETURN: bas.tp++; stmt_return(); break;
    case TOK_FOR:    bas.tp++; stmt_for();    break;
    case TOK_NEXT:   bas.tp++; stmt_next();   break;
    case TOK_END:    bas.tp++; bas.running = 0; break;
    case TOK_REM:    break;  /* skip */
    case TOK_CLS:    bas.tp++; stmt_cls();    break;
    case TOK_PSET:   bas.tp++; stmt_pset();   break;
    case TOK_LINE:   bas.tp++; stmt_line();   break;
    case TOK_DRAW:   bas.tp++; stmt_draw();   break;
    case TOK_VAR:
        /* Implicit LET: A=5 */
        stmt_let();
        break;
    case TOK_EOL:
        break;
    default:
        bas_error("SYNTAX ERROR");
        break;
    }
}

/* ====== Commands (immediate mode) ====== */

static void cmd_new(void)
{
    bas.prog_used = 0;
    for (int i = 0; i < 26; i++) bas.vars[i] = 0;
    bas.gosub_sp = 0;
    bas.for_sp = 0;
}

static void cmd_list(void)
{
    uint8_t *p = bas.prog;
    uint8_t *end = bas.prog + bas.prog_used;

    while (p < end) {
        uint16_t ln = ((uint16_t)p[0] << 8) | p[1];
        uint8_t len = p[2];
        uint8_t *tp = p + 3;
        uint8_t *tp_end = tp + len;

        uart_put_dec(ln);
        uart_putc(' ');

        /* Detokenize for display */
        while (tp < tp_end) {
            uint8_t t = *tp++;
            if (t == TOK_EOL) break;

            if (t == TOK_NUM) {
                int32_t val = (int32_t)(
                    (uint32_t)tp[0] | ((uint32_t)tp[1] << 8) |
                    ((uint32_t)tp[2] << 16) | ((uint32_t)tp[3] << 24));
                tp += 4;
                print_int(val);
            }
            else if (t == TOK_STR) {
                uint8_t slen = *tp++;
                uart_putc('"');
                for (int i = 0; i < slen; i++)
                    uart_putc((char)*tp++);
                uart_putc('"');
            }
            else if (t == TOK_VAR) {
                uart_putc((char)('A' + *tp++));
            }
            else if (t >= 0x80) {
                /* Keyword — look up in table */
                for (const kw_entry *kw = keywords; kw->name; kw++) {
                    if (kw->token == t) {
                        uart_puts(kw->name);
                        break;
                    }
                }
                uart_putc(' ');
            }
            else if (t == 'L') { uart_puts("<="); }
            else if (t == 'G') { uart_puts(">="); }
            else if (t == 'N') { uart_puts("<>"); }
            else {
                uart_putc((char)t);
            }
        }
        uart_puts("\n");

        /* Check Ctrl+C during long listings */
        if (uart_rx_available()) {
            int ch = uart_getc();
            if (ch == 0x03) { uart_puts("\n"); return; }
        }

        p += 3 + len;
    }
}

static void cmd_run(void)
{
    bas.running = 1;
    bas.exec_ptr = bas.prog;
    bas.gosub_sp = 0;
    bas.for_sp = 0;

    while (bas.running && bas.exec_ptr < bas.prog + bas.prog_used) {
        /* Yield periodically + check Ctrl+C */
        task_yield();
        if (uart_rx_available()) {
            int ch = uart_getc();
            if (ch == 0x03) {
                uart_puts("\nBREAK\n");
                bas.running = 0;
                return;
            }
        }

        /* Read line header */
        uint8_t *line_start = bas.exec_ptr;
        uint16_t ln = ((uint16_t)line_start[0] << 8) | line_start[1];
        uint8_t len = line_start[2];
        (void)ln;

        /* Set up token pointers */
        bas.tp = line_start + 3;
        bas.tp_end = bas.tp + len;

        /* Advance exec_ptr to next line BEFORE execution
         * (so GOTO/GOSUB can change it) */
        bas.exec_ptr = line_start + 3 + len;

        /* Execute the line */
        if (tok_peek() != TOK_EOL)
            exec_statement();
    }
    bas.running = 0;
}

static void cmd_save(void)
{
    /* Expect a string token with the filename */
    if (tok_peek() != TOK_STR) { bas_error("EXPECTED FILENAME"); return; }
    bas.tp++;
    uint8_t slen = *bas.tp++;
    char name[24];
    int ni = 0;
    for (int i = 0; i < slen && ni < 23; i++)
        name[ni++] = (char)*bas.tp++;
    name[ni] = '\0';

    if (bas.prog_used == 0) { uart_puts("NOTHING TO SAVE\n"); return; }

    if (fs_overwrite(name, bas.prog, bas.prog_used) == 0) {
        uart_puts("SAVED ");
        uart_put_dec(bas.prog_used);
        uart_puts(" BYTES\n");
    } else {
        bas_error("SAVE FAILED");
    }
}

static void cmd_load(void)
{
    if (tok_peek() != TOK_STR) { bas_error("EXPECTED FILENAME"); return; }
    bas.tp++;
    uint8_t slen = *bas.tp++;
    char name[24];
    int ni = 0;
    for (int i = 0; i < slen && ni < 23; i++)
        name[ni++] = (char)*bas.tp++;
    name[ni] = '\0';

    int size = fs_stat(name);
    if (size < 0) { bas_error("FILE NOT FOUND"); return; }

    /* Read file into temp heap buffer */
    uint8_t *buf = (uint8_t *)heap_alloc((uint32_t)size);
    if (!buf) { bas_error("NO MEMORY"); return; }

    int got = fs_read(name, buf, (uint32_t)size);
    if (got < 0) { heap_free(buf); bas_error("READ FAILED"); return; }

    /* Detect format: if first byte is ASCII digit or space, it's text */
    if (got > 0 && (is_digit((char)buf[0]) || buf[0] == ' ')) {
        /* Text .bas file — parse line by line */
        cmd_new();  /* clear program first */
        char line[LINE_BUF_LEN];
        int li = 0;
        int lines_loaded = 0;
        for (int i = 0; i <= got; i++) {
            char c = (i < got) ? (char)buf[i] : '\n';
            if (c == '\r') continue;
            if (c == '\n') {
                line[li] = '\0';
                if (li > 0) {
                    uint16_t lnum;
                    int tlen = tokenize(line, bas.tok_buf, TOK_BUF_LEN, &lnum);
                    if (tlen > 0 && lnum > 0) {
                        prog_insert(lnum, bas.tok_buf, tlen);
                        lines_loaded++;
                    }
                }
                li = 0;
            } else if (li < LINE_BUF_LEN - 1) {
                line[li++] = c;
            }
        }
        heap_free(buf);
        uart_puts("LOADED ");
        uart_put_dec((uint32_t)lines_loaded);
        uart_puts(" LINES\n");
    } else {
        /* Binary tokenized format (from SAVE) */
        if ((uint32_t)got > PROG_SIZE) { heap_free(buf); bas_error("FILE TOO BIG"); return; }
        for (int i = 0; i < got; i++)
            bas.prog[i] = buf[i];
        bas.prog_used = (uint16_t)got;
        heap_free(buf);
        uart_puts("LOADED ");
        uart_put_dec((uint32_t)got);
        uart_puts(" BYTES\n");
    }
}

/* ====== REPL ====== */

static void bas_readline(void)
{
    int pos = 0;
    for (;;) {
        int c = uart_getc();
        if (c < 0) { task_yield(); continue; }
        if (c == 0x03) {  /* Ctrl+C */
            bas.quit = 1;
            bas.line_buf[0] = '\0';
            uart_puts("\n");
            return;
        }
        if (c == '\r' || c == '\n') {
            uart_puts("\n");
            bas.line_buf[pos] = '\0';
            return;
        }
        if ((c == '\b' || c == 127) && pos > 0) {
            pos--;
            uart_puts("\b \b");
            continue;
        }
        if (pos < LINE_BUF_LEN - 1) {
            bas.line_buf[pos++] = (char)c;
            uart_putc((char)c);
        }
    }
}

/* ====== Entry point ====== */

void basic_enter(void)
{
    /* Allocate program buffer */
    bas.prog = (uint8_t *)heap_alloc(PROG_SIZE);
    if (!bas.prog) {
        uart_puts("BASIC: no memory for program buffer\n");
        return;
    }

    /* Init state */
    bas.prog_used = 0;
    bas.gosub_sp = 0;
    bas.for_sp = 0;
    bas.running = 0;
    bas.quit = 0;
    bas.rnd_state = get_tick_count();
    for (int i = 0; i < 26; i++) bas.vars[i] = 0;

    uart_puts("OsitoK BASIC v0.1\n");
    uart_puts("> ");

    while (!bas.quit) {
        bas_readline();
        if (bas.quit) break;

        /* Tokenize */
        uint16_t line_num;
        int tok_len = tokenize(bas.line_buf, bas.tok_buf, TOK_BUF_LEN, &line_num);
        if (tok_len < 0) {
            uart_puts("> ");
            continue;
        }

        if (line_num > 0) {
            /* Line with number -> store in program */
            /* Check if it's just a line number with nothing else (delete line) */
            if (tok_len == 1 && bas.tok_buf[0] == TOK_EOL) {
                prog_delete(line_num);
            } else {
                prog_insert(line_num, bas.tok_buf, tok_len);
            }
        } else {
            /* Immediate mode */
            bas.tp = bas.tok_buf;
            bas.tp_end = bas.tok_buf + tok_len;
            bas.running = 1;

            uint8_t t = tok_peek();
            switch (t) {
            case TOK_NEW:  bas.tp++; cmd_new();  break;
            case TOK_LIST: bas.tp++; cmd_list(); break;
            case TOK_RUN:  bas.tp++; cmd_run();  break;
            case TOK_BYE:  bas.quit = 1;         break;
            case TOK_SAVE: bas.tp++; cmd_save(); break;
            case TOK_LOAD: bas.tp++; cmd_load(); break;
            case TOK_EOL:  break;
            default:
                exec_statement();
                break;
            }
            bas.running = 0;
        }

        if (!bas.quit)
            uart_puts("> ");
    }

    uart_puts("BYE\n");
    heap_free(bas.prog);
    bas.prog = nullptr;
}

} /* extern "C" */
