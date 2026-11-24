/*
 * OsitoK Win32 Layer — Microsoft Cabinet (.cab) reader. See cab.h.
 *
 * A folder prefix is decompressed through the requested file's last block.
 * Its MSZIP history spans CFDATA blocks and the prefix is cached until close.
 */

#include "cab.h"
#include "cab_lzx.h"

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
    cab_info_t info;
    uint8_t *cache;
    uint32_t cache_len;
    int cache_folder;
};
static uint32_t file_at(cab_archive_t *, int, uint32_t *, uint32_t *, uint16_t *);

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
    uint32_t declared = rd32(data + 8);
    if (declared < 36 || data[24] != 3 || data[25] != 1) return 0;
    if (declared < len) len = declared;

    cab_archive_t *c = (cab_archive_t *)kmalloc(sizeof(*c));
    if (!c) return 0;
    memset(c, 0, sizeof(*c));
    c->data = data;
    c->len  = len;
    c->cache_folder = -1;

    uint32_t coffFiles = rd32(data + 16);
    c->cfolders = rd16(data + 26);
    c->cfiles   = rd16(data + 28);
    uint16_t flags = rd16(data + 30);
    c->info.flags = flags;
    c->info.set_id = rd16(data + 32);
    c->info.number = rd16(data + 34);
    c->info.next_cabinet = c->info.next_disk = "";

    uint32_t off = 36;
    if (flags & CAB_FLAG_RESERVE_PRESENT) {
        if (off + 4 > len) { kfree(c); return 0; }
        uint16_t cbCFHeader = rd16(data + off);
        c->cbCFFolder = data[off + 2];
        c->cbCFData   = data[off + 3];
        off += 4 + cbCFHeader;
        if (off > len) { kfree(c); return 0; }
    }
    if (flags & CAB_FLAG_PREV_CABINET) {
        off = skip_str(data, off, len);   /* szCabinetPrev */
        if (off >= len) { kfree(c); return 0; }
        off = skip_str(data, off, len);   /* szDiskPrev */
        if (off >= len) { kfree(c); return 0; }
    }
    if (flags & CAB_FLAG_NEXT_CABINET) {
        if (off >= len) { kfree(c); return 0; }
        c->info.next_cabinet = (const char *)data + off;
        off = skip_str(data, off, len);
        if (off >= len) { kfree(c); return 0; }
        c->info.next_disk = (const char *)data + off;
        off = skip_str(data, off, len);
        if (off >= len) { kfree(c); return 0; }
    }

    if (off > len || (uint32_t)c->cfolders > (len - off) / (8 + c->cbCFFolder) ||
        coffFiles < off + (uint32_t)c->cfolders * (8 + c->cbCFFolder) ||
        coffFiles > len) {
        kfree(c); return 0;
    }
    c->folder_tab = off;
    c->files_off  = coffFiles;
    if (c->cfiles && !file_at(c, c->cfiles - 1, 0, 0, 0)) { kfree(c); return 0; }
    return c;
}

