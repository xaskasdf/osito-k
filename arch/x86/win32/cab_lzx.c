/* LZX stream decoder for CAB folders. Format references: Microsoft's Cabinet
 * SDK LZX specification and MS-PATCH 2.1/2.3/2.5 for its corrected tree order,
 * path-length deltas and 1/2 MiB slot counts. No delta reference data, extended
 * matches, or WIM block headers are accepted. */
#include "cab_lzx.h"
extern void *kmalloc(uint64_t size);
extern void kfree(void *pointer);

#define LZX_MAIN 656U
#define LZX_FAST_BITS 9U
typedef struct {
    uint16_t count[17], first_symbol[17], symbols[LZX_MAIN];
    uint32_t first_code[17];
    uint16_t fast[1U << LZX_FAST_BITS];
} LZX_TREE;
typedef struct {
    const uint8_t *input;
    uint32_t size, offset;
    uint64_t bits;
    unsigned available, bad;
} LZX_BITS;
struct cab_lzx {
    uint32_t window, produced, remaining, repeat[3], translation_size;
    unsigned elements, header, block_type, raw_odd;
    uint8_t main_lengths[LZX_MAIN], length_lengths[249];
    LZX_TREE main, length, aligned, pretree;
};

static uint32_t lzx_bits(LZX_BITS *b, unsigned count)
{
    if (!count) return 0;
    while (b->available < count) {
        if (b->size - b->offset < 2) { b->bad = 1; return 0; }
        unsigned word = b->input[b->offset] | (unsigned)b->input[b->offset + 1] << 8;
        b->offset += 2;
        b->bits = (b->bits << 16) | word;
        b->available += 16;
    }
    b->available -= count;
    return (uint32_t)(b->bits >> b->available) & ((1U << count) - 1U);
}

static int lzx_build_tree(LZX_TREE *t, const uint8_t *lengths, unsigned count)
{
    memset(t, 0, sizeof(*t));
    for (unsigned i = 0; i < count; i++) {
        if (lengths[i] > 16) return -1;
        if (lengths[i]) t->count[lengths[i]]++;
    }
    unsigned used = 0, code = 0, slots = 1;
    for (unsigned n = 1; n <= 16; n++) {
        slots <<= 1;
        if (t->count[n] > slots) return -1;
        slots -= t->count[n];
        code = (code + t->count[n - 1]) << 1;
        t->first_code[n] = code;
        t->first_symbol[n] = (uint16_t)used;
        used += t->count[n];
    }
    /* Empty trees are valid when no symbol from that alphabet is used. */
    if (used && slots) return -1;
    uint16_t next[17];
    memcpy(next, t->first_symbol, sizeof(next));
    for (unsigned i = 0; i < count; i++) {
        unsigned n = lengths[i];
        if (!n) continue;
        unsigned index = next[n]++;
        t->symbols[index] = (uint16_t)i;
        if (n <= LZX_FAST_BITS) {
            unsigned value = t->first_code[n] + index - t->first_symbol[n];
            unsigned start = value << (LZX_FAST_BITS - n);
            unsigned end = start + (1U << (LZX_FAST_BITS - n));
            for (unsigned j = start; j < end; j++) t->fast[j] = (uint16_t)((n << 10) | i);
        }
    }
    return 0;
}

static int lzx_symbol(LZX_BITS *b, const LZX_TREE *t)
{
    if (b->available < LZX_FAST_BITS && b->size - b->offset >= 2) {
        unsigned word = b->input[b->offset] | (unsigned)b->input[b->offset + 1] << 8;
        b->offset += 2; b->bits = (b->bits << 16) | word; b->available += 16;
    }
    if (b->available >= LZX_FAST_BITS) {
        unsigned key = (unsigned)(b->bits >> (b->available - LZX_FAST_BITS)) & 511U;
        unsigned entry = t->fast[key];
        if (entry) { b->available -= entry >> 10; return (int)(entry & 1023U); }
    }
    unsigned code = 0;
    for (unsigned n = 1; n <= 16; n++) {
        code = (code << 1) | lzx_bits(b, 1);
        if (b->bad) return -1;
        if (code >= t->first_code[n] && code - t->first_code[n] < t->count[n])
            return t->symbols[t->first_symbol[n] + code - t->first_code[n]];
    }
    return -1;
}

