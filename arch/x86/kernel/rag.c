/*
 * rag.c — Path 1 (network-attached) RAG retrieval for osito-a.
 *
 * Reads docs/rag-integration-guide.md for the wire layout and
 * algorithm; this file is the in-kernel implementation. Single
 * coarse pipeline per query:
 *
 *   1. POST /embed → 1024 fp32 (L2-normalized; the edge already
 *      normalizes, but we re-normalize defensively).
 *   2. Score against K fp32 centroids via dot product → top-N
 *      clusters.
 *   3. Binarize the query (sign-bit-pack 1024 dims → 128 bytes).
 *   4. Fetch each top-N cluster blob, popcount the XOR of every
 *      row's binary embedding vs the query, maintain a top-K
 *      ranked array.
 *   5. Fetch the cluster's texts/NNNN.jsonl, linear-scan for
 *      each top-K row_id, copy title + text into the result.
 *
 * Embedder match: the corpus on R2 was embedded with BAAI/bge-large-
 * en-v1.5 and the query MUST be embedded in the same space, so we
 * always go through the edge /embed endpoint (NOT through the
 * kernel's local mxbai embedder — different vector space, cosine is
 * meaningless across them).
 *
 * Trust: both endpoints are CF-fronted with chains that share the
 * GTS WE1 intermediate cert we already pin (kernel/cert_pin.c).
 * No new pinning needed — chain walking matches on intermediate.
 */

#include "../include/types.h"
#include "rag.h"
#include "http.h"

extern void   serial_puts(const char *s);
extern void   serial_putdec(uint64_t v);
extern void  *kmalloc(uint64_t);
extern void   kfree(void *);
extern float  f16_to_f32(uint16_t h);
extern float  sqrtf_bare(float);

/* VFS — we try osfs2 first for every shard fetch.  The bake script
 * at tools/rag-bake.sh pre-installs `rag/<corpus>/...` into the
 * image; reading locally skips the multi-HTTPS path that the kernel
 * TCP stack currently can't handle (see docs/rag-native.md). */
typedef struct { uint32_t fs_version; uint32_t ino; void *data; uint64_t size; } vfs_stub_t;
extern bool  vfs_find(const char *path, int mode, void *out);
extern int   vfs_read(void *node, uint64_t offset, void *buf, uint64_t len);

/* osfs2 write path — for the `rag_refresh` agent tool. */
extern bool  osfs2_is_mounted(void);
extern int   osfs2_delete(const char *name);
extern void *osfs2_create(const char *name, uint64_t size);
extern int   osfs2_write(void *file, uint64_t offset, const void *buf, uint64_t len);

#define RAG_FACTORY_HOST    "factory.naranjositos.tech"
#define RAG_EMBED_HOST      "rag.naranjositos.tech"
#define RAG_DEFAULT_TOPN    4
#define RAG_DIM             1024
#define RAG_DIM_BYTES       128         /* 1024 / 8, sign-packed binary */
#define RAG_STRIDE_BYTES    132         /* 4 byte row_id + 128 byte binary */

/* ── module state ───────────────────────────────────────────── */

static char       g_corpus[32];
static uint32_t   g_n_clusters;
static uint32_t   g_dim;
static uint32_t   g_stride_bytes;
static float     *g_centroids;      /* g_n_clusters * g_dim fp32 */
static bool       g_ready;

/* ── tiny utilities (per-module to keep coupling minimal) ───── */

static uint32_t kstrlen(const char *s) {
    uint32_t n = 0; while (s && s[n]) n++; return n;
}
static int kstrncmp(const char *a, const char *b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (uint8_t)a[i] - (uint8_t)b[i];
        if (a[i] == 0) return 0;
    }
    return 0;
}
static const char *kstrnstr(const char *hay, uint32_t hlen, const char *needle) {
    uint32_t nl = kstrlen(needle);
    if (nl == 0 || nl > hlen) return 0;
    for (uint32_t i = 0; i + nl <= hlen; i++) {
        if (kstrncmp(hay + i, needle, nl) == 0) return hay + i;
    }
    return 0;
}

/* Build a path string into `buf` from segments. Returns bytes
 * written (excluding NUL). Stops cleanly at cap-1. */
