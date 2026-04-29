/*
 * OsitoK - zForth host layer
 *
 * Provides: zf_host_sys (I/O + hardware syscalls), zf_host_parse_num,
 * zf_host_trace. Also: forth_enter() REPL and forth_run() file executor.
 * Embedded core.zf bootstrap defines if/else/fi, do/loop, etc.
 */

#include "forth/zforth.h"
#include "drivers/uart.h"
#include "drivers/video.h"
#include "kernel/task.h"
#include "mem/heap.h"
#include "fs/ositofs.h"
#include "gfx/wire3d.h"
#include "gfx/ships.h"

extern "C" {

/* ====== Forth context (persistent across invocations) ====== */

static zf_ctx forth_ctx;
static int forth_inited = 0;

/* ====== Embedded core.zf bootstrap ====== */

static const char core_zf[] =
    /* I/O syscalls */
    ": emit    0 sys ; "
    ": .       1 sys ; "
    ": tell    2 sys ; "
    /* OsitoK hardware syscalls */
    ": fb-clear  128 sys ; "
    ": fb-pixel  129 sys ; "
    ": fb-line   130 sys ; "
    ": fb-flush  131 sys ; "
    ": fb-text   132 sys ; "
    ": yield     133 sys ; "
    ": ticks     134 sys ; "
    ": delay     135 sys ; "
    ": wire-render 136 sys ; "
    ": wire-models 137 sys ; "
    /* Dictionary access shortcuts */
    ": !    0 !! ; "
    ": @    0 @@ ; "
    ": ,    0 ,, ; "
    ": #    0 ## ; "
    ": !j   64 !! ; "
    ": ,j   64 ,, ; "
    /* Compiler state */
    ": [ 0 compiling ! ; immediate "
    ": ] 1 compiling ! ; "
    ": postpone 1 _postpone ! ; immediate "
    /* Operators */
    ": 1+ 1 + ; "
    ": 1- 1 - ; "
    ": over 1 pick ; "
    ": +!   dup @ rot + swap ! ; "
    ": inc  1 swap +! ; "
    ": dec  -1 swap +! ; "
    ": <    - <0 ; "
    ": >    swap < ; "
    ": <=   over over >r >r < r> r> = + ; "
    ": >=   swap <= ; "
    ": =0   0 = ; "
    ": not  =0 ; "
    ": !=   = not ; "
    ": cr   10 emit ; "
    ": br   32 emit ; "
    ": ..   dup . ; "
    ": here h @ ; "
    /* Memory */
    ": allot  h +! ; "
    ": const : ' lit , , postpone ; ; "
    ": constant >r : r> postpone literal postpone ; ; "
    ": variable >r here r> postpone , constant ; "
    /* Control flow */
    ": begin   here ; immediate "
    ": again   ' jmp , , ; immediate "
    ": until   ' jmp0 , , ; immediate "
    ": if      ' jmp0 , here 0 ,j ; immediate "
    ": unless  ' not , postpone if ; immediate "
    ": else    ' jmp , here 0 ,j swap here swap !j ; immediate "
    ": fi      here swap !j ; immediate "
    /* Loops */
    ": i ' lit , 0 , ' pickr , ; immediate "
    ": j ' lit , 2 , ' pickr , ; immediate "
    ": do ' swap , ' >r , ' >r , here ; immediate "
    ": loop+ ' r> , ' + , ' dup , ' >r , ' lit , 1 , ' pickr , ' >= , ' jmp0 , , ' r> , ' drop , ' r> , ' drop , ; immediate "
    ": loop ' lit , 1 , postpone loop+ ; immediate "
    /* String literals */
    ": s\" compiling @ if ' lits , here 0 , fi here begin key dup 34 = if drop "
    "compiling @ if here swap - swap ! else dup here swap - fi exit else , fi "
    "again ; immediate "
    ": .\" compiling @ if postpone s\" ' tell , else begin key dup 34 = if drop exit else emit fi again "
    "fi ; immediate "
;

/* ====== Lazy init ====== */

static void forth_ensure_init(void)
{
    if (forth_inited) return;
    zf_init(&forth_ctx, 0);
    zf_bootstrap(&forth_ctx);
    zf_result r = zf_eval(&forth_ctx, core_zf);
    if (r != ZF_OK) {
        uart_puts("zf: core.zf bootstrap error ");
        uart_put_dec(r);
        uart_puts("\n");
    }
    forth_inited = 1;
}

/* ====== Error names ====== */

static const char *zf_error_name(zf_result r)
{
    switch (r) {
    case ZF_OK:                      return "ok";
    case ZF_ABORT_INTERNAL_ERROR:    return "internal-error";
    case ZF_ABORT_OUTSIDE_MEM:       return "outside-mem";
    case ZF_ABORT_DSTACK_UNDERRUN:   return "dstack-underrun";
    case ZF_ABORT_DSTACK_OVERRUN:    return "dstack-overrun";
    case ZF_ABORT_RSTACK_UNDERRUN:   return "rstack-underrun";
    case ZF_ABORT_RSTACK_OVERRUN:    return "rstack-overrun";
    case ZF_ABORT_NOT_A_WORD:        return "not-a-word";
    case ZF_ABORT_COMPILE_ONLY_WORD: return "compile-only";
    case ZF_ABORT_INVALID_SIZE:      return "invalid-size";
    case ZF_ABORT_DIVISION_BY_ZERO:  return "division-by-zero";
    case ZF_ABORT_INVALID_USERVAR:   return "invalid-uservar";
    case ZF_ABORT_EXTERNAL:          return "external-abort";
    default:                         return "unknown";
    }
}

/* ====== Host callbacks ====== */

zf_input_state zf_host_sys(zf_ctx *ctx, zf_syscall_id id, const char *last_word)
{
    (void)last_word;

    switch ((int)id) {

    case ZF_SYSCALL_EMIT: {
        char c = (char)zf_pop(ctx);
        uart_putc(c);
        break;
    }

    case ZF_SYSCALL_PRINT: {
        zf_cell v = zf_pop(ctx);
        /* Simple decimal print */
        if (v < 0) { uart_putc('-'); v = -v; }
        uart_put_dec((uint32_t)v);
        uart_putc(' ');
        break;
    }

    case ZF_SYSCALL_TELL: {
        zf_cell len = zf_pop(ctx);
        zf_addr addr = zf_pop(ctx);
        /* Print string from dictionary */
        for (zf_cell i = 0; i < len; i++) {
            uint8_t c;
            zf_addr a = addr + i;
            /* Read byte from dictionary */
            if (a < ZF_DICT_SIZE)
                c = ctx->dict[a];
            else
                c = '?';
            uart_putc((char)c);
        }
        break;
    }

    /* OsitoK hardware syscalls */
    case ZF_SYSCALL_USER + 0:  /* fb-clear */
        fb_clear();
        break;

    case ZF_SYSCALL_USER + 1: { /* fb-pixel ( x y -- ) */
        int y = (int)zf_pop(ctx);
        int x = (int)zf_pop(ctx);
        fb_set_pixel(x, y);
        break;
    }

    case ZF_SYSCALL_USER + 2: { /* fb-line ( x0 y0 x1 y1 -- ) */
        int y1 = (int)zf_pop(ctx);
        int x1 = (int)zf_pop(ctx);
        int y0 = (int)zf_pop(ctx);
        int x0 = (int)zf_pop(ctx);
        fb_line(x0, y0, x1, y1);
        break;
    }

    case ZF_SYSCALL_USER + 3:  /* fb-flush */
        fb_flush();
        break;

    case ZF_SYSCALL_USER + 4: { /* fb-text ( col row addr len -- ) */
        zf_cell len = zf_pop(ctx);
        zf_addr addr = zf_pop(ctx);
        int row = (int)zf_pop(ctx);
        int col = (int)zf_pop(ctx);
        /* Build temp string from dict */
        char tmp[33];
        int n = (len > 32) ? 32 : (int)len;
        for (int i = 0; i < n; i++) {
            if (addr + i < ZF_DICT_SIZE)
                tmp[i] = (char)ctx->dict[addr + i];
            else
                tmp[i] = '?';
        }
        tmp[n] = '\0';
        fb_text_puts(col, row, tmp);
        break;
    }

    case ZF_SYSCALL_USER + 5:  /* yield */
        task_yield();
        break;

    case ZF_SYSCALL_USER + 6:  /* ticks ( -- n ) */
        zf_push(ctx, (zf_cell)get_tick_count());
        break;

    case ZF_SYSCALL_USER + 7: { /* delay ( ticks -- ) */
        uint32_t t = (uint32_t)zf_pop(ctx);
        task_delay_ticks(t);
        break;
    }

    case ZF_SYSCALL_USER + 8: { /* wire-render ( model rx ry rz -- ) */
        uint8_t rz = (uint8_t)zf_pop(ctx);
        uint8_t ry = (uint8_t)zf_pop(ctx);
        uint8_t rx = (uint8_t)zf_pop(ctx);
        int idx = (int)zf_pop(ctx);
        /* 0=cube, 1-4=ships */
        const wire_model_t *m = &wire_cube;
        if (idx >= 1 && idx <= SHIP_COUNT)
            m = ship_list[idx - 1];
        mat3_t rot;
        mat3_rotate_x(&rot, rx);
        mat3_t ry_m; mat3_rotate_y(&ry_m, ry);
        mat3_t tmp; mat3_multiply(&tmp, &ry_m, &rot);
        mat3_t rz_m; mat3_rotate_z(&rz_m, rz);
        mat3_multiply(&rot, &rz_m, &tmp);
        vec3_t pos = vec3(0, 0, FIX16(6));
        wire_render(m, &rot, pos, FIX16(64));
        break;
    }

    case ZF_SYSCALL_USER + 9: { /* wire-models ( -- n ) */
        zf_push(ctx, SHIP_COUNT + 1); /* cube + ships */
        break;
    }

    default:
        /* Unknown syscall — ignore */
        break;
    }

    return ZF_INPUT_INTERPRET;
}


void zf_host_trace(zf_ctx *ctx, const char *fmt, va_list va)
{
    (void)ctx;
    (void)fmt;
    (void)va;
    /* Trace disabled — no-op */
}


zf_cell zf_host_parse_num(zf_ctx *ctx, const char *buf)
{
    int neg = 0;
    zf_cell v = 0;
    const char *p = buf;

    /* Handle negative */
    if (*p == '-') { neg = 1; p++; }

    /* Hex: 0x... */
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        while (*p) {
            char c = *p++;
            if (c >= '0' && c <= '9')      v = (v << 4) | (c - '0');
            else if (c >= 'a' && c <= 'f')  v = (v << 4) | (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')  v = (v << 4) | (c - 'A' + 10);
            else { zf_abort(ctx, ZF_ABORT_NOT_A_WORD); }
        }
    } else {
        /* Decimal */
        if (*p < '0' || *p > '9') {
            zf_abort(ctx, ZF_ABORT_NOT_A_WORD);
        }
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (*p - '0');
            p++;
        }
        if (*p != '\0') {
            zf_abort(ctx, ZF_ABORT_NOT_A_WORD);
        }
    }

    return neg ? -v : v;
}


