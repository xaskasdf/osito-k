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

/* ── GPT-2 byte-level inverse mapping ───────────────────────── */

/*
 * GPT-2 BPE stores token strings with a bijective mapping from the 256
 * possible bytes onto a set of printable Unicode characters:
 *   - Printable ASCII (0x21-0x7E), ¡-¬ (0xA1-0xAC), ®-ÿ (0xAE-0xFF)
 *     map to themselves (same Unicode code point as byte value).
 *   - The remaining 68 "problem" bytes (0x00-0x20, 0x7F, 0x80-0xA0, 0xAD)
 *     map to U+0100..U+0143 in order of their byte values.
 *
 * This table gives the actual byte for code points U+0100..U+0143:
 *   index 0  = U+0100 (Ā) → byte 0x00
 *   index 32 = U+0120 (Ġ) → byte 0x20  (SPACE — the common case)
 *   index 33 = U+0121 (ġ) → byte 0x7F  (DEL)
 *   index 34 = U+0122 (Ģ) → byte 0x80
 *   ...
 *   index 66 = U+0142 (ł) → byte 0xA0  (NBSP)
 *   index 67 = U+0143 (Ń) → byte 0xAD  (soft hyphen)
 */
static const uint8_t gpt2_cp_to_byte[68] = {
    /* 0x00-0x20: bytes 0..32 map to U+0100..U+0120 */
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
    16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
    /* 0x7F: byte 127 → U+0121 */
    127,
    /* 0x80-0xA0: bytes 128..160 → U+0122..U+0142 */
    128,129,130,131,132,133,134,135,136,137,138,139,
    140,141,142,143,144,145,146,147,148,149,150,151,
    152,153,154,155,156,157,158,159,160,
    /* 0xAD: byte 173 → U+0143 */
    173,
};

/*
 * Decode one GPT-2-encoded token string (stored in GGUF as UTF-8)
 * to the actual byte sequence it represents.
 * Returns number of output bytes written to `out`.
 */
static uint32_t gpt2_decode_bytes(const uint8_t *in, uint16_t in_len,
                                   uint8_t *out, uint32_t out_max)
{
    uint32_t r = 0, i = 0;
    while (i < in_len && r < out_max) {
        uint8_t  b0    = in[i];
        uint32_t cp;
        uint32_t width;

        /* Decode UTF-8 to a Unicode code point */
        if (b0 < 0x80) {
            cp = b0; width = 1;
        } else if ((b0 & 0xE0) == 0xC0 && i + 1 < in_len) {
            cp = ((b0 & 0x1F) << 6) | (in[i+1] & 0x3F); width = 2;
        } else if ((b0 & 0xF0) == 0xE0 && i + 2 < in_len) {
            cp = ((b0 & 0x0F) << 12) | ((in[i+1] & 0x3F) << 6) | (in[i+2] & 0x3F); width = 3;
        } else if ((b0 & 0xF8) == 0xF0 && i + 3 < in_len) {
            cp = ((b0 & 0x07) << 18) | ((in[i+1] & 0x3F) << 12) |
                 ((in[i+2] & 0x3F) << 6) | (in[i+3] & 0x3F); width = 4;
        } else {
            /* Invalid UTF-8 — pass through */
            out[r++] = b0; i++; continue;
        }
        i += width;

        /* Apply inverse GPT-2 mapping */
        if (cp >= 0x0100 && cp <= 0x0143) {
            /* Non-printable byte encoded in the Ā..Ń range */
            out[r++] = gpt2_cp_to_byte[cp - 0x0100];
        } else if (cp <= 0x00FF) {
            /* Direct byte (printable ASCII, ¡-¬, ®-ÿ) */
            out[r++] = (uint8_t)cp;
        } else {
            /* Real Unicode (e.g. in special tokens like <|im_start|>) — re-encode */
            if (cp < 0x800 && r + 1 < out_max) {
                out[r++] = 0xC0 | (cp >> 6);
                out[r++] = 0x80 | (cp & 0x3F);
            } else if (cp < 0x10000 && r + 2 < out_max) {
                out[r++] = 0xE0 | (cp >> 12);
                out[r++] = 0x80 | ((cp >> 6) & 0x3F);
                out[r++] = 0x80 | (cp & 0x3F);
            }
        }
    }
    return r;
}

/*
 * Encode raw bytes to GPT-2 vocab form (inverse of gpt2_decode_bytes).
 * Each input byte → its GPT-2 Unicode code point → UTF-8.
 * Used before vocab lookups so raw input bytes match the stored token strings.
 */
static uint32_t gpt2_encode_bytes(const uint8_t *in, uint32_t in_len,
                                   uint8_t *out, uint32_t out_max)
{
    uint32_t r = 0;
    for (uint32_t i = 0; i < in_len && r + 2 < out_max; i++) {
        uint8_t  b  = in[i];
        uint32_t cp;
        /* Printable ranges map to themselves */
        if ((b >= 33 && b <= 126) || (b >= 161 && b <= 172) || b >= 174) {
            cp = b;
        } else if (b <= 32) {
            cp = 0x0100u + b;             /* 0x00-0x20 → U+0100-U+0120 */
        } else if (b == 127) {
            cp = 0x0121u;
        } else if (b <= 160) {
            cp = 0x0122u + (b - 128);     /* 0x80-0xA0 → U+0122-U+0142 */
        } else {
            cp = 0x0143u;                 /* 0xAD → U+0143 */
        }
        /* Emit UTF-8 */
        if (cp < 0x80) {
            out[r++] = (uint8_t)cp;
        } else {                          /* U+0080-U+07FF: always 2 bytes here */
            out[r++] = (uint8_t)(0xC0 | (cp >> 6));
            out[r++] = (uint8_t)(0x80 | (cp & 0x3F));
        }
    }
    return r;
}

