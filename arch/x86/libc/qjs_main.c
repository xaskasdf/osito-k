/*
 * qjs_main.c — Minimal QuickJS REPL for OsitoK bare-metal
 *
 * Uses JS_NewRuntime2 with custom allocator, JS_NewContextRaw with
 * selective intrinsic loading (skip Date, skip Atomics).
 * Simple read-eval-print loop via stdin/stdout.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"

/* ── Custom allocator (wraps our malloc/free) ── */

static void *qjs_malloc(JSMallocState *s, size_t size)
{
    void *p;
    if (size == 0) return NULL;
    if (s->malloc_size + size > s->malloc_limit)
        return NULL;
    p = malloc(size);
    if (p) {
        s->malloc_count++;
        s->malloc_size += size;
    }
    return p;
}

static void qjs_free(JSMallocState *s, void *ptr)
{
    if (!ptr) return;
    s->malloc_count--;
    /* We can't track exact size freed with our bump allocator,
     * but QuickJS internally tracks sizes for us */
    free(ptr);
}

static void *qjs_realloc(JSMallocState *s, void *ptr, size_t size)
{
    if (!ptr) return qjs_malloc(s, size);
    if (size == 0) {
        qjs_free(s, ptr);
        return NULL;
    }
    if (s->malloc_size + size > s->malloc_limit)
        return NULL;
    void *p = realloc(ptr, size);
    if (p) {
        /* Size tracking is approximate — ok for bare-metal */
        s->malloc_size += size;
    }
    return p;
}

static size_t qjs_malloc_usable_size(const void *ptr)
{
    (void)ptr;
    return 0;  /* Unknown — QuickJS handles this gracefully */
}

static const JSMallocFunctions qjs_mf = {
    .js_malloc = qjs_malloc,
    .js_free = qjs_free,
    .js_realloc = qjs_realloc,
    .js_malloc_usable_size = qjs_malloc_usable_size,
};

/* ── Console print function ── */

static JSValue js_print(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    (void)this_val;
    for (int i = 0; i < argc; i++) {
        if (i != 0) putchar(' ');
        const char *str = JS_ToCString(ctx, argv[i]);
        if (str) {
            fputs(str, stdout);
            JS_FreeCString(ctx, str);
        }
    }
    putchar('\n');
    fflush(stdout);
    return JS_UNDEFINED;
}

/* ── Line input from stdin ── */

static int read_line(char *buf, int size)
{
    int i = 0;
    while (i < size - 1) {
        int c = fgetc(stdin);
        if (c == EOF) return (i > 0) ? i : -1;
        if (c == '\n') break;
        buf[i++] = (char)c;
    }
    buf[i] = '\0';
    return i;
}

/* ── Print JS exception ── */

static void dump_exception(JSContext *ctx)
{
    JSValue exc = JS_GetException(ctx);
    const char *str = JS_ToCString(ctx, exc);
    if (str) {
        fprintf(stderr, "Error: %s\n", str);
        JS_FreeCString(ctx, str);
    }

    /* Print stack trace if available */
    if (JS_IsObject(exc)) {
        JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
        if (!JS_IsUndefined(stack)) {
            const char *stack_str = JS_ToCString(ctx, stack);
            if (stack_str) {
                fprintf(stderr, "%s\n", stack_str);
                JS_FreeCString(ctx, stack_str);
            }
        }
        JS_FreeValue(ctx, stack);
    }
    JS_FreeValue(ctx, exc);
}

/* ── Main ── */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* Create runtime with custom allocator */
    JSRuntime *rt = JS_NewRuntime2(&qjs_mf, NULL);
    if (!rt) {
        fprintf(stderr, "qjs: cannot create runtime\n");
        return 1;
    }

    /* 8 MB memory limit, 256 KB stack */
    JS_SetMemoryLimit(rt, 8 * 1024 * 1024);
    JS_SetMaxStackSize(rt, 256 * 1024);

    /* Create context with selected intrinsics (skip Date for now) */
    JSContext *ctx = JS_NewContextRaw(rt);
    if (!ctx) {
        fprintf(stderr, "qjs: cannot create context\n");
        JS_FreeRuntime(rt);
        return 1;
    }

    /* Add standard intrinsics */
    JS_AddIntrinsicBaseObjects(ctx);
    JS_AddIntrinsicDate(ctx);         /* Date works (UTC stub) */
    JS_AddIntrinsicEval(ctx);
    JS_AddIntrinsicStringNormalize(ctx);
    JS_AddIntrinsicRegExp(ctx);
    JS_AddIntrinsicJSON(ctx);
    JS_AddIntrinsicProxy(ctx);
    JS_AddIntrinsicMapSet(ctx);
    JS_AddIntrinsicTypedArrays(ctx);
    JS_AddIntrinsicPromise(ctx);
    JS_AddIntrinsicBigInt(ctx);
    JS_AddIntrinsicBigFloat(ctx);
    JS_AddIntrinsicBigDecimal(ctx);

    /* Add global console.log = print */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log",
        JS_NewCFunction(ctx, js_print, "log", 1));
    JS_SetPropertyStr(ctx, global, "console", console);

    /* Also add print as a global */
    JS_SetPropertyStr(ctx, global, "print",
        JS_NewCFunction(ctx, js_print, "print", 1));
    JS_FreeValue(ctx, global);

    /* If argv[1] given, evaluate it as a file */
    if (argc >= 2) {
        FILE *f = fopen(argv[1], "r");
        if (!f) {
            fprintf(stderr, "qjs: cannot open '%s'\n", argv[1]);
        } else {
            /* Read entire file */
            fseek(f, 0, SEEK_END);
            long len = ftell(f);
            fseek(f, 0, SEEK_SET);
            char *src = malloc(len + 1);
            if (src) {
                fread(src, 1, len, f);
                src[len] = '\0';
                JSValue val = JS_Eval(ctx, src, len, argv[1],
                                      JS_EVAL_TYPE_GLOBAL);
                if (JS_IsException(val))
                    dump_exception(ctx);
                JS_FreeValue(ctx, val);
                free(src);
            }
            fclose(f);
        }
        goto cleanup;
    }

    /* REPL */
    printf("QuickJS 2024-01-13 on OsitoK (bare-metal x86-64)\n");
    printf("Type .exit to quit\n");

    char line[1024];
    for (;;) {
        printf("js> ");
        fflush(stdout);
        int n = read_line(line, sizeof(line));
        if (n < 0) break;  /* EOF */
        if (n == 0) continue;

        /* Exit command */
        if (strcmp(line, ".exit") == 0 || strcmp(line, "exit") == 0)
            break;

        /* Evaluate */
        JSValue val = JS_Eval(ctx, line, n, "<input>",
                              JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(val)) {
            dump_exception(ctx);
        } else if (!JS_IsUndefined(val)) {
            const char *str = JS_ToCString(ctx, val);
            if (str) {
                printf("%s\n", str);
                JS_FreeCString(ctx, str);
            }
        }
        JS_FreeValue(ctx, val);
    }

cleanup:
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
