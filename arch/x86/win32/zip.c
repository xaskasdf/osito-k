/*
 * OsitoK Win32 Layer — ZIP / OPC reader. See zip.h.
 */

#include "zip.h"

extern void serial_puts(const char *s);
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);
extern int zlib_inflate_raw(const uint8_t *src, uint32_t src_len,
                            uint8_t *dst, uint32_t start_off, uint32_t *dst_len);

#define SIG_EOCD   0x06054b50u
#define SIG_CD     0x02014b50u
#define SIG_LOCAL  0x04034b50u

struct zip_archive {
    const uint8_t *data;
    uint32_t len;
    uint32_t cd_off;
    int count;
};

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)(p[0] | ((uint32_t)p[1] << 8) |
                      ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

zip_archive_t *zip_open(const uint8_t *data, uint32_t len)
{
    if (!data || len < 22) return 0;

    uint32_t maxback = (len > 22 + 65535u) ? (22 + 65535u) : len;
    uint32_t eocd = 0; int found = 0;
    for (uint32_t i = 22; i <= maxback; i++) {
        uint32_t p = len - i;
        if (rd32(data + p) == SIG_EOCD) { eocd = p; found = 1; break; }
    }
    if (!found) return 0;

    zip_archive_t *z = (zip_archive_t *)kmalloc(sizeof(*z));
    if (!z) return 0;
    memset(z, 0, sizeof(*z));
    z->data   = data;
    z->len    = len;
    z->count  = rd16(data + eocd + 10);
    z->cd_off = rd32(data + eocd + 16);
    if (z->cd_off >= len) { kfree(z); return 0; }
    return z;
}

void zip_close(zip_archive_t *z)
{
    if (z) kfree(z);
}

int zip_entry_count(zip_archive_t *z)
{
    return z ? z->count : 0;
}

/* Walk to central-directory record #idx; returns its offset or 0. */
static uint32_t cd_at(zip_archive_t *z, int idx)
{
    uint32_t off = z->cd_off;
    for (int i = 0; i < z->count; i++) {
        if (off + 46 > z->len || rd32(z->data + off) != SIG_CD) return 0;
        if (i == idx) return off;
        uint16_t fn = rd16(z->data + off + 28);
        uint16_t ex = rd16(z->data + off + 30);
        uint16_t cm = rd16(z->data + off + 32);
        off += 46u + fn + ex + cm;
    }
    return 0;
}

int zip_enum(zip_archive_t *z, int idx, char *name_out, int cap, uint32_t *usize)
{
    if (!z) return 0;
    uint32_t off = cd_at(z, idx);
    if (!off) return 0;
    uint16_t fn = rd16(z->data + off + 28);
    if (usize) *usize = rd32(z->data + off + 24);
    const char *fname = (const char *)(z->data + off + 46);
    int n = fn;
    if (n > cap - 1) n = cap - 1;
    for (int i = 0; i < n; i++) name_out[i] = fname[i];
    name_out[n] = 0;
    return 1;
}

static int name_match(const char *fname, uint16_t fnlen, const char *name)
{
    uint16_t i = 0;
    for (; i < fnlen; i++)
        if (name[i] == 0 || name[i] != fname[i]) return 0;
    return name[i] == 0;
}

static uint8_t *extract_local(zip_archive_t *z, uint32_t lho, uint16_t method,
                              uint32_t csize, uint32_t usize, uint32_t *out_size)
{
    if (lho + 30 > z->len || rd32(z->data + lho) != SIG_LOCAL) return 0;
    uint16_t lfn = rd16(z->data + lho + 26);
    uint16_t lex = rd16(z->data + lho + 28);
    uint32_t doff = lho + 30u + lfn + lex;
    if ((uint64_t)doff + csize > z->len) return 0;

    const uint8_t *cdata = z->data + doff;
    uint8_t *out = (uint8_t *)kmalloc(usize ? usize : 1);
    if (!out) return 0;

    if (method == 0) {                       /* stored */
        uint32_t n = usize <= csize ? usize : csize;
        memcpy(out, cdata, n);
    } else if (method == 8) {                /* deflate */
        uint32_t cap = usize;
        if (zlib_inflate_raw(cdata, csize, out, 0, &cap) != 0) { kfree(out); return 0; }
    } else {
        kfree(out); return 0;
    }
    if (out_size) *out_size = usize;
    return out;
}

uint8_t *zip_extract(zip_archive_t *z, const char *name, uint32_t *out_size)
{
    if (!z || !name) return 0;
    uint32_t off = z->cd_off;
    for (int i = 0; i < z->count; i++) {
        if (off + 46 > z->len || rd32(z->data + off) != SIG_CD) return 0;
        uint16_t method = rd16(z->data + off + 10);
        uint32_t csize  = rd32(z->data + off + 20);
        uint32_t usize  = rd32(z->data + off + 24);
        uint16_t fn = rd16(z->data + off + 28);
        uint16_t ex = rd16(z->data + off + 30);
        uint16_t cm = rd16(z->data + off + 32);
        uint32_t lho = rd32(z->data + off + 42);
        const char *fname = (const char *)(z->data + off + 46);

        if (name_match(fname, fn, name))
            return extract_local(z, lho, method, csize, usize, out_size);

        off += 46u + fn + ex + cm;
    }
    return 0;
}
