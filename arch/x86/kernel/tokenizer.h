/*
 * OsitoK x86-64 — BPE Tokenizer
 *
 * Byte-pair encoding tokenizer for Llama 3 models.
 * Vocabulary loaded from GGUF metadata (tokenizer.ggml.tokens + merges).
 *
 * Algorithm: OpenAI tiktoken-style BPE.
 * Ported from xasko's C++ tiktoken implementation (reason_agent).
 */

#ifndef OSITOK_TOKENIZER_H
#define OSITOK_TOKENIZER_H

#include "../include/types.h"

/* ── Limits ─────────────────────────────────────────────────── */

#define TOK_MAX_VOCAB       200000  /* Max tokens (Llama 3 = 128256) */
#define TOK_MAX_TOKEN_LEN   128     /* Max bytes per token */
#define TOK_MAX_MERGES      200000  /* Max merge rules */
#define TOK_MAX_ENCODE_LEN  8192    /* Max input text bytes */

/* ── Token entry ────────────────────────────────────────────── */

typedef struct {
    uint8_t  bytes[TOK_MAX_TOKEN_LEN];  /* Token bytes (UTF-8) */
    uint16_t len;                        /* Length in bytes */
    uint32_t rank;                       /* BPE rank (= token ID) */
} tok_entry_t;

/* ── Merge rule ─────────────────────────────────────────────── */

typedef struct {
    uint32_t a;     /* First token ID */
    uint32_t b;     /* Second token ID */
    uint32_t rank;  /* Merge priority (lower = merge first) */
} tok_merge_t;

/* ── Tokenizer state ────────────────────────────────────────── */

typedef struct {
    /* Vocabulary: token_id → bytes */
    tok_entry_t *vocab;         /* Array [vocab_size] */
    uint32_t     vocab_size;

    /* Merge rules (sorted by rank) */
    tok_merge_t *merges;
    uint32_t     merge_count;

    /* Hash table for fast byte-sequence → token_id lookup */
    uint32_t    *hash_table;    /* Hash → vocab index, 0 = empty */
    uint32_t     hash_size;     /* Power of 2 */

    /* Special token IDs */
    uint32_t bos_id;
    uint32_t eos_id;
    uint32_t pad_id;

    bool ready;
} tokenizer_t;

/* ── API ────────────────────────────────────────────────────── */

/*
 * tok_init — Initialize tokenizer from GGUF metadata.
 *
 * tokens:     Array of token strings (from tokenizer.ggml.tokens)
 * token_lens: Array of string lengths
 * n_tokens:   Number of tokens (vocab size)
 * merges:     Array of merge rule strings "token_a token_b" (from tokenizer.ggml.merges)
 * merge_lens: Array of merge string lengths
 * n_merges:   Number of merge rules
 * bos_id:     Beginning-of-sequence token ID
 * eos_id:     End-of-sequence token ID
 *
 * Returns 0 on success, -1 on error.
 */
int tok_init(tokenizer_t *tok,
             const char **tokens, const uint32_t *token_lens, uint32_t n_tokens,
             const char **merges, const uint32_t *merge_lens, uint32_t n_merges,
             uint32_t bos_id, uint32_t eos_id);

/*
 * tok_encode — Encode text to token IDs.
 *
 * text:     Input text (UTF-8, null-terminated)
 * text_len: Input text length in bytes
 * out:      Output token ID array
 * max_out:  Maximum number of tokens to produce
 *
 * Returns number of tokens produced, or -1 on error.
 */
int tok_encode(const tokenizer_t *tok, const char *text, uint32_t text_len,
               uint32_t *out, uint32_t max_out);

/*
 * tok_decode — Decode token IDs to text.
 *
 * tokens:   Input token ID array
 * n_tokens: Number of token IDs
 * out:      Output text buffer
 * max_out:  Output buffer size
 *
 * Returns bytes written (not null-terminated), or -1 on error.
 */
int tok_decode(const tokenizer_t *tok, const uint32_t *tokens, uint32_t n_tokens,
               char *out, uint32_t max_out);

/*
 * tok_decode_one — Decode a single token to text.
 *
 * Returns pointer to static buffer (valid until next call), or NULL.
 */
const char *tok_decode_one(const tokenizer_t *tok, uint32_t token_id);

/*
 * tok_free — Free tokenizer resources.
 */
void tok_free(tokenizer_t *tok);

#endif /* OSITOK_TOKENIZER_H */