static uint32_t kpath_join(char *buf, uint32_t cap, const char *const *segs) {
    uint32_t off = 0;
    for (int i = 0; segs[i]; i++) {
        for (uint32_t k = 0; segs[i][k] && off + 1 < cap; k++)
            buf[off++] = segs[i][k];
    }
    if (off < cap) buf[off] = 0;
    return off;
}
static uint32_t kappend_u32_pad4(char *buf, uint32_t off, uint32_t cap, uint32_t v) {
    /* 4-digit zero-padded, e.g. 17 -> "0017" — matches the shard
     * file-name convention. */
    char tmp[5]; tmp[0] = (char)('0' + (v / 1000) % 10);
                 tmp[1] = (char)('0' + (v /  100) % 10);
                 tmp[2] = (char)('0' + (v /   10) % 10);
                 tmp[3] = (char)('0' + (v        ) % 10);
                 tmp[4] = 0;
    for (int i = 0; i < 4 && off + 1 < cap; i++) buf[off++] = tmp[i];
    return off;
}

/* JSON helpers — same scan-based approach used by the OAI server. */

static int json_find_int_in(const char *body, uint32_t len, const char *key) {
    char needle[64];
    uint32_t nl = 0;
    needle[nl++] = '"';
    for (uint32_t i = 0; key[i] && nl + 4 < sizeof needle; i++) needle[nl++] = key[i];
    needle[nl++] = '"'; needle[nl] = 0;
    const char *m = kstrnstr(body, len, needle);
    if (!m) return -1;
    uint32_t p = (uint32_t)(m - body) + nl;
    while (p < len && (body[p] == ' ' || body[p] == ':' ||
                         body[p] == '\t' || body[p] == '\n')) p++;
    int v = 0, got = 0;
    while (p < len && body[p] >= '0' && body[p] <= '9') {
        v = v * 10 + (body[p] - '0'); p++; got = 1;
    }
    return got ? v : -1;
}

/* Parse a float starting at *p (advance *p). Sets *ok. Subset of
 * the JSON-number grammar — enough for "-1.234e-5" style. */
static float kparse_float(const char **p_io, const char *end, int *ok) {
    const char *p = *p_io;
    int sign = 1;
    if (p < end && *p == '-') { sign = -1; p++; }
    else if (p < end && *p == '+') p++;
    float v = 0.0f; int digits = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        v = v * 10.0f + (float)(*p - '0'); p++; digits++;
    }
    if (p < end && *p == '.') {
        p++;
        float scale = 0.1f;
        /* Cap fractional precision at 9 digits.  float32 mantissa is
         * 23 bits ~= 7 decimal digits; beyond ~10 fractional digits
         * `scale *= 0.1f` drops below the normal range and produces
         * denormal operands.  Kernels that don't set MXCSR.DAZ/FTZ
         * (ours, currently) raise #XM SIMD exception on denormal
         * arithmetic, which previously killed agent_s0 mid-embed-
         * parse on the bge-large response (~17-digit fractions). */
        int frac_digits = 0;
        while (p < end && *p >= '0' && *p <= '9') {
            if (frac_digits < 9) {
                v += (float)(*p - '0') * scale;
                scale *= 0.1f;
                frac_digits++;
            }
            p++; digits++;
        }
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        p++;
        int es = 1;
        if (p < end && *p == '-') { es = -1; p++; }
        else if (p < end && *p == '+') p++;
        int e = 0;
        while (p < end && *p >= '0' && *p <= '9') { e = e*10 + (*p - '0'); p++; }
        float mul = 1.0f;
        for (int i = 0; i < e; i++) mul *= 10.0f;
        v = (es < 0) ? (v / mul) : (v * mul);
    }
    *p_io = p;
    if (ok) *ok = digits > 0 ? 1 : 0;
    return sign * v;
}

/* Decode a JSON-escaped string body into `out` (NUL-terminated).
 * `p_io` points at the opening quote on entry; advances past the
 * closing quote. Returns bytes copied. */
static uint32_t kjson_decode_str(const char *body, uint32_t len,
                                   uint32_t *p_io, char *out, uint32_t cap)
{
    uint32_t p = *p_io;
    if (p >= len || body[p] != '"') return 0;
    p++;
    uint32_t n = 0;
    while (p < len && body[p] != '"' && n + 1 < cap) {
        char c = body[p];
        if (c == '\\' && p + 1 < len) {
            char esc = body[p + 1];
            switch (esc) {
                case 'n': out[n++] = '\n'; break;
                case 'r': out[n++] = '\r'; break;
                case 't': out[n++] = '\t'; break;
                case '"': out[n++] = '"';  break;
                case '\\':out[n++] = '\\'; break;
                case '/': out[n++] = '/';  break;
                case 'u': p += 4; break;       /* skip \uXXXX */
                default:  out[n++] = esc;  break;
            }
            p += 2;
        } else {
            out[n++] = c; p++;
        }
    }
    if (p < len && body[p] == '"') p++;
    out[n] = 0;
    *p_io = p;
    return n;
}

/* ── math kernels ──────────────────────────────────────────── */

