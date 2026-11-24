/*
 * OsitoK Win32 Layer — OLE2 / Compound File Binary Format reader
 *
 * See ole2.h. Read-only. Sufficient to crack open an MSI (CFBF) and
 * pull out its named streams (_StringData, _StringPool, table streams,
 * embedded #cabinets). All multi-byte fields are little-endian, matching
 * the x86-64 host, so byte buffers read as uint32 directly where handy.
 */

#include "ole2.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);

#define OLE2_ENDOFCHAIN 0xFFFFFFFEu
#define OLE2_FREESECT   0xFFFFFFFFu
#define OLE2_FATSECT    0xFFFFFFFDu
#define OLE2_DIFSECT    0xFFFFFFFCu

struct ole2_file {
    const uint8_t *data;
    uint32_t len;
    uint32_t sector_shift;     /* 9 (512) or 12 (4096) */
    uint32_t sector_size;
    uint32_t sector_count;     /* Physical sectors after the header. */
    uint32_t mini_sector_size; /* usually 64 */
    uint32_t mini_cutoff;      /* usually 4096 */
    uint32_t *fat;             /* sector chain table */
    uint32_t fat_entries;
    uint32_t *minifat;         /* mini-sector chain table */
    uint32_t minifat_entries;
    uint8_t  *ministream;      /* materialized mini stream container */
    uint32_t ministream_size;
    uint8_t  *dirbuf;          /* materialized directory (128B entries) */
    uint32_t dir_count;
};