static int lzx_read_lengths(cab_lzx_t *s, LZX_BITS *b, uint8_t *lengths, unsigned count)
{
    uint8_t pre[20];
    for (unsigned i = 0; i < 20; i++) pre[i] = (uint8_t)lzx_bits(b, 4);
    if (b->bad || lzx_build_tree(&s->pretree, pre, 20)) return -1;
    for (unsigned i = 0; i < count;) {
        int symbol = lzx_symbol(b, &s->pretree);
        if (symbol < 0) return -1;
        unsigned run = 1, value;
        if (symbol <= 16) value = (lengths[i] + 17U - (unsigned)symbol) % 17U;
        else if (symbol == 17 || symbol == 18) {
            run = lzx_bits(b, symbol == 17 ? 4 : 5) + (symbol == 17 ? 4U : 20U);
            value = 0;
        } else {
            run = 4U + lzx_bits(b, 1);
            symbol = lzx_symbol(b, &s->pretree);
            if (symbol < 0 || symbol > 16) return -1;
            value = (lengths[i] + 17U - (unsigned)symbol) % 17U;
        }
        if (b->bad || run > count - i) return -1;
        while (run--) lengths[i++] = (uint8_t)value;
    }
    return 0;
}

static int lzx_start_block(cab_lzx_t *s, LZX_BITS *b)
{
    s->block_type = lzx_bits(b, 3);
    s->remaining = lzx_bits(b, 24);
    if (b->bad || !s->remaining || s->block_type < 1 || s->block_type > 3) return -1;
    if (s->block_type == 3) {
        /* Raw data starts after 1..16 alignment bits, including a whole word
         * when the block header already ended on a word boundary. */
        lzx_bits(b, b->available ? b->available : 16);
        if (b->bad || b->size - b->offset < 12) return -1;
        for (unsigned i = 0; i < 3; i++) {
            const uint8_t *p = b->input + b->offset;
            s->repeat[i] = p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
            b->offset += 4;
        }
        s->raw_odd = s->remaining & 1U;
        return 0;
    }
    if (s->block_type == 2) {
        uint8_t lengths[8];
        for (unsigned i = 0; i < 8; i++) lengths[i] = (uint8_t)lzx_bits(b, 3);
        if (b->bad || lzx_build_tree(&s->aligned, lengths, 8)) return -1;
    }
    if (lzx_read_lengths(s, b, s->main_lengths, 256) ||
        lzx_read_lengths(s, b, s->main_lengths + 256, s->elements - 256) ||
        lzx_build_tree(&s->main, s->main_lengths, s->elements) ||
        lzx_read_lengths(s, b, s->length_lengths, 249) ||
        lzx_build_tree(&s->length, s->length_lengths, 249)) return -1;
    return 0;
}

static uint32_t lzx_offset(cab_lzx_t *s, LZX_BITS *b, unsigned slot)
{
    if (slot < 3) {
        uint32_t value = s->repeat[slot];
        s->repeat[slot] = s->repeat[0]; s->repeat[0] = value;
        return value;
    }
    unsigned extra = slot < 4 ? 0 : slot / 2 - 1;
    if (extra > 17) extra = 17;
    uint32_t base = slot < 4 ? slot : slot < 38 ? (2U + (slot & 1U)) << extra
                                                   : 524288U + (slot - 38U) * 131072U;
    uint32_t footer;
    if (s->block_type == 2 && extra >= 3) {
        footer = lzx_bits(b, extra - 3) << 3;
        int aligned = lzx_symbol(b, &s->aligned);
        if (aligned < 0) { b->bad = 1; return 0; }
        footer |= (unsigned)aligned;
    } else footer = lzx_bits(b, extra);
    uint32_t value = base + footer - 2;
    s->repeat[2] = s->repeat[1]; s->repeat[1] = s->repeat[0]; s->repeat[0] = value;
    return value;
}