static void l2_normalize(float *v, uint32_t dim) {
    float s = 0.0f;
    for (uint32_t i = 0; i < dim; i++) s += v[i] * v[i];
    if (s <= 0.0f) return;
    float inv = 1.0f / sqrtf_bare(s);
    for (uint32_t i = 0; i < dim; i++) v[i] *= inv;
}

static float dot_f32(const float *a, const float *b, uint32_t dim) {
    float s = 0.0f;
    for (uint32_t i = 0; i < dim; i++) s += a[i] * b[i];
    return s;
}

/* Pick top-N indices in `scores` by descending value. */
static void topn_indices(const float *scores, uint32_t k_total,
                          uint32_t n, uint32_t *out, float *out_score) {
    for (uint32_t i = 0; i < n; i++) { out[i] = (uint32_t)-1; out_score[i] = -1e30f; }
    for (uint32_t i = 0; i < k_total; i++) {
        float s = scores[i];
        for (uint32_t k = 0; k < n; k++) {
            if (s > out_score[k]) {
                for (int j = (int)n - 1; j > (int)k; j--) {
                    out[j] = out[j-1]; out_score[j] = out_score[j-1];
                }
                out[k] = i; out_score[k] = s;
                break;
            }
        }
    }
}

/* Sign-bit-pack 1024 floats into 128 bytes, MSB-first within byte
 * (matches the corpus's binary encoding per the integration guide). */
static void binarize_query(const float *v, uint32_t dim, uint8_t *out) {
    for (uint32_t i = 0; i < dim / 8; i++) {
        uint8_t b = 0;
        for (int k = 0; k < 8; k++) {
            float f = v[i * 8 + k];
            if (f >= 0.0f) b |= (uint8_t)(0x80 >> k);
        }
        out[i] = b;
    }
}

/* Hamming distance via POPCNT.  Returns the number of bit positions
 * where the two 128-byte (1024-bit) embeddings differ. Smaller is
 * more similar; we'll convert to a similarity score downstream.
 *
 * __builtin_popcountll would compile to a libgcc helper
 * (__popcountdi2) we don't link; the inline `popcntq` instruction
 * is present on every CPU we run on (Sandy Bridge+/Bulldozer+). */
static inline uint64_t popcnt64(uint64_t x) {
#ifdef WASM_BUILD
    return (uint64_t)__builtin_popcountll(x);
#else
    uint64_t r;
    __asm__("popcntq %1, %0" : "=r"(r) : "rm"(x));
    return r;
#endif
}
static uint32_t hamming_128(const uint8_t *a, const uint8_t *b) {
    const uint64_t *A = (const uint64_t *)a;
    const uint64_t *B = (const uint64_t *)b;
    uint32_t d = 0;
    for (int i = 0; i < 16; i++) {
        d += (uint32_t)popcnt64(A[i] ^ B[i]);
    }
    return d;
}

/* ── shard fetch — VFS first, HTTPS fallback ──────────────── */

/* Try to read `osfs_name` from osfs2 into `out` (up to `cap`).
 * Returns bytes read on success, -1 on miss. */
static int rag_vfs_get(const char *osfs_name, void *out, uint32_t cap) {
    vfs_stub_t node;
    if (!vfs_find(osfs_name, 0, &node)) return -1;
    uint64_t want = node.size < cap ? node.size : (uint64_t)cap;
    int n = vfs_read(&node, 0, out, want);
    if (n < 0) return -1;
    return n;
}

/* ── HTTP wrappers ─────────────────────────────────────────── */

/* GET a binary blob into `out`, expecting at most `cap` bytes. The
 * connection is opened, fetched, drained, closed in one call —
 * a stateless one-shot. Returns bytes received, or -1 on failure. */
/* Sleep `ticks` ticks of the 100 Hz IDT timer. Used to space TCP
 * connections in rag_http_get_blob — CF closes connections after
 * each response, and the kernel's TCP slot cycles through TIME_WAIT
 * faster than its slirp peer expects, causing the next handshake
 * to lose its ServerHello on the wire. A short sleep between calls
 * keeps each connection isolated. */
#ifdef WASM_BUILD
extern void emscripten_sleep(unsigned int ms);
#else
extern uint64_t idt_get_ticks(void);
#endif
static void rag_sleep_ticks(uint32_t n) {
#ifdef WASM_BUILD
    emscripten_sleep(n * 10);
#else
    uint64_t deadline = idt_get_ticks() + n;
    while (idt_get_ticks() < deadline) {
        __asm__ volatile ("sti; hlt" ::: "memory");
    }
#endif
}

