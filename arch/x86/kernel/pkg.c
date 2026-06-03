/*
 * OsitoK x86-64 — Native package manager (`pkg`)  — see pkg.h
 *
 * Reuses: http.c (HTTPS), ositofs2.c (storage), crypto.h (SHA-256),
 * process.c (proc_exec). No new infrastructure.
 */
#include "../include/types.h"
#include "http.h"
#include "crypto.h"
#include "zlib.h"
#include "pkg.h"

/* ── Config ─────────────────────────────────────────────────── */
#define PKG_HOST       "pkg.naranjositos.tech"   /* dedicated x86-64 pkg domain */
#define PKG_BASE       "/"                        /* binaries at domain root */
#define PKG_CATALOG    "/catalog.json"
#define PKG_CACHE      "pkg/catalog.json"   /* OsitoFS path (flat, no leading /) */
#define PKG_CATALOG_MAX (256u * 1024u)
#define PKG_DEP_DEPTH   8

/* ── Kernel externs ─────────────────────────────────────────── */
extern void *kmalloc(uint64_t size);
extern void  kfree(void *p);
extern int   strcmp(const char *a, const char *b);

extern void *osfs2_find(const char *name);
extern void *osfs2_create(const char *name, uint64_t size);
extern int   osfs2_write(void *file, uint64_t offset, const void *buf, uint64_t len);
extern int   osfs2_read(void *file, uint64_t offset, void *buf, uint64_t len);
extern int   osfs2_delete(const char *name);
extern void *osfs2_get_file(int index);
extern const char *osfs2_file_name(void *file);
extern uint64_t osfs2_file_size(void *file);
extern int   osfs2_is_mounted(void);
extern int   disk_flush(void);

extern int   proc_exec(const char *filename, int argc, const char **argv);

/* ── Tiny string helpers (avoid libc header coupling) ───────── */
static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }

static void scpy(char *d, const char *s, unsigned max) {
    unsigned i = 0;
    if (!max) return;
    for (; s[i] && i < max - 1; i++) d[i] = s[i];
    d[i] = 0;
}

/* append src to d (bounded by total size of d = max) */
static void scat(char *d, const char *s, unsigned max) {
    unsigned n = slen(d), i = 0;
    for (; s[i] && n + 1 < max; i++) d[n++] = s[i];
    d[n] = 0;
}

/* emit an unsigned decimal via the output sink */
static void emit_num(pkg_out_fn out, uint64_t n) {
    char b[24]; int i = 24; b[--i] = 0;
    if (n == 0) b[--i] = '0';
    while (n && i > 0) { b[--i] = (char)('0' + (n % 10)); n /= 10; }
    out(&b[i]);
}

