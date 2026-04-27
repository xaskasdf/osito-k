/*
 * OsitoK x86-64 — HID Report Descriptor parser implementation
 *
 * See hid_parser.h for API and design notes.
 *
 * The parser is a single forward pass over the descriptor. It maintains
 * three sets of state per HID 1.11 §6.2.2:
 *
 *   Global state — sticky across items (Usage Page, Logical Min/Max,
 *                  Report Size, Report Count, Report ID).
 *   Local state  — cleared after every Main item (Usage, Usage Min/Max).
 *   Bit cursor   — current bit offset in the active Input report.
 *
 * On each Input main item we emit one or more hid_field_t records into
 * `out->fields[]` and advance the bit cursor. After the walk we scan
 * the recorded fields to populate the keyboard/mouse quick-lookup
 * indices.
 *
 * Long items (prefix 0xFE) are accepted and skipped; we don't attempt
 * to interpret them. Push (0xA4) and Pop (0xB4) are accepted and the
 * stack is honored — keyboards rarely use them but games do.
 */

#include "hid_parser.h"
#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t v);
extern void serial_puthex(uint64_t v, int width);

/* HID short-item prefix layout: bSize(2) | bType(2) | bTag(4).
 * bType: 0=Main, 1=Global, 2=Local, 3=Reserved. */
#define ITEM_TYPE(p)  (((p) >> 2) & 0x3)
#define ITEM_TAG(p)   (((p) >> 4) & 0xF)
#define ITEM_SIZE(p)  ((p) & 0x3)  /* 0,1,2,3 → 0,1,2,4 bytes of data */

/* Main item tags. */
#define MAIN_INPUT       0x8
#define MAIN_OUTPUT      0x9
#define MAIN_FEATURE     0xB
#define MAIN_COLLECTION  0xA
#define MAIN_END_COLL    0xC

/* Global item tags. */
#define GLOB_USAGE_PAGE  0x0
#define GLOB_LOGICAL_MIN 0x1
#define GLOB_LOGICAL_MAX 0x2
#define GLOB_PHYSICAL_MIN 0x3
#define GLOB_PHYSICAL_MAX 0x4
#define GLOB_UNIT_EXP    0x5
#define GLOB_UNIT        0x6
#define GLOB_REPORT_SIZE 0x7
#define GLOB_REPORT_ID   0x8
#define GLOB_REPORT_CNT  0x9
#define GLOB_PUSH        0xA
#define GLOB_POP         0xB

/* Local item tags. */
#define LOC_USAGE        0x0
#define LOC_USAGE_MIN    0x1
#define LOC_USAGE_MAX    0x2

/* Globals stack for Push/Pop. */
#define GLOB_STACK_DEPTH 4

typedef struct {
    uint16_t usage_page;
    int32_t  logical_min;
    int32_t  logical_max;
    uint8_t  report_size;
    uint8_t  report_id;
    uint16_t report_count;
} hid_global_t;

/* Read `size_code` (0..3 → 0,1,2,4 bytes) of unsigned data from `*p`,
 * advance the pointer. Caller has already verified at least that many
 * bytes remain. */
static uint32_t read_uval(const uint8_t **p, uint8_t size_code)
{
    uint32_t v = 0;
    int n = (size_code == 3) ? 4 : size_code;
    const uint8_t *q = *p;
    for (int i = 0; i < n; i++)
        v |= ((uint32_t)q[i]) << (i * 8);
    *p += n;
    return v;
}

/* Same but sign-extended based on the byte width. */
static int32_t read_sval(const uint8_t **p, uint8_t size_code)
{
    uint32_t u = read_uval(p, size_code);
    int n = (size_code == 3) ? 4 : size_code;
    if (n == 0) return 0;
    int bits = n * 8;
    uint32_t sign_bit = 1u << (bits - 1);
    if (u & sign_bit) {
        /* Sign-extend by setting all bits above bit (bits-1). */
        u |= ~((1u << bits) - 1);
    }
    return (int32_t)u;
}

