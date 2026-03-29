/*
 * OsitoK WASM — Kernel Entry Point
 *
 * Replaces arch/x86/boot/boot_efi.c + arch/x86/kernel/main.c.
 * Init sequence: serial → fb → heap → (fetch GGUF) → shell.
 */

#include "../include/types.h"
#include "../../x86/fs/gguf.h"
#include "../../x86/kernel/inference.h"
#include "../../x86/kernel/tokenizer.h"
#include <emscripten.h>
#include <stdlib.h>
#include <string.h>

/* ── External declarations ───────────────────────────────────── */

extern void serial_init(void);
extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t v);
extern void fb_init(uint32_t *base, uint32_t w, uint32_t h, uint32_t pitch);
extern void fb_clear(void);
extern void heap_init(void);
extern void kb_init(void);
extern void term_init(void);
extern void shell_run(void);
extern void sys_caps_init(void);

extern int  gguf_load_from_mem(gguf_model_t *model, void *data, uint64_t size);
extern int  gguf_load_tokenizer(gguf_model_t *model, gguf_tokenizer_t *tok);
extern void *prompt_llama;

/* Tokenizer global (tokenizer.c) */
extern tokenizer_t g_tokenizer;

/* ── Model / inference state ─────────────────────────────────── */

static gguf_model_t     g_model;
static llama_state_t    g_llama;
static gguf_tokenizer_t g_gguf_tok;

/* ── Model pre-fetch bridge (no EM_ASYNC_JS needed) ─────────── */

/*
 * The HTML page starts fetching the model immediately via regular JS.
 * These three EM_JS stubs poll window.__modelFetchDone / __modelBuf,
 * so C can wait with emscripten_sleep (already in ASYNCIFY_IMPORTS).
 */

EM_JS(int, js_model_ready, (), {
    return window.__modelFetchDone ? 1 : 0;
});

EM_JS(uint32_t, js_model_size, (), {
    return (window.__modelBuf ? window.__modelBuf.length : 0) >>> 0;
});

EM_JS(void, js_model_copy, (uint8_t *dst), {
    if (window.__modelBuf) HEAPU8.set(window.__modelBuf, dst >>> 0);
});

/*
 * wasm_wait_for_model — block (via Asyncify) until JS fetch completes,
 * copy model data into a malloc'd WASM buffer, return it.
 */
static void *wasm_wait_for_model(uint32_t *out_size)
{
    serial_puts("[WASM] Waiting for model download...\n");
    while (!js_model_ready())
        emscripten_sleep(200);

    uint32_t sz = js_model_size();
    if (sz < 1024) {
        *out_size = 0;
        return NULL;
    }

    void *buf = malloc(sz);
    if (!buf) {
        serial_puts("[WASM] malloc failed for model buffer\n");
        *out_size = 0;
        return NULL;
    }

    js_model_copy((uint8_t *)buf);
    *out_size = sz;
    return buf;
}

/* ── Model loading ───────────────────────────────────────────── */

/*
 * URL of the GGUF model file.
 * Change to the actual hosted model URL before deploying.
 * Recommended: a Q4_0 model ≤ 50MB for comfortable browser load times.
 * Example: SmolLM2-135M-Q4_0 (~88MB), brandon-tiny-Q4_0 (~6MB).
 */
#define MODEL_URL "https://factory.naranjositos.tech/models/smollm2-135m-instruct-q4_0.gguf"

static void load_model(void)
{
    uint32_t size = 0;
    void *data = wasm_wait_for_model(&size);

    if (!data || size < 1024) {
        serial_puts("[WASM] Model unavailable — 'chat' disabled\n\n");
        if (data) free(data);
        return;
    }

    serial_puts("[WASM] Downloaded ");
    serial_putdec((uint64_t)size / (1024 * 1024));
    serial_puts(" MB, parsing GGUF...\n");

    if (gguf_load_from_mem(&g_model, data, (uint64_t)size) < 0) {
        serial_puts("[WASM] GGUF parse failed\n\n");
        free(data);
        return;
    }

    /* Extract tokenizer from GGUF metadata + initialize global tokenizer */
    if (gguf_load_tokenizer(&g_model, &g_gguf_tok) == 0) {
        tok_init(&g_tokenizer,
                 (const char **)g_gguf_tok.tokens, g_gguf_tok.token_lens, g_gguf_tok.n_tokens,
                 (const char **)g_gguf_tok.merges,  g_gguf_tok.merge_lens, g_gguf_tok.n_merges,
                 g_gguf_tok.bos_id, g_gguf_tok.eos_id);
        serial_puts("[WASM] Tokenizer ready\n");
    }

    /* Initialize Llama inference state (max 256 tokens context) */
    if (llama_init(&g_llama, &g_model, 256) < 0) {
        serial_puts("[WASM] Llama init failed\n\n");
        return;
    }

    prompt_llama = &g_llama;
    serial_puts("[WASM] Model ready — try 'chat hello'\n\n");
}

/* ── Entry point ─────────────────────────────────────────────── */

int main(void)
{
    serial_init();
    fb_init(NULL, 800, 600, 800);
    fb_clear();
    heap_init();
    sys_caps_init();
    kb_init();
    term_init();

    load_model();

    shell_run();
    return 0;
}
