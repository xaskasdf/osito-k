/*
 * arch/x86/kernel/sys_inference.c — Inference as a kernel syscall
 *
 * Exposes the kernel's Llama forward pass to userspace processes via
 * SYS_INFERENCE (500). Any ELF can tokenize a prompt, call syscall 500
 * with the token array, and get response tokens back — no library, no
 * framework, just the kernel.
 *
 * Related syscalls:
 *   500 SYS_INFERENCE        — run forward+sample loop
 *   501 SYS_INFERENCE_RESET  — rewind KV cache position
 *   502 SYS_INFERENCE_STATE  — query model parameters
 *   503 SYS_INFERENCE_TOKENIZE   — BPE encode a UTF-8 string
 *   504 SYS_INFERENCE_DETOKENIZE — decode tokens to UTF-8
 */

#include "inference.h"
#include "tokenizer.h"
#include "../include/types.h"
#include "../include/stdint.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t v);

/* Access to the prompt-server's model state (main.c:prompt_llama). */
extern llama_state_t *prompt_llama;

/* Sampling + forward (inference.c) */
extern uint32_t sample_topp(const float *logits, uint32_t vocab_size,
                             float temperature, float top_p);
extern int llama_forward(llama_state_t *s, uint32_t token);

/* Global tokenizer instance (tokenizer.c) */
extern tokenizer_t g_tokenizer;

/* Scheduler yield from syscall context */
extern void sched_yield(void);

/* ── Simple spinlock for serialization ───────────────────────── */
static volatile uint32_t inf_lock;

static int try_lock(void)
{
    uint32_t expected = 0;
    uint32_t desired = 1;
    uint8_t ok;
    __asm__ volatile("lock cmpxchgl %3, %1; setz %0"
        : "=q"(ok), "+m"(inf_lock), "+a"(expected)
        : "r"(desired)
        : "memory");
    return ok ? 1 : 0;
}

static void unlock(void)
{
    __asm__ volatile("" ::: "memory");
    inf_lock = 0;
}

/* ── Userspace VA validation (best-effort) ───────────────────── */
static int ua_readable(const void *p, uint64_t n)
{
    /* Conservative: reject obviously bad pointers. Full page-walk
     * validation is in process.c:proc_validate_user_ptr(); if not
     * available we just bounds-check the canonical user range. */
    uint64_t addr = (uint64_t)p;
    if (addr == 0) return 0;
    if (addr + n < addr) return 0;            /* wrap */
    if (addr >= 0x0000800000000000ULL) return 0;  /* kernel half */
    return 1;
}

static int ua_writable(void *p, uint64_t n) { return ua_readable(p, n); }

/* ── Public syscall entry points ─────────────────────────────── */

/* SYS_INFERENCE (500)
 * Args: user prompt tokens, n_prompt, user output buffer, max_output,
 *       temperature * 1000 (integer, e.g. 700 = 0.7).
 * Returns: number of output tokens generated, or negative errno. */
int64_t sys_inference(uint32_t *prompt_toks, uint32_t n_prompt,
                      uint32_t *out_toks, uint32_t max_out,
                      uint32_t temp_x1000)
{
    if (!prompt_llama) return -19;            /* -ENODEV */
    if (!prompt_toks || !out_toks) return -14;/* -EFAULT */
    if (n_prompt == 0 || max_out == 0) return -22; /* -EINVAL */
    if (!ua_readable(prompt_toks, (uint64_t)n_prompt * 4)) return -14;
    if (!ua_writable(out_toks, (uint64_t)max_out * 4)) return -14;

    if (!try_lock()) return -16;              /* -EBUSY */

    float temp = (float)temp_x1000 / 1000.0f;
    if (temp < 0.01f) temp = 0.01f;
    if (temp > 2.0f)  temp = 2.0f;

    /* Prompt processing */
    for (uint32_t i = 0; i < n_prompt; i++)
        llama_forward(prompt_llama, prompt_toks[i]);

    /* Generation loop */
    uint32_t gen = 0;
    uint32_t next = sample_topp(prompt_llama->logits,
                                 prompt_llama->vocab_size, temp, 0.9f);

    while (gen < max_out) {
        /* Llama 3 EOS tokens */
        if (next == 128001u || next == 128009u) break;

        out_toks[gen++] = next;

        /* Yield every 8 tokens so the rest of the system keeps running. */
        if ((gen & 7) == 0) sched_yield();

        llama_forward(prompt_llama, next);
        next = sample_topp(prompt_llama->logits,
                           prompt_llama->vocab_size, temp, 0.9f);
    }

    unlock();
    return (int64_t)gen;
}

