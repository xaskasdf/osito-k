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

    /* Pre-dequantize F16 → F32 once at load. Costs ~2x memory on the
     * affected tensors but eliminates per-matvec scalar dequant in the
     * inner loop, which was ~13x slower than F32 in WASM (151 ms/tok
     * → ~25-40 ms/tok target after this conversion). */
    gguf_dequant_f16_to_f32(&g_model);

    /* Extract tokenizer from GGUF metadata + initialize global tokenizer.
     * Dispatch by tokenizer.ggml.model: "llama" (SPM, with scores) →
     * tok_init_spm; "gpt2" or unset (BPE w/ merges) → tok_init. */
    if (gguf_load_tokenizer(&g_model, &g_gguf_tok) == 0) {
        bool is_spm = (g_gguf_tok.tok_model[0] == 'l') &&
                      g_gguf_tok.scores != NULL;
        if (is_spm) {
            tok_init_spm(&g_tokenizer,
                         (const char **)g_gguf_tok.tokens, g_gguf_tok.token_lens,
                         g_gguf_tok.scores, g_gguf_tok.token_types,
                         g_gguf_tok.n_tokens,
                         g_gguf_tok.bos_id, g_gguf_tok.eos_id,
                         g_gguf_tok.unk_id, g_gguf_tok.pad_id);
            serial_puts("[WASM] Tokenizer ready (SPM)\n");
        } else {
            tok_init(&g_tokenizer,
                     (const char **)g_gguf_tok.tokens, g_gguf_tok.token_lens, g_gguf_tok.n_tokens,
                     (const char **)g_gguf_tok.merges,  g_gguf_tok.merge_lens, g_gguf_tok.n_merges,
                     g_gguf_tok.bos_id, g_gguf_tok.eos_id);
            serial_puts("[WASM] Tokenizer ready (BPE)\n");
        }
    }

    /* Initialize Llama inference state (max 256 tokens context) */
    if (llama_init(&g_llama, &g_model, 512) < 0) {
        serial_puts("[WASM] Llama init failed\n\n");
        return;
    }

    prompt_llama = &g_llama;
    serial_puts("[WASM] Model ready — try 'chat hello'\n\n");
}

/* ── OsitoFS image loading ───────────────────────────────────── */

/*
 * Fetch OsitoFS .img from R2 and mount it.
 * The image contains game data (PAK files etc.) accessible via `ls` and `exec`.
 */

#define FS_IMG_URL "https://factory.naranjositos.tech/images/q2_wasm.img"

EM_JS(int, js_fsimg_ready, (), { return window.__fsImageDone ? 1 : 0; });
EM_JS(uint32_t, js_fsimg_size, (), { return (window.__fsImageBuf ? window.__fsImageBuf.length : 0) >>> 0; });
EM_JS(void, js_fsimg_copy, (uint8_t *dst), { if (window.__fsImageBuf) HEAPU8.set(window.__fsImageBuf, dst >>> 0); });

extern int  osfs2_mount(uint64_t part_offset);
extern void wasm_nvme_set_buffer(void *buf, uint64_t size);

static void load_filesystem(void)
{
    serial_puts("[WASM] Waiting for filesystem image...\n");
    while (!js_fsimg_ready())
        emscripten_sleep(200);

    uint32_t sz = js_fsimg_size();
    if (sz < 4096) {
        serial_puts("[WASM] No filesystem image (standalone mode)\n\n");
        return;
    }

    void *buf = malloc(sz);
    if (!buf) {
        serial_puts("[WASM] Failed to allocate FS buffer\n\n");
        return;
    }

    js_fsimg_copy((uint8_t *)buf);
    serial_puts("[WASM] FS image: ");
    serial_putdec((uint64_t)sz / (1024 * 1024));
    serial_puts(" MB\n");

    /* Set as NVMe backend for OsitoFS */
    wasm_nvme_set_buffer(buf, (uint64_t)sz);

    /* Mount OsitoFS (partition offset 0 = raw image, no GPT) */
    if (osfs2_mount(0) < 0) {
        serial_puts("[WASM] OsitoFS mount failed\n\n");
    } else {
        serial_puts("[WASM] OsitoFS mounted\n\n");
    }
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
    load_filesystem();

    shell_run();
    return 0;
}
