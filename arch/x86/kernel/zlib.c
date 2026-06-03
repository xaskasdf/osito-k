/*
 * OsitoK x86-64 — Minimal zlib (DEFLATE)
 *
 * Inflate: full DEFLATE decompression (stored, fixed, dynamic Huffman + LZ77).
 * Deflate: fixed Huffman encoding with LZ77 hash chain matching.
 * Framing: zlib header (CMF+FLG) + adler32 trailer.
 *
 * Reference: RFC 1950 (zlib), RFC 1951 (DEFLATE).
 */

#include "zlib.h"

/* ── Helpers ─────────────────────────────────────────────────── */

/* ── Adler-32 (RFC 1950) ────────────────────────────────────── */

uint32_t zlib_adler32(const uint8_t *data, uint32_t len)
{
    uint32_t a = 1, b = 0;
    for (uint32_t i = 0; i < len; i++) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

/* ══════════════════════════════════════════════════════════════
 *  INFLATE (decompress)
 * ══════════════════════════════════════════════════════════════ */

/* ── Bitstream reader (LSB-first) ───────────────────────────── */

typedef struct {
    const uint8_t *data;
    uint32_t size;
    uint32_t pos;
    uint32_t bits;
    int nbits;
} bitreader_t;

static uint32_t br_read(bitreader_t *br, int n)
{
    while (br->nbits < n) {
        if (br->pos >= br->size) return 0;
        br->bits |= (uint32_t)br->data[br->pos++] << br->nbits;
        br->nbits += 8;
    }
    uint32_t val = br->bits & ((1u << n) - 1);
    br->bits >>= n;
    br->nbits -= n;
    return val;
}

/* ── Huffman table (canonical, sorted by symbol) ────────────── */

#define HUFF_MAX_BITS  15
#define HUFF_MAX_LIT   288
#define HUFF_MAX_DIST  32

typedef struct {
    uint16_t counts[HUFF_MAX_BITS + 1];
    uint16_t symbols[HUFF_MAX_LIT];
} huffman_t;

static void huff_build(huffman_t *h, const uint8_t *lengths, int n)
{
    for (int i = 0; i <= HUFF_MAX_BITS; i++) h->counts[i] = 0;
    for (int i = 0; i < n; i++)
        if (lengths[i] <= HUFF_MAX_BITS) h->counts[lengths[i]]++;

    /* Length-0 symbols are UNUSED — they must not reserve slots in the
     * symbol table. Without this, offsets[1] = counts[0] instead of 0, so
     * every symbol is shifted by the number of unused codes and huff_decode
     * (which reads from index 0) returns garbage / fails. Fixed-Huffman
     * tables have no length-0 codes so they worked by luck; every real
     * DYNAMIC-Huffman stream (all practical zlib/gzip data) was corrupted —
     * the gcc sysroot's 283 MB cpio.z failed on its first symbol until this. */
    h->counts[0] = 0;

    uint16_t offsets[HUFF_MAX_BITS + 1];
    offsets[0] = 0;
    for (int i = 1; i <= HUFF_MAX_BITS; i++)
        offsets[i] = offsets[i - 1] + h->counts[i - 1];

    for (int i = 0; i < n; i++)
        if (lengths[i])
            h->symbols[offsets[lengths[i]]++] = (uint16_t)i;
}

static int huff_decode(bitreader_t *br, const huffman_t *h)
{
    int code = 0, first = 0, idx = 0;
    for (int len = 1; len <= HUFF_MAX_BITS; len++) {
        code |= (int)br_read(br, 1);
        int count = h->counts[len];
        if (code - count < first)
            return h->symbols[idx + (code - first)];
        idx += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

/* ── Length/distance base values + extra bits ────────────────── */

static const uint16_t len_base[29] = {
    3,4,5,6,7,8,9,10, 11,13,15,17, 19,23,27,31,
    35,43,51,59, 67,83,99,115, 131,163,195,227, 258
};
static const uint8_t len_extra[29] = {
    0,0,0,0,0,0,0,0, 1,1,1,1, 2,2,2,2,
    3,3,3,3, 4,4,4,4, 5,5,5,5, 0
};

static const uint16_t dist_base[30] = {
    1,2,3,4, 5,7,9,13, 17,25,33,49, 65,97,129,193,
    257,385,513,769, 1025,1537,2049,3073, 4097,6145,8193,12289, 16385,24577
};
static const uint8_t dist_extra[30] = {
    0,0,0,0, 1,1,2,2, 3,3,4,4, 5,5,6,6,
    7,7,8,8, 9,9,10,10, 11,11,12,12, 13,13
};

/* ── Fixed Huffman tables (RFC 1951 §3.2.6) ─────────────────── */

static void build_fixed_tables(huffman_t *lit, huffman_t *dist)
{
    uint8_t lengths[288];
    for (int i =   0; i <= 143; i++) lengths[i] = 8;
    for (int i = 144; i <= 255; i++) lengths[i] = 9;
    for (int i = 256; i <= 279; i++) lengths[i] = 7;
    for (int i = 280; i <= 287; i++) lengths[i] = 8;
    huff_build(lit, lengths, 288);

    for (int i = 0; i < 32; i++) lengths[i] = 5;
    huff_build(dist, lengths, 32);
}

/* ── Dynamic Huffman tables (RFC 1951 §3.2.7) ───────────────── */

static const uint8_t cl_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static int decode_dynamic(bitreader_t *br, huffman_t *lit, huffman_t *dist)
{
    int hlit  = (int)br_read(br, 5) + 257;
    int hdist = (int)br_read(br, 5) + 1;
    int hclen = (int)br_read(br, 4) + 4;

    /* Code length code lengths */
    uint8_t cl_lens[19];
    for (int i = 0; i < 19; i++) cl_lens[i] = 0;
    for (int i = 0; i < hclen; i++)
        cl_lens[cl_order[i]] = (uint8_t)br_read(br, 3);

    huffman_t cl_huff;
    huff_build(&cl_huff, cl_lens, 19);

    /* Decode literal/length + distance code lengths */
    int total = hlit + hdist;
    uint8_t lengths[288 + 32];
    int idx = 0;

    while (idx < total) {
        int sym = huff_decode(br, &cl_huff);
        if (sym < 0) return -1;

        if (sym < 16) {
            lengths[idx++] = (uint8_t)sym;
        } else if (sym == 16) {
            int repeat = (int)br_read(br, 2) + 3;
            uint8_t prev = idx > 0 ? lengths[idx - 1] : 0;
            for (int i = 0; i < repeat && idx < total; i++)
                lengths[idx++] = prev;
        } else if (sym == 17) {
            int repeat = (int)br_read(br, 3) + 3;
            for (int i = 0; i < repeat && idx < total; i++)
                lengths[idx++] = 0;
        } else { /* sym == 18 */
            int repeat = (int)br_read(br, 7) + 11;
            for (int i = 0; i < repeat && idx < total; i++)
                lengths[idx++] = 0;
        }
    }

    huff_build(lit, lengths, hlit);
    huff_build(dist, lengths + hlit, hdist);
    return 0;
}

/* ── Main inflate ───────────────────────────────────────────── */

int zlib_inflate(const uint8_t *src, uint32_t src_len,
                 uint8_t *dst, uint32_t *dst_len)
{
    if (src_len < 6) return -1;

    /* zlib header check */
    uint8_t cmf = src[0], flg = src[1];
    if ((cmf & 0x0F) != 8) return -1;          /* CM = deflate */
    if (((cmf * 256 + flg) % 31) != 0) return -1; /* FCHECK */

    bitreader_t br = { src + 2, src_len - 2, 0, 0, 0 };
    uint32_t out = 0, max_out = *dst_len;

    int bfinal;
    do {
        bfinal = (int)br_read(&br, 1);
        int btype = (int)br_read(&br, 2);

        if (btype == 0) {
            /* Stored block — align to byte boundary */
            br.bits = 0; br.nbits = 0;
            if (br.pos + 4 > br.size) return -1;
            uint16_t len  = br.data[br.pos] | ((uint16_t)br.data[br.pos + 1] << 8);
            br.pos += 4; /* skip len + nlen */
            for (uint16_t i = 0; i < len; i++) {
                if (out >= max_out) return -2;
                if (br.pos >= br.size) return -1;
                dst[out++] = br.data[br.pos++];
            }
        } else if (btype == 1 || btype == 2) {
            huffman_t lit_h, dist_h;
            if (btype == 1) {
                build_fixed_tables(&lit_h, &dist_h);
            } else {
                if (decode_dynamic(&br, &lit_h, &dist_h) < 0) return -1;
            }

            for (;;) {
                int sym = huff_decode(&br, &lit_h);
                if (sym < 0) return -1;

                if (sym < 256) {
                    if (out >= max_out) return -2;
                    dst[out++] = (uint8_t)sym;
                } else if (sym == 256) {
                    break;
                } else {
                    /* Length + distance */
                    int li = sym - 257;
                    if (li < 0 || li >= 29) return -1;
                    uint32_t length = len_base[li] + br_read(&br, len_extra[li]);
                    int dsym = huff_decode(&br, &dist_h);
                    if (dsym < 0 || dsym >= 30) return -1;
                    uint32_t distance = dist_base[dsym] + br_read(&br, dist_extra[dsym]);

                    if (distance > out) return -1;
                    for (uint32_t i = 0; i < length; i++) {
                        if (out >= max_out) return -2;
                        dst[out] = dst[out - distance];
                        out++;
                    }
                }
            }
        } else {
            return -1; /* reserved block type */
        }
    } while (!bfinal);

    *dst_len = out;

    /* Verify adler32 (last 4 bytes of src, big-endian) */
    /* Verify adler32 (last 4 bytes of src, big-endian) */
    if (src_len >= 4) {
        uint32_t expected = ((uint32_t)src[src_len - 4] << 24) |
                            ((uint32_t)src[src_len - 3] << 16) |
                            ((uint32_t)src[src_len - 2] << 8)  |
                            src[src_len - 1];
        uint32_t actual = zlib_adler32(dst, out);
        if (actual != expected) return -3;
    }

    return 0;
}

/* ══════════════════════════════════════════════════════════════
 *  DEFLATE (compress) — fixed Huffman + LZ77 hash chain
 * ══════════════════════════════════════════════════════════════ */

/* ── Bitstream writer (LSB-first) ───────────────────────────── */

typedef struct {
    uint8_t  *data;
    uint32_t  max;
    uint32_t  pos;
    uint32_t  bits;
    int       nbits;
} bitwriter_t;

static void bw_init(bitwriter_t *bw, uint8_t *dst, uint32_t max)
{
    bw->data = dst;
    bw->max = max;
    bw->pos = 0;
    bw->bits = 0;
    bw->nbits = 0;
}

static void bw_write(bitwriter_t *bw, uint32_t val, int n)
{
    bw->bits |= val << bw->nbits;
    bw->nbits += n;
    while (bw->nbits >= 8) {
        if (bw->pos < bw->max)
            bw->data[bw->pos++] = (uint8_t)(bw->bits & 0xFF);
        bw->bits >>= 8;
        bw->nbits -= 8;
    }
}

/* Write bits in reverse order (MSB-first, for Huffman codes) */
static void bw_write_rev(bitwriter_t *bw, uint32_t code, int n)
{
    uint32_t rev = 0;
    for (int i = 0; i < n; i++)
        rev |= ((code >> i) & 1) << (n - 1 - i);
    bw_write(bw, rev, n);
}

static void bw_flush(bitwriter_t *bw)
{
    if (bw->nbits > 0 && bw->pos < bw->max) {
        bw->data[bw->pos++] = (uint8_t)(bw->bits & 0xFF);
        bw->bits = 0;
        bw->nbits = 0;
    }
}

/* ── Fixed Huffman encoding ─────────────────────────────────── */

/* Emit literal byte using fixed Huffman code */
static void emit_literal(bitwriter_t *bw, uint8_t byte)
{
    if (byte <= 143) {
        /* 00110000 + byte (8 bits) → reverse of (0x30 + byte) */
        bw_write_rev(bw, 0x30 + byte, 8);
    } else {
        /* 110010000 + (byte - 144) (9 bits) → reverse of (0x190 + byte - 144) */
        bw_write_rev(bw, 0x190 + byte - 144, 9);
    }
}

/* Emit end-of-block (symbol 256 → fixed code 0000000, 7 bits) */
static void emit_eob(bitwriter_t *bw)
{
    bw_write_rev(bw, 0x00, 7);
}

/* Emit length code (fixed Huffman) */
static void emit_length(bitwriter_t *bw, uint32_t length)
{
    /* Find length code */
    int code = 0;
    for (int i = 0; i < 29; i++) {
        if (i == 28) { code = i; break; }
        if (length < len_base[i + 1]) { code = i; break; }
    }
    int sym = 257 + code;
    uint32_t extra = length - len_base[code];

    /* Fixed Huffman for 257-279: 7-bit (0000001..0010111) */
    /* For 280-287: 8-bit (11000000..11000111) */
    if (sym <= 279) {
        bw_write_rev(bw, sym - 256, 7);
    } else {
        bw_write_rev(bw, 0xC0 + sym - 280, 8);
    }

    if (len_extra[code] > 0)
        bw_write(bw, extra, len_extra[code]);
}

/* Emit distance code (fixed: all 5-bit) */
static void emit_distance(bitwriter_t *bw, uint32_t distance)
{
    int code = 0;
    for (int i = 0; i < 30; i++) {
        if (i == 29) { code = i; break; }
        if (distance < dist_base[i + 1]) { code = i; break; }
    }
    uint32_t extra = distance - dist_base[code];

    /* Distance codes are 5-bit, reversed */
    bw_write_rev(bw, code, 5);

    if (dist_extra[code] > 0)
        bw_write(bw, extra, dist_extra[code]);
}

/* ── LZ77 hash chain ────────────────────────────────────────── */

#define HASH_SIZE  4096
#define HASH_MASK  (HASH_SIZE - 1)
#define MAX_MATCH  258
#define MIN_MATCH  3
#define WINDOW     32768

static uint32_t lz77_hash(const uint8_t *p)
{
    return ((uint32_t)p[0] * 31 * 31 + (uint32_t)p[1] * 31 + p[2]) & HASH_MASK;
}

/* ── Main deflate ───────────────────────────────────────────── */

int zlib_deflate(const uint8_t *src, uint32_t src_len,
                 uint8_t *dst, uint32_t *dst_len)
{
    uint32_t max = *dst_len;
    if (max < 8) return -1;

    /* zlib header: CMF=0x78 (deflate, window 32K), FLG=0x01 (no dict, level 0) */
    /* CMF*256 + FLG must be divisible by 31 */
    dst[0] = 0x78;
    dst[1] = 0x01; /* 0x7801 % 31 == 0 ✓ */

    bitwriter_t bw;
    bw_init(&bw, dst + 2, max - 6); /* reserve 4 bytes for adler32 */

    /* Single fixed Huffman block (BFINAL=1, BTYPE=01) */
    bw_write(&bw, 1, 1); /* BFINAL */
    bw_write(&bw, 1, 2); /* BTYPE = fixed Huffman */

    /* LZ77 with hash chain */
    int16_t hash_head[HASH_SIZE];
    int16_t hash_prev[WINDOW];
    for (int i = 0; i < HASH_SIZE; i++) hash_head[i] = -1;

    uint32_t i = 0;
    while (i < src_len) {
        uint32_t best_len = 0, best_dist = 0;

        if (i + MIN_MATCH <= src_len) {
            uint32_t h = lz77_hash(src + i);
            int16_t pos = hash_head[h];
            int chain = 0;

            while (pos >= 0 && chain < 32) {
                uint32_t d = i - (uint32_t)pos;
                if (d > WINDOW || d == 0) break;

                /* Count match length */
                uint32_t ml = 0;
                uint32_t limit = src_len - i;
                if (limit > MAX_MATCH) limit = MAX_MATCH;
                while (ml < limit && src[pos + ml] == src[i + ml])
                    ml++;

                if (ml >= MIN_MATCH && ml > best_len) {
                    best_len = ml;
                    best_dist = d;
                    if (ml == MAX_MATCH) break;
                }

                pos = hash_prev[(uint32_t)pos & (WINDOW - 1)];
                chain++;
            }

            /* Update hash chain */
            if (i < WINDOW)
                hash_prev[i & (WINDOW - 1)] = hash_head[h];
            else
                hash_prev[i & (WINDOW - 1)] = -1;
            hash_head[h] = (i < 32768) ? (int16_t)i : -1;
        }

        if (best_len >= MIN_MATCH) {
            emit_length(&bw, best_len);
            emit_distance(&bw, best_dist);
            /* Insert skipped positions into hash */
            for (uint32_t j = 1; j < best_len && i + j + 2 < src_len; j++) {
                uint32_t h2 = lz77_hash(src + i + j);
                uint32_t pos2 = i + j;
                if (pos2 < WINDOW)
                    hash_prev[pos2 & (WINDOW - 1)] = hash_head[h2];
                else
                    hash_prev[pos2 & (WINDOW - 1)] = -1;
                hash_head[h2] = (pos2 < 32768) ? (int16_t)pos2 : -1;
            }
            i += best_len;
        } else {
            emit_literal(&bw, src[i]);
            i++;
        }
    }

    emit_eob(&bw);
    bw_flush(&bw);

    /* Adler-32 trailer (big-endian) */
    uint32_t adler = zlib_adler32(src, src_len);
    uint32_t total = 2 + bw.pos;
    if (total + 4 > max) return -1;
    dst[total]     = (uint8_t)(adler >> 24);
    dst[total + 1] = (uint8_t)(adler >> 16);
    dst[total + 2] = (uint8_t)(adler >> 8);
    dst[total + 3] = (uint8_t)(adler);

    *dst_len = total + 4;
    return 0;
}
