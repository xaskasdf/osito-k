/*
 * ositok.h — convenience header for in-browser C/C++ programs compiled
 * inside OsitoK-wasm via clang.wasm. Available as `#include <ositok.h>`.
 *
 * Layer 1: stdio convenience (oi_puts/oi_print/oi_log) routed via WASI
 *          fd_write to the OsitoK terminal.
 * Layer 2: kernel-bridge imports (osito_env module): oi_random_u32,
 *          oi_now_us, oi_log_kernel — wired in shared.js's App class.
 */
#ifndef OSITOK_H
#define OSITOK_H

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define OSITOK_VERSION "0.2"

/* ── Layer 1: stdio convenience ─────────────────────────────── */

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

/* ── Layer 2: kernel-bridge imports (osito_env) ─────────────── */

#ifdef __cplusplus
extern "C" {
#endif

/* Uniform 32-bit random integer (worker uses Math.random under the hood). */
__attribute__((import_module("osito_env"), import_name("oi_random_u32")))
extern uint32_t oi_random_u32(void);

/* High-resolution monotonic clock in microseconds (performance.now * 1000). */
__attribute__((import_module("osito_env"), import_name("oi_now_us")))
extern uint64_t oi_now_us(void);

/* Log a length-bounded string to the terminal with a [oi] cyan prefix. */
__attribute__((import_module("osito_env"), import_name("oi_log_kernel")))
extern void oi_log_kernel_raw(const char *ptr, uint32_t len);

static inline void oi_log_kernel(const char *s) {
    /* strlen on a const string is safe; bound to 4 KB to be conservative. */
    size_t n = 0;
    while (n < 4096 && s[n]) n++;
    oi_log_kernel_raw(s, (uint32_t)n);
}

/* Synchronous chat with the OsitoK in-kernel LLM (SmolLM2-135M etc.).
 * Worker blocks via Atomics.wait while the kernel main thread runs
 * inference (1-5s typical). Returns bytes written to `out` or -1.
 * Requires cross-origin isolation (SharedArrayBuffer) — serve the
 * page with COOP=same-origin + COEP=credentialless (see arch/wasm/serve.py). */
__attribute__((import_module("osito_env"), import_name("oi_chat")))
extern int32_t oi_chat_raw(const char *prompt, uint32_t prompt_len,
                           char *out, uint32_t max_out);

static inline int32_t oi_chat(const char *prompt, char *out, uint32_t max_out) {
    size_t n = 0;
    while (n < 4096 && prompt[n]) n++;
    int32_t r = oi_chat_raw(prompt, (uint32_t)n, out, max_out);
    if (r >= 0 && (uint32_t)r < max_out) out[r] = 0;
    return r;
}

#ifdef __cplusplus
}
#endif

#endif /* OSITOK_H */
