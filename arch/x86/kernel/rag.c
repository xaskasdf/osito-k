/*
 * rag.c — Browser-RAG client for osito-k.
 *
 * Talks to factory.naranjositos.tech/rag/<corpus>/ for the index shards
 * and rag.naranjositos.tech/embed for the Cloudflare Workers AI
 * bge-large embedder.
 *
 * Path 1: network-attached. Pure fetch loop — no caching yet. After we
 * verify it works we can persist centroids + hot shards to OsitoFS for
 * a hybrid offline mode.
 *
 * Algorithm (per query):
 *   1. POST query to /embed → 1024 f32 vector (already L2-normalized
 *      by the Worker; we re-normalize defensively).
 *   2. Score against K centroids (dot product).
 *   3. Pick top-N clusters.
 *   4. Sign-binarize the query into 128 bytes.
 *   5. Fetch top-N cluster blobs in series (Asyncify makes parallel
 *      tricky to write; series is plenty fast for N=4).
 *   6. Hamming top-K across all fetched rows.
 *   7. Fetch each contributing cluster's texts/NNNN.jsonl and resolve
 *      row_id → {title, text}.
 *
 * Output rows are returned as a single buffer of newline-separated
 * "title :: text" pairs, ready to splice into a chat prompt.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

extern void serial_puts(const char *);
extern void serial_putdec(uint64_t);
extern void *malloc(unsigned long);
extern void  free(void *);

extern uint8_t *wasm_url_fetch(const char *url, int *out_size);
extern uint8_t *wasm_http_post_json(const char *url, const char *body, int *out_size);

extern float f16_to_f32(uint16_t h);
extern double sqrt(double);

/* OsitoFS for shard caching — accessed by string name. */
extern void    *osfs2_find(const char *name);
extern void    *osfs2_create(const char *name, uint64_t size);
extern int      osfs2_read(void *file, uint64_t offset, void *buf, uint64_t len);
extern int      osfs2_write(void *file, uint64_t offset, const void *buf, uint64_t len);
extern uint64_t osfs2_file_size(void *file);
extern int      osfs2_delete(const char *name);
extern int      osfs2_is_mounted(void);

/* Fetch a URL, but cache the body to OsitoFS at `fs_name` so subsequent
 * calls (across page reloads, via IndexedDB persistence) skip the
 * network. Caller frees the returned buffer. */
static uint8_t *rag_fetch_cached(const char *url, const char *fs_name, int *out_size)
{
    /* Cache hit path. */
    if (osfs2_is_mounted()) {
        void *f = osfs2_find(fs_name);
        if (f) {
            uint64_t sz = osfs2_file_size(f);
            if (sz > 0 && sz < (uint64_t)64 * 1024 * 1024) {
                uint8_t *buf = (uint8_t *)malloc((size_t)sz);
                if (buf && osfs2_read(f, 0, buf, sz) == 0) {
                    if (out_size) *out_size = (int)sz;
                    return buf;
                }
                if (buf) free(buf);
            }
        }
    }
    /* Cache miss: hit the network. */
    int sz = 0;
    uint8_t *blob = wasm_url_fetch(url, &sz);
    if (!blob) return NULL;
    /* Store to OsitoFS (best effort — if write fails we still return
     * the network buffer). */
    if (osfs2_is_mounted() && sz > 0) {
        /* Replace any stale copy. */
        osfs2_delete(fs_name);
        void *f = osfs2_create(fs_name, (uint64_t)sz);
        if (f) osfs2_write(f, 0, blob, (uint64_t)sz);
    }
    if (out_size) *out_size = sz;
    return blob;
}

#define RAG_DIM       1024
#define RAG_DIM_BYTES (RAG_DIM / 8)   /* 128 bytes packed binary */
#define RAG_TOP_N      4              /* clusters fetched per query */
#define RAG_TOP_K      4              /* hits returned to LM */