static int hexnib(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ── Minimal JSON scanning (trusted, sha-verified catalog) ──── */

/* Find the opening quote of the next `"key"` token in [s,end). */
static const char *jkey(const char *s, const char *end, const char *key) {
    unsigned kl = slen(key);
    for (const char *p = s; p + kl + 2 <= end; p++) {
        if (p[0] != '"') continue;
        unsigned i = 0;
        for (; i < kl; i++) if (p[1 + i] != key[i]) break;
        if (i == kl && p[1 + kl] == '"') return p;
    }
    return 0;
}

/* Extract string value of `key` within [s,end). Returns len or -1. */
static int jstr(const char *s, const char *end, const char *key, char *out, int max) {
    const char *p = jkey(s, end, key);
    if (!p) return -1;
    p += 1 + slen(key) + 1;             /* past `"key"` */
    while (p < end && *p != ':') p++;
    if (p >= end) return -1;
    p++;
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r')) p++;
    if (p >= end || *p != '"') return -1;
    p++;
    int i = 0;
    while (p < end && *p != '"' && i < max - 1) out[i++] = *p++;
    out[i] = 0;
    return i;
}

/* Extract numeric value of `key` within [s,end). Returns 0 / -1. */
static int jnum(const char *s, const char *end, const char *key, uint64_t *v) {
    const char *p = jkey(s, end, key);
    if (!p) return -1;
    p += 1 + slen(key) + 1;
    while (p < end && *p != ':') p++;
    if (p >= end) return -1;
    p++;
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r')) p++;
    if (p >= end || *p < '0' || *p > '9') return -1;
    uint64_t r = 0;
    while (p < end && *p >= '0' && *p <= '9') r = r * 10 + (uint64_t)(*p++ - '0');
    *v = r;
    return 0;
}

/* Is `want` a quoted element of the array value of `key` in [s,end)? */
static int jarray_has(const char *s, const char *end, const char *key, const char *want) {
    const char *p = jkey(s, end, key);
    if (!p) return 0;
    p += 1 + slen(key) + 1;
    while (p < end && *p != '[') { if (*p == ',' || *p == '}') return 0; p++; }
    if (p >= end) return 0;
    p++;
    unsigned wl = slen(want);
    while (p < end && *p != ']') {
        if (*p == '"') {
            const char *q = p + 1;
            const char *e = q;
            while (e < end && *e != '"') e++;
            unsigned el = (unsigned)(e - q);
            if (el == wl) {
                unsigned i = 0; for (; i < wl; i++) if (q[i] != want[i]) break;
                if (i == wl) return 1;
            }
            p = (e < end) ? e + 1 : end;
        } else p++;
    }
    return 0;
}

/* ── Catalog entry ──────────────────────────────────────────── */
typedef struct {
    char     name[64];
    char     file[96];
    char     kind[12];      /* "bin" / "lib" / "sysroot" (default "bin") */
    char     dest[128];     /* install path for lib/sysroot */
    char     desc[160];
    uint64_t size;
    uint64_t usize;         /* uncompressed size (kind:sysroot) */
    uint8_t  sha256[32];
    int      has_sha;
    const char *win;        /* window start (for deps iteration) */
    const char *win_end;
} pkg_entry_t;

static void parse_sha(const char *s, const char *end, pkg_entry_t *e) {
    char hex[80];
    e->has_sha = 0;
    int n = jstr(s, end, "sha256", hex, sizeof hex);
    if (n != 64) return;
    for (int i = 0; i < 32; i++) {
        int hi = hexnib(hex[2 * i]), lo = hexnib(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return;
        e->sha256[i] = (uint8_t)((hi << 4) | lo);
    }
    e->has_sha = 1;
}

static void fill_entry(const char *s, const char *end, pkg_entry_t *e) {
    e->name[0] = e->file[0] = e->dest[0] = e->desc[0] = 0;
    e->size = 0; e->usize = 0;
    e->win = s; e->win_end = end;
    jstr(s, end, "name", e->name, sizeof e->name);
    if (jstr(s, end, "file", e->file, sizeof e->file) <= 0)
        scpy(e->file, e->name, sizeof e->file);
    if (jstr(s, end, "kind", e->kind, sizeof e->kind) <= 0)
        scpy(e->kind, "bin", sizeof e->kind);
    jstr(s, end, "dest", e->dest, sizeof e->dest);
    jstr(s, end, "desc", e->desc, sizeof e->desc);
    jnum(s, end, "size", &e->size);
    jnum(s, end, "usize", &e->usize);
    parse_sha(s, end, e);
}

/* Find a package by name, or by membership in its "provides" array. */
static int catalog_find(const char *cat, const char *want, pkg_entry_t *e) {
    const char *end = cat + slen(cat);
    const char *p = cat;
    for (;;) {
        const char *o = jkey(p, end, "name");
        if (!o) break;
        const char *o2 = jkey(o + 1, end, "name");
        const char *wend = o2 ? o2 : end;
        char nm[64];
        if (jstr(o, wend, "name", nm, sizeof nm) > 0) {
            if (strcmp(nm, want) == 0 || jarray_has(o, wend, "provides", want)) {
                fill_entry(o, wend, e);
                return 1;
            }
        }
        if (!o2) break;
        p = o2;
    }
    return 0;
}

/* ── Download → RAM, with SHA-256, then persist to OsitoFS ────── */
typedef struct {
    uint8_t *buf;       /* RAM staging buffer (size = Content-Length) */
    uint64_t cap;       /* buffer capacity */
    uint64_t off;       /* bytes received so far */
    sha256_ctx sha;
    int      err;
} fetch_ctx_t;

static int fetch_cb(const void *data, uint32_t len, void *vctx) {
    fetch_ctx_t *c = (fetch_ctx_t *)vctx;
    if (c->off + len > c->cap) { c->err = 1; return -1; }   /* server overran CL */
    memcpy(c->buf + c->off, data, len);
    sha256_update(&c->sha, data, len);
    c->off += len;
    return 0;
}

/* GET host+path, stream body into OsitoFS file `dest`, fill out_sha[32].
 * Returns 0 ok, -1 error. */
static int fetch_to_osfs(const char *host, const char *path, const char *dest,
                         uint64_t expect, uint8_t out_sha[32], pkg_out_fn out) {
    http_session_t *s = (http_session_t *)kmalloc(sizeof(http_session_t));
    http_response_t *resp = (http_response_t *)kmalloc(sizeof(http_response_t));
    if (!s || !resp) { out("pkg: out of memory\n"); if (s) kfree(s); if (resp) kfree(resp); return -1; }

    if (http_open(s, host) < 0) {
        out("pkg: http_open failed (DNS/TLS)\n");
        kfree(s); kfree(resp); return -1;
    }
    const char *headers[] = { "Connection: close", "User-Agent: OsitoK/1.0 (pkg)", 0 };
    if (http_request(s, "GET", path, host, headers, 0, 0, resp) < 0) {
        out("pkg: http_request failed\n");
        http_close(s); kfree(s); kfree(resp); return -1;
    }
    if (resp->status_code != 200) {
        out("pkg: HTTP "); emit_num(out, (uint64_t)resp->status_code); out("\n");
        http_close(s); kfree(s); kfree(resp); return -1;
    }
    const char *cl = http_get_header(resp, "Content-Length");
    uint64_t clen = 0;
    if (cl) for (const char *q = cl; *q >= '0' && *q <= '9'; q++) clen = clen * 10 + (uint64_t)(*q - '0');
    if (clen == 0) {
        out("pkg: missing Content-Length (chunked unsupported)\n");
        http_close(s); kfree(s); kfree(resp); return -1;
    }
    if (expect && clen != expect) {
        out("pkg: catalog/server size mismatch\n");
        http_close(s); kfree(s); kfree(resp); return -1;
    }

    /* Stage the whole body in RAM, then write to disk in ONE pass after the
     * connection is closed.  The old path wrote each chunk to NVMe inline
     * inside the read callback; under QEMU TCG those synchronous writes
     * starved net_poll long enough that the TCP receive window filled and
     * the server (Cloudflare) closed the connection mid-transfer (~2 MB in).
     * Receiving at network speed into RAM and deferring the disk write
     * decouples the two so the full transfer completes.  Buffer size =
     * Content-Length; the heap auto-grows with RAM (sysroot-scale matches
     * the -m the install already needs). */
    uint8_t *body = (uint8_t *)kmalloc(clen);
    if (!body) {
        out("pkg: out of memory ("); emit_num(out, clen); out(" bytes)\n");
        http_close(s); kfree(s); kfree(resp); return -1;
    }

    fetch_ctx_t c;
    c.buf = body; c.cap = clen; c.off = 0; c.err = 0;
    sha256_init(&c.sha);
    out("pkg: downloading "); emit_num(out, clen); out(" bytes...\n");
    int got = http_read_body(s, resp, fetch_cb, &c);
    http_close(s); kfree(s); kfree(resp);

    if (got < 0 || c.err || c.off != clen) {
        out("pkg: download failed/short ("); emit_num(out, c.off);
        out("/"); emit_num(out, clen); out(")\n");
        kfree(body);
        return -1;
    }
    sha256_final(&c.sha, out_sha);

    /* Persist — no network in flight now, so a slow NVMe write is harmless. */
    osfs2_delete(dest);                     /* idempotent */
    void *f = osfs2_create(dest, clen);
    if (!f) {
        out("pkg: osfs2_create failed (no contiguous space for ");
        emit_num(out, clen); out(" bytes?)\n");
        kfree(body);
        return -1;
    }
    if (osfs2_write(f, 0, body, clen) < 0) {
        out("pkg: disk write failed\n");
        osfs2_delete(dest); kfree(body);
        return -1;
    }
    kfree(body);
    disk_flush();
    return 0;
}

/* Buffered GET (for the small catalog). Returns kmalloc'd NUL-terminated
 * buffer (caller frees) or 0. */
static char *http_get_buf(const char *host, const char *path, uint64_t maxsz, pkg_out_fn out) {
    http_session_t *s = (http_session_t *)kmalloc(sizeof(http_session_t));
    http_response_t *resp = (http_response_t *)kmalloc(sizeof(http_response_t));
    if (!s || !resp) { if (s) kfree(s); if (resp) kfree(resp); return 0; }

    if (http_open(s, host) < 0) { out("pkg: http_open failed (DNS/TLS)\n"); kfree(s); kfree(resp); return 0; }
    const char *headers[] = { "Connection: close", "User-Agent: OsitoK/1.0 (pkg)", 0 };
    if (http_request(s, "GET", path, host, headers, 0, 0, resp) < 0 || resp->status_code != 200) {
        if (resp->status_code && resp->status_code != 200) { out("pkg: HTTP "); emit_num(out, (uint64_t)resp->status_code); out("\n"); }
        else out("pkg: request failed\n");
        http_close(s); kfree(s); kfree(resp); return 0;
    }
    const char *cl = http_get_header(resp, "Content-Length");
    uint64_t clen = 0;
    if (cl) for (const char *q = cl; *q >= '0' && *q <= '9'; q++) clen = clen * 10 + (uint64_t)(*q - '0');
    if (clen == 0 || clen >= maxsz) { out("pkg: catalog too large/missing length\n"); http_close(s); kfree(s); kfree(resp); return 0; }

    char *buf = (char *)kmalloc(clen + 1);
    if (!buf) { http_close(s); kfree(s); kfree(resp); return 0; }
    int got = http_read_body_full(s, resp, buf, (uint32_t)clen);
    http_close(s); kfree(s); kfree(resp);
    if (got < 0 || (uint64_t)got != clen) { kfree(buf); return 0; }
    buf[clen] = 0;
    return buf;
}

/* ── Catalog cache ──────────────────────────────────────────── */

/* Fetch catalog from the network and cache it to OsitoFS. Returns buffer. */
static char *catalog_refresh(pkg_out_fn out) {
    char *buf = http_get_buf(PKG_HOST, PKG_CATALOG, PKG_CATALOG_MAX, out);
    if (!buf) return 0;
    uint64_t n = slen(buf);
    osfs2_delete(PKG_CACHE);
    void *f = osfs2_create(PKG_CACHE, n);
    if (f) { osfs2_write(f, 0, buf, n); disk_flush(); }
    return buf;
}

/* Load the cached catalog from OsitoFS (no network). Returns buffer or 0. */
static char *catalog_load_cached(void) {
    void *f = osfs2_find(PKG_CACHE);
    if (!f) return 0;
    uint64_t sz = osfs2_file_size(f);
    if (sz == 0 || sz >= PKG_CATALOG_MAX) return 0;
    char *buf = (char *)kmalloc(sz + 1);
    if (!buf) return 0;
    if (osfs2_read(f, 0, buf, sz) < 0) { kfree(buf); return 0; }
    buf[sz] = 0;
    return buf;
}

/* Get catalog: cached if present, else refresh from network. */
static char *catalog_get(pkg_out_fn out) {
    char *buf = catalog_load_cached();
    if (buf) return buf;
    return catalog_refresh(out);
}

/* ── sha compare ────────────────────────────────────────────── */
static int sha_eq(const uint8_t a[32], const uint8_t b[32]) {
    for (int i = 0; i < 32; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* ── CPIO newc extraction (kind:sysroot) ────────────────────── */
typedef struct {
    char magic[6]; char ino[8]; char mode[8]; char uid[8]; char gid[8];
    char nlink[8]; char mtime[8]; char filesize[8]; char devmajor[8];
    char devminor[8]; char rdevmajor[8]; char rdevminor[8];
    char namesize[8]; char check[8];
} cpio_hdr_t;   /* 110 bytes */

static uint32_t hex8(const char *s) {
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) { int d = hexnib(s[i]); if (d < 0) d = 0; v = (v << 4) | (uint32_t)d; }
    return v;
}

/* Extract a CPIO newc archive [base,size) into OsitoFS, each entry written as
 * "<dest>/<relative-name>". Returns number of regular files extracted. */
static int cpio_extract(const uint8_t *base, uint64_t size, const char *dest, pkg_out_fn out) {
    char dpre[160];
    scpy(dpre, dest, sizeof dpre);
    { unsigned l = slen(dpre); if (l && dpre[l - 1] != '/' && l + 1 < sizeof dpre) { dpre[l] = '/'; dpre[l + 1] = 0; } }

    const uint8_t *p = base, *end = base + size;
    int count = 0;
    while (p + sizeof(cpio_hdr_t) <= end) {
        const cpio_hdr_t *h = (const cpio_hdr_t *)p;
        if (!(h->magic[0] == '0' && h->magic[1] == '7' && h->magic[2] == '0' &&
              h->magic[3] == '7' && h->magic[4] == '0' && h->magic[5] == '1')) break;
        uint32_t namesize = hex8(h->namesize);
        uint32_t filesize = hex8(h->filesize);
        uint32_t mode     = hex8(h->mode);
        const char *name = (const char *)(p + sizeof(cpio_hdr_t));
        uint32_t hpn = (uint32_t)sizeof(cpio_hdr_t) + namesize; hpn = (hpn + 3) & ~3u;
        const uint8_t *data = p + hpn;
        uint32_t dpad = (filesize + 3) & ~3u;

        if (namesize == 11 && name[0] == 'T' && name[1] == 'R' && name[2] == 'A' &&
            name[3] == 'I' && name[4] == 'L' && name[5] == 'E' && name[6] == 'R') break;
        if (data + filesize > end) break;   /* corrupt/truncated */

        if ((mode & 0xF000u) == 0x8000u && filesize > 0) {     /* regular file */
            const char *fn = name;
            if (fn[0] == '.' && fn[1] == '/') fn += 2;
            while (fn[0] == '/') fn++;
            if (fn[0]) {
                char full[224];
                scpy(full, dpre, sizeof full);
                scat(full, fn, sizeof full);
                osfs2_delete(full);
                void *f = osfs2_create(full, filesize);
                if (f) { osfs2_write(f, 0, data, filesize); count++; }
                else { out("pkg: create failed: "); out(full); out("\n"); }
            }
        }
        p += hpn + dpad;
    }
    return count;
}

/* Install a kind:sysroot package: fetch zlib-compressed CPIO, verify sha,
 * inflate, extract to dest. A marker pkg/.sysroot-<name> makes it idempotent. */
static int install_sysroot(pkg_entry_t *e, int force, pkg_out_fn out) {
    /* dest MAY be empty: a sysroot whose CPIO carries full paths (e.g.
     * "usr/bin/gcc") extracts at the FS root — cpio_extract uses each
     * entry's own name verbatim when dest is "". Only usize is mandatory. */
    if (!e->usize)   { out("pkg: sysroot '"); out(e->name); out("' missing 'usize'\n"); return -1; }

    char marker[96];
    scpy(marker, "pkg/.sysroot-", sizeof marker);
    scat(marker, e->name, sizeof marker);
    if (!force && osfs2_find(marker)) return 0;     /* already extracted */

    char path[224];
    scpy(path, PKG_BASE, sizeof path);
    scat(path, e->file, sizeof path);
    out("pkg: install "); out(e->name); out(" (sysroot) <- "); out(PKG_HOST); out(path); out("\n");

    const char *tmp = "pkg/.sysroot.tmp";
    uint8_t got[32];
    if (fetch_to_osfs(PKG_HOST, path, tmp, e->size, got, out) < 0) return -1;
    if (e->has_sha && !sha_eq(got, e->sha256)) {
        out("pkg: SHA-256 MISMATCH for "); out(e->name); out(" — abort\n");
        osfs2_delete(tmp); return -1;
    }

    void *tf = osfs2_find(tmp);
    uint64_t csz = tf ? osfs2_file_size(tf) : 0;
    if (!csz) { out("pkg: temp read failed\n"); osfs2_delete(tmp); return -1; }
    uint8_t *cbuf = (uint8_t *)kmalloc(csz);
    if (!cbuf || osfs2_read(tf, 0, cbuf, csz) < 0) {
        out("pkg: out of memory / read failed\n");
        if (cbuf) kfree(cbuf);
        osfs2_delete(tmp); return -1;
    }

    uint32_t usz = (uint32_t)e->usize;
    uint8_t *ubuf = (uint8_t *)kmalloc(usz);
    if (!ubuf) {
        out("pkg: out of memory (usize "); emit_num(out, usz); out(")\n");
        kfree(cbuf); osfs2_delete(tmp); return -1;
    }
    uint32_t outlen = usz;
    int zr = zlib_inflate(cbuf, (uint32_t)csz, ubuf, &outlen);
    kfree(cbuf);
    if (zr != 0) {
        out("pkg: decompress failed (");
        out(zr == -2 ? "overflow — usize too small?" : zr == -3 ? "checksum" : "format");
        out(")\n");
        kfree(ubuf); osfs2_delete(tmp); return -1;
    }

    out("pkg: extracting "); emit_num(out, outlen); out(" bytes -> "); out(e->dest); out("\n");
    int n = cpio_extract(ubuf, outlen, e->dest, out);
    kfree(ubuf);
    osfs2_delete(tmp);

    if (n <= 0) { out("pkg: no files extracted (not a CPIO newc archive?)\n"); disk_flush(); return -1; }

    void *mf = osfs2_create(marker, 1);
    if (mf) { uint8_t one = 1; osfs2_write(mf, 0, &one, 1); }
    disk_flush();

    out("pkg: installed "); out(e->name);
    if (e->has_sha) out(" (sha256 ok)");
    out(" — "); emit_num(out, (uint64_t)n); out(" files\n");
    return 0;
}

/* ── Install one package (resolves deps, verifies sha) ──────── */
static int install_one(const char *cat, const char *name, int depth, int force, pkg_out_fn out) {
    if (depth > PKG_DEP_DEPTH) { out("pkg: dependency depth exceeded\n"); return -1; }

    pkg_entry_t e;
    if (!catalog_find(cat, name, &e)) {
        out("pkg: not in catalog: "); out(name); out("\n");
        return -1;
    }

    /* Resolve deps first (iterate the "deps" array of this entry's window). */
    {
        const char *p = jkey(e.win, e.win_end, "deps");
        if (p) {
            p += 1 + 4 + 1;                 /* past `"deps"` */
            while (p < e.win_end && *p != '[') { if (*p == '}' || *p == ',') { p = e.win_end; break; } p++; }
            if (p < e.win_end) {
                p++;
                while (p < e.win_end && *p != ']') {
                    if (*p == '"') {
                        char dep[64]; int i = 0; p++;
                        while (p < e.win_end && *p != '"' && i < 63) dep[i++] = *p++;
                        dep[i] = 0;
                        if (dep[0] && install_one(cat, dep, depth + 1, force, out) < 0) return -1;
                        if (p < e.win_end) p++;
                    } else p++;
                }
            }
        }
    }

    /* Destination path by kind. */
    char dest[160];
    int is_bin = (strcmp(e.kind, "bin") == 0);
    int is_lib = (strcmp(e.kind, "lib") == 0);
    if (is_bin) {
        scpy(dest, "pkg/", sizeof dest);
        scat(dest, e.file, sizeof dest);
    } else if (is_lib) {
        const char *d = e.dest;
        if (d[0] == '/') d++;
        if (!d[0]) { out("pkg: lib package missing 'dest': "); out(name); out("\n"); return -1; }
        scpy(dest, d, sizeof dest);
    } else if (strcmp(e.kind, "sysroot") == 0) {
        return install_sysroot(&e, force, out);
    } else {
        out("pkg: unknown kind '"); out(e.kind); out("'\n");
        return -1;
    }

    /* Already installed at the right size? Skip unless forced. */
    if (!force) {
        void *ex = osfs2_find(dest);
        if (ex && (e.size == 0 || osfs2_file_size(ex) == e.size)) {
            return 0;   /* satisfied (quiet, so deps don't spam) */
        }
    }

    /* Build URL path = PKG_BASE + file. */
    char path[224];
    scpy(path, PKG_BASE, sizeof path);
    scat(path, e.file, sizeof path);

    out("pkg: install "); out(e.name);
    out(" ("); out(e.kind); out(") <- "); out(PKG_HOST); out(path); out("\n");

    uint8_t got_sha[32];
    if (fetch_to_osfs(PKG_HOST, path, dest, e.size, got_sha, out) < 0)
        return -1;

    /* Verify SHA-256. */
    if (e.has_sha && !sha_eq(got_sha, e.sha256)) {
        out("pkg: SHA-256 MISMATCH for "); out(e.name); out(" — deleting\n");
        osfs2_delete(dest);
        return -1;
    }

    /* ELF-magic sanity for executables. */
    if (is_bin) {
        uint8_t m[4] = {0};
        void *f = osfs2_find(dest);
        if (f) osfs2_read(f, 0, m, 4);
        if (!(m[0] == 0x7F && m[1] == 'E' && m[2] == 'L' && m[3] == 'F')) {
            out("pkg: "); out(e.name); out(" is not ELF — deleting\n");
            osfs2_delete(dest);
            return -1;
        }
    }

    out("pkg: installed "); out(e.name);
    if (e.has_sha) out(" (sha256 ok)");
    out(" -> "); out(dest); out("\n");
    return 0;
}

/* Run an installed (or auto-installed) bin package. argv0_skip = number of
 * leading argv entries to drop (e.g. "pkg run name" -> 3). */
static int run_pkg(const char *cat, const char *name, int argc, char **argv,
                   int argv0_skip, pkg_out_fn out) {
    pkg_entry_t e;
    if (!catalog_find(cat, name, &e)) { out("pkg: not in catalog: "); out(name); out("\n"); return -1; }
    if (strcmp(e.kind, "bin") != 0) { out("pkg: not runnable (kind "); out(e.kind); out(")\n"); return -1; }

    char dest[160];
    scpy(dest, "pkg/", sizeof dest);
    scat(dest, e.file, sizeof dest);

    if (!osfs2_find(dest)) {
        if (install_one(cat, name, 0, 0, out) < 0) return -1;
    }
    if (!osfs2_find(dest)) { out("pkg: install did not produce "); out(dest); out("\n"); return -1; }

    const char *xargv[16];
    int xc = 0;
    xargv[xc++] = dest;
    for (int i = argv0_skip; i < argc && xc < 15; i++) xargv[xc++] = argv[i];
    xargv[xc] = 0;
    return proc_exec(dest, xc, xargv);
}

/* ── Subcommands: list / search ─────────────────────────────── */
static void cmd_list(const char *cat, const char *query, pkg_out_fn out) {
    const char *end = cat + slen(cat);
    const char *p = cat;
    int matches = 0;
    for (;;) {
        const char *o = jkey(p, end, "name");
        if (!o) break;
        const char *o2 = jkey(o + 1, end, "name");
        const char *wend = o2 ? o2 : end;
        char nm[64], kind[12], desc[160];
        if (jstr(o, wend, "name", nm, sizeof nm) > 0) {
            if (jstr(o, wend, "kind", kind, sizeof kind) <= 0) scpy(kind, "bin", sizeof kind);
            desc[0] = 0; jstr(o, wend, "desc", desc, sizeof desc);
            int show = 1;
            if (query && query[0]) {
                /* substring match on name or desc */
                show = 0;
                for (const char *a = nm; *a && !show; a++) { const char *x = a, *y = query; while (*x && *y && *x == *y) { x++; y++; } if (!*y) show = 1; }
                for (const char *a = desc; *a && !show; a++) { const char *x = a, *y = query; while (*x && *y && *x == *y) { x++; y++; } if (!*y) show = 1; }
            }
            if (show) {
                out("  "); out(nm);
                out(" ["); out(kind); out("]");
                if (desc[0]) { out(" — "); out(desc); }
                out("\n");
                matches++;
            }
        }
        if (!o2) break;
        p = o2;
    }
    if (!matches) out("pkg: no matches\n");
}

static void cmd_installed(pkg_out_fn out) {
    int count = 0;
    for (int i = 0; i < 4096; i++) {
        void *f = osfs2_get_file(i);
        if (!f) continue;
        const char *nm = osfs2_file_name(f);
        if (!nm) continue;
        if (nm[0] == 'p' && nm[1] == 'k' && nm[2] == 'g' && nm[3] == '/' &&
            strcmp(nm, PKG_CACHE) != 0) {   /* skip the cached catalog */
            out("  "); out(nm);
            out(" ("); emit_num(out, osfs2_file_size(f)); out(" B)\n");
            count++;
        }
    }
    if (!count) out("pkg: nothing installed\n");
}

static void cmd_info(const char *cat, const char *name, pkg_out_fn out) {
    pkg_entry_t e;
    if (!catalog_find(cat, name, &e)) { out("pkg: not in catalog: "); out(name); out("\n"); return; }
    out("name:  "); out(e.name); out("\n");
    out("kind:  "); out(e.kind); out("\n");
    out("file:  "); out(PKG_HOST); out(PKG_BASE); out(e.file); out("\n");
    out("size:  "); emit_num(out, e.size); out(" bytes\n");
    out("sha:   "); out(e.has_sha ? "present" : "(none)"); out("\n");
    if (e.dest[0]) { out("dest:  "); out(e.dest); out("\n"); }
    if (e.desc[0]) { out("desc:  "); out(e.desc); out("\n"); }
}

static void cmd_help(pkg_out_fn out) {
    out("pkg — install and run binaries from naranjositos.tech\n\n");
    out("  pkg refresh              fetch + cache the catalog\n");
    out("  pkg list                 list available packages\n");
    out("  pkg search <q>           substring filter on name/desc\n");
    out("  pkg info <name>          show package metadata\n");
    out("  pkg install <name>       download + verify + install (with deps)\n");
    out("  pkg installed            list locally-installed packages\n");
    out("  pkg run <name> [args]    install if needed, then run\n");
    out("  pkg uninstall <name>     remove a local package\n");
    out("\nAbsent commands that match a package auto-install + run (lazy).\n");
}

/* ── Public: pkg command ────────────────────────────────────── */
int pkg_cmd(int argc, char **argv, pkg_out_fn out) {
    const char *sub = (argc >= 2) ? argv[1] : "help";

    if (strcmp(sub, "help") == 0) { cmd_help(out); return 0; }

    if (!osfs2_is_mounted()) { out("pkg: no filesystem mounted\n"); return -1; }

    if (strcmp(sub, "refresh") == 0) {
        char *cat = catalog_refresh(out);
        if (!cat) { out("pkg: catalog refresh failed\n"); return -1; }
        out("pkg: catalog cached ("); emit_num(out, slen(cat)); out(" bytes)\n");
        kfree(cat);
        return 0;
    }
    if (strcmp(sub, "installed") == 0) { cmd_installed(out); return 0; }
    if (strcmp(sub, "uninstall") == 0 || strcmp(sub, "rm") == 0) {
        if (argc < 3) { out("usage: pkg uninstall <name>\n"); return -1; }
        char *cat = catalog_load_cached();
        char dest[160];
        pkg_entry_t e;
        if (cat && catalog_find(cat, argv[2], &e) && strcmp(e.kind, "bin") == 0) {
            scpy(dest, "pkg/", sizeof dest); scat(dest, e.file, sizeof dest);
        } else {
            scpy(dest, "pkg/", sizeof dest); scat(dest, argv[2], sizeof dest);
        }
        if (cat) kfree(cat);
        if (osfs2_delete(dest) == 0) { out("pkg: removed "); out(dest); out("\n"); disk_flush(); }
        else { out("pkg: not installed: "); out(argv[2]); out("\n"); }
        return 0;
    }

    /* Subcommands needing the catalog. */
    char *cat = catalog_get(out);
    if (!cat) { out("pkg: no catalog (try 'pkg refresh' with network up)\n"); return -1; }
    int rc = 0;

    if (strcmp(sub, "list") == 0) {
        cmd_list(cat, 0, out);
    } else if (strcmp(sub, "search") == 0) {
        cmd_list(cat, (argc >= 3) ? argv[2] : 0, out);
    } else if (strcmp(sub, "info") == 0) {
        if (argc < 3) { out("usage: pkg info <name>\n"); rc = -1; }
        else cmd_info(cat, argv[2], out);
    } else if (strcmp(sub, "install") == 0) {
        if (argc < 3) { out("usage: pkg install <name>\n"); rc = -1; }
        else rc = install_one(cat, argv[2], 0, 0, out);
    } else if (strcmp(sub, "run") == 0) {
        if (argc < 3) { out("usage: pkg run <name> [args]\n"); rc = -1; }
        else rc = run_pkg(cat, argv[2], argc, argv, 3, out);
    } else {
        out("pkg: unknown subcommand '"); out(sub); out("' (try 'pkg help')\n");
        rc = -1;
    }

    kfree(cat);
    return rc;
}

/* ── Public: lazy-load hook ─────────────────────────────────── */
int pkg_lazy_try(const char *cmd, int argc, char **argv, pkg_out_fn out) {
    static int in_lazy = 0;
    if (in_lazy) return 0;                    /* re-entrancy guard */
    if (!cmd || !cmd[0]) return 0;
    if (!osfs2_is_mounted()) return 0;

    char *cat = catalog_load_cached();        /* cached only — no network for typos */
    if (!cat) return 0;

    pkg_entry_t e;
    int known = catalog_find(cat, cmd, &e) && strcmp(e.kind, "bin") == 0;
    if (!known) { kfree(cat); return 0; }

    in_lazy = 1;
    out("[pkg] '"); out(cmd); out("' is a package — fetching...\n");
    /* run_pkg with argv0_skip=1 so argv[1..] are passed through to the program */
    run_pkg(cat, cmd, argc, argv, 1, out);
    in_lazy = 0;
    kfree(cat);
    return 1;                                  /* handled (installed/ran or reported error) */
}
