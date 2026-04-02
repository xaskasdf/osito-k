/*
 * uzdecomp — Unreal Tournament .uz file decompressor
 *
 * Decompresses .uz files (Unreal Engine 1 format) to their original
 * .unr/.utx/.u files. Pipeline: Huffman → MTF → BWT → RLE
 *
 * Based on Tim Sweeney's FCodec implementation from UT99 public source.
 *
 * Usage: uzdecomp input.uz [output_file]
 *        If output_file not given, uses the original filename from the .uz header.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── Dynamic buffer ─────────────────────────────────────────── */

typedef struct {
    uint8_t *data;
    int      len;
    int      cap;
    int      pos;  /* read position */
} buf_t;

static void buf_init(buf_t *b)
{
    b->data = NULL;
    b->len = b->cap = b->pos = 0;
}

static void buf_free(buf_t *b)
{
    free(b->data);
    buf_init(b);
}

static void buf_ensure(buf_t *b, int need)
{
    if (b->cap >= need) return;
    int nc = b->cap ? b->cap : 4096;
    while (nc < need) nc *= 2;
    b->data = realloc(b->data, nc);
    b->cap = nc;
}

static void buf_push(buf_t *b, uint8_t byte)
{
    buf_ensure(b, b->len + 1);
    b->data[b->len++] = byte;
}

static int buf_read(buf_t *b, void *dst, int n)
{
    int avail = b->len - b->pos;
    if (n > avail) n = avail;
    if (n > 0) { memcpy(dst, b->data + b->pos, n); b->pos += n; }
    return n;
}

static int buf_at_end(buf_t *b)
{
    return b->pos >= b->len;
}

static uint8_t buf_read8(buf_t *b)
{
    uint8_t v = 0;
    buf_read(b, &v, 1);
    return v;
}

static int32_t buf_read32(buf_t *b)
{
    uint8_t t[4];
    buf_read(b, t, 4);
    return (int32_t)(t[0] | (t[1]<<8) | (t[2]<<16) | ((uint32_t)t[3]<<24));
}

/* ── Bit reader ─────────────────────────────────────────────── */

typedef struct {
    const uint8_t *data;
    int    total_bits;
    int    pos;  /* current bit position */
} bitreader_t;

static void br_init(bitreader_t *r, const uint8_t *data, int num_bytes)
{
    r->data = data;
    r->total_bits = num_bytes * 8;
    r->pos = 0;
}

static int br_read_bit(bitreader_t *r)
{
    if (r->pos >= r->total_bits) return 0;
    int byte_idx = r->pos / 8;
    int bit_idx  = r->pos % 8;
    r->pos++;
    return (r->data[byte_idx] >> bit_idx) & 1;
}

static uint8_t br_read_byte(bitreader_t *r)
{
    uint8_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= (br_read_bit(r) << i);
    return v;
}

static int br_at_end(bitreader_t *r)
{
    return r->pos >= r->total_bits;
}

/* ── Huffman tree ───────────────────────────────────────────── */

typedef struct huff_node {
    int ch;  /* -1 = internal node */
    struct huff_node *child[2];
} huff_node_t;

static huff_node_t *huff_alloc(int ch)
{
    huff_node_t *n = calloc(1, sizeof(huff_node_t));
    n->ch = ch;
    return n;
}

static void huff_free(huff_node_t *n)
{
    if (!n) return;
    huff_free(n->child[0]);
    huff_free(n->child[1]);
    free(n);
}

static void huff_read_table(huff_node_t *n, bitreader_t *r)
{
    if (br_read_bit(r)) {
        /* internal node: has 2 children */
        n->child[0] = huff_alloc(-1);
        n->child[1] = huff_alloc(-1);
        huff_read_table(n->child[0], r);
        huff_read_table(n->child[1], r);
    } else {
        /* leaf: read 8-bit character */
        n->ch = br_read_byte(r);
    }
}

/* ── Huffman decode ─────────────────────────────────────────── */

static void huffman_decode(buf_t *in, buf_t *out)
{
    /* Read total output byte count (INT32 LE) */
    int32_t total = buf_read32(in);

    /* Remaining data is the bitstream */
    int bit_data_len = in->len - in->pos;
    bitreader_t reader;
    br_init(&reader, in->data + in->pos, bit_data_len);

    /* Read Huffman tree */
    huff_node_t *root = huff_alloc(-1);
    huff_read_table(root, &reader);

    /* Decode symbols */
    buf_ensure(out, total);
    for (int32_t i = 0; i < total; i++) {
        huff_node_t *node = root;
        while (node->ch == -1) {
            int bit = br_read_bit(&reader);
            node = node->child[bit];
        }
        buf_push(out, (uint8_t)node->ch);
    }

    huff_free(root);
}

