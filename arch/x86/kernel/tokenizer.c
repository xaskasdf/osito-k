/*
 * OsitoK x86-64 — BPE Tokenizer
 *
 * Byte-pair encoding for Llama 3 models.
 * Algorithm ported from xasko's C++ tiktoken (reason_agent/tools/tiktoken.cpp).
 *
 * Two encoding paths:
 *   1. Direct lookup: if the byte sequence is a known token, return it.
 *   2. BPE merge: iteratively merge the lowest-rank pair until done.
 *
 * Vocabulary and merge rules extracted from GGUF metadata by gguf.c.
 */

#include "../include/types.h"
#include "tokenizer.h"

/* ── External functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);

/* Global tokenizer instance (initialized from GGUF metadata) */
tokenizer_t g_tokenizer;

/* Global decode helper for use from inference.c */
const char *tok_global_decode(uint32_t id)
{
    if (!g_tokenizer.ready) return NULL;
    return tok_decode_one(&g_tokenizer, id);
}

/* ── Internal helpers ────────────────────────────────────────── */

static uint32_t hash_bytes(const uint8_t *data, uint32_t len)
{
    uint32_t h = 0x811c9dc5;  /* FNV-1a */
    for (uint32_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 0x01000193;
    }
    return h;
}

/* Look up a byte sequence in the hash table → token ID.
 * Returns token ID, or (uint32_t)-1 if not found. */
static uint32_t tok_lookup(const tokenizer_t *tok,
                           const uint8_t *bytes, uint32_t len)
{
    uint32_t mask = tok->hash_size - 1;
    uint32_t h = hash_bytes(bytes, len) & mask;

    for (uint32_t probe = 0; probe < tok->hash_size; probe++) {
        uint32_t idx = tok->hash_table[(h + probe) & mask];
        if (idx == 0) return (uint32_t)-1;  /* empty slot */

        uint32_t vi = idx - 1;  /* 1-based in table */
        const tok_entry_t *e = &tok->vocab[vi];
        if (e->len == (uint16_t)len) {
            bool match = true;
            for (uint32_t j = 0; j < len; j++) {
                if (e->bytes[j] != bytes[j]) { match = false; break; }
            }
            if (match) return e->rank;
        }
    }
    return (uint32_t)-1;
}

/* Insert byte sequence into hash table */
static void tok_hash_insert(tokenizer_t *tok, uint32_t vocab_idx)
{
    const tok_entry_t *e = &tok->vocab[vocab_idx];
    uint32_t mask = tok->hash_size - 1;
    uint32_t h = hash_bytes(e->bytes, e->len) & mask;

    for (uint32_t probe = 0; probe < tok->hash_size; probe++) {
        uint32_t slot = (h + probe) & mask;
        if (tok->hash_table[slot] == 0) {
            tok->hash_table[slot] = vocab_idx + 1;  /* 1-based */
            return;
        }
    }
    /* Table full — should not happen with 2x size */
}

/* ── Initialization ──────────────────────────────────────────── */

int tok_init(tokenizer_t *tok,
             const char **tokens, const uint32_t *token_lens, uint32_t n_tokens,
             const char **merges, const uint32_t *merge_lens, uint32_t n_merges,
             uint32_t bos_id, uint32_t eos_id)
{
    memset(tok, 0, sizeof(*tok));

    if (n_tokens == 0 || n_tokens > TOK_MAX_VOCAB) {
        serial_puts("[TOK] Invalid vocab size\n");
        return -1;
    }

    /* Allocate vocab */
    tok->vocab = (tok_entry_t *)kmalloc((uint64_t)n_tokens * sizeof(tok_entry_t));
    if (!tok->vocab) {
        serial_puts("[TOK] Failed to alloc vocab\n");
        return -1;
    }
    memset(tok->vocab, 0, (size_t)n_tokens * sizeof(tok_entry_t));

    /* Fill vocab */
    tok->vocab_size = n_tokens;
    for (uint32_t i = 0; i < n_tokens; i++) {
        uint32_t len = token_lens[i];
        if (len > TOK_MAX_TOKEN_LEN) len = TOK_MAX_TOKEN_LEN;
        memcpy(tok->vocab[i].bytes, tokens[i], len);
        tok->vocab[i].len = (uint16_t)len;
        tok->vocab[i].rank = i;  /* token ID = position in vocab */
    }

    /* Build hash table (2x vocab for low collision) */
    tok->hash_size = 1;
    while (tok->hash_size < n_tokens * 2) tok->hash_size <<= 1;
    tok->hash_table = (uint32_t *)kmalloc((uint64_t)tok->hash_size * 4);
    if (!tok->hash_table) {
        serial_puts("[TOK] Failed to alloc hash table\n");
        kfree(tok->vocab);
        return -1;
    }
    memset(tok->hash_table, 0, (size_t)tok->hash_size * 4);

    for (uint32_t i = 0; i < n_tokens; i++) {
        tok_hash_insert(tok, i);
    }

    /* Parse merge rules (format: "token_a token_b") */
    if (n_merges > 0 && merges) {
        tok->merges = (tok_merge_t *)kmalloc((uint64_t)n_merges * sizeof(tok_merge_t));
        if (!tok->merges) {
            serial_puts("[TOK] Failed to alloc merges\n");
            /* Continue without merges — direct lookup still works */
        } else {
            tok->merge_count = 0;
            for (uint32_t i = 0; i < n_merges; i++) {
                const char *line = merges[i];
                uint32_t llen = merge_lens[i];

                /* Find space separator */
                uint32_t sp = 0;
                while (sp < llen && line[sp] != ' ') sp++;
                if (sp == 0 || sp >= llen) continue;

                /* Look up both halves as tokens */
                uint32_t a = tok_lookup(tok, (const uint8_t *)line, sp);
                uint32_t b = tok_lookup(tok, (const uint8_t *)(line + sp + 1),
                                        llen - sp - 1);
                if (a == (uint32_t)-1 || b == (uint32_t)-1) continue;

                tok->merges[tok->merge_count].a = a;
                tok->merges[tok->merge_count].b = b;
                tok->merges[tok->merge_count].rank = tok->merge_count;
                tok->merge_count++;
            }
        }
    }

    tok->bos_id = bos_id;
    tok->eos_id = eos_id;
    tok->pad_id = (uint32_t)-1;
    tok->ready = true;

    serial_puts("[TOK] Tokenizer ready: ");
    serial_putdec(tok->vocab_size);
    serial_puts(" tokens, ");
    serial_putdec(tok->merge_count);
    serial_puts(" merges\n");

    return 0;
}

