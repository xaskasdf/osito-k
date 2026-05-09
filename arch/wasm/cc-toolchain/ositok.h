/*
 * ositok.h — convenience header for in-browser C/C++ programs compiled
 * inside OsitoK-wasm via clang.wasm. Available as `#include <ositok.h>`.
 *
 * Routes stdio to the OsitoK terminal via the WASI fd_write bridge
 * (already wired in the App class from binji/wasm-clang). Future
 * versions will expose kernel APIs (oi_chat for LLM, oi_dlopen, etc.)
 * via custom WASI imports.
 */
#ifndef OSITOK_H
#define OSITOK_H

#include <stdio.h>
#include <string.h>

#define OSITOK_VERSION "0.1"

static inline int oi_puts(const char *s) {
    int r = fputs(s, stdout);
    fputc('\n', stdout);
    return r;
}

static inline int oi_print(const char *s) {
    return fputs(s, stdout);
}

static inline void oi_log(const char *s) {
    fputs(s, stderr);
    fputc('\n', stderr);
}

#endif /* OSITOK_H */
