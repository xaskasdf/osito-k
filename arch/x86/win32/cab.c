/*
 * OsitoK Win32 Layer — Microsoft Cabinet (.cab) reader. See cab.h.
 *
 * A folder is decompressed in one shot into a contiguous buffer; the MSZIP
 * LZ77 window therefore spans CFDATA blocks (each block's 'CK'+DEFLATE
 * payload is fed to zlib_inflate_raw with start_off = bytes produced so far).
 */

#include "cab.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);

/* Raw DEFLATE (no zlib header), defined in kernel/zlib.c. */
extern int zlib_inflate_raw(const uint8_t *src, uint32_t src_len,
                            uint8_t *dst, uint32_t start_off, uint32_t *dst_len);

#define CAB_FLAG_PREV_CABINET   0x0001
#define CAB_FLAG_NEXT_CABINET   0x0002
#define CAB_FLAG_RESERVE_PRESENT 0x0004

struct cab_archive {
    const uint8_t *data;
    uint32_t len;
    uint16_t cfolders;
    uint16_t cfiles;
    uint8_t  cbCFFolder;   /* per-folder reserved bytes */
    uint8_t  cbCFData;     /* per-CFDATA reserved bytes */
    uint32_t folder_tab;   /* offset of first CFFOLDER */
    uint32_t files_off;    /* offset of first CFFILE (coffFiles) */
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

static int cab_streq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static uint32_t skip_str(const uint8_t *d, uint32_t off, uint32_t len)
{
    while (off < len && d[off]) off++;
    return (off < len) ? off + 1 : off;
}

cab_archive_t *cab_open(const uint8_t *data, uint32_t len)
{
    if (!data || len < 36) return 0;
    if (data[0] != 'M' || data[1] != 'S' || data[2] != 'C' || data[3] != 'F')
        return 0;

    cab_archive_t *c = (cab_archive_t *)kmalloc(sizeof(*c));
    if (!c) return 0;
    memset(c, 0, sizeof(*c));
    c->data = data;
    c->len  = len;

    uint32_t coffFiles = rd32(data + 16);
    c->cfolders = rd16(data + 26);
    c->cfiles   = rd16(data + 28);
    uint16_t flags = rd16(data + 30);

    uint32_t off = 36;
    if (flags & CAB_FLAG_RESERVE_PRESENT) {
        if (off + 4 > len) { kfree(c); return 0; }
        uint16_t cbCFHeader = rd16(data + off);
        c->cbCFFolder = data[off + 2];
        c->cbCFData   = data[off + 3];
        off += 4 + cbCFHeader;
    }
    if (flags & CAB_FLAG_PREV_CABINET) {
        off = skip_str(data, off, len);   /* szCabinetPrev */
        off = skip_str(data, off, len);   /* szDiskPrev */
    }
    if (flags & CAB_FLAG_NEXT_CABINET) {
        off = skip_str(data, off, len);   /* szCabinetNext */
        off = skip_str(data, off, len);   /* szDiskNext */
    }

    if (off + (uint32_t)c->cfolders * (8 + c->cbCFFolder) > len ||
        coffFiles >= len) {
        kfree(c); return 0;
    }
    c->folder_tab = off;
    c->files_off  = coffFiles;
    return c;
}

void cab_close(cab_archive_t *c)
{
    if (c) kfree(c);
}

int cab_file_count(cab_archive_t *c)
{
    return c ? (int)c->cfiles : 0;
}

static const uint8_t *folder_entry(cab_archive_t *c, int i)
{
    return c->data + c->folder_tab + (uint32_t)i * (8 + c->cbCFFolder);
}

/* Walk to CFFILE #idx; returns its byte offset, or 0 past the end. */
static uint32_t file_at(cab_archive_t *c, int idx,
                        uint32_t *cbFile, uint32_t *uoff, uint16_t *ifolder)
{
    uint32_t off = c->files_off;
    for (int i = 0; i < (int)c->cfiles; i++) {
        if (off + 16 >= c->len) return 0;
        const char *name = (const char *)(c->data + off + 16);
        uint32_t nl = 0;
        while (off + 16 + nl < c->len && name[nl]) nl++;
        if (i == idx) {
            if (cbFile)  *cbFile  = rd32(c->data + off);
            if (uoff)    *uoff    = rd32(c->data + off + 4);
            if (ifolder) *ifolder = rd16(c->data + off + 8);
            return off;
        }
        off += 16 + nl + 1;
    }
    return 0;
}

const char *cab_file_name(cab_archive_t *c, int idx)
{
    if (!c) return 0;
    uint32_t off = file_at(c, idx, 0, 0, 0);
    if (!off) return 0;
    return (const char *)(c->data + off + 16);
}

/* Decompress an entire folder into a fresh buffer. */
static uint8_t *decompress_folder(cab_archive_t *c, int folder_idx,
                                  uint32_t *out_size)
{
    if (folder_idx < 0 || folder_idx >= (int)c->cfolders) return 0;
    const uint8_t *fe = folder_entry(c, folder_idx);
    uint32_t coffData = rd32(fe);
    uint16_t cCFData  = rd16(fe + 4);
    int method = rd16(fe + 6) & 0x000F;   /* 0=none, 1=MSZIP */

    /* Pass 1: total uncompressed size. */
    uint32_t total = 0, off = coffData;
    for (uint16_t b = 0; b < cCFData; b++) {
        if (off + 8 > c->len) return 0;
        uint16_t cbData   = rd16(c->data + off + 4);
        uint16_t cbUncomp = rd16(c->data + off + 6);
        total += cbUncomp;
        off += 8 + c->cbCFData + cbData;
    }
    if (!total) return 0;

    uint8_t *out = (uint8_t *)kmalloc(total);
    if (!out) return 0;

    /* Pass 2: decompress, carrying the window across blocks. */
    uint32_t produced = 0;
    off = coffData;
    for (uint16_t b = 0; b < cCFData; b++) {
        if (off + 8 > c->len) { kfree(out); return 0; }
        uint16_t cbData   = rd16(c->data + off + 4);
        uint16_t cbUncomp = rd16(c->data + off + 6);
        const uint8_t *blk = c->data + off + 8 + c->cbCFData;
        if ((uint32_t)(blk - c->data) + cbData > c->len) { kfree(out); return 0; }

        if (method == 0) {                       /* stored */
            if (produced + cbUncomp > total) { kfree(out); return 0; }
            memcpy(out + produced, blk, cbUncomp);
            produced += cbUncomp;
        } else if (method == 1) {                /* MSZIP */
            if (cbData < 2 || blk[0] != 'C' || blk[1] != 'K') {
                kfree(out); return 0;
            }
            uint32_t cap = total;   /* in: max capacity; out: new total */
            if (zlib_inflate_raw(blk + 2, cbData - 2, out, produced, &cap) != 0) {
                kfree(out); return 0;
            }
            produced = cap;
        } else {                                 /* LZX/Quantum: unsupported */
            serial_puts("[CAB] unsupported compression method\n");
            kfree(out); return 0;
        }
        off += 8 + c->cbCFData + cbData;
    }

    *out_size = produced;
    return out;
}

uint8_t *cab_extract(cab_archive_t *c, const char *name, uint32_t *out_size)
{
    if (!c || !name) return 0;

    uint32_t off = c->files_off;
    for (int i = 0; i < (int)c->cfiles; i++) {
        if (off + 16 >= c->len) return 0;
        uint32_t cbFile  = rd32(c->data + off);
        uint32_t uoff    = rd32(c->data + off + 4);
        uint16_t ifolder = rd16(c->data + off + 8);
        const char *fname = (const char *)(c->data + off + 16);
        uint32_t nl = 0;
        while (off + 16 + nl < c->len && fname[nl]) nl++;

        if (cab_streq(fname, name)) {
            uint32_t fsize = 0;
            uint8_t *fold = decompress_folder(c, (int)ifolder, &fsize);
            if (!fold) return 0;
            if ((uint64_t)uoff + cbFile > fsize) { kfree(fold); return 0; }
            uint8_t *res = (uint8_t *)kmalloc(cbFile ? cbFile : 1);
            if (!res) { kfree(fold); return 0; }
            memcpy(res, fold + uoff, cbFile);
            kfree(fold);
            if (out_size) *out_size = cbFile;
            return res;
        }
        off += 16 + nl + 1;
    }
    return 0;
}
