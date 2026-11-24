/*
 * OsitoK x86-64 — HID Report Descriptor parser
 *
 * Parses USB HID Class Report Descriptors per HID 1.11 spec §6.2.2.
 * Extracts the bit-layout of input reports so the runtime can demux
 * arbitrary HID devices (boot-protocol or report-protocol, simple or
 * composite) without hardcoded assumptions.
 *
 * Usage:
 *   hid_caps_t caps;
 *   hid_parse(report_desc, desc_len, &caps);
 *   if (caps.has_keyboard) { ... use caps.kbd_* offsets ... }
 *   if (caps.has_mouse)    { ... use caps.mouse_* offsets ... }
 *
 * Field extraction at runtime:
 *   uint32_t v = hid_extract(report, bit_off, bit_size);
 *   int32_t  s = hid_extract_signed(report, bit_off, bit_size);
 *
 * NOTE: This is a "useful subset" parser — sufficient for keyboards,
 * mice, tablets, and most gamepads. It tracks Usage Pages 0x01
 * (Generic Desktop), 0x07 (Keyboard), 0x09 (Buttons), 0x0C (Consumer)
 * and the typical Main/Global/Local item set. Long items (0xFE), Push
 * (0xA4), Pop (0xB4), and Designator/String items are accepted but
 * ignored.
 */

#ifndef HID_PARSER_H
#define HID_PARSER_H

#include "../include/types.h"

/* Maximum input fields tracked per device (Input Main items). */
#define HID_MAX_FIELDS 32

/* Usage Pages we recognize. */
#define HID_PAGE_DESKTOP   0x01  /* X, Y, Wheel, etc. */
#define HID_PAGE_KEYBOARD  0x07  /* Keys + modifier byte */
#define HID_PAGE_BUTTON    0x09  /* Mouse buttons */
#define HID_PAGE_CONSUMER  0x0C  /* Volume, media keys */

/* Generic Desktop Usages. */
#define HID_USAGE_X        0x30
#define HID_USAGE_Y        0x31
#define HID_USAGE_WHEEL    0x38

/* Input flag bits (per HID 1.11 §6.2.2.5). */
#define HID_INPUT_CONST    (1 << 0)  /* Constant data (padding) */
#define HID_INPUT_VAR      (1 << 1)  /* Variable (vs Array) */
#define HID_INPUT_REL      (1 << 2)  /* Relative (vs Absolute) */

/*
 * One Input field as described by an Input main item plus the global
 * state at the time it was emitted. A field with VAR=1 has `count`
 * independent values, each `bit_size` bits wide, mapped to usages
 * usage_min..usage_max (one per element). With VAR=0 (Array), each
 * element holds a single usage index value.
 */
typedef struct {
    uint16_t bit_offset;   /* Offset in bits from start of report (post-ID). */
    uint16_t bit_size;     /* Size of each element in bits. */
    uint16_t count;        /* Number of elements. */
    uint16_t usage_page;   /* Usage Page this field belongs to. */
    uint16_t usage_min;    /* Usage range start (inclusive). */
    uint16_t usage_max;    /* Usage range end (inclusive). */
    int32_t  logical_min;  /* Logical minimum (signed). */
    int32_t  logical_max;  /* Logical maximum (signed). */
    uint8_t  flags;        /* HID_INPUT_* flags. */
    uint8_t  report_id;    /* 0 if no Report ID grouping. */
} hid_field_t;

/*
 * Parsed device capabilities. After hid_parse(), the kernel can ask
 * "does this device deliver keyboard reports?" via has_keyboard, and
 * if so, where in the report to find the modifier byte and keycode
 * array. Same for mouse.
 *
 * For keyboard reports we expect either:
 *   - Boot-protocol style: 1 byte modifiers (8 button flags, page 0x07
 *     usages 0xE0..0xE7) + 6 bytes keycode array (page 0x07, 0x00..0xFF).
 *   - Report-protocol with the same usage layout but possibly different
 *     bit packing.
 *
 * For mice: a button bitmap (page 0x09) + relative X (page 0x01 usage
 * 0x30) + relative Y (page 0x01 usage 0x31) + optional wheel.
 */
typedef struct {
    hid_field_t fields[HID_MAX_FIELDS];
    uint8_t     n_fields;

    bool        has_report_id;     /* True if any Report ID was declared. */
    uint16_t    total_bits;        /* Largest Input report size in bits. */

    /* ── Keyboard quick-lookup (set if has_keyboard). ── */
    bool        has_keyboard;
    int         kbd_mods_field;    /* Index of variable field on page 7 covering 0xE0..0xE7. */
    int         kbd_keys_field;    /* Index of array field on page 7 (keycode slots). */
    uint8_t     kbd_report_id;     /* 0 if no ID. */

    /* ── Mouse quick-lookup (set if has_mouse). ── */
    bool        has_mouse;
    int         mouse_btn_field;   /* Page 9 button field. */
    int         mouse_x_field;     /* Page 1 usage 0x30. */
    int         mouse_y_field;     /* Page 1 usage 0x31. */
    int         mouse_wheel_field; /* Page 1 usage 0x38, -1 if absent. */
    /* Index of the X/Y/wheel element WITHIN its declaring field.
     * A descriptor that uses Usage_Minimum(X)..Usage_Maximum(Y) with
     * REPORT_COUNT(2) (QEMU usb-mouse, most boot mice) declares X and
     * Y as two consecutive elements of one field. Without recording
     * the element index here the extractor reads `bit_offset` for
     * both axes → identical dx/dy → cursor moves only on the diagonal. */
    uint8_t     mouse_x_elem;
    uint8_t     mouse_y_elem;
    uint8_t     mouse_wheel_elem;
    uint8_t     mouse_report_id;   /* 0 if no ID. */
} hid_caps_t;

/*
 * Parse a HID Report Descriptor.
 *
 * Returns 0 on success, -1 on malformed input. Partial parses are
 * possible: the function clears `out` first and fills as it walks; if
 * it returns -1 the caller should not trust has_keyboard/has_mouse.
 *
 * `desc` points to the raw descriptor bytes, `len` is its size in bytes.
 */
int hid_parse(const uint8_t *desc, uint16_t len, hid_caps_t *out);

/*
 * Extract an unsigned value of `bit_size` bits from `report` at the
 * given `bit_offset`. Handles non-byte-aligned and odd-width fields.
 * For sizes >32 bits the result is truncated.
 */
uint32_t hid_extract(const uint8_t *report, uint16_t bit_offset, uint8_t bit_size);

/*
 * Same as hid_extract but sign-extends the result based on `bit_size`.
 * Useful for relative axes (mouse dx/dy).
 */
int32_t hid_extract_signed(const uint8_t *report, uint16_t bit_offset, uint8_t bit_size);

#endif /* HID_PARSER_H */