/* SYS_INFERENCE_RESET (501) — rewind KV cache so next call starts fresh. */
int64_t sys_inference_reset(void)
{
    if (!prompt_llama) return -19;
    if (!try_lock()) return -16;
    prompt_llama->pos = 0;
    unlock();
    return 0;
}

/* SYS_INFERENCE_STATE (502) — copy parameters to user buffer.
 * Layout matches `inference_state_t` in ositok.h (user-visible header). */
typedef struct {
    uint32_t loaded;
    uint32_t vocab_size;
    uint32_t ctx_len;
    uint32_t current_pos;
    uint64_t tokens_generated_total;
} inference_state_public_t;

static uint64_t total_tokens_generated;  /* cumulative across all callers */

int64_t sys_inference_state(void *user_buf, uint64_t buf_size)
{
    if (buf_size < sizeof(inference_state_public_t)) return -22;
    if (!ua_writable(user_buf, sizeof(inference_state_public_t))) return -14;

    inference_state_public_t st;
    st.loaded      = (prompt_llama != 0);
    st.vocab_size  = prompt_llama ? prompt_llama->vocab_size : 0;
    st.ctx_len     = prompt_llama ? prompt_llama->max_seq : 0;
    st.current_pos = prompt_llama ? prompt_llama->pos : 0;
    st.tokens_generated_total = total_tokens_generated;

    /* byte-copy into user buffer */
    uint8_t *dst = (uint8_t *)user_buf;
    const uint8_t *src = (const uint8_t *)&st;
    for (uint64_t i = 0; i < sizeof(st); i++) dst[i] = src[i];
    return (int64_t)sizeof(st);
}

/* SYS_INFERENCE_TOKENIZE (503) — BPE encode text into tokens.
 * Args: text (NUL-terminated), out_toks[], max_out. Returns count. */
int64_t sys_inference_tokenize(const char *text, uint32_t *out_toks,
                                uint32_t max_out)
{
    if (!text || !out_toks) return -14;
    /* Minimal bound: text must have a NUL within first 64KB */
    uint64_t tlen = 0;
    const char *p = text;
    while (tlen < 64 * 1024 && p[tlen]) tlen++;
    if (tlen == 64 * 1024) return -22;
    if (!ua_readable(text, tlen + 1)) return -14;
    if (!ua_writable(out_toks, (uint64_t)max_out * 4)) return -14;

    int n = tok_encode(&g_tokenizer, text, (uint32_t)tlen, out_toks, max_out);
    if (n < 0) return -5;  /* -EIO */
    return (int64_t)n;
}

/* SYS_INFERENCE_DETOKENIZE (504) — token array → UTF-8 bytes.
 * Args: tokens[], n, dst_buf, dst_size. Returns bytes written (not counting
 * trailing NUL, which is added if space permits). */
int64_t sys_inference_detokenize(const uint32_t *tokens, uint32_t n,
                                  char *dst, uint64_t dst_size)
{
    if (!tokens || !dst) return -14;
    if (!ua_readable(tokens, (uint64_t)n * 4)) return -14;
    if (!ua_writable(dst, dst_size)) return -14;

    /* tok_decode_one returns a pointer to a static buffer; copy bytes out. */
    uint64_t off = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (off >= dst_size) break;
        const char *s = tok_decode_one(&g_tokenizer, tokens[i]);
        if (!s) break;
        while (*s && off + 1 < dst_size) dst[off++] = *s++;
    }
    if (off < dst_size) dst[off] = 0;
    return (int64_t)off;
}
