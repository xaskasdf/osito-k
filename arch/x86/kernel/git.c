/*
 * OsitoK x86-64 — Git-Compatible Version Control
 *
 * Implements standard git object model on OsitoFS flat namespace.
 * Objects stored as .git/objects/XX/YYY... (zlib-compressed, SHA-1 addressed).
 * Refs stored as .git/refs/heads/<branch> (text files with SHA-1 hex).
 * Index stored as .git/index (simple text: "mode sha1 name\n" per entry).
 *
 * All object formats (blob, tree, commit) are byte-compatible with
 * standard git — you can clone this repo on any machine.
 */

#include "git.h"
#include "crypto.h"
#include "zlib.h"

/* ── External functions ─────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);

extern void fb_puts(const char *s);
extern void fb_puts_color(const char *s, uint32_t color);
extern void fb_putc(char c, uint32_t color);
extern void fb_putdec(uint64_t val);

/* OsitoFS */
extern void *osfs2_find(const char *name);
extern int   osfs2_read(void *file, uint64_t offset, void *buf, uint64_t len);
extern int   osfs2_write(void *file, uint64_t offset, const void *buf, uint64_t len);
extern void *osfs2_create(const char *name, uint64_t size);
extern int   osfs2_delete(const char *name);
extern uint64_t osfs2_file_size(void *file);
extern void *osfs2_file_at(uint32_t index);
extern const char *osfs2_file_name(void *file);

/* Heap */
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);

/* ── Helpers ─────────────────────────────────────────────────── */

static int git_strlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void git_strcpy(char *dst, const char *src)
{
    while ((*dst++ = *src++));
}