static int rag_http_get_blob(const char *hostname, const char *path,
                              void *out, uint32_t cap)
{
    /* Try up to 3 times with a 200 ms gap between attempts. CF
     * closes the TCP after every response; back-to-back fresh
     * handshakes occasionally lose the ServerHello on our slirp
     * NAT path (documented in docs/session-2026-05-10.md as the
     * rapid-fire TCP regression). Two retries cover the
     * typical observed failure window without changing the TCP
     * stack. */
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) rag_sleep_ticks(200);    /* ~2 s — gives CF / slirp time to settle */

        http_session_t s;
        if (http_open(&s, hostname) < 0) {
            serial_puts("[RAG] http_open failed (attempt ");
            serial_putdec((uint64_t)attempt);
            serial_puts(")\n");
            continue;
        }
        http_response_t resp;
        if (http_request(&s, "GET", path, hostname, 0, 0, 0, &resp) < 0 ||
            resp.status_code != 200) {
            http_close(&s);
            continue;
        }
        int n = http_read_body_full(&s, &resp, out, cap);
        http_close(&s);
        if (n > 0) return n;
    }
    serial_puts("[RAG] http_get_blob exhausted retries for ");
    serial_puts(path);
    serial_puts("\n");
    return -1;
}

/* Try osfs2 first; if missing, fall back to HTTPS. `local_name` is
 * the osfs2 stored name (e.g. "rag/simple_en/meta.json"); `path` is
 * the URL path (e.g. "/rag/simple_en/meta.json"). Returns bytes
 * fetched, or -1 on both miss. */
static int rag_get_with_fallback(const char *local_name, const char *path,
                                   void *out, uint32_t cap)
{
    int n = rag_vfs_get(local_name, out, cap);
    if (n > 0) {
        serial_puts("[RAG] vfs hit ");
        serial_puts(local_name);
        serial_puts(" (");
        serial_putdec((uint64_t)n);
        serial_puts("B)\n");
        return n;
    }
    serial_puts("[RAG] vfs miss ");
    serial_puts(local_name);
    serial_puts(" — fetching HTTPS\n");
    return rag_http_get_blob(RAG_FACTORY_HOST, path, out, cap);
}

/* POST a small JSON body, drain the response into `out`. Retries
 * up to 3 times — the same partial-recv issue that bites the
 * multi-conn GET path can also clip a single response if the
 * server's reply spans multiple TLS records and tls_recv times
 * out between them. Each attempt is a fresh TCP+TLS handshake. */
static int rag_http_post_json(const char *hostname, const char *path,
                               const char *body, uint32_t body_len,
                               void *out, uint32_t cap)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) rag_sleep_ticks(50);     /* 500 ms */

        http_session_t s;
        if (http_open(&s, hostname) < 0) continue;
        const char *hdrs[] = {
            "Content-Type: application/json",
            "Accept: application/json",
            0
        };
        http_response_t resp;
        if (http_request(&s, "POST", path, hostname, hdrs, body, body_len, &resp) < 0 ||
            resp.status_code != 200) {
            http_close(&s);
            continue;
        }
        int n = http_read_body_full(&s, &resp, out, cap);
        http_close(&s);
        /* Require at minimum half of the advertised Content-Length —
         * a clipped response is worse than no response (parser would
         * succeed on partial data and produce garbage). */
        if (resp.content_length == 0) {
            if (n > 0) return n;
        } else if ((uint32_t)n >= resp.content_length) {
            return n;
        }
        serial_puts("[RAG] POST short (attempt ");
        serial_putdec((uint64_t)attempt);
        serial_puts("): got ");
        serial_putdec((uint64_t)n);
        serial_puts(" of ");
        serial_putdec((uint64_t)resp.content_length);
        serial_puts("\n");
    }
    return -1;
}

/* ── init ──────────────────────────────────────────────────── */

bool rag_is_ready(void) { return g_ready; }

