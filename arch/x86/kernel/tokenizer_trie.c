/*
 * OsitoK x86-64 — Trie-Based Tokenizer
 *
 * Compressed trie from vocabulary for O(n) greedy longest-match
 * tokenization. Replaces the BPE merge loop for encoding.
 *
 * Memory: ~2-4 MB for 128K vocabulary.
 * Speed: O(input_length * max_token_length) ≈ O(n * 20).
 */

#include "../include/types.h"
#include "../include/paging.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);

/* ── Compressed trie node ───────────────────────────────────── */

/* Each node stores up to 16 children in a sorted array.
 * For nodes with more children, we use a 256-byte direct map. */

#define TRIE_INLINE_MAX  16

typedef struct trie_node {
    int32_t token_id;        /* >= 0 if this node is a complete token, -1 otherwise */
    uint8_t edge_count;
    uint8_t use_direct;      /* 1 = direct[256] map, 0 = sorted edges */
    union {
        struct {
            uint8_t  bytes[TRIE_INLINE_MAX];
            int32_t  children[TRIE_INLINE_MAX];
        } sorted;
        int32_t *direct;     /* 256-entry child index array, -1 = none */
    };
} trie_node_t;

/* ── Trie state ─────────────────────────────────────────────── */

#define TRIE_MAX_NODES  (512 * 1024)

static trie_node_t *trie_nodes;
static int32_t trie_node_count;
static bool    trie_ready;

static int32_t trie_alloc_node(void)
{
    if (trie_node_count >= TRIE_MAX_NODES) return -1;
    trie_node_t *n = &trie_nodes[trie_node_count];
    n->token_id = -1;
    n->edge_count = 0;
    n->use_direct = 0;
    return trie_node_count++;
}

/* Find child for byte b in a sorted-edges node */
static int32_t trie_find_child(trie_node_t *n, uint8_t b)
{
    if (n->use_direct)
        return n->direct[b];

    /* Binary search in sorted edge array */
    int lo = 0, hi = (int)n->edge_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (n->sorted.bytes[mid] == b) return n->sorted.children[mid];
        if (n->sorted.bytes[mid] < b) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

/* Insert child for byte b */
static int32_t trie_add_child(trie_node_t *n, uint8_t b)
{
    int32_t child = trie_alloc_node();
    if (child < 0) return -1;

    if (n->use_direct) {
        n->direct[b] = child;
        n->edge_count++;
        return child;
    }

    if (n->edge_count >= TRIE_INLINE_MAX) {
        /* Promote to direct map */
        int32_t *dm = (int32_t *)PHYS_TO_VIRT(
            mem_alloc_aligned(256 * sizeof(int32_t), 8));
        if (!dm) return -1;
        for (int i = 0; i < 256; i++) dm[i] = -1;
        /* Copy existing edges */
        for (int i = 0; i < n->edge_count; i++)
            dm[n->sorted.bytes[i]] = n->sorted.children[i];
        dm[b] = child;
        n->direct = dm;
        n->use_direct = 1;
        n->edge_count++;
        return child;
    }

    /* Insert sorted */
    int pos = n->edge_count;
    for (int i = 0; i < n->edge_count; i++) {
        if (n->sorted.bytes[i] > b) { pos = i; break; }
    }
    /* Shift right */
    for (int i = n->edge_count; i > pos; i--) {
        n->sorted.bytes[i] = n->sorted.bytes[i - 1];
        n->sorted.children[i] = n->sorted.children[i - 1];
    }
    n->sorted.bytes[pos] = b;
    n->sorted.children[pos] = child;
    n->edge_count++;
    return child;
}

/* ── Build trie from vocabulary ─────────────────────────────── */

int trie_build(const uint8_t **vocab_bytes, const uint32_t *vocab_lens,
               uint32_t vocab_size)
{
    /* Allocate node pool */
    uint64_t pool_size = TRIE_MAX_NODES * sizeof(trie_node_t);
    trie_nodes = (trie_node_t *)PHYS_TO_VIRT(
        mem_alloc_aligned(pool_size, 4096));
    if (!trie_nodes) {
        serial_puts("[TRIE] Failed to allocate node pool\n");
        return -1;
    }
    memset(trie_nodes, 0, pool_size);
    trie_node_count = 0;

    /* Create root node */
    trie_alloc_node();

    /* Insert each vocabulary token into the trie */
    uint32_t inserted = 0;
    for (uint32_t t = 0; t < vocab_size; t++) {
        if (!vocab_bytes[t] || vocab_lens[t] == 0) continue;

        int32_t node = 0;  /* root */
        for (uint32_t i = 0; i < vocab_lens[t]; i++) {
            uint8_t b = vocab_bytes[t][i];
            int32_t child = trie_find_child(&trie_nodes[node], b);
            if (child < 0) {
                child = trie_add_child(&trie_nodes[node], b);
                if (child < 0) goto done;
            }
            node = child;
        }
        trie_nodes[node].token_id = (int32_t)t;
        inserted++;
    }

done:
    trie_ready = true;

    serial_puts("[TRIE] Built: ");
    serial_putdec(inserted);
    serial_puts(" tokens, ");
    serial_putdec((uint64_t)trie_node_count);
    serial_puts(" nodes (");
    serial_putdec((uint64_t)trie_node_count * sizeof(trie_node_t) / 1024);
    serial_puts(" KB)\n");
    return 0;
}

/* ── Greedy longest-match tokenization ──────────────────────── */

int trie_tokenize(const char *text, uint32_t text_len,
                  uint32_t *tokens_out, uint32_t max_tokens)
{
    if (!trie_ready || !trie_nodes) return -1;

    uint32_t pos = 0;
    uint32_t n_tokens = 0;

    while (pos < text_len && n_tokens < max_tokens) {
        int32_t node = 0;  /* root */
        int32_t best_token = -1;
        uint32_t best_len = 0;

        /* Walk trie, remembering the last valid token */
        for (uint32_t i = pos; i < text_len; i++) {
            uint8_t b = (uint8_t)text[i];
            int32_t child = trie_find_child(&trie_nodes[node], b);
            if (child < 0) break;

            node = child;
            if (trie_nodes[node].token_id >= 0) {
                best_token = trie_nodes[node].token_id;
                best_len = i - pos + 1;
            }
        }

        if (best_token >= 0) {
            tokens_out[n_tokens++] = (uint32_t)best_token;
            pos += best_len;
        } else {
            /* Unknown byte: emit as byte-level token (token_id = byte + 3
             * for Llama 3 byte fallback convention) */
            tokens_out[n_tokens++] = (uint32_t)((uint8_t)text[pos]) + 3;
            pos++;
        }
    }

    return (int)n_tokens;
}

bool trie_is_ready(void) { return trie_ready; }