/* ── BPE Encode ──────────────────────────────────────────────── */

/* Look up merge rank for token pair (a, b).
 * Returns merge rank, or (uint32_t)-1 if no merge exists. */
static uint32_t find_merge_rank(const tokenizer_t *tok, uint32_t a, uint32_t b)
{
    /* Linear scan — acceptable for reasonable merge counts.
     * Could be optimized with hash table on (a,b) pairs. */
    for (uint32_t i = 0; i < tok->merge_count; i++) {
        if (tok->merges[i].a == a && tok->merges[i].b == b)
            return tok->merges[i].rank;
    }
    return (uint32_t)-1;
}

/* BPE encode a single word (byte sequence) into token IDs.
 *
 * Algorithm from tiktoken (OpenAI) / reason_agent:
 *   1. Start with one token per byte (or per known token)
 *   2. Find the pair with lowest merge rank
 *   3. Merge that pair (two tokens become one)
 *   4. Repeat until no more merges
 *
 * Returns number of tokens produced. */
static int bpe_encode_word(const tokenizer_t *tok,
                           const uint8_t *word, uint32_t word_len,
                           uint32_t *out, uint32_t max_out)
{
    if (word_len == 0) return 0;

    /* Fast path: entire word is a single known token */
    uint32_t direct = tok_lookup(tok, word, word_len);
    if (direct != (uint32_t)-1) {
        if (max_out < 1) return -1;
        out[0] = direct;
        return 1;
    }

    /* Start with individual bytes */
    uint32_t ids[TOK_MAX_TOKEN_LEN + 1];
    uint32_t n_ids = 0;

    /* Try to start with known multi-byte tokens using greedy forward matching */
    uint32_t pos = 0;
    while (pos < word_len && n_ids < TOK_MAX_TOKEN_LEN) {
        /* Try decreasing lengths to find longest known token */
        bool found = false;
        uint32_t try_len = word_len - pos;
        if (try_len > 16) try_len = 16;  /* Cap at reasonable token length */

        for (uint32_t tl = try_len; tl >= 2; tl--) {
            uint32_t tid = tok_lookup(tok, word + pos, tl);
            if (tid != (uint32_t)-1) {
                ids[n_ids++] = tid;
                pos += tl;
                found = true;
                break;
            }
        }
        if (!found) {
            /* Single byte */
            uint32_t tid = tok_lookup(tok, word + pos, 1);
            if (tid != (uint32_t)-1) {
                ids[n_ids++] = tid;
            } else {
                /* Unknown byte — use byte value directly (fallback) */
                ids[n_ids++] = word[pos];
            }
            pos++;
        }
    }

    /* If we already have merges, apply BPE merge loop */
    if (tok->merge_count > 0 && n_ids > 1) {
        /* Iterative merge: find lowest-rank pair, merge, repeat */
        for (;;) {
            /* Find pair with lowest merge rank */
            uint32_t best_rank = (uint32_t)-1;
            uint32_t best_pos = (uint32_t)-1;

            for (uint32_t i = 0; i + 1 < n_ids; i++) {
                uint32_t rank = find_merge_rank(tok, ids[i], ids[i + 1]);
                if (rank != (uint32_t)-1 && rank < best_rank) {
                    best_rank = rank;
                    best_pos = i;
                }
            }

            if (best_pos == (uint32_t)-1) break;  /* No more merges */

            /* Merge: concatenate the bytes of ids[best_pos] and ids[best_pos+1] */
            const tok_entry_t *ea = &tok->vocab[ids[best_pos]];
            const tok_entry_t *eb = &tok->vocab[ids[best_pos + 1]];
            uint8_t merged[TOK_MAX_TOKEN_LEN];
            uint32_t mlen = ea->len + eb->len;
            if (mlen > TOK_MAX_TOKEN_LEN) break;
            memcpy(merged, ea->bytes, ea->len);
            memcpy(merged + ea->len, eb->bytes, eb->len);

            /* Look up merged sequence */
            uint32_t merged_id = tok_lookup(tok, merged, mlen);
            if (merged_id == (uint32_t)-1) break;  /* Shouldn't happen */

            /* Replace pair with merged token */
            ids[best_pos] = merged_id;
            /* Shift remaining */
            for (uint32_t i = best_pos + 1; i + 1 < n_ids; i++) {
                ids[i] = ids[i + 1];
            }
            n_ids--;
        }
    }

    /* Copy result */
    if (n_ids > max_out) return -1;
    for (uint32_t i = 0; i < n_ids; i++) {
        out[i] = ids[i];
    }
    return (int)n_ids;
}