/* Track per-Main-item local state. Cleared after each Main. */
typedef struct {
    uint16_t usage[16];   /* Usage queue — up to 16 explicit Usage items per Main. */
    uint8_t  usage_count;
    uint16_t usage_min;
    uint16_t usage_max;
    bool     has_usage_range;
} hid_local_t;

static void emit_input_field(hid_caps_t *out,
                              const hid_global_t *g,
                              const hid_local_t *l,
                              uint8_t flags,
                              uint16_t *bit_cursor)
{
    uint32_t total_bits = (uint32_t)g->report_size * (uint32_t)g->report_count;

    /* Constant padding fields don't deserve a slot — they're ignored
     * at runtime. We still advance the bit cursor. */
    if (flags & HID_INPUT_CONST) {
        *bit_cursor += total_bits;
        return;
    }

    if (out->n_fields >= HID_MAX_FIELDS) {
        /* Out of slots — still advance the cursor so subsequent fields
         * keep correct offsets, but drop this one. */
        *bit_cursor += total_bits;
        return;
    }

    hid_field_t *f = &out->fields[out->n_fields++];
    f->bit_offset  = *bit_cursor;
    f->bit_size    = g->report_size;
    f->count       = g->report_count;
    f->usage_page  = g->usage_page;
    f->logical_min = g->logical_min;
    f->logical_max = g->logical_max;
    f->flags       = flags;
    f->report_id   = g->report_id;

    /* Decide usage range:
     *   - Explicit Usage Min/Max from local state takes priority.
     *   - Else, the first Usage in the local queue (Variable arrays use
     *     count usages from the queue; we just record min..min+count-1).
     *   - Else, leave 0..0 — caller can decide based on usage_page. */
    if (l->has_usage_range) {
        f->usage_min = l->usage_min;
        f->usage_max = l->usage_max;
    } else if (l->usage_count > 0) {
        if (flags & HID_INPUT_VAR) {
            /* Variable: each element has its own usage; if the local
             * queue has count entries use them; else fan out from the
             * first usage. */
            f->usage_min = l->usage[0];
            uint16_t last = (l->usage_count >= g->report_count)
                            ? l->usage[g->report_count - 1]
                            : (uint16_t)(l->usage[0] + g->report_count - 1);
            f->usage_max = last;
        } else {
            /* Array: one usage per element index, range of selectable
             * usages declared via Usage Min/Max — but the queue style
             * applies too. Pick the first usage as the only known. */
            f->usage_min = l->usage[0];
            f->usage_max = l->usage[0];
        }
    } else {
        f->usage_min = 0;
        f->usage_max = 0;
    }

    *bit_cursor += total_bits;
}