void cab_close(cab_archive_t *c)
{
    if (c) { if (c->cache) kfree(c->cache); kfree(c); }
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
    if (!c || idx < 0 || idx >= c->cfiles) return 0;
    uint32_t off = c->files_off;
    for (int i = 0; i < (int)c->cfiles; i++) {
        if (off > c->len || c->len - off <= 16) return 0;
        const char *name = (const char *)(c->data + off + 16);
        uint32_t nl = 0;
        while (off + 16 + nl < c->len && name[nl]) nl++;
        if (nl == c->len - off - 16) return 0;
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

int cab_file_info(cab_archive_t *c, int idx, cab_file_info_t *info)
{
    if (!info) return -1;
    uint32_t off = file_at(c, idx, 0, 0, 0);
    if (!off) return -1;
    *info = (cab_file_info_t){
        .name = (const char *)c->data + off + 16,
        .size = rd32(c->data + off), .offset = rd32(c->data + off + 4),
        .folder = rd16(c->data + off + 8), .date = rd16(c->data + off + 10),
        .time = rd16(c->data + off + 12), .attributes = rd16(c->data + off + 14),
        .compression = 0xffff
    };
    if (info->folder < c->cfolders)
        info->compression = rd16(folder_entry(c, info->folder) + 6) & 15;
    return 0;
}

int cab_get_info(cab_archive_t *c, cab_info_t *info)
{
    if (!c || !info) return -1;
    *info = c->info;
    return 0;
}

/* MS-CAB CFDATA checksum: little-endian DWORD XOR with the final 1-3 bytes
 * accumulated most-significant-byte first. Zero means checksum omitted. */
static uint32_t cab_checksum(const uint8_t *data, uint32_t length)
{
    uint32_t value = 0;
    while (length >= 4) { value ^= rd32(data); data += 4; length -= 4; }
    uint32_t tail = 0;
    while (length--) tail = (tail << 8) | *data++;
    return value ^ tail;
}

/* Decompress only the needed prefix; skipping a file does not validate its
 * compressed blocks. This also permits early files in a truncated cabinet. */
static uint8_t *decompress_folder(cab_archive_t *c, int folder_idx,
                                  uint32_t needed, uint32_t *out_size)
{
    if (folder_idx < 0 || folder_idx >= (int)c->cfolders) return 0;
    const uint8_t *fe = folder_entry(c, folder_idx);
    uint32_t coffData = rd32(fe);
    uint16_t cCFData  = rd16(fe + 4);
    uint16_t compression = rd16(fe + 6);
    int method = compression & 0x000F;   /* 0=none, 1=MSZIP, 3=LZX */

    /* Pass 1: capacity through the requested file's final block. */
    uint32_t total = 0, off = coffData;
    uint16_t blocks = 0;
    for (; blocks < cCFData && total < needed; blocks++) {
        if (off > c->len || c->len - off < 8U + c->cbCFData) return 0;
        uint16_t cbData   = rd16(c->data + off + 4);
        uint16_t cbUncomp = rd16(c->data + off + 6);
        if (!cbUncomp || cbUncomp > 32768U ||
            cbData > c->len - off - 8U - c->cbCFData) return 0;
        total += cbUncomp;
        off += 8 + c->cbCFData + cbData;
    }
    if (total < needed || !total || (method != 0 && method != 1 && method != 3)) return 0;

    uint8_t *out = (uint8_t *)kmalloc(total);
    if (!out) return 0;
    cab_lzx_t *lzx = method == 3 ? cab_lzx_create(compression >> 8) : 0;
    if (method == 3 && !lzx) { kfree(out); return 0; }

    /* Pass 2: decompress, carrying the window across blocks. */
    uint32_t produced = 0;
    off = coffData;
    for (uint16_t b = 0; b < blocks; b++) {
        if (off + 8 > c->len) goto fail;
        uint16_t cbData   = rd16(c->data + off + 4);
        uint16_t cbUncomp = rd16(c->data + off + 6);
        const uint8_t *blk = c->data + off + 8 + c->cbCFData;
        if ((uint32_t)(blk - c->data) + cbData > c->len) goto fail;
        uint32_t checksum = rd32(c->data + off);
        if (checksum && checksum != cab_checksum(c->data + off + 4,
                                                4U + c->cbCFData + cbData)) {
            goto fail;
        }

        if (method == 0) {                       /* stored */
            if (cbData != cbUncomp || cbUncomp > total - produced) goto fail;
            memcpy(out + produced, blk, cbUncomp);
            produced += cbUncomp;
        } else if (method == 1) {                /* MSZIP */
            if (cbData < 2 || blk[0] != 'C' || blk[1] != 'K') {
                goto fail;
            }
            uint32_t cap = produced + cbUncomp;
            if (zlib_inflate_raw(blk + 2, cbData - 2, out, produced, &cap) != 0 ||
                cap != produced + cbUncomp) {
                goto fail;
            }
            produced = cap;
        } else {                                 /* LZX */
            if (cab_lzx_frame(lzx, blk, cbData, out, produced, cbUncomp)) goto fail;
            produced += cbUncomp;
        }
        off += 8 + c->cbCFData + cbData;
    }

    cab_lzx_translate(lzx, out, produced);
    cab_lzx_destroy(lzx);
    *out_size = produced;
    return out;
fail:
    cab_lzx_destroy(lzx);
    kfree(out);
    return 0;
}

uint8_t *cab_extract_index(cab_archive_t *c, int idx, uint32_t *out_size)
{
    cab_file_info_t info;
    if (out_size) *out_size = 0;
    if (cab_file_info(c, idx, &info) || info.folder >= c->cfolders ||
        (uint64_t)info.offset + info.size > 0x7fffffffU) return 0;
    if (info.size && (c->cache_folder != info.folder ||
                     c->cache_len < info.offset + info.size)) {
        uint32_t size = 0;
        uint8_t *buffer = decompress_folder(c, info.folder, info.offset + info.size, &size);
        if (!buffer) return 0;
        if (c->cache) kfree(c->cache);
        c->cache = buffer; c->cache_len = size; c->cache_folder = info.folder;
    }
    uint8_t *result = kmalloc(info.size ? info.size : 1);
    if (!result) return 0;
    if (info.size) memcpy(result, c->cache + info.offset, info.size);
    if (out_size) *out_size = info.size;
    return result;
}

uint8_t *cab_extract(cab_archive_t *c, const char *name, uint32_t *out_size)
{
    if (!c || !name) return 0;
    for (int i = 0; i < c->cfiles; i++) {
        const char *member = cab_file_name(c, i);
        if (!member) return 0;
        if (cab_streq(member, name)) return cab_extract_index(c, i, out_size);
    }
    return 0;
}
