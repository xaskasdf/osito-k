/* Win32 Unicode folding. Immutable tables, per-call scratch, UTF-16 lengths. */
#include "fold_string.h"
#include "kernel32_shim.h"
#include "compat32.h"
extern void *kmalloc(uint64_t bytes);
extern void kfree(void *pointer);

typedef struct { uint32_t cp, offset; uint8_t length, compatibility; } fold_decomposition_t;
typedef struct { uint32_t cp; uint8_t value; } fold_combining_t;
typedef struct { uint32_t first, second, composed; } fold_composition_t;
typedef struct { uint32_t cp; uint8_t length; uint32_t values[4]; } fold_mapping_t;
#include "fold_tables.h"
#define COUNT(a) (sizeof(a)/sizeof((a)[0]))
#define FOLD_LIMIT 0x7fffffffU
#define FOLD_ZONE 0x10U
#define FOLD_PRECOMPOSED 0x20U
#define FOLD_COMPOSITE 0x40U
#define FOLD_DIGITS 0x80U
#define FOLD_LIGATURES 0x2000U

typedef struct { uint32_t *data, count, capacity; } fold_vector_t;

static int fold_push(fold_vector_t *v, uint32_t cp)
{
    if (v->count == FOLD_LIMIT) return 0;
    if (v->count == v->capacity) {
        uint32_t next = v->capacity ? v->capacity > FOLD_LIMIT/2 ?
                        FOLD_LIMIT : v->capacity*2 : 64;
        uint32_t *data = kmalloc((uint64_t)next*sizeof(*data));
        if (!data) return 0;
        if (v->count) memcpy(data, v->data, (uint64_t)v->count*sizeof(*data));
        kfree(v->data); v->data = data; v->capacity = next;
    }
    v->data[v->count++] = cp; return 1;
}

static const fold_mapping_t *fold_mapping(const fold_mapping_t *table,
                                          uint32_t count, uint32_t cp)
{
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        uint32_t mid = lo+(hi-lo)/2;
        if (table[mid].cp < cp) lo = mid+1; else hi = mid;
    }
    return lo < count && table[lo].cp == cp ? table+lo : NULL;
}

static uint8_t fold_ccc(uint32_t cp)
{
    uint32_t lo = 0, hi = COUNT(fold_combining_classes);
    while (lo < hi) {
        uint32_t mid = lo+(hi-lo)/2;
        if (fold_combining_classes[mid].cp < cp) lo = mid+1; else hi = mid;
    }
    return lo < COUNT(fold_combining_classes) && fold_combining_classes[lo].cp == cp ?
           fold_combining_classes[lo].value : 0;
}

static int fold_decompose(fold_vector_t *out, uint32_t cp, int compatibility)
{
    /* UAX #15 algorithmic Hangul decomposition; no 11,172-entry table. */
    if (cp >= 0xac00 && cp < 0xd7a4) {
        uint32_t index = cp-0xac00;
        return fold_push(out, 0x1100+index/588) &&
               fold_push(out, 0x1161+(index%588)/28) &&
               (!(index%28) || fold_push(out, 0x11a7+index%28));
    }
    uint32_t lo = 0, hi = COUNT(fold_decompositions);
    while (lo < hi) {
        uint32_t mid = lo+(hi-lo)/2;
        if (fold_decompositions[mid].cp < cp) lo = mid+1; else hi = mid;
    }
    if (lo == COUNT(fold_decompositions) || fold_decompositions[lo].cp != cp ||
        (fold_decompositions[lo].compatibility && !compatibility)) return fold_push(out, cp);
    const fold_decomposition_t *entry = fold_decompositions+lo;
    for (unsigned i = 0; i < entry->length; i++)
        if (!fold_decompose(out, fold_decomposition_values[entry->offset+i], compatibility)) return 0;
    return 1;
}