/* Very small JSON-like extractor: find "key" then return the float
 * after the colon. Returns 0.0 if not found. */
static float json_num_after(const char *s, const char *key)
{
    const char *p = strstr(s, key);
    if (!p) return 0.0f;
    p += strlen(key);
    while (*p && *p != ':') p++;
    if (!*p) return 0.0f;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    /* Naive parseFloat. */
    float sign = 1.0f;
    if (*p == '-') { sign = -1.0f; p++; }
    float n = 0.0f, frac = 0.0f, fdiv = 1.0f;
    int saw_dot = 0;
    while (*p) {
        if (*p >= '0' && *p <= '9') {
            if (!saw_dot) n = n * 10.0f + (float)(*p - '0');
            else { frac = frac * 10.0f + (float)(*p - '0'); fdiv *= 10.0f; }
            p++;
        } else if (*p == '.') { saw_dot = 1; p++; }
        else break;
    }
    /* Handle exponent for the embed response (scientific notation). */
    float val = sign * (n + frac / fdiv);
    if (*p == 'e' || *p == 'E') {
        p++;
        int esign = 1;
        if (*p == '-') { esign = -1; p++; }
        else if (*p == '+') p++;
        int e = 0;
        while (*p >= '0' && *p <= '9') { e = e * 10 + (*p - '0'); p++; }
        float mult = 1.0f;
        for (int i = 0; i < e; i++) mult *= 10.0f;
        if (esign < 0) val /= mult;
        else            val *= mult;
    }
    return val;
}

static int parse_embed_response(const char *json, float *out, int max)
{
    /* {"model":"...","dim":1024,"data":[[f, f, ...]],"edge_ms":...}
     * Walk to the first '[' inside "data", then to the inner '[',
     * then parse floats until the inner ']'. */
    const char *p = strstr(json, "\"data\"");
    if (!p) return -1;
    p = strchr(p, '[');
    if (!p) return -1;
    p = strchr(p + 1, '[');   /* inner */
    if (!p) return -1;
    p++;
    int n = 0;
    while (*p && *p != ']' && n < max) {
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t') p++;
        if (*p == ']') break;
        float sign = 1.0f;
        if (*p == '-') { sign = -1.0f; p++; }
        float a = 0.0f, frac = 0.0f, fdiv = 1.0f;
        int saw_dot = 0;
        while (*p && *p != ',' && *p != ']') {
            if (*p >= '0' && *p <= '9') {
                if (!saw_dot) a = a * 10.0f + (float)(*p - '0');
                else { frac = frac * 10.0f + (float)(*p - '0'); fdiv *= 10.0f; }
            } else if (*p == '.') saw_dot = 1;
            else if (*p == 'e' || *p == 'E') {
                p++;
                int esign = 1;
                if (*p == '-') { esign = -1; p++; }
                else if (*p == '+') p++;
                int e = 0;
                while (*p >= '0' && *p <= '9') { e = e * 10 + (*p - '0'); p++; }
                float mult = 1.0f;
                for (int i = 0; i < e; i++) mult *= 10.0f;
                if (esign < 0) a /= mult; else a *= mult;
                p--; /* compensate for outer p++ */
            }
            p++;
        }
        out[n++] = sign * (a + frac / fdiv);
    }
    return n;
}

static void l2_normalize(float *v, int n)
{
    float ssq = 0.0f;
    for (int i = 0; i < n; i++) ssq += v[i] * v[i];
    if (ssq <= 0.0f) return;
    float inv = 1.0f / (float)sqrt((double)ssq);
    for (int i = 0; i < n; i++) v[i] *= inv;
}

static void score_centroids(const float *q, const uint16_t *c_fp16,
                             int n_clusters, int dim, float *scores)
{
    for (int k = 0; k < n_clusters; k++) {
        float s = 0.0f;
        const uint16_t *row = c_fp16 + (uint64_t)k * dim;
        for (int d = 0; d < dim; d++)
            s += q[d] * f16_to_f32(row[d]);
        scores[k] = s;
    }
}