int rag_init(const char *corpus)
{
    if (g_ready) return 0;
    if (!corpus || !corpus[0]) return -1;
    uint32_t cl = 0;
    while (corpus[cl] && cl + 1 < sizeof g_corpus) { g_corpus[cl] = corpus[cl]; cl++; }
    g_corpus[cl] = 0;

    serial_puts("[RAG] init corpus=");
    serial_puts(g_corpus);
    serial_puts("\n");

    /* meta.json — osfs2 first, HTTPS fallback. The flat osfs2 name
     * uses no leading slash so `vfs_find` does an exact match;
     * the URL path keeps the leading slash for the HTTP request. */
    char path[128], local[128];
    const char *seg_url[]   = { "/rag/", g_corpus, "/meta.json", 0 };
    const char *seg_local[] = {  "rag/", g_corpus, "/meta.json", 0 };
    kpath_join(path,  sizeof path,  seg_url);
    kpath_join(local, sizeof local, seg_local);
    static char meta[2048];
    int n = rag_get_with_fallback(local, path, meta, sizeof meta - 1);
    if (n <= 0) return -1;
    meta[n] = 0;
    int nc = json_find_int_in(meta, (uint32_t)n, "n_clusters");
    int dim = json_find_int_in(meta, (uint32_t)n, "dim");
    int stride = json_find_int_in(meta, (uint32_t)n, "stride_bytes");
    if (nc <= 0 || dim != RAG_DIM || stride != RAG_STRIDE_BYTES) {
        serial_puts("[RAG] meta unexpected\n");
        return -1;
    }
    g_n_clusters = (uint32_t)nc;
    g_dim = (uint32_t)dim;
    g_stride_bytes = (uint32_t)stride;
    serial_puts("[RAG] meta ok n_clusters=");
    serial_putdec((uint64_t)g_n_clusters);
    serial_puts(" dim=");
    serial_putdec((uint64_t)g_dim);
    serial_puts("\n");

    /* centroids.fp16.bin — same VFS-first pattern. */
    const char *seg_cent_u[] = { "/rag/", g_corpus, "/centroids.fp16.bin", 0 };
    const char *seg_cent_l[] = {  "rag/", g_corpus, "/centroids.fp16.bin", 0 };
    kpath_join(path,  sizeof path,  seg_cent_u);
    kpath_join(local, sizeof local, seg_cent_l);
    uint32_t cent_bytes = g_n_clusters * g_dim * 2u;
    uint8_t *raw = (uint8_t *)kmalloc(cent_bytes);
    if (!raw) return -1;
    int got = rag_get_with_fallback(local, path, raw, cent_bytes);
    if (got <= 0 || (uint32_t)got != cent_bytes) {
        serial_puts("[RAG] centroid download short got=");
        serial_putdec((uint64_t)(int64_t)got);   /* signed-aware print */
        serial_puts("\n");
        kfree(raw);
        return -1;
    }
    g_centroids = (float *)kmalloc(g_n_clusters * g_dim * sizeof(float));
    if (!g_centroids) { kfree(raw); return -1; }
    const uint16_t *h16 = (const uint16_t *)raw;
    for (uint32_t i = 0; i < g_n_clusters * g_dim; i++) {
        g_centroids[i] = f16_to_f32(h16[i]);
    }
    kfree(raw);
    serial_puts("[RAG] centroids loaded\n");
    g_ready = true;
    return 0;
}

/* ── per-query pipeline ────────────────────────────────────── */

/* JSON-escape the question into a body of the form {"query":"..."}. */
static uint32_t build_embed_body(const char *q, char *out, uint32_t cap) {
    uint32_t off = 0;
    const char *p = "{\"query\":\"";
    while (*p && off + 1 < cap) out[off++] = *p++;
    for (uint32_t i = 0; q[i] && off + 4 < cap; i++) {
        char c = q[i];
        if      (c == '"')  { out[off++] = '\\'; out[off++] = '"';  }
        else if (c == '\\') { out[off++] = '\\'; out[off++] = '\\'; }
        else if (c == '\n') { out[off++] = '\\'; out[off++] = 'n';  }
        else if (c == '\r') { /* drop */ }
        else if (c < 0x20)  { /* drop */ }
        else                { out[off++] = c; }
    }
    p = "\"}";
    while (*p && off + 1 < cap) out[off++] = *p++;
    if (off < cap) out[off] = 0;
    return off;
}

/* Parse the /embed JSON response and fill `out` with `dim` floats.
 * Response shape:  {"model":"...","dim":1024,"data":[[ ... ]],"edge_ms":N}
 * Returns 0 on success, -1 on parse failure. */
static int parse_embed_response(const char *body, uint32_t len,
                                  float *out, uint32_t dim)
{
    const char *m = kstrnstr(body, len, "\"data\"");
    if (!m) return -1;
    uint32_t p = (uint32_t)(m - body) + 6;
    /* skip whitespace, colon, '[', '[' */
    while (p < len && (body[p] == ' ' || body[p] == ':' || body[p] == '\t' ||
                         body[p] == '[' || body[p] == '\n')) p++;

    const char *cur = body + p, *end = body + len;
    for (uint32_t i = 0; i < dim; i++) {
        while (cur < end && (*cur == ',' || *cur == ' ' || *cur == '\n')) cur++;
        int ok = 0;
        float v = kparse_float(&cur, end, &ok);
        if (!ok) return -1;
        out[i] = v;
    }
    return 0;
}

/* Scan a JSONL line of {"id":N,"title":"...","text":"..."}.  Returns
 * 1 if the id matches `want_id`, else 0.  On match, copies title +
 * text into the destination buffers. */
