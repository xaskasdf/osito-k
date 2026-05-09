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

/* Tokenizer format. Picked at init time, drives encode/decode strategy.
 *   TOK_FMT_BPE_GPT2 — Llama 3 byte-level BPE with U+0100..U+0143
 *                      problem-byte mapping. Requires merges.
 *   TOK_FMT_SPM      — SentencePiece (with `▁` U+2581 word marker).
 *                      Uses scores+types for type classification; merges
 *                      are absent. Decode is a literal piece concat
 *                      with `▁` → ' ' substitution.
 */
typedef enum {
    TOK_FMT_BPE_GPT2 = 0,
    TOK_FMT_SPM      = 1,
} tok_format_t;

/* SPM token type codes (match SPM proto enum + GGUF tokenizer.ggml.token_type) */
#define TOK_TYPE_NORMAL        1
#define TOK_TYPE_UNKNOWN       2
#define TOK_TYPE_CONTROL       3
#define TOK_TYPE_USER_DEFINED  4
#define TOK_TYPE_UNUSED        5
#define TOK_TYPE_BYTE          6

/* ── Tokenizer state ────────────────────────────────────────── */

typedef struct {
    /* Format selector — set by tok_init / tok_init_spm */
    tok_format_t format;

    /* Vocabulary: token_id → bytes */
    tok_entry_t *vocab;         /* Array [vocab_size] */
    uint32_t     vocab_size;

    /* Merge rules (sorted by rank) — only populated for TOK_FMT_BPE_GPT2 */
    tok_merge_t *merges;
    uint32_t     merge_count;

    /* Hash table for fast byte-sequence → token_id lookup (BPE only) */
    uint32_t    *hash_table;    /* Hash → vocab index, 0 = empty */
    uint32_t     hash_size;     /* Power of 2 */

    /* SPM aux arrays (NULL for BPE). Both [vocab_size] when present. */
    float    *scores;
    uint32_t *token_types;

    /* Special token IDs (UINT32_MAX = unset) */
    uint32_t bos_id;
    uint32_t eos_id;
    uint32_t pad_id;
    uint32_t unk_id;

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
 * tok_init_spm — Initialize tokenizer with SentencePiece vocab.
 *
 * scores:      Per-token SPM scores (negative log-probs). Length n_tokens.
 * token_types: Per-token type code (TOK_TYPE_*). Length n_tokens.
 *
 * Merges are not used for SPM. The SPM consumer in this kernel is
 * decode-only (encoding is done host-side; agents receive token IDs
 * over the nvme queue). Encoding is therefore not implemented for
 * SPM and tok_encode will return -1 for SPM-format tokenizers.
 *
 * Returns 0 on success, -1 on error.
 */
int tok_init_spm(tokenizer_t *tok,
                 const char **tokens, const uint32_t *token_lens,
                 const float *scores, const uint32_t *token_types,
                 uint32_t n_tokens,
                 uint32_t bos_id, uint32_t eos_id,
                 uint32_t unk_id, uint32_t pad_id);

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