int hid_parse(const uint8_t *desc, uint16_t len, hid_caps_t *out)
{
    if (!desc || !out) return -1;

    /* Zero out output (sets has_keyboard/has_mouse=false, n_fields=0,
     * etc.). */
    for (uint32_t i = 0; i < sizeof(*out); i++)
        ((uint8_t *)out)[i] = 0;

    out->mouse_wheel_field = -1;
    out->kbd_mods_field    = -1;
    out->kbd_keys_field    = -1;
    out->mouse_btn_field   = -1;
    out->mouse_x_field     = -1;
    out->mouse_y_field     = -1;

    hid_global_t glob = {0};
    hid_global_t glob_stack[GLOB_STACK_DEPTH];
    int glob_sp = 0;

    hid_local_t loc = {0};

    uint16_t bit_cursor = 0;

    const uint8_t *p   = desc;
    const uint8_t *end = desc + len;

    while (p < end) {
        uint8_t prefix = *p++;

        /* Long item: 0xFE bSize bTag dataBytes... Skip. */
        if (prefix == 0xFE) {
            if (p + 2 > end) return -1;
            uint8_t data_size = *p++;
            /* tag */ p++;
            if (p + data_size > end) return -1;
            p += data_size;
            continue;
        }

        uint8_t size_code = ITEM_SIZE(prefix);
        uint8_t data_len  = (size_code == 3) ? 4 : size_code;
        if (p + data_len > end) return -1;

        uint8_t type = ITEM_TYPE(prefix);
        uint8_t tag  = ITEM_TAG(prefix);

        if (type == 0) {  /* Main */
            uint32_t mdata = read_uval(&p, size_code);

            switch (tag) {
            case MAIN_INPUT:
                emit_input_field(out, &glob, &loc, (uint8_t)mdata, &bit_cursor);
                if (bit_cursor > out->total_bits)
                    out->total_bits = bit_cursor;
                break;

            case MAIN_OUTPUT:
            case MAIN_FEATURE:
                /* Output and Feature reports use a separate bit space —
                 * we don't track them. Just consume. */
                break;

            case MAIN_COLLECTION:
            case MAIN_END_COLL:
                /* Collections group Usages — we don't need their
                 * structure, only the leaf Inputs. */
                break;

            default:
                break;
            }

            /* Local state is cleared after every Main item per HID 1.11 §6.2.2.8. */
            loc.usage_count = 0;
            loc.has_usage_range = false;
            loc.usage_min = 0;
            loc.usage_max = 0;

        } else if (type == 1) {  /* Global */
            switch (tag) {
            case GLOB_USAGE_PAGE: {
                /* Usage Page is unsigned. */
                uint32_t v = read_uval(&p, size_code);
                glob.usage_page = (uint16_t)v;
                break;
            }
            case GLOB_LOGICAL_MIN: glob.logical_min = read_sval(&p, size_code); break;
            case GLOB_LOGICAL_MAX: glob.logical_max = read_sval(&p, size_code); break;
            case GLOB_REPORT_SIZE: glob.report_size = (uint8_t)read_uval(&p, size_code); break;
            case GLOB_REPORT_ID: {
                glob.report_id = (uint8_t)read_uval(&p, size_code);
                /* HID 1.11 §6.2.2.7: when Report ID is used, every report
                 * begins with the 1-byte ID. We track it as a flag; the
                 * runtime extractor must skip the leading byte. */
                if (glob.report_id) {
                    out->has_report_id = true;
                    /* Reset cursor: each Report ID starts its own bit
                     * stream, but for our quick-lookup we record the
                     * first ID's offsets and assume single-ID devices
                     * are the common case. */
                    bit_cursor = 0;
                }
                break;
            }
            case GLOB_REPORT_CNT: glob.report_count = (uint16_t)read_uval(&p, size_code); break;
            case GLOB_PUSH:
                if (glob_sp < GLOB_STACK_DEPTH) glob_stack[glob_sp++] = glob;
                /* Skip data even though Push is supposed to be sizeless. */
                p += data_len;
                break;
            case GLOB_POP:
                if (glob_sp > 0) glob = glob_stack[--glob_sp];
                p += data_len;
                break;
            case GLOB_PHYSICAL_MIN:
            case GLOB_PHYSICAL_MAX:
            case GLOB_UNIT_EXP:
            case GLOB_UNIT:
                /* Accepted but ignored — affects scaling, not layout. */
                p += data_len;
                break;
            default:
                p += data_len;
                break;
            }

        } else if (type == 2) {  /* Local */
            switch (tag) {
            case LOC_USAGE: {
                uint32_t u = read_uval(&p, size_code);
                /* Short-form Usage (1 byte) inherits the current Usage
                 * Page as its high 16 bits. Long-form (4 bytes) carries
                 * the page in bits 16..31. */
                uint16_t usage = (size_code >= 3) ? (uint16_t)(u & 0xFFFF) : (uint16_t)u;
                if (loc.usage_count < 16)
                    loc.usage[loc.usage_count++] = usage;
                break;
            }
            case LOC_USAGE_MIN:
                loc.usage_min = (uint16_t)read_uval(&p, size_code);
                loc.has_usage_range = true;
                break;
            case LOC_USAGE_MAX:
                loc.usage_max = (uint16_t)read_uval(&p, size_code);
                loc.has_usage_range = true;
                break;
            default:
                /* Designator/String items — accept and ignore. */
                p += data_len;
                break;
            }

        } else {
            /* Reserved type — skip. */
            p += data_len;
        }
    }

    /* Post-pass: scan fields to identify keyboard/mouse capability. */
    for (int i = 0; i < out->n_fields; i++) {
        const hid_field_t *f = &out->fields[i];

        /* Keyboard modifier byte: page 7, variable, usage range covers
         * 0xE0..0xE7 (LCtrl..RGUI), bit_size=1, count=8 typically. */
        if (f->usage_page == HID_PAGE_KEYBOARD &&
            (f->flags & HID_INPUT_VAR) &&
            f->usage_min >= 0xE0 && f->usage_max <= 0xE7) {
            out->has_keyboard = true;
            out->kbd_mods_field = i;
            out->kbd_report_id = f->report_id;
        }

        /* Keyboard keycode array: page 7, NOT variable (Array), usually
         * count=6 (boot) but can be larger. logical_max gives the max
         * keycode value (typically 101 or 0xFF). */
        if (f->usage_page == HID_PAGE_KEYBOARD &&
            !(f->flags & HID_INPUT_VAR) &&
            f->bit_size == 8) {
            out->has_keyboard = true;
            out->kbd_keys_field = i;
            out->kbd_report_id = f->report_id;
        }

        /* Mouse buttons: page 9, variable. */
        if (f->usage_page == HID_PAGE_BUTTON &&
            (f->flags & HID_INPUT_VAR)) {
            out->has_mouse = true;
            out->mouse_btn_field = i;
            out->mouse_report_id = f->report_id;
        }

        /* Mouse X/Y/wheel: page 1, variable, single usage. */
        if (f->usage_page == HID_PAGE_DESKTOP && (f->flags & HID_INPUT_VAR)) {
            if (f->usage_min <= HID_USAGE_X && f->usage_max >= HID_USAGE_X) {
                out->has_mouse = true;
                out->mouse_x_field = i;
                out->mouse_report_id = f->report_id;
            }
            if (f->usage_min <= HID_USAGE_Y && f->usage_max >= HID_USAGE_Y) {
                out->has_mouse = true;
                out->mouse_y_field = i;
            }
            if (f->usage_min <= HID_USAGE_WHEEL && f->usage_max >= HID_USAGE_WHEEL) {
                out->mouse_wheel_field = i;
            }
        }
    }

    /* If we found keys but never a modifier byte, accept the device as
     * keyboard anyway — some compact keyboards omit the dedicated mod
     * byte and report mods as ordinary keys. */
    if (out->kbd_keys_field >= 0)
        out->has_keyboard = true;

    /* Mouse needs at least button or X/Y to count. */
    if (out->mouse_btn_field < 0 && out->mouse_x_field < 0 && out->mouse_y_field < 0)
        out->has_mouse = false;

    return 0;
}