static int parse_jsonl_line(const char *line, uint32_t line_len,
                              uint32_t want_id,
                              char *title_out, uint32_t title_cap,
                              char *text_out,  uint32_t text_cap)
{
    int id = json_find_int_in(line, line_len, "id");
    if (id != (int)want_id) return 0;

    const char *m_title = kstrnstr(line, line_len, "\"title\"");
    const char *m_text  = kstrnstr(line, line_len, "\"text\"");
    if (m_title) {
        uint32_t p = (uint32_t)(m_title - line) + 7;
        while (p < line_len && (line[p] == ' ' || line[p] == ':')) p++;
        kjson_decode_str(line, line_len, &p, title_out, title_cap);
    } else if (title_cap) title_out[0] = 0;
    if (m_text) {
        uint32_t p = (uint32_t)(m_text - line) + 6;
        while (p < line_len && (line[p] == ' ' || line[p] == ':')) p++;
        kjson_decode_str(line, line_len, &p, text_out, text_cap);
    } else if (text_cap) text_out[0] = 0;
    return 1;
}

/* Walk a JSONL blob line by line, looking for any `want_ids[k]` and
 * filling the corresponding `hits[hit_idx[k]]`. Returns count of
 * row_ids resolved. */
static uint32_t resolve_texts_in_blob(const char *blob, uint32_t blob_len,
                                        const uint32_t *want_ids,
                                        const int *hit_idx,
                                        uint32_t n_want,
                                        rag_hit_t *hits)
{
    uint32_t resolved = 0;
    uint32_t off = 0;
    while (off < blob_len) {
        uint32_t end = off;
        while (end < blob_len && blob[end] != '\n') end++;
        if (end > off) {
            for (uint32_t k = 0; k < n_want; k++) {
                if (hit_idx[k] < 0) continue;
                if (hits[hit_idx[k]].title[0] != 0) continue;  /* already filled */
                if (parse_jsonl_line(blob + off, end - off, want_ids[k],
                                       hits[hit_idx[k]].title, RAG_TITLE_CAP,
                                       hits[hit_idx[k]].text,  RAG_TEXT_CAP)) {
                    resolved++;
                    if (resolved >= n_want) return resolved;
                    break;
                }
            }
        }
        off = end + 1;
    }
    return resolved;
}

/* ── one-shot refresh of a single corpus file from URL → osfs2 ─ */

int rag_refresh(const char *corpus, const char *sub)
{
    if (!corpus || !sub) return -1;
    if (!osfs2_is_mounted()) {
        serial_puts("[RAG] refresh: osfs2 not mounted\n");
        return -1;
    }

    char url[160], local[160];
    /* /rag/<corpus>/<sub> */
    uint32_t up = 0; const char *p;
    p = "/rag/"; while (*p && up + 1 < sizeof url) url[up++] = *p++;
    for (uint32_t i = 0; corpus[i] && up + 1 < sizeof url; i++) url[up++] = corpus[i];
    if (up + 1 < sizeof url) url[up++] = '/';
    for (uint32_t i = 0; sub[i] && up + 1 < sizeof url; i++) url[up++] = sub[i];
    url[up] = 0;
    /* rag/<corpus>/<sub> (no leading slash) */
    uint32_t lp = 0;
    p = "rag/"; while (*p && lp + 1 < sizeof local) local[lp++] = *p++;
    for (uint32_t i = 0; corpus[i] && lp + 1 < sizeof local; i++) local[lp++] = corpus[i];
    if (lp + 1 < sizeof local) local[lp++] = '/';
    for (uint32_t i = 0; sub[i] && lp + 1 < sizeof local; i++) local[lp++] = sub[i];
    local[lp] = 0;

    serial_puts("[RAG] refresh GET ");
    serial_puts(url);
    serial_puts(" → osfs2:");
    serial_puts(local);
    serial_puts("\n");

    /* Probe with a small buffer first to get the size... actually
     * we'd need HEAD or a content-length read.  Simpler: try 4 MB
     * scratch which covers every individual shard in simple_en. */
    uint32_t cap = 4u * 1024u * 1024u;
    uint8_t *buf = (uint8_t *)kmalloc(cap);
    if (!buf) {
        serial_puts("[RAG] refresh: alloc failed\n");
        return -1;
    }
    int n = rag_http_get_blob(RAG_FACTORY_HOST, url, buf, cap);
    if (n <= 0) {
        kfree(buf);
        return -1;
    }

    /* Replace any prior osfs2 entry; ignore delete failure (may be new). */
    osfs2_delete(local);
    void *f = osfs2_create(local, (uint64_t)n);
    if (!f) {
        serial_puts("[RAG] refresh: osfs2_create failed\n");
        kfree(buf);
        return -1;
    }
    int wr = osfs2_write(f, 0, buf, (uint64_t)n);
    kfree(buf);
    if (wr < 0) {
        serial_puts("[RAG] refresh: osfs2_write failed\n");
        return -1;
    }
    serial_puts("[RAG] refresh ok ");
    serial_putdec((uint64_t)n);
    serial_puts(" bytes\n");
    return n;
}