/* Simple word boundary splitter for BPE.
 * Splits on transitions between: alpha, digit, whitespace, other.
 * This is a simplified version of the tiktoken regex patterns. */
static int char_class(uint8_t c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return 0;  /* alpha */
    if (c >= '0' && c <= '9') return 1;  /* digit */
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') return 2;   /* space */
    return 3;  /* other */
}

int tok_encode(const tokenizer_t *tok, const char *text, uint32_t text_len,
               uint32_t *out, uint32_t max_out)
{
    if (!tok->ready) return -1;
    if (text_len == 0) return 0;

    const uint8_t *buf = (const uint8_t *)text;
    uint32_t total = 0;
    uint32_t pos = 0;

    while (pos < text_len && total < max_out) {
        /* Find word boundary: consume characters of same class,
         * with special handling for leading space + alpha. */
        uint32_t start = pos;

        /* Leading space attached to next word (tiktoken convention) */
        if (buf[pos] == ' ' && pos + 1 < text_len &&
            char_class(buf[pos + 1]) == 0) {
            pos++;  /* Include space with the word */
        }

        int cls = char_class(buf[pos]);
        pos++;

        if (cls == 0) {
            /* Alpha: consume full word (including apostrophe contractions) */
            while (pos < text_len) {
                uint8_t c = buf[pos];
                if (char_class(c) == 0) {
                    pos++;
                } else if (c == '\'' && pos + 1 < text_len &&
                           char_class(buf[pos + 1]) == 0) {
                    pos++;  /* Include apostrophe */
                } else {
                    break;
                }
            }
        } else if (cls == 1) {
            /* Digits: consume up to 3 (tiktoken groups digits in 1-3) */
            uint32_t dcount = 1;
            while (pos < text_len && char_class(buf[pos]) == 1 && dcount < 3) {
                pos++;
                dcount++;
            }
        } else if (cls == 2) {
            /* Whitespace: consume run, but stop if next is alpha (to attach) */
            while (pos < text_len && char_class(buf[pos]) == 2) {
                /* Check if next non-space is alpha → break to attach space */
                if (buf[pos] == ' ' && pos + 1 < text_len &&
                    char_class(buf[pos + 1]) == 0) {
                    break;
                }
                pos++;
            }
        } else {
            /* Other: consume consecutive same-class */
            while (pos < text_len && char_class(buf[pos]) == 3) {
                pos++;
            }
        }

        /* BPE-encode this word */
        int n = bpe_encode_word(tok, buf + start, pos - start,
                                out + total, max_out - total);
        if (n < 0) return -1;
        total += (uint32_t)n;
    }

    return (int)total;
}

/* ── Decode ──────────────────────────────────────────────────── */

int tok_decode(const tokenizer_t *tok, const uint32_t *tokens, uint32_t n_tokens,
               char *out, uint32_t max_out)
{
    if (!tok->ready) return -1;

    uint32_t written = 0;
    for (uint32_t i = 0; i < n_tokens; i++) {
        uint32_t id = tokens[i];
        if (id >= tok->vocab_size) continue;

        const tok_entry_t *e = &tok->vocab[id];
        uint32_t len = e->len;
        if (written + len > max_out) break;
        memcpy(out + written, e->bytes, len);
        written += len;
    }

    return (int)written;
}

static char tok_decode_buf[TOK_MAX_TOKEN_LEN + 1];

const char *tok_decode_one(const tokenizer_t *tok, uint32_t token_id)
{
    if (!tok->ready || token_id >= tok->vocab_size)
        return NULL;

    const tok_entry_t *e = &tok->vocab[token_id];
    memcpy(tok_decode_buf, e->bytes, e->len);
    tok_decode_buf[e->len] = '\0';
    return tok_decode_buf;
}

/* ── Cleanup ─────────────────────────────────────────────────── */

void tok_free(tokenizer_t *tok)
{
    if (tok->vocab) { kfree(tok->vocab); tok->vocab = NULL; }
    if (tok->merges) { kfree(tok->merges); tok->merges = NULL; }
    if (tok->hash_table) { kfree(tok->hash_table); tok->hash_table = NULL; }
    tok->ready = false;
}