static int fold_order(fold_vector_t *v)
{
    /* Stable counting sort per non-starter run keeps long hostile combining
     * sequences linear in length, rather than quadratic insertion sorting. */
    uint32_t *scratch = NULL, allocated = 0;
    for (uint32_t start = 0; start < v->count;) {
        if (!fold_ccc(v->data[start])) { start++; continue; }
        uint32_t end = start+1; int sorted = 1;
        uint8_t last = fold_ccc(v->data[start]);
        while (end < v->count) {
            uint8_t cc = fold_ccc(v->data[end]);
            if (!cc) break;
            if (cc < last) sorted = 0;
            last = cc; end++;
        }
        uint32_t length = end-start;
        if (!sorted) {
            if (length > allocated) {
                kfree(scratch); scratch = kmalloc((uint64_t)length*4);
                if (!scratch) return 0;
                allocated = length;
            }
            uint32_t positions[256] = {0}, offset = 0;
            for (uint32_t i = start; i < end; i++) positions[fold_ccc(v->data[i])]++;
            for (unsigned i = 0; i < 256; i++) {
                uint32_t count = positions[i]; positions[i] = offset; offset += count;
            }
            for (uint32_t i = start; i < end; i++)
                scratch[positions[fold_ccc(v->data[i])]++] = v->data[i];
            memcpy(v->data+start, scratch, (uint64_t)length*4);
        }
        start = end;
    }
    kfree(scratch); return 1;
}

static uint32_t fold_composed(uint32_t a, uint32_t b)
{
    if (a >= 0x1100 && a < 0x1113 && b >= 0x1161 && b < 0x1176)
        return 0xac00+((a-0x1100)*21+b-0x1161)*28;
    if (a >= 0xac00 && a < 0xd7a4 && (a-0xac00)%28 == 0 && b > 0x11a7 && b < 0x11c3)
        return a+b-0x11a7;
    uint32_t lo = 0, hi = COUNT(fold_compositions);
    while (lo < hi) {
        uint32_t mid = lo+(hi-lo)/2;
        const fold_composition_t *entry = fold_compositions+mid;
        if (entry->first < a || (entry->first == a && entry->second < b)) lo = mid+1;
        else hi = mid;
    }
    return lo < COUNT(fold_compositions) && fold_compositions[lo].first == a &&
           fold_compositions[lo].second == b ? fold_compositions[lo].composed : 0;
}

static void fold_compose(fold_vector_t *v)
{
    uint32_t used = 0, starter = FOLD_LIMIT; uint8_t last = 0;
    for (uint32_t i = 0; i < v->count; i++) {
        uint32_t cp = v->data[i]; uint8_t cc = fold_ccc(cp);
        uint32_t combined = starter != FOLD_LIMIT && (last < cc || !last) ?
                            fold_composed(v->data[starter], cp) : 0;
        if (combined) { v->data[starter] = combined; continue; }
        if (!cc) starter = used;
        v->data[used++] = cp; last = cc;
    }
    v->count = used;
}

static unsigned fold_units(uint32_t cp) { return cp > 0xffff ? 2 : 1; }
static unsigned fold_emit(PWSTR out, uint32_t cp)
{
    if (cp <= 0xffff) { out[0] = (WCHAR)cp; return 1; }
    cp -= 0x10000; out[0] = (WCHAR)(0xd800+(cp>>10));
    out[1] = (WCHAR)(0xdc00+(cp&1023)); return 2;
}