/* ====== Shell: forth REPL ====== */

void forth_enter(void)
{
    forth_ensure_init();

    uart_puts("zf: ready (Ctrl+C exit)\n");

    char line[128];
    int pos = 0;

    for (;;) {
        int c = uart_getc();
        if (c < 0) {
            task_yield();
            continue;
        }

        if (c == 0x03) {  /* Ctrl+C */
            uart_puts("\n");
            return;
        }

        if (c == '\r' || c == '\n') {
            uart_puts("\n");
            line[pos] = '\0';

            if (pos > 0) {
                zf_result r = zf_eval(&forth_ctx, line);
                if (r == ZF_OK) {
                    uart_puts(" ok\n");
                } else {
                    uart_puts(" error: ");
                    uart_puts(zf_error_name(r));
                    uart_puts("\n");
                }
            }

            pos = 0;
        } else if (c == '\b' || c == 127) {
            if (pos > 0) {
                pos--;
                uart_puts("\b \b");
            }
        } else if (pos < 127) {
            line[pos++] = (char)c;
            uart_putc((char)c);
        }
    }
}


/* ====== Shell: run <file.zf> ====== */

void forth_run(const char *filename)
{
    forth_ensure_init();

    int size = fs_stat(filename);
    if (size < 0) {
        uart_puts("not found: ");
        uart_puts(filename);
        uart_puts("\n");
        return;
    }
    if (size == 0) {
        uart_puts("empty file\n");
        return;
    }

    /* +1 for null terminator */
    uint8_t *buf = (uint8_t *)heap_alloc((uint32_t)size + 1);
    if (!buf) {
        uart_puts("no memory (need ");
        uart_put_dec((uint32_t)size + 1);
        uart_puts(" bytes)\n");
        return;
    }

    int got = fs_read(filename, buf, (uint32_t)size);
    if (got != size) {
        uart_puts("read error\n");
        heap_free(buf);
        return;
    }
    buf[size] = '\0';  /* null-terminate for zf_eval */

    zf_result r = zf_eval(&forth_ctx, (const char *)buf);
    if (r == ZF_OK) {
        uart_puts(" ok\n");
    } else {
        uart_puts(" error: ");
        uart_puts(zf_error_name(r));
        uart_puts("\n");
    }

    heap_free(buf);
}


} /* extern "C" */