/* ── MTF (Move-to-Front) decode ─────────────────────────────── */

static void mtf_decode(buf_t *in, buf_t *out)
{
    uint8_t list[256];
    for (int i = 0; i < 256; i++) list[i] = i;

    buf_ensure(out, in->len - in->pos);
    while (!buf_at_end(in)) {
        uint8_t idx = buf_read8(in);
        uint8_t ch = list[idx];
        buf_push(out, ch);

        /* Move ch to position 0 */
        for (int i = idx; i > 0; i--)
            list[i] = list[i - 1];
        list[0] = ch;
    }
}

/* ── BWT (Burrows-Wheeler Transform) decode ─────────────────── */

#define BWT_MAX_BUFFER 0x40000

static void bwt_decode(buf_t *in, buf_t *out)
{
    int32_t *temp = malloc((BWT_MAX_BUFFER + 2) * sizeof(int32_t));
    uint8_t *decomp = malloc(BWT_MAX_BUFFER + 2);

    while (!buf_at_end(in)) {
        int32_t compress_length = buf_read32(in);
        int32_t first           = buf_read32(in);
        int32_t last            = buf_read32(in);

        if (compress_length <= 0 || compress_length > BWT_MAX_BUFFER) {
            fprintf(stderr, "BWT: invalid chunk length %d\n", compress_length);
            break;
        }

        int decomp_length = compress_length + 1;

        /* Read BWT-transformed data (CompressLength + 1 bytes) */
        if (buf_read(in, decomp, decomp_length) < decomp_length) {
            fprintf(stderr, "BWT: truncated data\n");
            break;
        }

        /* Inverse BWT using counting sort */
        int count[257];
        int running_total[257];
        memset(count, 0, sizeof(count));

        /* Count occurrences (sentinel at position 'last' maps to symbol 256) */
        for (int i = 0; i < decomp_length; i++) {
            int sym = (i != last) ? decomp[i] : 256;
            count[sym]++;
        }

        /* Cumulative sums */
        int sum = 0;
        int dc[257];
        memset(dc, 0, sizeof(dc));
        for (int i = 0; i < 257; i++) {
            running_total[i] = sum;
            sum += count[i];
            /* dc used as per-symbol counter below */
        }

        /* Build transform vector */
        for (int i = 0; i < decomp_length; i++) {
            int sym = (i != last) ? decomp[i] : 256;
            temp[running_total[sym] + dc[sym]] = i;
            dc[sym]++;
        }

        /* Follow the chain from 'first' for (decomp_length - 1) steps */
        int idx = first;
        for (int j = 0; j < decomp_length - 1; j++) {
            buf_push(out, decomp[idx]);
            idx = temp[idx];
        }
    }

    free(temp);
    free(decomp);
}

/* ── RLE decode ─────────────────────────────────────────────── */

#define RLE_LEAD 5

static void rle_decode(buf_t *in, buf_t *out)
{
    int count = 0;
    uint8_t prev_char = 0;

    while (!buf_at_end(in)) {
        uint8_t b = buf_read8(in);
        buf_push(out, b);

        if (b != prev_char) {
            prev_char = b;
            count = 1;
        } else if (++count == RLE_LEAD) {
            /* Next byte is total run count */
            if (buf_at_end(in)) break;
            uint8_t c = buf_read8(in);
            int extra = c - RLE_LEAD;
            for (int i = 0; i < extra; i++)
                buf_push(out, b);
            count = 0;
        }
    }
}

/* ── Read Unreal compact index ──────────────────────────────── */

static int read_compact_index(FILE *f)
{
    uint8_t b;
    if (fread(&b, 1, 1, f) != 1) return 0;

    int sign  = b & 0x80;
    int value = b & 0x3F;

    if (b & 0x40) {
        int shift = 6;
        do {
            if (fread(&b, 1, 1, f) != 1) break;
            value |= (b & 0x7F) << shift;
            shift += 7;
        } while (b & 0x80);
    }

    return sign ? -value : value;
}