static int git_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static int git_strncmp(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

static void git_memcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

static int git_memcmp(const void *a, const void *b, uint32_t n)
{
    const uint8_t *pa = (const uint8_t *)a, *pb = (const uint8_t *)b;
    for (uint32_t i = 0; i < n; i++)
        if (pa[i] != pb[i]) return pa[i] - pb[i];
    return 0;
}

/* Integer to decimal string, returns length */
static int itoa_dec(uint32_t val, char *buf)
{
    char tmp[16];
    int n = 0;
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    while (val) { tmp[n++] = '0' + (val % 10); val /= 10; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = '\0';
    return n;
}

/* ── SHA-1 hex conversion ───────────────────────────────────── */

static const char hex_chars[] = "0123456789abcdef";

void git_sha1_to_hex(const uint8_t sha1[20], char hex[41])
{
    for (int i = 0; i < 20; i++) {
        hex[i * 2]     = hex_chars[sha1[i] >> 4];
        hex[i * 2 + 1] = hex_chars[sha1[i] & 0xF];
    }
    hex[40] = '\0';
}

int git_hex_to_sha1(const char *hex, uint8_t sha1[20])
{
    for (int i = 0; i < 20; i++) {
        uint8_t hi, lo;
        char c;

        c = hex[i * 2];
        if (c >= '0' && c <= '9')      hi = c - '0';
        else if (c >= 'a' && c <= 'f') hi = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') hi = c - 'A' + 10;
        else return -1;

        c = hex[i * 2 + 1];
        if (c >= '0' && c <= '9')      lo = c - '0';
        else if (c >= 'a' && c <= 'f') lo = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') lo = c - 'A' + 10;
        else return -1;

        sha1[i] = (hi << 4) | lo;
    }
    return 0;
}

/* ── OsitoFS wrappers for git paths ─────────────────────────── */

/* Read entire small file into buffer. Returns size or -1. */
static int git_read_file(const char *path, void *buf, uint32_t max)
{
    void *f = osfs2_find(path);
    if (!f) return -1;
    uint64_t size = osfs2_file_size(f);
    if (size > max) return -1;
    if (osfs2_read(f, 0, buf, size) < 0) return -1;
    return (int)size;
}

/* Write data to a new file (delete old if exists). Returns 0 or -1. */
static int git_write_file(const char *path, const void *data, uint32_t len)
{
    /* Delete existing */
    if (osfs2_find(path))
        osfs2_delete(path);

    void *f = osfs2_create(path, len);
    if (!f) return -1;
    return osfs2_write(f, 0, data, len);
}

/* Check if file exists */
static int git_file_exists(const char *path)
{
    return osfs2_find(path) != NULL;
}

/* ── Object store ───────────────────────────────────────────── */

/* Build object path: .git/objects/XX/YYYY...YYYY */
static void object_path(const uint8_t sha1[20], char *path)
{
    /* ".git/objects/" + 2 hex + "/" + 38 hex + NUL = 56 chars */
    char hex[41];
    git_sha1_to_hex(sha1, hex);

    git_strcpy(path, ".git/objects/");
    path[13] = hex[0];
    path[14] = hex[1];
    path[15] = '/';
    git_memcpy(path + 16, hex + 2, 38);
    path[54] = '\0';
}

/* Hash and store a git object. Returns 0 on success.
 * type_str: "blob", "tree", "commit"
 * data: raw object content
 * Computes SHA-1 of "type size\0data", zlib-compresses, stores. */
static int object_write(const char *type_str, const uint8_t *data, uint32_t data_len,
                         uint8_t sha1_out[20])
{
    /* Build header: "type size\0" */
    char header[64];
    int hlen = 0;
    const char *p = type_str;
    while (*p) header[hlen++] = *p++;
    header[hlen++] = ' ';
    char szbuf[16];
    int szlen = itoa_dec(data_len, szbuf);
    git_memcpy(header + hlen, szbuf, szlen);
    hlen += szlen;
    header[hlen++] = '\0'; /* NUL separator */

    /* SHA-1 of header + data */
    sha1_ctx ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, header, hlen);
    sha1_update(&ctx, data, data_len);
    sha1_final(&ctx, sha1_out);

    /* Check if object already exists */
    char path[64];
    object_path(sha1_out, path);
    if (git_file_exists(path)) return 0; /* already have it */

    /* Compress header + data */
    uint32_t raw_len = hlen + data_len;
    uint8_t *raw = (uint8_t *)kmalloc(raw_len);
    if (!raw) return -1;
    git_memcpy(raw, header, hlen);
    git_memcpy(raw + hlen, data, data_len);

    uint32_t comp_max = raw_len + raw_len / 100 + 64;
    uint8_t *comp = (uint8_t *)kmalloc(comp_max);
    if (!comp) { kfree(raw); return -1; }

    uint32_t comp_len = comp_max;
    if (zlib_deflate(raw, raw_len, comp, &comp_len) < 0) {
        kfree(raw);
        kfree(comp);
        return -1;
    }

    kfree(raw);

    /* Store to .git/objects/XX/YYY... */
    int ret = git_write_file(path, comp, comp_len);
    kfree(comp);
    return ret;
}

/* Read a git object. Caller must kfree(*out_data).
 * Returns object type string in type_buf, data in *out_data, size in *out_len. */
static int object_read(const uint8_t sha1[20], char *type_buf,
                        uint8_t **out_data, uint32_t *out_len)
{
    char path[64];
    object_path(sha1, path);

    void *f = osfs2_find(path);
    if (!f) return -1;

    uint64_t fsize = osfs2_file_size(f);
    if (fsize == 0 || fsize > 16 * 1024 * 1024) return -1;

    uint8_t *comp = (uint8_t *)kmalloc(fsize);
    if (!comp) return -1;
    if (osfs2_read(f, 0, comp, fsize) < 0) { kfree(comp); return -1; }

    /* Decompress — allocate generous output buffer */
    uint32_t dec_max = (uint32_t)(fsize * 4);
    if (dec_max < 4096) dec_max = 4096;

retry:;
    uint8_t *dec = (uint8_t *)kmalloc(dec_max);
    if (!dec) { kfree(comp); return -1; }

    uint32_t dec_len = dec_max;
    int ret = zlib_inflate(comp, (uint32_t)fsize, dec, &dec_len);
    if (ret == -2 && dec_max < 64 * 1024 * 1024) {
        /* Output overflow — try larger buffer */
        kfree(dec);
        dec_max *= 4;
        goto retry;
    }
    kfree(comp);
    if (ret < 0) { kfree(dec); return -1; }

    /* Parse header: "type size\0data" */
    int hi = 0;
    while (hi < (int)dec_len && dec[hi] != ' ' && hi < 15)
        type_buf[hi] = (char)dec[hi], hi++;
    type_buf[hi] = '\0';
    hi++; /* skip space */

    /* Parse size */
    uint32_t obj_size = 0;
    while (hi < (int)dec_len && dec[hi] != '\0') {
        obj_size = obj_size * 10 + (dec[hi] - '0');
        hi++;
    }
    hi++; /* skip NUL */

    /* Extract data */
    uint32_t data_len = dec_len - hi;
    *out_data = (uint8_t *)kmalloc(data_len + 1);
    if (!*out_data) { kfree(dec); return -1; }
    git_memcpy(*out_data, dec + hi, data_len);
    (*out_data)[data_len] = '\0';
    *out_len = data_len;

    kfree(dec);
    return 0;
}

/* ── Index (staging area) ───────────────────────────────────── */

static git_index_t staging;

/* Load index from .git/index (text format: "mode sha1hex name\n") */
static int index_load(void)
{
    staging.count = 0;
    char buf[16384];
    int len = git_read_file(".git/index", buf, sizeof(buf) - 1);
    if (len <= 0) return 0;
    buf[len] = '\0';

    char *p = buf;
    while (*p && staging.count < GIT_MAX_INDEX) {
        git_index_entry_t *e = &staging.entries[staging.count];

        /* Parse mode */
        e->mode = 0;
        while (*p >= '0' && *p <= '9') {
            e->mode = e->mode * 8 + (*p - '0'); /* octal */
            p++;
        }
        if (*p == ' ') p++;

        /* Parse SHA-1 hex */
        char hex[41];
        git_memcpy(hex, p, 40);
        hex[40] = '\0';
        git_hex_to_sha1(hex, e->sha1);
        p += 40;
        if (*p == ' ') p++;

        /* Parse name */
        int ni = 0;
        while (*p && *p != '\n' && ni < 63)
            e->name[ni++] = *p++;
        e->name[ni] = '\0';
        if (*p == '\n') p++;

        staging.count++;
    }
    return 0;
}

/* Save index to .git/index */
static int index_save(void)
{
    char buf[16384];
    int pos = 0;

    for (int i = 0; i < staging.count; i++) {
        git_index_entry_t *e = &staging.entries[i];

        /* Mode (octal) */
        char mode[8];
        uint32_t m = e->mode;
        int mi = 0;
        char tmp[8];
        if (m == 0) { mode[0] = '0'; mi = 1; }
        else {
            while (m > 0) { tmp[mi++] = '0' + (m & 7); m >>= 3; }
            for (int j = 0; j < mi; j++) mode[j] = tmp[mi - 1 - j];
        }
        git_memcpy(buf + pos, mode, mi);
        pos += mi;
        buf[pos++] = ' ';

        /* SHA-1 hex */
        char hex[41];
        git_sha1_to_hex(e->sha1, hex);
        git_memcpy(buf + pos, hex, 40);
        pos += 40;
        buf[pos++] = ' ';

        /* Name */
        int nlen = git_strlen(e->name);
        git_memcpy(buf + pos, e->name, nlen);
        pos += nlen;
        buf[pos++] = '\n';
    }

    return git_write_file(".git/index", buf, pos);
}

/* Find entry in index by name */
static int index_find(const char *name)
{
    for (int i = 0; i < staging.count; i++)
        if (git_strcmp(staging.entries[i].name, name) == 0)
            return i;
    return -1;
}

/* ── Refs ────────────────────────────────────────────────────── */

/* Read HEAD — returns ref name (e.g. "refs/heads/master") or raw SHA-1 hex */
static int read_head(char *ref_out, int max)
{
    char buf[128];
    int len = git_read_file(".git/HEAD", buf, sizeof(buf) - 1);
    if (len <= 0) return -1;
    buf[len] = '\0';

    /* Strip trailing newline */
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';

    if (git_strncmp(buf, "ref: ", 5) == 0) {
        /* Symbolic ref */
        int rlen = len - 5;
        if (rlen >= max) rlen = max - 1;
        git_memcpy(ref_out, buf + 5, rlen);
        ref_out[rlen] = '\0';
        return 0;
    }

    /* Direct SHA-1 */
    if (len >= max) len = max - 1;
    git_memcpy(ref_out, buf, len);
    ref_out[len] = '\0';
    return 1; /* 1 = detached HEAD */
}

/* Resolve ref to SHA-1 hash. Returns 0 on success, -1 if ref doesn't exist. */
static int resolve_ref(const char *ref, uint8_t sha1[20])
{
    /* Build path: .git/ + ref (e.g. ".git/refs/heads/master") */
    char path[64];
    int pi = 0;
    const char *prefix = ".git/";
    while (*prefix) path[pi++] = *prefix++;
    const char *rp = ref;
    while (*rp && pi < 62) path[pi++] = *rp++;
    path[pi] = '\0';

    char buf[64];
    int len = git_read_file(path, buf, sizeof(buf) - 1);
    if (len <= 0) return -1;
    buf[len] = '\0';

    /* Strip newline */
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';

    return git_hex_to_sha1(buf, sha1);
}

/* Write ref (SHA-1 hex + newline) */
static int write_ref(const char *ref, const uint8_t sha1[20])
{
    char path[64];
    int pi = 0;
    const char *prefix = ".git/";
    while (*prefix) path[pi++] = *prefix++;
    const char *rp = ref;
    while (*rp && pi < 62) path[pi++] = *rp++;
    path[pi] = '\0';

    char content[42];
    git_sha1_to_hex(sha1, content);
    content[40] = '\n';

    return git_write_file(path, content, 41);
}

/* ── Tree objects ────────────────────────────────────────────── */

/* Build tree object from index.
 * Tree format: entries of "mode name\0sha1_raw" concatenated. */
static int tree_write(uint8_t sha1_out[20])
{
    /* Calculate tree size */
    uint32_t tree_size = 0;
    for (int i = 0; i < staging.count; i++) {
        /* mode(octal) + ' ' + name + '\0' + sha1(20) */
        uint32_t m = staging.entries[i].mode;
        int ml = 0;
        if (m == 0) ml = 1;
        else { while (m > 0) { ml++; m >>= 3; } }
        tree_size += ml + 1 + git_strlen(staging.entries[i].name) + 1 + 20;
    }

    uint8_t *tree_data = (uint8_t *)kmalloc(tree_size);
    if (!tree_data) return -1;

    uint32_t pos = 0;
    for (int i = 0; i < staging.count; i++) {
        git_index_entry_t *e = &staging.entries[i];

        /* Mode (octal, no leading zeros except "100644") */
        char mode[8];
        uint32_t m = e->mode;
        int ml = 0;
        char tmp[8];
        if (m == 0) { mode[0] = '0'; ml = 1; }
        else {
            while (m > 0) { tmp[ml++] = '0' + (m & 7); m >>= 3; }
            for (int j = 0; j < ml; j++) mode[j] = tmp[ml - 1 - j];
        }

        git_memcpy(tree_data + pos, mode, ml);
        pos += ml;
        tree_data[pos++] = ' ';

        int nlen = git_strlen(e->name);
        git_memcpy(tree_data + pos, e->name, nlen);
        pos += nlen;
        tree_data[pos++] = '\0';

        git_memcpy(tree_data + pos, e->sha1, 20);
        pos += 20;
    }

    int ret = object_write("tree", tree_data, pos, sha1_out);
    kfree(tree_data);
    return ret;
}

/* Parse tree object, call callback for each entry */
typedef void (*tree_walk_fn)(uint32_t mode, const char *name, const uint8_t *sha1, void *ctx);

static int tree_walk(const uint8_t *tree_sha1, tree_walk_fn fn, void *ctx)
{
    char type[16];
    uint8_t *data;
    uint32_t len;
    if (object_read(tree_sha1, type, &data, &len) < 0) return -1;
    if (git_strcmp(type, "tree") != 0) { kfree(data); return -1; }

    uint32_t pos = 0;
    while (pos < len) {
        /* Parse mode */
        uint32_t mode = 0;
        while (pos < len && data[pos] != ' ') {
            mode = mode * 8 + (data[pos] - '0');
            pos++;
        }
        pos++; /* skip space */

        /* Parse name */
        const char *name = (const char *)(data + pos);
        while (pos < len && data[pos] != '\0') pos++;
        pos++; /* skip NUL */

        /* SHA-1 (20 bytes raw) */
        const uint8_t *sha1 = data + pos;
        pos += 20;

        fn(mode, name, sha1, ctx);
    }

    kfree(data);
    return 0;
}

/* ── Commit objects ──────────────────────────────────────────── */

/* Build and write commit object */
static int commit_write(const uint8_t tree_sha1[20],
                         const uint8_t *parent_sha1, /* NULL if initial commit */
                         const char *message,
                         uint8_t sha1_out[20])
{
    char buf[4096];
    int pos = 0;

    /* tree <sha1hex>\n */
    git_memcpy(buf + pos, "tree ", 5); pos += 5;
    char hex[41];
    git_sha1_to_hex(tree_sha1, hex);
    git_memcpy(buf + pos, hex, 40); pos += 40;
    buf[pos++] = '\n';

    /* parent <sha1hex>\n (if not initial) */
    if (parent_sha1) {
        git_memcpy(buf + pos, "parent ", 7); pos += 7;
        git_sha1_to_hex(parent_sha1, hex);
        git_memcpy(buf + pos, hex, 40); pos += 40;
        buf[pos++] = '\n';
    }

    /* author and committer (fixed identity for bare-metal) */
    const char *author = "author OsitoK <osito@bare-metal> 0 +0000\n";
    int alen = git_strlen(author);
    git_memcpy(buf + pos, author, alen); pos += alen;

    const char *committer = "committer OsitoK <osito@bare-metal> 0 +0000\n";
    int clen = git_strlen(committer);
    git_memcpy(buf + pos, committer, clen); pos += clen;

    /* blank line + message */
    buf[pos++] = '\n';
    int mlen = git_strlen(message);
    git_memcpy(buf + pos, message, mlen); pos += mlen;
    buf[pos++] = '\n';

    return object_write("commit", (const uint8_t *)buf, pos, sha1_out);
}

/* Parse commit object fields */
typedef struct {
    uint8_t tree[20];
    uint8_t parent[20];
    bool    has_parent;
    char    author[128];
    char    message[512];
} commit_info_t;

static int commit_parse(const uint8_t sha1[20], commit_info_t *info)
{
    char type[16];
    uint8_t *data;
    uint32_t len;
    if (object_read(sha1, type, &data, &len) < 0) return -1;
    if (git_strcmp(type, "commit") != 0) { kfree(data); return -1; }

    info->has_parent = false;
    info->author[0] = '\0';
    info->message[0] = '\0';

    char *p = (char *)data;
    char *end = p + len;

    while (p < end) {
        if (*p == '\n') {
            /* Blank line = start of message */
            p++;
            int mi = 0;
            while (p < end && mi < 511) {
                info->message[mi++] = *p++;
            }
            /* Strip trailing newline */
            while (mi > 0 && (info->message[mi - 1] == '\n' || info->message[mi - 1] == '\r'))
                mi--;
            info->message[mi] = '\0';
            break;
        }

        if (git_strncmp(p, "tree ", 5) == 0) {
            git_hex_to_sha1(p + 5, info->tree);
            while (p < end && *p != '\n') p++;
            p++;
        } else if (git_strncmp(p, "parent ", 7) == 0) {
            git_hex_to_sha1(p + 7, info->parent);
            info->has_parent = true;
            while (p < end && *p != '\n') p++;
            p++;
        } else if (git_strncmp(p, "author ", 7) == 0) {
            int ai = 0;
            const char *ap = p + 7;
            while (ap < end && *ap != '\n' && ai < 127)
                info->author[ai++] = *ap++;
            info->author[ai] = '\0';
            while (p < end && *p != '\n') p++;
            p++;
        } else {
            /* Skip other headers */
            while (p < end && *p != '\n') p++;
            p++;
        }
    }

    kfree(data);
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 *  Public API — Git commands
 * ══════════════════════════════════════════════════════════════ */

int git_init(void)
{
    if (git_file_exists(".git/HEAD")) {
        fb_puts("  Already a git repository\n");
        return 0;
    }

    /* Create HEAD pointing to master */
    const char *head = "ref: refs/heads/master\n";
    if (git_write_file(".git/HEAD", head, git_strlen(head)) < 0) {
        fb_puts("  Error: cannot create .git/HEAD\n");
        return -1;
    }

    fb_puts("  Initialized empty git repository\n");
    serial_puts("[GIT] Initialized repository\n");
    return 0;
}

int git_add(const char *filename)
{
    if (!git_file_exists(".git/HEAD")) {
        fb_puts("  Not a git repository (run 'git init' first)\n");
        return -1;
    }

    /* Read the file from OsitoFS */
    void *f = osfs2_find(filename);
    if (!f) {
        fb_puts("  Error: file not found: ");
        fb_puts(filename);
        fb_puts("\n");
        return -1;
    }

    uint64_t size = osfs2_file_size(f);
    if (size > 4 * 1024 * 1024) {
        fb_puts("  Error: file too large (max 4MB)\n");
        return -1;
    }

    uint8_t *content = (uint8_t *)kmalloc(size + 1);
    if (!content) return -1;
    if (osfs2_read(f, 0, content, size) < 0) {
        kfree(content);
        return -1;
    }

    /* Create blob object */
    uint8_t sha1[20];
    if (object_write("blob", content, (uint32_t)size, sha1) < 0) {
        kfree(content);
        fb_puts("  Error: cannot create blob object\n");
        return -1;
    }
    kfree(content);

    /* Update index */
    index_load();

    int idx = index_find(filename);
    if (idx >= 0) {
        /* Update existing entry */
        git_memcpy(staging.entries[idx].sha1, sha1, 20);
    } else {
        /* Add new entry */
        if (staging.count >= GIT_MAX_INDEX) {
            fb_puts("  Error: index full\n");
            return -1;
        }
        git_index_entry_t *e = &staging.entries[staging.count++];
        git_memcpy(e->sha1, sha1, 20);
        e->mode = 0100644;
        int nlen = git_strlen(filename);
        if (nlen > 63) nlen = 63;
        git_memcpy(e->name, filename, nlen);
        e->name[nlen] = '\0';
    }

    index_save();

    char hex[41];
    git_sha1_to_hex(sha1, hex);
    fb_puts("  add ");
    fb_puts(filename);
    fb_puts(" [");
    hex[7] = '\0'; /* short hash */
    fb_puts(hex);
    fb_puts("]\n");

    return 0;
}

int git_commit(const char *message)
{
    if (!git_file_exists(".git/HEAD")) {
        fb_puts("  Not a git repository\n");
        return -1;
    }

    /* Load index */
    index_load();
    if (staging.count == 0) {
        fb_puts("  Nothing to commit (empty index)\n");
        return -1;
    }

    /* Write tree from index */
    uint8_t tree_sha1[20];
    if (tree_write(tree_sha1) < 0) {
        fb_puts("  Error: cannot write tree\n");
        return -1;
    }

    /* Resolve current HEAD for parent */
    char head_ref[64];
    int detached = read_head(head_ref, sizeof(head_ref));
    if (detached < 0) {
        fb_puts("  Error: cannot read HEAD\n");
        return -1;
    }

    uint8_t parent_sha1[20];
    bool has_parent = false;
    if (detached == 1) {
        /* Detached HEAD — direct SHA-1 */
        if (git_hex_to_sha1(head_ref, parent_sha1) == 0)
            has_parent = true;
    } else {
        /* Symbolic ref */
        if (resolve_ref(head_ref, parent_sha1) == 0)
            has_parent = true;
    }

    /* Write commit */
    uint8_t commit_sha1[20];
    if (commit_write(tree_sha1, has_parent ? parent_sha1 : NULL,
                      message, commit_sha1) < 0) {
        fb_puts("  Error: cannot write commit\n");
        return -1;
    }

    /* Update HEAD ref */
    if (detached == 1) {
        /* Update HEAD directly */
        char hex[42];
        git_sha1_to_hex(commit_sha1, hex);
        hex[40] = '\n';
        git_write_file(".git/HEAD", hex, 41);
    } else {
        /* Update branch ref */
        write_ref(head_ref, commit_sha1);
    }

    /* Show result */
    char hex[41];
    git_sha1_to_hex(commit_sha1, hex);
    hex[7] = '\0';
    fb_puts("  [");
    /* Branch name */
    const char *bn = head_ref;
    /* Find last / */
    const char *slash = bn;
    while (*bn) { if (*bn == '/') slash = bn + 1; bn++; }
    fb_puts(slash);
    fb_puts(" ");
    fb_puts(hex);
    fb_puts("] ");
    fb_puts(message);
    fb_puts("\n");

    fb_puts("  ");
    fb_putdec(staging.count);
    fb_puts(" file(s) committed\n");

    serial_puts("[GIT] Commit ");
    git_sha1_to_hex(commit_sha1, hex);
    serial_puts(hex);
    serial_puts(" \"");
    serial_puts(message);
    serial_puts("\"\n");

    return 0;
}

int git_log(void)
{
    if (!git_file_exists(".git/HEAD")) {
        fb_puts("  Not a git repository\n");
        return -1;
    }

    /* Resolve HEAD to commit */
    char head_ref[64];
    int detached = read_head(head_ref, sizeof(head_ref));
    if (detached < 0) {
        fb_puts("  Error: cannot read HEAD\n");
        return -1;
    }

    uint8_t sha1[20];
    if (detached == 1) {
        if (git_hex_to_sha1(head_ref, sha1) < 0) return -1;
    } else {
        if (resolve_ref(head_ref, sha1) < 0) {
            fb_puts("  No commits yet\n");
            return 0;
        }
    }

    /* Walk commit chain */
    int count = 0;
    while (count < 50) { /* limit to 50 commits */
        commit_info_t info;
        if (commit_parse(sha1, &info) < 0) break;

        char hex[41];
        git_sha1_to_hex(sha1, hex);

        /* commit SHA-1 */
        fb_puts_color("  commit ", 0x00CCCC00);
        fb_puts_color(hex, 0x00CCCC00);
        fb_puts("\n");

        /* Author */
        fb_puts("  Author: ");
        fb_puts(info.author);
        fb_puts("\n");

        /* Message */
        fb_puts("\n      ");
        fb_puts(info.message);
        fb_puts("\n\n");

        count++;

        if (!info.has_parent) break;
        git_memcpy(sha1, info.parent, 20);
    }

    if (count == 0) {
        fb_puts("  No commits yet\n");
    }

    return 0;
}

int git_status(void)
{
    if (!git_file_exists(".git/HEAD")) {
        fb_puts("  Not a git repository\n");
        return -1;
    }

    /* Show current branch */
    char head_ref[64];
    int detached = read_head(head_ref, sizeof(head_ref));
    if (detached == 0) {
        const char *bn = head_ref;
        const char *slash = bn;
        while (*bn) { if (*bn == '/') slash = bn + 1; bn++; }
        fb_puts("  On branch ");
        fb_puts_color(slash, 0x0000FF00);
        fb_puts("\n");
    } else if (detached == 1) {
        fb_puts("  HEAD detached at ");
        head_ref[7] = '\0';
        fb_puts(head_ref);
        fb_puts("\n");
    }

    /* Load index */
    index_load();

    /* Check if HEAD commit exists */
    uint8_t head_sha1[20];
    bool has_head = false;
    if (detached == 1) {
        has_head = (git_hex_to_sha1(head_ref, head_sha1) == 0);
    } else if (detached == 0) {
        has_head = (resolve_ref(head_ref, head_sha1) == 0);
    }

    if (!has_head && staging.count > 0) {
        fb_puts("\n  Changes to be committed (initial):\n");
        for (int i = 0; i < staging.count; i++) {
            fb_puts("    new file: ");
            fb_puts_color(staging.entries[i].name, 0x0000FF00);
            fb_puts("\n");
        }
    } else if (has_head && staging.count > 0) {
        /* Compare index vs HEAD tree */
        commit_info_t cinfo;
        if (commit_parse(head_sha1, &cinfo) == 0) {
            /* Walk HEAD tree to find differences */
            fb_puts("\n  Changes to be committed:\n");
            for (int i = 0; i < staging.count; i++) {
                /* For simplicity, just show staged files */
                fb_puts("    modified: ");
                fb_puts_color(staging.entries[i].name, 0x0000FF00);
                fb_puts("\n");
            }
        }
    }

    /* Check for unstaged changes — compare OsitoFS files vs index */
    bool found_unstaged = false;
    for (uint32_t i = 0; ; i++) {
        void *f = osfs2_file_at(i);
        if (!f) break;

        /* Get filename — the file pointer is osfs2_file_t* */
        const char *fname = osfs2_file_name(f);

        /* Skip git internal files and binaries */
        if (git_strncmp(fname, ".git/", 5) == 0) continue;
        if (git_strncmp(fname, ".git", 4) == 0) continue;

        /* Skip large files (likely binaries) */
        uint64_t fsize = osfs2_file_size(f);
        if (fsize > 1024 * 1024) continue;

        /* Check if in index */
        int idx = index_find(fname);
        if (idx < 0) {
            /* Untracked file */
            if (!found_unstaged) {
                fb_puts("\n  Untracked files:\n");
                found_unstaged = true;
            }
            fb_puts("    ");
            fb_puts_color(fname, 0x00FF0000);
            fb_puts("\n");
        }
    }

    if (staging.count == 0 && !found_unstaged) {
        fb_puts("\n  Nothing to commit, working tree clean\n");
    }

    return 0;
}

int git_diff(void)
{
    if (!git_file_exists(".git/HEAD")) {
        fb_puts("  Not a git repository\n");
        return -1;
    }

    index_load();

    /* For each file in index, compare with working tree */
    for (int i = 0; i < staging.count; i++) {
        git_index_entry_t *e = &staging.entries[i];

        /* Read current file */
        void *f = osfs2_find(e->name);
        if (!f) {
            fb_puts_color("--- a/", 0x00FF0000);
            fb_puts_color(e->name, 0x00FF0000);
            fb_puts("\n  (deleted)\n");
            continue;
        }

        uint64_t fsize = osfs2_file_size(f);
        if (fsize > 256 * 1024) continue; /* skip large files */

        uint8_t *content = (uint8_t *)kmalloc(fsize + 1);
        if (!content) continue;
        if (osfs2_read(f, 0, content, fsize) < 0) { kfree(content); continue; }
        content[fsize] = '\0';

        /* Hash current content */
        uint8_t cur_sha1[20];
        sha1_ctx ctx;
        char header[64];
        int hlen = 0;
        git_memcpy(header, "blob ", 5); hlen = 5;
        char szbuf[16];
        int szlen = itoa_dec((uint32_t)fsize, szbuf);
        git_memcpy(header + hlen, szbuf, szlen); hlen += szlen;
        header[hlen++] = '\0';
        sha1_init(&ctx);
        sha1_update(&ctx, header, hlen);
        sha1_update(&ctx, content, (uint32_t)fsize);
        sha1_final(&ctx, cur_sha1);

        if (git_memcmp(cur_sha1, e->sha1, 20) == 0) {
            kfree(content);
            continue; /* no changes */
        }

        /* Show diff header */
        fb_puts_color("diff --git a/", 0x00FFFFFF);
        fb_puts(e->name);
        fb_puts(" b/");
        fb_puts(e->name);
        fb_puts("\n");

        /* Read blob from index */
        char type[16];
        uint8_t *old_data;
        uint32_t old_len;
        if (object_read(e->sha1, type, &old_data, &old_len) < 0) {
            fb_puts("  (cannot read staged version)\n");
            kfree(content);
            continue;
        }

        /* Simple line-by-line diff (show old lines as -, new lines as +) */
        fb_puts_color("--- a/", 0x00FF0000);
        fb_puts(e->name);
        fb_puts("\n");
        fb_puts_color("+++ b/", 0x0000FF00);
        fb_puts(e->name);
        fb_puts("\n");

        /* Count lines in old and new */
        int old_lines = 1, new_lines = 1;
        for (uint32_t j = 0; j < old_len; j++) if (old_data[j] == '\n') old_lines++;
        for (uint64_t j = 0; j < fsize; j++) if (content[j] == '\n') new_lines++;

        fb_puts("@@ -1,");
        fb_putdec(old_lines);
        fb_puts(" +1,");
        fb_putdec(new_lines);
        fb_puts(" @@\n");

        /* Show old lines */
        const char *lp = (const char *)old_data;
        while (*lp) {
            fb_puts_color("-", 0x00FF0000);
            while (*lp && *lp != '\n') { fb_putc(*lp, 0x00FF0000); lp++; }
            fb_puts("\n");
            if (*lp == '\n') lp++;
        }

        /* Show new lines */
        lp = (const char *)content;
        while (*lp) {
            fb_puts_color("+", 0x0000FF00);
            while (*lp && *lp != '\n') { fb_putc(*lp, 0x0000FF00); lp++; }
            fb_puts("\n");
            if (*lp == '\n') lp++;
        }

        kfree(old_data);
        kfree(content);
    }

    return 0;
}

int git_branch(const char *name)
{
    if (!git_file_exists(".git/HEAD")) {
        fb_puts("  Not a git repository\n");
        return -1;
    }

    if (name == NULL) {
        /* List branches — scan .git/refs/heads/ */
        char head_ref[64];
        read_head(head_ref, sizeof(head_ref));

        fb_puts("  Branches:\n");
        for (uint32_t i = 0; ; i++) {
            void *f = osfs2_file_at(i);
            if (!f) break;
            const char *fname = osfs2_file_name(f);

            if (git_strncmp(fname, ".git/refs/heads/", 16) != 0) continue;
            const char *bname = fname + 16;

            /* Check if current */
            char full_ref[64];
            git_memcpy(full_ref, "refs/heads/", 11);
            git_strcpy(full_ref + 11, bname);

            if (git_strcmp(full_ref, head_ref) == 0) {
                fb_puts("  * ");
                fb_puts_color(bname, 0x0000FF00);
            } else {
                fb_puts("    ");
                fb_puts(bname);
            }
            fb_puts("\n");
        }
        return 0;
    }

    /* Create new branch pointing at current HEAD */
    char head_ref[64];
    int detached = read_head(head_ref, sizeof(head_ref));
    uint8_t sha1[20];

    if (detached == 1) {
        if (git_hex_to_sha1(head_ref, sha1) < 0) {
            fb_puts("  Cannot resolve HEAD\n");
            return -1;
        }
    } else {
        if (resolve_ref(head_ref, sha1) < 0) {
            fb_puts("  No commits yet — cannot create branch\n");
            return -1;
        }
    }

    char ref[64];
    git_memcpy(ref, "refs/heads/", 11);
    git_strcpy(ref + 11, name);

    if (write_ref(ref, sha1) < 0) {
        fb_puts("  Error creating branch\n");
        return -1;
    }

    fb_puts("  Created branch '");
    fb_puts(name);
    fb_puts("'\n");
    return 0;
}

/* Callback for checkout — rebuilds index from tree */
static void checkout_tree_cb(uint32_t mode, const char *name, const uint8_t *sha, void *ctx)
{
    (void)ctx;
    if (staging.count >= GIT_MAX_INDEX) return;
    git_index_entry_t *e = &staging.entries[staging.count++];
    e->mode = mode;
    git_memcpy(e->sha1, sha, 20);
    int nl = git_strlen(name);
    if (nl > 63) nl = 63;
    git_memcpy(e->name, name, nl);
    e->name[nl] = '\0';
}

int git_checkout(const char *branch)
{
    if (!git_file_exists(".git/HEAD")) {
        fb_puts("  Not a git repository\n");
        return -1;
    }

    /* Check branch exists */
    char ref[64];
    git_memcpy(ref, "refs/heads/", 11);
    git_strcpy(ref + 11, branch);

    uint8_t sha1[20];
    if (resolve_ref(ref, sha1) < 0) {
        fb_puts("  Error: branch '");
        fb_puts(branch);
        fb_puts("' not found\n");
        return -1;
    }

    /* Update HEAD */
    char head_content[64];
    int pos = 0;
    git_memcpy(head_content, "ref: ", 5); pos = 5;
    int rlen = git_strlen(ref);
    git_memcpy(head_content + pos, ref, rlen); pos += rlen;
    head_content[pos++] = '\n';

    if (git_write_file(".git/HEAD", head_content, pos) < 0) {
        fb_puts("  Error updating HEAD\n");
        return -1;
    }

    /* Update index from commit's tree */
    commit_info_t cinfo;
    if (commit_parse(sha1, &cinfo) == 0) {
        staging.count = 0;
        tree_walk(cinfo.tree, checkout_tree_cb, NULL);
        index_save();
    }

    fb_puts("  Switched to branch '");
    fb_puts(branch);
    fb_puts("'\n");

    serial_puts("[GIT] Checkout ");
    serial_puts(branch);
    serial_puts("\n");

    return 0;
}