cab_lzx_t *cab_lzx_create(unsigned window_bits)
{
    if (window_bits < 15 || window_bits > 21) return 0;
    cab_lzx_t *s = kmalloc(sizeof(*s));
    if (!s) return 0;
    memset(s, 0, sizeof(*s));
    s->window = 1U << window_bits;
    unsigned slots = window_bits < 20 ? window_bits * 2 : window_bits == 20 ? 42 : 50;
    s->elements = 256 + 8 * slots;
    s->repeat[0] = s->repeat[1] = s->repeat[2] = 1;
    return s;
}

void cab_lzx_destroy(cab_lzx_t *s) { if (s) kfree(s); }

int cab_lzx_frame(cab_lzx_t *s, const uint8_t *input, uint32_t input_size,
                  uint8_t *output, uint32_t offset, uint32_t frame_size)
{
    if (!s || !input || !output || !frame_size || frame_size > 32768 ||
        input_size > 38912 || offset != s->produced || (offset & 32767U) ||
        offset > 0x7fffffffU - frame_size) return -1;
    LZX_BITS b = { .input = input, .size = input_size };
    if (!s->header) {
        unsigned translated = lzx_bits(&b, 1);
        if (translated) {
            uint32_t high = lzx_bits(&b, 16);
            s->translation_size = (high << 16) | lzx_bits(&b, 16);
        }
        if (b.bad) return -1;
        s->header = 1;
    }
    uint32_t end = offset + frame_size, position = offset;
    while (position < end) {
        if (!s->remaining && lzx_start_block(s, &b)) return -1;
        uint32_t count = end - position;
        if (count > s->remaining) count = s->remaining;
        if (s->block_type == 3) {
            if (count > b.size - b.offset) return -1;
            memcpy(output + position, b.input + b.offset, count);
            b.offset += count; position += count; s->remaining -= count;
            if (!s->remaining && s->raw_odd) {
                if (b.offset == b.size) return -1;
                b.offset++; s->raw_odd = 0;
            }
            continue;
        }
        int symbol = lzx_symbol(&b, &s->main);
        if (symbol < 0) return -1;
        if (symbol < 256) { output[position++] = (uint8_t)symbol; s->remaining--; continue; }
        unsigned token = (unsigned)symbol - 256, length = token & 7U;
        if (length == 7) {
            int footer = lzx_symbol(&b, &s->length);
            if (footer < 0) return -1;
            length += (unsigned)footer;
        }
        length += 2;
        uint32_t distance = lzx_offset(s, &b, token >> 3);
        if (b.bad || !distance || distance >= s->window || distance > position || length > count) return -1;
        for (unsigned i = 0; i < length; i++) output[position + i] = output[position + i - distance];
        position += length; s->remaining -= length;
    }
    /* Only partial-word padding may remain in a compressed frame. */
    if (b.bad || b.offset != b.size || b.available >= 16) return -1;
    s->produced = end;
    return 0;
}

void cab_lzx_translate(cab_lzx_t *s, uint8_t *output, uint32_t size)
{
    if (!s || !s->translation_size || !output || size > s->produced) return;
    for (uint32_t frame = 0; frame < size && frame < 0x40000000U; frame += 32768) {
        uint32_t length = size - frame;
        if (length > 32768) length = 32768;
        if (length <= 10) continue;
        for (uint32_t i = 0; i < length - 10; i++) {
            uint8_t *p = output + frame + i;
            if (*p != 0xe8) continue;
            uint32_t raw = p[1] | (uint32_t)p[2] << 8 | (uint32_t)p[3] << 16 | (uint32_t)p[4] << 24;
            int64_t absolute = (int32_t)raw, current = frame + i;
            int64_t extent = (int32_t)s->translation_size;
            if (absolute >= -current && absolute < extent) {
                uint32_t relative = (uint32_t)(absolute >= 0 ? absolute - current : absolute + extent);
                for (unsigned j = 0; j < 4; j++) p[j + 1] = (uint8_t)(relative >> (j * 8));
            }
            i += 4;
        }
    }
}