int WINAPI FoldStringW(DWORD flags, PCWSTR source, int length,
                       PWSTR destination, int capacity)
{
    DWORD error = 0; int result = 0;
    fold_vector_t text = {0};
    if (!source || source == destination || !length || capacity < 0 ||
        (!destination && capacity)) { error = 87; goto done; }
    if (!flags || (flags & ~0x20f0U) || (flags & 0x60U) == 0x60U ||
        ((flags & FOLD_LIGATURES) && (flags & 0x60U))) { error = 1004; goto done; }
    BOOL compat = g_compat32_mode != 0;
    uint32_t count = length > 0 ? (uint32_t)length : 0;
    if (length < 0) {
        do {
            if (count == FOLD_LIMIT ||
                !win32_user_range_readable(source+count, 2, compat)) { error = 87; goto done; }
        } while (source[count++]);
    } else if (!win32_user_range_readable(source, (uint64_t)count*2, compat)) {
        error = 87; goto done;
    }
    int normalize = (flags & 0x70U) != 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t cp = source[i];
        if (cp >= 0xd800 && cp < 0xdc00 && i+1 < count &&
            source[i+1] >= 0xdc00 && source[i+1] <= 0xdfff)
            cp = 0x10000+((cp-0xd800)<<10)+(source[++i]-0xdc00);
        if (normalize && ((cp >= 0xd800 && cp <= 0xdfff) ||
            (cp >= 0xfdd0 && cp <= 0xfdef) || (cp & 0xffffU) >= 0xfffe)) {
            error = 1113; goto done;
        }
        const fold_mapping_t *map = normalize && (flags & FOLD_LIGATURES) ?
            fold_mapping(fold_ligatures, COUNT(fold_ligatures), cp) : NULL;
        unsigned entries = map ? map->length : 1;
        for (unsigned j = 0; j < entries; j++) {
            uint32_t value = map ? map->values[j] : cp;
            /* With normalization enabled Windows folds digits into the
             * temporary input first, so the normalization size is exact. */
            const fold_mapping_t *digit = normalize && (flags & FOLD_DIGITS) ?
                fold_mapping(fold_digits, COUNT(fold_digits), value) : NULL;
            if (digit) value = digit->values[0];
            if (!(normalize ? fold_decompose(&text, value, (flags & FOLD_ZONE) != 0) :
                              fold_push(&text, value))) { error = 14; goto done; }
        }
    }
    if (normalize) {
        if (!fold_order(&text)) { error = 14; goto done; }
        if (!(flags & FOLD_COMPOSITE)) fold_compose(&text);
    }
    /* Without normalization the digit-folding query returns the pre-digit
     * maximum, even when a supplementary digit contracts. */
    uint64_t required = 0;
    for (uint32_t i = 0; i < text.count; i++) {
        const fold_mapping_t *map = !normalize && (flags & FOLD_LIGATURES) ?
            fold_mapping(fold_ligatures, COUNT(fold_ligatures), text.data[i]) : NULL;
        if (map) for (unsigned j = 0; j < map->length; j++) required += fold_units(map->values[j]);
        else required += fold_units(text.data[i]);
    }
    if (required > FOLD_LIMIT) { error = 14; goto done; }
    if (!capacity) { result = (int)required; goto done; }
    /* Failure does not provide a usable translated string. Preflight the
     * whole buffer instead of exposing partially normalized characters. */
    if (required > (uint32_t)capacity) { error = 122; goto done; }
    if (!win32_user_range_writable(destination, required*2, compat)) { error = 87; goto done; }
    uint32_t used = 0;
    for (uint32_t i = 0; i < text.count; i++) {
        uint32_t cp = text.data[i];
        const fold_mapping_t *lig = !normalize && (flags & FOLD_LIGATURES) ?
            fold_mapping(fold_ligatures, COUNT(fold_ligatures), cp) : NULL;
        unsigned entries = lig ? lig->length : 1;
        for (unsigned j = 0; j < entries; j++) {
            uint32_t value = lig ? lig->values[j] : cp;
            const fold_mapping_t *digit = !normalize && (flags & FOLD_DIGITS) ?
                fold_mapping(fold_digits, COUNT(fold_digits), value) : NULL;
            if (digit) value = digit->values[0]; /* Generated digit maps are 1:1 scalars. */
            used += fold_emit(destination+used, value);
        }
    }
    result = (int)used;
done:
    kfree(text.data);
    if (error) SetLastError(error);
    return result;
}