/* Global tokenizer instance (initialized from GGUF metadata) */
tokenizer_t g_tokenizer;

/* Global decode helper for use from inference.c */
const char *tok_global_decode(uint32_t id)
{
    if (!g_tokenizer.ready) return NULL;
    return tok_decode_one(&g_tokenizer, id);
}

/* Check if tokenizer is initialized (opaque access for shell.c) */
bool tok_is_ready(const tokenizer_t *tok)
{
    return tok && tok->ready;
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
 * GPT-2 vocab stores token strings in byte-level Unicode encoding
 * (e.g., space 0x20 → "Ġ" [0xC4,0xA0]).  Input raw bytes must be
 * GPT-2-encoded before vocab lookup, which is what gpt2_encode_bytes does.
 *
 * Algorithm: greedy forward matching, then BPE merge loop.
 *
 * Returns number of tokens produced. */
static int bpe_encode_word(const tokenizer_t *tok,
                           const uint8_t *word, uint32_t word_len,
                           uint32_t *out, uint32_t max_out)
{
    if (word_len == 0) return 0;

    /* GPT-2 byte-encode the word (raw bytes → vocab-compatible UTF-8) */
    uint8_t enc[TOK_MAX_TOKEN_LEN * 2 + 4];
    uint32_t enc_len = gpt2_encode_bytes(word, word_len, enc, sizeof(enc));

    /* Fast path: entire word is a single known token */
    uint32_t direct = tok_lookup(tok, enc, enc_len);
    if (direct != (uint32_t)-1) {
        if (max_out < 1) return -1;
        out[0] = direct;
        return 1;
    }

    uint32_t ids[TOK_MAX_TOKEN_LEN + 1];
    uint32_t n_ids = 0;

    /* Start with one token per GPT-2 character unit (proper BPE initialization).
     * Each input byte → 1 or 2 encoded bytes → one vocab token.
     * The BPE merge loop below will combine them according to merge rules. */
    uint32_t pos = 0;
    while (pos < enc_len && n_ids < TOK_MAX_TOKEN_LEN) {
        uint32_t unit_len = 1;
        if ((enc[pos] & 0xE0) == 0xC0 && pos + 1 < enc_len)
            unit_len = 2;  /* 2-byte UTF-8 for GPT-2 special byte */
        uint32_t tid = tok_lookup(tok, enc + pos, unit_len);
        if (tid != (uint32_t)-1) {
            ids[n_ids++] = tid;
        } else {
            ids[n_ids++] = enc[pos];  /* fallback */
        }
        pos += unit_len;
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
        uint32_t n = gpt2_decode_bytes(e->bytes, e->len,
                                        (uint8_t *)(out + written),
                                        max_out - written);
        written += n;
    }

    return (int)written;
}

/* Decode buffer — twice the max token length to accommodate GPT-2 expansion */
static uint8_t tok_decode_buf[TOK_MAX_TOKEN_LEN * 2 + 1];

const char *tok_decode_one(const tokenizer_t *tok, uint32_t token_id)
{
    if (!tok->ready || token_id >= tok->vocab_size)
        return NULL;

    const tok_entry_t *e = &tok->vocab[token_id];
    uint32_t n = gpt2_decode_bytes(e->bytes, e->len,
                                    tok_decode_buf,
                                    sizeof(tok_decode_buf) - 1);
    tok_decode_buf[n] = '\0';
    return (const char *)tok_decode_buf;
}

/* ── Special token helpers ───────────────────────────────────── */

uint32_t tok_get_bos_id(const void *tok)
{
    return ((const tokenizer_t *)tok)->bos_id;
}

uint32_t tok_get_eos_id(const void *tok)
{
    return ((const tokenizer_t *)tok)->eos_id;
}

/*
 * tok_find_special — search vocab for exact string s.
 * Returns token ID if found, UINT32_MAX otherwise.
 */
uint32_t tok_find_special(const void *tok, const char *s)
{
    const tokenizer_t *t = (const tokenizer_t *)tok;
    uint32_t slen = 0;
    while (s[slen]) slen++;
    for (uint32_t i = 0; i < t->vocab_size; i++) {
        if (t->vocab[i].len == (uint16_t)slen &&
            memcmp(t->vocab[i].bytes, s, slen) == 0)
            return i;
    }
    return UINT32_MAX;
}

/* ── Cleanup ─────────────────────────────────────────────────── */

void tok_free(tokenizer_t *tok)
{
    if (tok->vocab) { kfree(tok->vocab); tok->vocab = NULL; }
    if (tok->merges) { kfree(tok->merges); tok->merges = NULL; }
    if (tok->hash_table) { kfree(tok->hash_table); tok->hash_table = NULL; }
    tok->ready = false;
}