static void top_n_indices(const float *scores, int n_total, int n_top,
                           int *out_idx)
{
    /* Naive: scan for max n_top times. n_total = 256 or 1024,
     * n_top ≤ 16. Plenty fast. */
    for (int t = 0; t < n_top; t++) out_idx[t] = -1;
    for (int t = 0; t < n_top; t++) {
        int best = -1;
        float bv = -1e30f;
        for (int k = 0; k < n_total; k++) {
            /* Skip already-picked. */
            int taken = 0;
            for (int j = 0; j < t; j++) if (out_idx[j] == k) { taken = 1; break; }
            if (taken) continue;
            if (scores[k] > bv) { bv = scores[k]; best = k; }
        }
        out_idx[t] = best;
    }
}

static void binarize_query(const float *q, int dim, uint8_t *out)
{
    /* MSB-first within each byte, to match the index. */
    memset(out, 0, (size_t)dim / 8);
    for (int i = 0; i < dim; i++) {
        if (q[i] > 0.0f) {
            out[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
        }
    }
}

static int hamming_distance(const uint8_t *a, const uint8_t *b, int bytes)
{
    int d = 0;
    for (int i = 0; i < bytes; i++) {
        uint8_t x = a[i] ^ b[i];
        /* __builtin_popcount expects unsigned int. */
        d += __builtin_popcount((unsigned)x);
    }
    return d;
}

typedef struct {
    int      dist;       /* hamming distance (smaller = better) */
    uint32_t row_id;
    int      cluster_idx; /* index into top_clusters[] */
} rag_hit_t;

static void topk_insert(rag_hit_t *topk, int k, int dist, uint32_t row_id,
                         int cluster_idx)
{
    /* Find worst (largest dist) slot. */
    int worst = 0;
    for (int i = 1; i < k; i++)
        if (topk[i].dist > topk[worst].dist) worst = i;
    if (dist < topk[worst].dist) {
        topk[worst].dist = dist;
        topk[worst].row_id = row_id;
        topk[worst].cluster_idx = cluster_idx;
    }
}

static int topk_sort_cmp(const rag_hit_t *a, const rag_hit_t *b)
{
    return a->dist - b->dist;
}

/* Build the URL "https://factory.naranjositos.tech/rag/<corpus>/<path>". */
static void build_rag_url(char *dst, int dst_max,
                          const char *corpus, const char *suffix)
{
    int n = 0;
    const char *prefix = "https://factory.naranjositos.tech/rag/";
    for (const char *p = prefix; *p && n < dst_max - 1; p++) dst[n++] = *p;
    for (const char *p = corpus; *p && n < dst_max - 1; p++) dst[n++] = *p;
    if (n < dst_max - 1) dst[n++] = '/';
    for (const char *p = suffix; *p && n < dst_max - 1; p++) dst[n++] = *p;
    dst[n] = 0;
}

/* For "clusters/NNNN.bin" / "texts/NNNN.jsonl" — 4-digit zero-padded. */
static void cluster_path(char *dst, int max, const char *kind, int n, const char *ext)
{
    /* kind is "clusters" or "texts"; ext is ".bin" or ".jsonl". */
    int p = 0;
    for (const char *q = kind; *q && p < max - 1; q++) dst[p++] = *q;
    if (p < max - 1) dst[p++] = '/';
    if (p < max - 1) dst[p++] = (char)('0' + (n / 1000) % 10);
    if (p < max - 1) dst[p++] = (char)('0' + (n / 100)  % 10);
    if (p < max - 1) dst[p++] = (char)('0' + (n / 10)   % 10);
    if (p < max - 1) dst[p++] = (char)('0' + n % 10);
    for (const char *q = ext; *q && p < max - 1; q++) dst[p++] = *q;
    dst[p] = 0;
}

/* Locate "title": "..." or "text": "..." in a JSONL line. Writes a
 * unescaped copy to out (max bytes). Returns length, 0 if not found. */
static int json_string_value(const char *line, const char *key,
                              char *out, int max)
{
    const char *p = strstr(line, key);
    if (!p) return 0;
    p += strlen(key);
    while (*p && *p != ':') p++;
    if (!*p) return 0;
    while (*p && *p != '"') p++;
    if (*p != '"') return 0;
    p++;
    int n = 0;
    while (*p && *p != '"' && n < max - 1) {
        if (*p == '\\' && *(p+1)) {
            char c = *(p+1);
            if      (c == 'n')  out[n++] = '\n';
            else if (c == 't')  out[n++] = '\t';
            else if (c == '"')  out[n++] = '"';
            else if (c == '\\') out[n++] = '\\';
            else { out[n++] = c; }
            p += 2;
        } else {
            out[n++] = *p++;
        }
    }
    out[n] = 0;
    return n;
}

/* Public entry point — populates `result` with formatted retrieval
 * output (one "Title :: Text" per line, up to RAG_TOP_K hits).
 * Returns 0 on success, -1 on any fetch failure. */
int rag_retrieve(const char *corpus, const char *query,
                 char *result, int result_max)
{
    /* Sanity check: ensure result buffer is reasonable. */
    if (!result || result_max < 64) return -1;
    result[0] = 0;

    /* ── Step 1: fetch query embedding via Worker /embed. ── */
    serial_puts("[rag] embedding query...\n");
    char body[1024];
    int n = 0;
    {
        const char *p = "{\"query\":\"";
        for (; *p && n < (int)sizeof(body) - 1; p++) body[n++] = *p;
        for (const char *q = query; *q && n < (int)sizeof(body) - 4; q++) {
            if (*q == '"' || *q == '\\') {
                if (n < (int)sizeof(body) - 2) body[n++] = '\\';
            }
            if (n < (int)sizeof(body) - 2) body[n++] = *q;
        }
        body[n++] = '"';
        body[n++] = '}';
        body[n] = 0;
    }
    int emsz = 0;
    uint8_t *embed_resp = wasm_http_post_json(
        "https://rag.naranjositos.tech/embed", body, &emsz);
    if (!embed_resp) {
        serial_puts("[rag] /embed POST failed\n");
        return -1;
    }
    float *qvec = (float *)malloc(RAG_DIM * sizeof(float));
    int got = parse_embed_response((const char *)embed_resp, qvec, RAG_DIM);
    free(embed_resp);
    if (got != RAG_DIM) {
        serial_puts("[rag] embed parse failed, got=");
        serial_putdec((uint64_t)got);
        serial_puts("\n");
        free(qvec);
        return -1;
    }
    l2_normalize(qvec, RAG_DIM);

    /* ── Step 2: fetch centroids (cached on OsitoFS after first call —
     * survives reload via IndexedDB persistence so repeat queries
     * skip this 524 KB download). ── */
    serial_puts("[rag] fetch centroids...\n");
    char url[256], fs_path[128];
    build_rag_url(url, sizeof(url), corpus, "centroids.fp16.bin");
    int fn = 0;
    const char *fp = "rag/";
    for (const char *q = fp; *q && fn < (int)sizeof(fs_path) - 1; q++) fs_path[fn++] = *q;
    for (const char *q = corpus; *q && fn < (int)sizeof(fs_path) - 1; q++) fs_path[fn++] = *q;
    const char *cs = "/centroids.fp16.bin";
    for (const char *q = cs; *q && fn < (int)sizeof(fs_path) - 1; q++) fs_path[fn++] = *q;
    fs_path[fn] = 0;
    int csz = 0;
    uint8_t *c_buf = rag_fetch_cached(url, fs_path, &csz);
    if (!c_buf) { serial_puts("[rag] centroids fetch failed\n"); free(qvec); return -1; }
    int n_clusters = csz / (RAG_DIM * 2);

    /* ── Step 3: score centroids, pick top-N. ── */
    float *scores = (float *)malloc((size_t)n_clusters * sizeof(float));
    score_centroids(qvec, (const uint16_t *)c_buf, n_clusters, RAG_DIM, scores);
    int top_clusters[RAG_TOP_N];
    top_n_indices(scores, n_clusters, RAG_TOP_N, top_clusters);
    serial_puts("[rag] top clusters: ");
    for (int i = 0; i < RAG_TOP_N; i++) {
        serial_putdec((uint64_t)top_clusters[i]);
        serial_puts(" ");
    }
    serial_puts("\n");
    free(scores);
    free(c_buf);

    /* ── Step 4: binarize query. ── */
    uint8_t qubin[RAG_DIM_BYTES];
    binarize_query(qvec, RAG_DIM, qubin);
    free(qvec);

    /* ── Step 5-6: fetch top-N cluster blobs + accumulate hamming top-K. ── */
    rag_hit_t topk[RAG_TOP_K];
    for (int i = 0; i < RAG_TOP_K; i++) { topk[i].dist = 99999; topk[i].row_id = 0; topk[i].cluster_idx = -1; }

    for (int c = 0; c < RAG_TOP_N; c++) {
        int cluster_id = top_clusters[c];
        char path[64];
        cluster_path(path, sizeof(path), "clusters", cluster_id, ".bin");
        build_rag_url(url, sizeof(url), corpus, path);
        /* Cache cluster shard by full fs path. */
        char shard_fs[128]; int sf = 0;
        const char *pre = "rag/";
        for (const char *q = pre; *q && sf < (int)sizeof(shard_fs) - 1; q++) shard_fs[sf++] = *q;
        for (const char *q = corpus; *q && sf < (int)sizeof(shard_fs) - 1; q++) shard_fs[sf++] = *q;
        if (sf < (int)sizeof(shard_fs) - 1) shard_fs[sf++] = '/';
        for (const char *q = path; *q && sf < (int)sizeof(shard_fs) - 1; q++) shard_fs[sf++] = *q;
        shard_fs[sf] = 0;
        int bsz = 0;
        uint8_t *blob = rag_fetch_cached(url, shard_fs, &bsz);
        if (!blob) {
            serial_puts("[rag] cluster fetch failed: cluster ");
            serial_putdec((uint64_t)cluster_id); serial_puts("\n");
            continue;
        }
        uint32_t count = *(uint32_t *)blob;
        const uint8_t *p = blob + 4;
        int stride = 4 + RAG_DIM_BYTES;  /* 132 */
        for (uint32_t r = 0; r < count; r++) {
            uint32_t row_id = *(const uint32_t *)p;
            const uint8_t *ubin = p + 4;
            int dist = hamming_distance(qubin, ubin, RAG_DIM_BYTES);
            topk_insert(topk, RAG_TOP_K, dist, row_id, c);
            p += stride;
        }
        free(blob);
    }
    /* Sort topk ascending. Insertion sort, only 4 elems. */
    for (int i = 1; i < RAG_TOP_K; i++) {
        for (int j = i; j > 0 && topk_sort_cmp(&topk[j-1], &topk[j]) > 0; j--) {
            rag_hit_t tmp = topk[j]; topk[j] = topk[j-1]; topk[j-1] = tmp;
        }
    }

    /* ── Step 7: resolve texts. Fetch each contributing cluster's
     * jsonl and find matching row_id. To minimize fetches, cache
     * each cluster's jsonl by index. ── */
    char *texts_cache[RAG_TOP_N] = {0};
    int   texts_cache_size[RAG_TOP_N] = {0};

    int rp = 0;
    for (int h = 0; h < RAG_TOP_K; h++) {
        int ci = topk[h].cluster_idx;
        if (ci < 0) continue;
        if (!texts_cache[ci]) {
            char path[64];
            cluster_path(path, sizeof(path), "texts", top_clusters[ci], ".jsonl");
            build_rag_url(url, sizeof(url), corpus, path);
            /* Cache texts file too. */
            char tfs[128]; int tf = 0;
            const char *pre = "rag/";
            for (const char *q = pre; *q && tf < (int)sizeof(tfs) - 1; q++) tfs[tf++] = *q;
            for (const char *q = corpus; *q && tf < (int)sizeof(tfs) - 1; q++) tfs[tf++] = *q;
            if (tf < (int)sizeof(tfs) - 1) tfs[tf++] = '/';
            for (const char *q = path; *q && tf < (int)sizeof(tfs) - 1; q++) tfs[tf++] = *q;
            tfs[tf] = 0;
            int tsz = 0;
            uint8_t *t = rag_fetch_cached(url, tfs, &tsz);
            if (!t) {
                serial_puts("[rag] texts fetch failed: ");
                serial_putdec((uint64_t)top_clusters[ci]); serial_puts("\n");
                continue;
            }
            char *zt = (char *)malloc((size_t)tsz + 1);
            memcpy(zt, t, (size_t)tsz);
            zt[tsz] = 0;
            free(t);
            texts_cache[ci] = zt;
            texts_cache_size[ci] = tsz;
        }
        /* Scan JSONL for line containing "id": row_id. JSONL format
         * stores `{"id": NNN, "title": ...}` with a SPACE after the
         * colon, so we match `"id":` then skip whitespace and check
         * digits. */
        const char *txt = texts_cache[ci];
        uint32_t rid = topk[h].row_id;
        char ridstr[16]; int rln = 0;
        if (rid == 0) ridstr[rln++] = '0';
        else {
            uint32_t tmp = rid;
            char rev[16]; int rl = 0;
            while (tmp) { rev[rl++] = (char)('0' + tmp % 10); tmp /= 10; }
            while (rl > 0) ridstr[rln++] = rev[--rl];
        }
        ridstr[rln] = 0;
        const char *line = txt;
        const char *found = NULL;
        while (line && *line) {
            const char *nl = strchr(line, '\n');
            /* Each line starts with `{"id":` (maybe with space). */
            const char *idkey = strstr(line, "\"id\":");
            if (idkey && (!nl || idkey < nl)) {
                const char *p = idkey + 5;  /* past "id": */
                while (*p == ' ' || *p == '\t') p++;
                /* Compare ridstr against p. */
                int j;
                for (j = 0; j < rln; j++) if (p[j] != ridstr[j]) break;
                if (j == rln && (p[rln] == ',' || p[rln] == ' ' || p[rln] == '}')) {
                    found = line;
                    break;
                }
            }
            if (!nl) break;
            line = nl + 1;
        }
        if (found) {
            char title[256], text[1024];
            int tn = json_string_value(found, "\"title\"", title, sizeof(title));
            int tx = json_string_value(found, "\"text\"",  text,  sizeof(text));
            if (rp < result_max - 1) result[rp++] = '\n';
            /* "Wikipedia says: <title> — <text>" */
            const char *pre = "Wikipedia says: ";
            for (const char *q = pre; *q && rp < result_max - 1; q++) result[rp++] = *q;
            for (int i = 0; i < tn && rp < result_max - 1; i++) result[rp++] = title[i];
            if (rp < result_max - 3) { result[rp++] = ' '; result[rp++] = '-'; result[rp++] = ' '; }
            for (int i = 0; i < tx && rp < result_max - 1; i++) result[rp++] = text[i];
        }
    }
    result[rp < result_max ? rp : result_max - 1] = 0;

    for (int i = 0; i < RAG_TOP_N; i++) if (texts_cache[i]) free(texts_cache[i]);
    return 0;
}