/* ── Little-endian readers ───────────────────────────────────── */

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)(p[0] | ((uint32_t)p[1] << 8) |
                      ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static uint32_t sect_off(ole2_file_t *f, uint32_t sect)
{
    /* Special FAT values must never wrap around to a valid offset. */
    if (sect >= f->sector_count) return f->len;
    return (sect + 1) << f->sector_shift;
}

/* Follow a regular-FAT chain into a fresh buffer.
 * want==0 → read the whole chain; else read up to `want` bytes. */
static uint8_t *read_chain(ole2_file_t *f, uint32_t start,
                           uint32_t want, uint32_t *out_size)
{
    uint32_t ss = f->sector_size;
    uint32_t nsect = 0, s = start;
    if (out_size) *out_size = 0;
    while (s != OLE2_ENDOFCHAIN) {
        if (s >= f->fat_entries || s >= f->sector_count ||
            ++nsect > f->sector_count) return 0;
        s = f->fat[s];
    }

    uint32_t avail = nsect * ss;
    if (want > avail) return 0;
    uint32_t total = want ? want : avail;
    if (!total) return 0;

    uint8_t *buf = (uint8_t *)kmalloc(total);
    if (!buf) return 0;

    uint32_t copied = 0;
    s = start;
    while (copied < total) {
        uint32_t off = sect_off(f, s);
        uint32_t n = ss;
        if (n > total - copied) n = total - copied;
        memcpy(buf + copied, f->data + off, n);
        copied += n;
        s = f->fat[s];
    }
    if (out_size) *out_size = copied;
    return buf;
}

/* Follow a mini-FAT chain into a fresh buffer (small streams). */
static uint8_t *read_mini_chain(ole2_file_t *f, uint32_t start,
                                uint32_t want, uint32_t *out_size)
{
    if (out_size) *out_size = 0;
    if (!f->ministream || !f->minifat) return 0;
    uint32_t ms = f->mini_sector_size;
    uint32_t physical = f->ministream_size / ms;
    uint32_t nsect = 0, s = start;
    while (s != OLE2_ENDOFCHAIN) {
        if (s >= f->minifat_entries || s >= physical || ++nsect > physical) return 0;
        s = f->minifat[s];
    }

    uint32_t avail = nsect * ms;
    if (want > avail) return 0;
    uint32_t total = want ? want : avail;
    if (!total) return 0;

    uint8_t *buf = (uint8_t *)kmalloc(total);
    if (!buf) return 0;

    uint32_t copied = 0;
    s = start;
    while (copied < total) {
        uint32_t off = s * ms;
        uint32_t n = ms;
        if (n > total - copied) n = total - copied;
        memcpy(buf + copied, f->ministream + off, n);
        copied += n;
        s = f->minifat[s];
    }
    if (out_size) *out_size = copied;
    return buf;
}

/* ── Open / close ────────────────────────────────────────────── */

ole2_file_t *ole2_open(const uint8_t *data, uint32_t len)
{
    static const uint8_t sig[8] = {0xD0,0xCF,0x11,0xE0,0xA1,0xB1,0x1A,0xE1};
    if (!data || len < 512) return 0;
    for (int i = 0; i < 8; i++)
        if (data[i] != sig[i]) return 0;

    uint16_t version = rd16(data + 0x1A);
    uint16_t sshift = rd16(data + 0x1E);
    uint16_t mshift = rd16(data + 0x20);
    if (rd16(data + 0x1C) != 0xFFFE || mshift != 6 ||
        !((version == 3 && sshift == 9) || (version == 4 && sshift == 12)) ||
        rd32(data + 0x38) != 4096) return 0;
    uint32_t sector_size = 1U << sshift;
    if (len < 3U * sector_size || len % sector_size) return 0;

    ole2_file_t *f = (ole2_file_t *)kmalloc(sizeof(*f));
    if (!f) return 0;
    memset(f, 0, sizeof(*f));
    f->data = data;
    f->len  = len;

    f->sector_shift     = sshift;
    f->sector_size      = sector_size;
    f->sector_count     = len / sector_size - 1;
    f->mini_sector_size = 1u << mshift;
    f->mini_cutoff      = rd32(data + 0x38);

    uint32_t num_fat_sect    = rd32(data + 0x2C);
    uint32_t dir_start       = rd32(data + 0x30);
    uint32_t minifat_start   = rd32(data + 0x3C);
    uint32_t num_minifat     = rd32(data + 0x40);
    uint32_t difat_start     = rd32(data + 0x44);
    uint32_t num_difat_sect  = rd32(data + 0x48);

    if (!num_fat_sect || num_fat_sect > f->sector_count ||
        num_minifat > f->sector_count || num_difat_sect > f->sector_count) {
        kfree(f); return 0;
    }

    uint32_t per = f->sector_size / 4;   /* entries per sector */
    f->fat_entries = num_fat_sect * per;
    f->fat = (uint32_t *)kmalloc((uint64_t)f->fat_entries * 4);
    if (!f->fat) { kfree(f); return 0; }

    /* Header DIFAT: first 109 FAT-sector locations at offset 0x4C. */
    uint32_t fi = 0;
    for (uint32_t i = 0; i < 109 && fi < num_fat_sect; i++) {
        uint32_t loc = rd32(data + 0x4C + i * 4);
        if (loc >= f->sector_count) goto invalid;
        uint32_t off = sect_off(f, loc);
        for (uint32_t j = 0; j < per; j++)
            f->fat[fi * per + j] = rd32(data + off + j * 4);
        fi++;
    }

    /* DIFAT chain for any remaining FAT sectors (large files). */
    uint32_t ds = difat_start;
    for (uint32_t d = 0; d < num_difat_sect; d++) {
        if (ds >= f->sector_count) goto invalid;
        uint32_t off = sect_off(f, ds);
        uint32_t slots = per - 1;   /* last 4 bytes = next DIFAT sector */
        for (uint32_t i = 0; i < slots && fi < num_fat_sect; i++) {
            uint32_t loc = rd32(data + off + i * 4);
            if (loc >= f->sector_count) goto invalid;
            uint32_t foff = sect_off(f, loc);
            for (uint32_t j = 0; j < per; j++)
                f->fat[fi * per + j] = rd32(data + foff + j * 4);
            fi++;
        }
        ds = rd32(data + off + slots * 4);
    }
    if (fi != num_fat_sect || (num_difat_sect && ds != OLE2_ENDOFCHAIN)) goto invalid;

    /* mini-FAT (chain through the regular FAT; values reread as uint32). */
    if (num_minifat > 0) {
        uint32_t mfsize = 0;
        uint8_t *mf = read_chain(f, minifat_start, num_minifat * sector_size, &mfsize);
        if (!mf) goto invalid;
        f->minifat = (uint32_t *)mf;       /* LE buffer == uint32 on x86 */
        f->minifat_entries = mfsize / 4;
    }

    /* Directory. */
    uint32_t dirsize = 0;
    f->dirbuf = read_chain(f, dir_start, 0, &dirsize);
    if (!f->dirbuf) { ole2_close(f); return 0; }
    f->dir_count = dirsize / 128;

    /* A name occupies 64 bytes, regardless of the enumeration buffer size. */
    if (!f->dir_count || f->dirbuf[0x42] != 5) goto invalid;
    for (uint32_t i = 0; i < f->dir_count; i++) {
        const uint8_t *entry = f->dirbuf + i * 128;
        uint8_t type = entry[0x42];
        if (!type) continue;
        uint16_t length = rd16(entry + 0x40);
        if ((type != 1 && type != 2 && type != 5) || (i && type == 5) ||
            length < 2 || length > 64 || (length & 1) || rd16(entry + length - 2)) goto invalid;
        /* Public lengths are 32-bit; do not truncate version-4 stream sizes. */
        if (version == 4 && (type == 2 || type == 5) && rd32(entry + 0x7C)) goto invalid;
    }

    /* Materialize the mini stream container from the root entry (index 0). */
    if (f->dir_count > 0) {
        const uint8_t *root = f->dirbuf;
        uint32_t root_start = rd32(root + 0x74);
        uint32_t root_size  = rd32(root + 0x78);
        if (root_size > 0) {
            f->ministream = read_chain(f, root_start, root_size,
                                       &f->ministream_size);
            if (!f->ministream) goto invalid;
        }
    }

    return f;
invalid:
    ole2_close(f);
    return 0;
}

void ole2_close(ole2_file_t *f)
{
    if (!f) return;
    if (f->fat)        kfree(f->fat);
    if (f->minifat)    kfree(f->minifat);
    if (f->ministream) kfree(f->ministream);
    if (f->dirbuf)     kfree(f->dirbuf);
    kfree(f);
}

/* ── Accessors ───────────────────────────────────────────────── */

uint32_t ole2_count(ole2_file_t *f)
{
    return f ? f->dir_count : 0;
}

int ole2_enum(ole2_file_t *f, int index,
              uint16_t *name_out, int name_cap,
              uint32_t *size_out, int *type_out)
{
    if (!f || index < 0 || (uint32_t)index >= f->dir_count) return 0;
    const uint8_t *e = f->dirbuf + (uint32_t)index * 128;

    if (type_out) *type_out = e[0x42];

    uint16_t namelen = rd16(e + 0x40);   /* bytes, includes null terminator */
    int units = (int)(namelen / 2);
    if (units > 0) units--;
    if (units < 0) units = 0;

    if (name_out && name_cap > 0) {
        int n = units;
        if (n > name_cap - 1) n = name_cap - 1;
        for (int i = 0; i < n; i++)
            name_out[i] = rd16(e + i * 2);
        name_out[n] = 0;
    }
    if (size_out) *size_out = rd32(e + 0x78);
    return 1;
}

uint8_t *ole2_read_index(ole2_file_t *f, int index, uint32_t *out_size)
{
    if (out_size) *out_size = 0;
    if (!f || index < 0 || (uint32_t)index >= f->dir_count) return 0;
    const uint8_t *e = f->dirbuf + (uint32_t)index * 128;
    if (e[0x42] != 2) return 0;   /* not a stream */

    uint32_t start = rd32(e + 0x74);
    uint32_t size  = rd32(e + 0x78);
    if (size == 0) { if (out_size) *out_size = 0; return 0; }

    uint32_t got = 0;
    uint8_t *buf;
    if (size < f->mini_cutoff)
        buf = read_mini_chain(f, start, size, &got);
    else
        buf = read_chain(f, start, size, &got);
    if (buf && out_size) *out_size = got;
    return buf;
}

uint8_t *ole2_read_stream(ole2_file_t *f, const char *name, uint32_t *out_size)
{
    if (out_size) *out_size = 0;
    if (!f || !name) return 0;
    for (uint32_t i = 0; i < f->dir_count; i++) {
        const uint8_t *e = f->dirbuf + i * 128;
        if (e[0x42] != 2) continue;
        uint16_t namelen = rd16(e + 0x40);
        int units = (int)(namelen / 2);
        if (units > 0) units--;
        int k, match = 1;
        for (k = 0; k < units; k++) {
            uint16_t u = rd16(e + k * 2);
            if (name[k] == 0 || (uint16_t)(unsigned char)name[k] != u) {
                match = 0; break;
            }
        }
        if (match && name[k] == 0)
            return ole2_read_index(f, (int)i, out_size);
    }
    return 0;
}
