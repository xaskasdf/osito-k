/*
 * rag.h — kernel-side client for the osito-a-models RAG stack.
 *
 * Path 1 in docs/rag-integration-guide.md: the kernel hits the live
 * endpoints (factory.naranjositos.tech for shards, rag.naranjositos.tech
 * for query embedding) over HTTPS, does the retrieval math locally,
 * and returns the top-K hits as title/text pairs ready to splice
 * into a brandon-tiny prompt.
 *
 * Frozen invariants we depend on (per the integration guide):
 *   - Embedder is BAAI/bge-large-en-v1.5 (1024-d, L2-normalized).
 *   - Cluster row stride = 4 + 128 = 132 bytes.
 *   - Cluster blob header = 4-byte LE uint32 count.
 *   - Centroids file = K × dim × 2 bytes float16, row-major.
 *   - Texts are JSON-per-line: {"id": int, "title": str, "text": str}.
 *
 * MVP scope: simple_en corpus (256 clusters, no gzip on texts).
 * wiki_en (1024 clusters, gzipped texts) needs a zlib hook plus a
 * bigger centroid arena; left for a follow-up.
 */

#ifndef OSITOA_RAG_H
#define OSITOA_RAG_H

#include "../include/types.h"

#define RAG_MAX_HITS         8
#define RAG_TITLE_CAP      256
#define RAG_TEXT_CAP       768

typedef struct {
    uint32_t row_id;
    int      cluster_idx;
    float    similarity;   /* hamming-derived; higher = better */
    char     title[RAG_TITLE_CAP];
    char     text[RAG_TEXT_CAP];
} rag_hit_t;

/* One-time init.  Fetches meta.json + centroids.fp16.bin from
 * factory.naranjositos.tech and decodes the centroids into fp32.
 * Idempotent (subsequent calls are no-ops once g_ready is set).
 * Returns 0 on success, -1 on network or parse failure. */
int rag_init(const char *corpus);

/* Per-query retrieval. Embeds the question via rag.naranjositos.tech,
 * scores centroids locally, fetches top-N cluster blobs, hamming-
 * ranks, fetches matching texts, fills `hits[]` with up to `max_hits`
 * entries (capped at RAG_MAX_HITS). Returns the number of hits
 * filled, or -1 on failure. */
int rag_query(const char *question, rag_hit_t *hits, uint32_t max_hits);

/* Whether init has completed successfully. */
bool rag_is_ready(void);

/* Fetch a single corpus file from the live endpoint and write it
 * into osfs2 at `rag/<corpus>/<sub>` so it stays cached for offline
 * use.  `sub` is the path under the corpus root (e.g.
 * "centroids.fp16.bin" or "clusters/0042.bin").  Useful for
 * refreshing one shard at a time without re-downloading everything.
 * Returns bytes written on success, -1 on any failure.  Each call
 * is one TCP connection — see docs/rag-native.md about the rapid-
 * fire multi-conn limitation. */
int rag_refresh(const char *corpus, const char *sub);

#endif