/* ── Main ───────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s input.uz [output_file]\n", argv[0]);
        return 1;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "Cannot open: %s\n", argv[1]);
        return 1;
    }

    /* Read header */
    int32_t signature;
    if (fread(&signature, 4, 1, f) != 1) {
        fprintf(stderr, "Cannot read signature\n");
        fclose(f);
        return 1;
    }

    int new_format = 0;
    if (signature == 1234) {
        /* Standard format: RLE → BWT → MTF → Huffman */
    } else if (signature == 5678) {
        /* New format: RLE → BWT → MTF → RLE → Huffman */
        new_format = 1;
    } else {
        fprintf(stderr, "Unknown signature: %d (expected 1234 or 5678)\n", signature);
        fclose(f);
        return 1;
    }

    /* Read original filename */
    int name_len = read_compact_index(f);
    char orig_name[256];
    if (name_len <= 0 || name_len > 255) {
        fprintf(stderr, "Invalid filename length: %d\n", name_len);
        fclose(f);
        return 1;
    }
    if ((int)fread(orig_name, 1, name_len, f) != name_len) {
        fprintf(stderr, "Cannot read filename\n");
        fclose(f);
        return 1;
    }
    orig_name[name_len] = 0;
    /* Strip null terminator if present */
    if (name_len > 0 && orig_name[name_len - 1] == 0)
        orig_name[name_len - 1] = 0;

    fprintf(stderr, "Signature: %d (%s format)\n", signature,
            new_format ? "new (extra RLE)" : "standard");
    fprintf(stderr, "Original: %s\n", orig_name);

    /* Read compressed payload */
    long payload_start = ftell(f);
    fseek(f, 0, SEEK_END);
    long payload_size = ftell(f) - payload_start;
    fseek(f, payload_start, SEEK_SET);

    buf_t compressed;
    buf_init(&compressed);
    buf_ensure(&compressed, payload_size);
    compressed.len = fread(compressed.data, 1, payload_size, f);
    fclose(f);

    fprintf(stderr, "Compressed: %ld bytes\n", payload_size);

    /* Decompress: reverse the codec chain */
    /* Encode order (sig 1234): RLE → BWT → MTF → Huffman */
    /* Decode order (sig 1234): Huffman → MTF → BWT → RLE */
    /* Encode order (sig 5678): RLE → BWT → MTF → RLE → Huffman */
    /* Decode order (sig 5678): Huffman → RLE → MTF → BWT → RLE */

    buf_t stage1, stage2, stage3, stage4;
    buf_init(&stage1);
    buf_init(&stage2);
    buf_init(&stage3);
    buf_init(&stage4);

    /* Stage 1: Huffman decode */
    fprintf(stderr, "  Huffman decode...\n");
    huffman_decode(&compressed, &stage1);
    buf_free(&compressed);
    fprintf(stderr, "    → %d bytes\n", stage1.len);

    buf_t *current = &stage1;

    if (new_format) {
        /* Stage 1.5: Extra RLE decode */
        fprintf(stderr, "  RLE decode (extra pass)...\n");
        rle_decode(current, &stage2);
        buf_free(current);
        fprintf(stderr, "    → %d bytes\n", stage2.len);
        current = &stage2;
    }

    /* Stage 2: MTF decode */
    buf_t mtf_out;
    buf_init(&mtf_out);
    fprintf(stderr, "  MTF decode...\n");
    mtf_decode(current, &mtf_out);
    buf_free(current);
    fprintf(stderr, "    → %d bytes\n", mtf_out.len);

    /* Stage 3: BWT decode */
    buf_t bwt_out;
    buf_init(&bwt_out);
    fprintf(stderr, "  BWT decode...\n");
    bwt_decode(&mtf_out, &bwt_out);
    buf_free(&mtf_out);
    fprintf(stderr, "    → %d bytes\n", bwt_out.len);

    /* Stage 4: RLE decode */
    buf_t final_out;
    buf_init(&final_out);
    fprintf(stderr, "  RLE decode...\n");
    rle_decode(&bwt_out, &final_out);
    buf_free(&bwt_out);
    fprintf(stderr, "    → %d bytes\n", final_out.len);

    /* Write output */
    const char *out_name = (argc >= 3) ? argv[2] : orig_name;
    FILE *out = fopen(out_name, "wb");
    if (!out) {
        fprintf(stderr, "Cannot create: %s\n", out_name);
        buf_free(&final_out);
        return 1;
    }
    fwrite(final_out.data, 1, final_out.len, out);
    fclose(out);

    fprintf(stderr, "Wrote: %s (%d bytes)\n", out_name, final_out.len);
    buf_free(&final_out);

    return 0;
}