uint32_t hid_extract(const uint8_t *report, uint16_t bit_offset, uint8_t bit_size)
{
    if (bit_size == 0 || bit_size > 32) return 0;

    /* Byte-aligned fast path. */
    if ((bit_offset & 7) == 0 && (bit_size & 7) == 0) {
        uint16_t byte_off = bit_offset >> 3;
        uint8_t  bytes    = bit_size >> 3;
        uint32_t v = 0;
        for (uint8_t i = 0; i < bytes; i++)
            v |= ((uint32_t)report[byte_off + i]) << (i * 8);
        return v;
    }

    /* Slow path: walk bit-by-bit. */
    uint32_t v = 0;
    for (uint8_t i = 0; i < bit_size; i++) {
        uint16_t bit = bit_offset + i;
        uint8_t byte = report[bit >> 3];
        if (byte & (1u << (bit & 7)))
            v |= (1u << i);
    }
    return v;
}

int32_t hid_extract_signed(const uint8_t *report, uint16_t bit_offset, uint8_t bit_size)
{
    uint32_t u = hid_extract(report, bit_offset, bit_size);
    if (bit_size == 0 || bit_size >= 32) return (int32_t)u;
    uint32_t sign_bit = 1u << (bit_size - 1);
    if (u & sign_bit)
        u |= ~((1u << bit_size) - 1);
    return (int32_t)u;
}