int rag_query(const char *question, rag_hit_t *hits, uint32_t max_hits)
{
    if (!g_ready) {
        serial_puts("[RAG] not initialized\n");
        return -1;
    }
    if (!question || !hits || max_hits == 0) return -1;
    if (max_hits > RAG_MAX_HITS) max_hits = RAG_MAX_HITS;

    /* Zero hits up front so resolve loop can short-circuit on already-filled entries. */
    for (uint32_t i = 0; i < max_hits; i++) {
        hits[i].title[0] = 0; hits[i].text[0] = 0;
        hits[i].similarity = 0.0f; hits[i].row_id = 0; hits[i].cluster_idx = -1;
    }

    /* 1. POST /embed */
    static char embed_body[1024];
    uint32_t body_len = build_embed_body(question, embed_body, sizeof embed_body);
    static char embed_resp[32768];
    int n_resp = rag_http_post_json(RAG_EMBED_HOST, "/embed",
                                      embed_body, body_len,
                                      embed_resp, sizeof embed_resp - 1);
    if (n_resp <= 0) return -1;
    embed_resp[n_resp] = 0;
    serial_puts("[RAG] embed resp ");
    serial_putdec((uint64_t)n_resp);
    serial_puts("B head=\"");
    for (int i = 0; i < n_resp && i < 80; i++) {
        char c = embed_resp[i];
        char tmp[2] = { c == '\n' || c == '\r' ? ' ' : c, 0 };
        serial_puts(tmp);
    }
    serial_puts("\"\n");

    static float qvec[RAG_DIM];
    if (parse_embed_response(embed_resp, (uint32_t)n_resp, qvec, g_dim) < 0) {
        serial_puts("[RAG] embed parse failed\n");
        return -1;
    }
    l2_normalize(qvec, g_dim);

    /* 2. Centroid scores → top-N clusters */
    static float scores[1024];
    for (uint32_t i = 0; i < g_n_clusters; i++) {
        scores[i] = dot_f32(qvec, g_centroids + i * g_dim, g_dim);
    }
    uint32_t topn[RAG_DEFAULT_TOPN];
    float    topn_score[RAG_DEFAULT_TOPN];
    topn_indices(scores, g_n_clusters, RAG_DEFAULT_TOPN, topn, topn_score);

    serial_puts("[RAG] top-");
    serial_putdec((uint64_t)RAG_DEFAULT_TOPN);
    serial_puts(" clusters:");
    for (uint32_t i = 0; i < RAG_DEFAULT_TOPN; i++) {
        serial_puts(" ");
        serial_putdec((uint64_t)topn[i]);
    }
    serial_puts("\n");

    /* 3. Binarize query for hamming pass */
    static uint8_t qubin[RAG_DIM_BYTES];
    binarize_query(qvec, g_dim, qubin);

    /* 4. Fetch each top-N cluster, score every row, keep global top-K hits */
    /* Per-hit fields tracked here: smallest hamming distance + which
     * cluster it came from + the row_id. */
    uint32_t hit_dist[RAG_MAX_HITS];
    uint32_t hit_row [RAG_MAX_HITS];
    int      hit_clu [RAG_MAX_HITS];
    for (uint32_t i = 0; i < max_hits; i++) {
        hit_dist[i] = (uint32_t)-1;
        hit_row [i] = 0;
        hit_clu [i] = -1;
    }

    static uint8_t cluster_blob[200 * 1024];
    char cpath[128], clocal[128];
    for (uint32_t i = 0; i < RAG_DEFAULT_TOPN; i++) {
        uint32_t cidx = topn[i];
        const char *seg_u[] = { "/rag/", g_corpus, "/clusters/", 0 };
        const char *seg_l[] = {  "rag/", g_corpus, "/clusters/", 0 };
        uint32_t pl = kpath_join(cpath,  sizeof cpath,  seg_u);
        uint32_t ll = kpath_join(clocal, sizeof clocal, seg_l);
        pl = kappend_u32_pad4(cpath,  pl, sizeof cpath,  cidx);
        ll = kappend_u32_pad4(clocal, ll, sizeof clocal, cidx);
        const char *suf = ".bin";
        for (int k = 0; suf[k] && pl + 1 < sizeof cpath;  k++) cpath[pl++]  = suf[k];
        for (int k = 0; suf[k] && ll + 1 < sizeof clocal; k++) clocal[ll++] = suf[k];
        cpath[pl] = 0; clocal[ll] = 0;

        int n = rag_get_with_fallback(clocal, cpath,
                                        cluster_blob, sizeof cluster_blob);
        if (n <= 4) continue;
        uint32_t count = ((uint32_t)cluster_blob[0])
                       | ((uint32_t)cluster_blob[1] << 8)
                       | ((uint32_t)cluster_blob[2] << 16)
                       | ((uint32_t)cluster_blob[3] << 24);
        uint32_t need = 4 + count * RAG_STRIDE_BYTES;
        if (need > (uint32_t)n) {
            serial_puts("[RAG] cluster blob short\n");
            continue;
        }

        const uint8_t *row = cluster_blob + 4;
        for (uint32_t r = 0; r < count; r++, row += RAG_STRIDE_BYTES) {
            uint32_t row_id = ((uint32_t)row[0])
                            | ((uint32_t)row[1] << 8)
                            | ((uint32_t)row[2] << 16)
                            | ((uint32_t)row[3] << 24);
            uint32_t d = hamming_128(qubin, row + 4);

            /* Insertion sort: smaller distance is better. */
            for (uint32_t k = 0; k < max_hits; k++) {
                if (d < hit_dist[k]) {
                    for (int j = (int)max_hits - 1; j > (int)k; j--) {
                        hit_dist[j] = hit_dist[j-1];
                        hit_row [j] = hit_row [j-1];
                        hit_clu [j] = hit_clu [j-1];
                    }
                    hit_dist[k] = d;
                    hit_row [k] = row_id;
                    hit_clu [k] = (int)cidx;
                    break;
                }
            }
        }
    }

    /* Convert hamming distance to a similarity score in [0, 1]. */
    for (uint32_t i = 0; i < max_hits; i++) {
        if (hit_clu[i] < 0) continue;
        hits[i].row_id      = hit_row[i];
        hits[i].cluster_idx = hit_clu[i];
        hits[i].similarity  = 1.0f - (float)hit_dist[i] / (float)g_dim;
    }

    /* 5. Resolve texts.  Group top-K by cluster so we only fetch each
     * texts/NNNN.jsonl once. */
    static uint8_t fetched[256];   /* indexed by which-hit-already-fetched-its-cluster */
    for (uint32_t i = 0; i < sizeof fetched; i++) fetched[i] = 0;

    static char text_blob[256 * 1024];
    for (uint32_t i = 0; i < max_hits; i++) {
        if (hit_clu[i] < 0 || fetched[i]) continue;

        /* Gather all hits in the same cluster. */
        uint32_t batch_ids[RAG_MAX_HITS];
        int      batch_idx[RAG_MAX_HITS];
        uint32_t n_batch = 0;
        for (uint32_t k = i; k < max_hits; k++) {
            if (hit_clu[k] != hit_clu[i] || fetched[k]) continue;
            batch_ids[n_batch] = hit_row[k];
            batch_idx[n_batch] = (int)k;
            n_batch++;
            fetched[k] = 1;
        }
        if (n_batch == 0) continue;

        const char *seg_u[] = { "/rag/", g_corpus, "/texts/", 0 };
        const char *seg_l[] = {  "rag/", g_corpus, "/texts/", 0 };
        uint32_t pl = kpath_join(cpath,  sizeof cpath,  seg_u);
        uint32_t ll = kpath_join(clocal, sizeof clocal, seg_l);
        pl = kappend_u32_pad4(cpath,  pl, sizeof cpath,  (uint32_t)hit_clu[i]);
        ll = kappend_u32_pad4(clocal, ll, sizeof clocal, (uint32_t)hit_clu[i]);
        const char *suf = ".jsonl";
        for (int k = 0; suf[k] && pl + 1 < sizeof cpath;  k++) cpath[pl++]  = suf[k];
        for (int k = 0; suf[k] && ll + 1 < sizeof clocal; k++) clocal[ll++] = suf[k];
        cpath[pl] = 0; clocal[ll] = 0;

        int tn = rag_get_with_fallback(clocal, cpath,
                                         text_blob, sizeof text_blob - 1);
        if (tn <= 0) continue;
        text_blob[tn] = 0;
        resolve_texts_in_blob(text_blob, (uint32_t)tn, batch_ids, batch_idx,
                              n_batch, hits);
    }

    /* Count filled hits for the return value. */
    uint32_t out_n = 0;
    for (uint32_t i = 0; i < max_hits; i++) {
        if (hits[i].title[0]) out_n++;
    }
    serial_puts("[RAG] resolved ");
    serial_putdec((uint64_t)out_n);
    serial_puts(" hits\n");
    return (int)out_n;
}
