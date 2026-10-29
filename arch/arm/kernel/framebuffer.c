/*
 * framebuffer.c -- Framebuffer text console for Osito-K AArch64
 *
 * 32bpp text rendering using embedded 8x16 bitmap font (ported from x86).
 * All functions gracefully no-op when fb_base == NULL (e.g. QEMU virt).
 * For SM8350, platform_init() calls fb_init_platform() with splash FB address,
 * and serial.c mirrors every console byte here.
 */

#include "../include/hal.h"
#include "../include/types.h"

/* ── Framebuffer state ───────────────────────────────────────── */

#define FONT_W  8
#define FONT_H  16
#define FG_COLOR 0x00CCCCCC   /* Light gray */
#define BG_COLOR 0x00000000   /* Black */

static volatile uint32_t *fb_base;
static uint32_t fb_width, fb_height, fb_pitch;
static uint32_t text_col, text_row, max_cols, max_rows;
static uint32_t text_y0;    /* first pixel row of the text area */
static uint32_t scale;      /* glyph magnification (2 on the 1080-wide phone panel) */

/* What each text cell shows: (fg << 8) | char, spaces stored as CELL_BLANK
 * whatever their color. Scrolling redraws from here and never reads the
 * framebuffer, which is uncached. 0 = not drawn by us yet (BSS). */
#define TEXT_MAX_CELLS  24576      /* 135 x 153: 8x16 glyphs on 1080x2448 */
#define TEXT_MAX_COLS   256
#define CELL_BLANK      ' '
static uint32_t cells[TEXT_MAX_CELLS];

/* History view (fb_history) */
static int viewing;         /* the text area shows the log, not the console */
static uint32_t view_top;   /* log offset of its top line, which stays put */
static int view_missed;     /* output arrived while it was up */

/* ── Minimal 8×16 bitmap font (ASCII 32-126) ─────────────────── */
/* Each glyph: 16 bytes (one byte per row, MSB left) */

static const uint8_t font8x16[95][16] = {
    /* 32 ' ' */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    /* 33 '!' */ {0,0,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0,0x18,0x18,0,0,0,0},
    /* 34 '"' */ {0,0x66,0x66,0x66,0x24,0,0,0,0,0,0,0,0,0,0,0},
    /* 35 '#' */ {0,0,0x6C,0x6C,0xFE,0x6C,0x6C,0x6C,0xFE,0x6C,0x6C,0,0,0,0,0},
    /* 36 '$' */ {0x18,0x18,0x7C,0xC6,0xC0,0x7C,0x06,0xC6,0x7C,0x18,0x18,0,0,0,0,0},
    /* 37 '%' */ {0,0,0xC6,0xC6,0x0C,0x18,0x30,0x60,0xC6,0xC6,0,0,0,0,0,0},
    /* 38 '&' */ {0,0,0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0,0,0,0,0,0,0},
    /* 39 ''' */ {0,0x18,0x18,0x18,0x30,0,0,0,0,0,0,0,0,0,0,0},
    /* 40 '(' */ {0,0,0x0C,0x18,0x30,0x30,0x30,0x30,0x18,0x0C,0,0,0,0,0,0},
    /* 41 ')' */ {0,0,0x30,0x18,0x0C,0x0C,0x0C,0x0C,0x18,0x30,0,0,0,0,0,0},
    /* 42 '*' */ {0,0,0,0x66,0x3C,0xFF,0x3C,0x66,0,0,0,0,0,0,0,0},
    /* 43 '+' */ {0,0,0,0x18,0x18,0x7E,0x18,0x18,0,0,0,0,0,0,0,0},
    /* 44 ',' */ {0,0,0,0,0,0,0,0,0x18,0x18,0x30,0,0,0,0,0},
    /* 45 '-' */ {0,0,0,0,0,0x7E,0,0,0,0,0,0,0,0,0,0},
    /* 46 '.' */ {0,0,0,0,0,0,0,0,0x18,0x18,0,0,0,0,0,0},
    /* 47 '/' */ {0,0,0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0,0,0,0,0,0,0},
    /* 48 '0' */ {0,0,0x7C,0xC6,0xCE,0xDE,0xF6,0xE6,0xC6,0x7C,0,0,0,0,0,0},
    /* 49 '1' */ {0,0,0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x7E,0,0,0,0,0,0},
    /* 50 '2' */ {0,0,0x7C,0xC6,0x06,0x0C,0x18,0x30,0x60,0xFE,0,0,0,0,0,0},
    /* 51 '3' */ {0,0,0x7C,0xC6,0x06,0x3C,0x06,0x06,0xC6,0x7C,0,0,0,0,0,0},
    /* 52 '4' */ {0,0,0x0C,0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0,0,0,0,0,0},
    /* 53 '5' */ {0,0,0xFE,0xC0,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0,0,0,0,0,0},
    /* 54 '6' */ {0,0,0x38,0x60,0xC0,0xFC,0xC6,0xC6,0xC6,0x7C,0,0,0,0,0,0},
    /* 55 '7' */ {0,0,0xFE,0xC6,0x06,0x0C,0x18,0x30,0x30,0x30,0,0,0,0,0,0},
    /* 56 '8' */ {0,0,0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0xC6,0x7C,0,0,0,0,0,0},
    /* 57 '9' */ {0,0,0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x18,0x70,0,0,0,0,0,0},
    /* 58 ':' */ {0,0,0,0x18,0x18,0,0,0x18,0x18,0,0,0,0,0,0,0},
    /* 59 ';' */ {0,0,0,0x18,0x18,0,0,0x18,0x18,0x30,0,0,0,0,0,0},
    /* 60 '<' */ {0,0,0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0,0,0,0,0,0,0},
    /* 61 '=' */ {0,0,0,0,0x7E,0,0x7E,0,0,0,0,0,0,0,0,0},
    /* 62 '>' */ {0,0,0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0,0,0,0,0,0,0},
    /* 63 '?' */ {0,0,0x7C,0xC6,0x06,0x0C,0x18,0x18,0,0x18,0,0,0,0,0,0},
    /* 64 '@' */ {0,0,0x7C,0xC6,0xDE,0xDE,0xDE,0xDC,0xC0,0x7C,0,0,0,0,0,0},
    /* 65 'A' */ {0,0,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0,0,0,0,0,0},
    /* 66 'B' */ {0,0,0xFC,0x66,0x66,0x7C,0x66,0x66,0x66,0xFC,0,0,0,0,0,0},
    /* 67 'C' */ {0,0,0x3C,0x66,0xC0,0xC0,0xC0,0xC0,0x66,0x3C,0,0,0,0,0,0},
    /* 68 'D' */ {0,0,0xF8,0x6C,0x66,0x66,0x66,0x66,0x6C,0xF8,0,0,0,0,0,0},
    /* 69 'E' */ {0,0,0xFE,0x62,0x68,0x78,0x68,0x60,0x62,0xFE,0,0,0,0,0,0},
    /* 70 'F' */ {0,0,0xFE,0x62,0x68,0x78,0x68,0x60,0x60,0xF0,0,0,0,0,0,0},
    /* 71 'G' */ {0,0,0x3C,0x66,0xC0,0xC0,0xCE,0xC6,0x66,0x3A,0,0,0,0,0,0},
    /* 72 'H' */ {0,0,0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0,0,0,0,0,0},
    /* 73 'I' */ {0,0,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0,0,0,0,0,0},
    /* 74 'J' */ {0,0,0x1E,0x0C,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0,0,0,0,0,0},
    /* 75 'K' */ {0,0,0xE6,0x66,0x6C,0x78,0x78,0x6C,0x66,0xE6,0,0,0,0,0,0},
    /* 76 'L' */ {0,0,0xF0,0x60,0x60,0x60,0x60,0x60,0x62,0xFE,0,0,0,0,0,0},
    /* 77 'M' */ {0,0,0xC6,0xEE,0xFE,0xD6,0xC6,0xC6,0xC6,0xC6,0,0,0,0,0,0},
    /* 78 'N' */ {0,0,0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0xC6,0,0,0,0,0,0},
    /* 79 'O' */ {0,0,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0,0,0,0,0,0},
    /* 80 'P' */ {0,0,0xFC,0x66,0x66,0x7C,0x60,0x60,0x60,0xF0,0,0,0,0,0,0},
    /* 81 'Q' */ {0,0,0x7C,0xC6,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x0E,0,0,0,0,0},
    /* 82 'R' */ {0,0,0xFC,0x66,0x66,0x7C,0x6C,0x66,0x66,0xE6,0,0,0,0,0,0},
    /* 83 'S' */ {0,0,0x7C,0xC6,0xC0,0x7C,0x06,0x06,0xC6,0x7C,0,0,0,0,0,0},
    /* 84 'T' */ {0,0,0x7E,0x5A,0x18,0x18,0x18,0x18,0x18,0x3C,0,0,0,0,0,0},
    /* 85 'U' */ {0,0,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0,0,0,0,0,0},
    /* 86 'V' */ {0,0,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0,0,0,0,0,0},
    /* 87 'W' */ {0,0,0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x82,0,0,0,0,0,0},
    /* 88 'X' */ {0,0,0xC6,0xC6,0x6C,0x38,0x38,0x6C,0xC6,0xC6,0,0,0,0,0,0},
    /* 89 'Y' */ {0,0,0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x3C,0,0,0,0,0,0},
    /* 90 'Z' */ {0,0,0xFE,0xC6,0x0C,0x18,0x30,0x60,0xC6,0xFE,0,0,0,0,0,0},
    /* 91 '[' */ {0,0,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x3C,0,0,0,0,0,0},
    /* 92 '\' */ {0,0,0x80,0xC0,0x60,0x30,0x18,0x0C,0x06,0,0,0,0,0,0,0},
    /* 93 ']' */ {0,0,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0,0,0,0,0,0},
    /* 94 '^' */ {0x10,0x38,0x6C,0xC6,0,0,0,0,0,0,0,0,0,0,0,0},
    /* 95 '_' */ {0,0,0,0,0,0,0,0,0,0,0xFF,0,0,0,0,0},
    /* 96 '`' */ {0x30,0x18,0x0C,0,0,0,0,0,0,0,0,0,0,0,0,0},
    /* 97 'a' */ {0,0,0,0,0x78,0x0C,0x7C,0xCC,0xCC,0x76,0,0,0,0,0,0},
    /* 98 'b' */ {0,0,0xE0,0x60,0x7C,0x66,0x66,0x66,0x66,0xDC,0,0,0,0,0,0},
    /* 99 'c' */ {0,0,0,0,0x7C,0xC6,0xC0,0xC0,0xC6,0x7C,0,0,0,0,0,0},
    /*100 'd' */ {0,0,0x1C,0x0C,0x7C,0xCC,0xCC,0xCC,0xCC,0x76,0,0,0,0,0,0},
    /*101 'e' */ {0,0,0,0,0x7C,0xC6,0xFE,0xC0,0xC6,0x7C,0,0,0,0,0,0},
    /*102 'f' */ {0,0,0x1C,0x36,0x30,0x78,0x30,0x30,0x30,0x78,0,0,0,0,0,0},
    /*103 'g' */ {0,0,0,0,0x76,0xCC,0xCC,0xCC,0x7C,0x0C,0xCC,0x78,0,0,0,0},
    /*104 'h' */ {0,0,0xE0,0x60,0x6C,0x76,0x66,0x66,0x66,0xE6,0,0,0,0,0,0},
    /*105 'i' */ {0,0,0x18,0,0x38,0x18,0x18,0x18,0x18,0x3C,0,0,0,0,0,0},
    /*106 'j' */ {0,0,0x06,0,0x0E,0x06,0x06,0x06,0x66,0x66,0x3C,0,0,0,0,0},
    /*107 'k' */ {0,0,0xE0,0x60,0x66,0x6C,0x78,0x6C,0x66,0xE6,0,0,0,0,0,0},
    /*108 'l' */ {0,0,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0,0,0,0,0,0},
    /*109 'm' */ {0,0,0,0,0xEC,0xFE,0xD6,0xD6,0xC6,0xC6,0,0,0,0,0,0},
    /*110 'n' */ {0,0,0,0,0xDC,0x66,0x66,0x66,0x66,0x66,0,0,0,0,0,0},
    /*111 'o' */ {0,0,0,0,0x7C,0xC6,0xC6,0xC6,0xC6,0x7C,0,0,0,0,0,0},
    /*112 'p' */ {0,0,0,0,0xDC,0x66,0x66,0x66,0x7C,0x60,0x60,0xF0,0,0,0,0},
    /*113 'q' */ {0,0,0,0,0x76,0xCC,0xCC,0xCC,0x7C,0x0C,0x0C,0x1E,0,0,0,0},
    /*114 'r' */ {0,0,0,0,0xDC,0x76,0x60,0x60,0x60,0xF0,0,0,0,0,0,0},
    /*115 's' */ {0,0,0,0,0x7C,0xC6,0x70,0x1C,0xC6,0x7C,0,0,0,0,0,0},
    /*116 't' */ {0,0,0x10,0x30,0xFC,0x30,0x30,0x30,0x36,0x1C,0,0,0,0,0,0},
    /*117 'u' */ {0,0,0,0,0xCC,0xCC,0xCC,0xCC,0xCC,0x76,0,0,0,0,0,0},
    /*118 'v' */ {0,0,0,0,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0,0,0,0,0,0},
    /*119 'w' */ {0,0,0,0,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0,0,0,0,0,0},
    /*120 'x' */ {0,0,0,0,0xC6,0x6C,0x38,0x38,0x6C,0xC6,0,0,0,0,0,0},
    /*121 'y' */ {0,0,0,0,0xC6,0xC6,0xC6,0x7E,0x06,0x0C,0xF8,0,0,0,0,0},
    /*122 'z' */ {0,0,0,0,0xFE,0x8C,0x18,0x30,0x62,0xFE,0,0,0,0,0,0},
    /*123 '{' */ {0,0,0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0,0,0,0,0,0,0},
    /*124 '|' */ {0,0,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0,0,0,0,0,0},
    /*125 '}' */ {0,0,0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0,0,0,0,0,0,0},
    /*126 '~' */ {0,0x76,0xDC,0,0,0,0,0,0,0,0,0,0,0,0,0},
};

/* ── Public API ──────────────────────────────────────────────── */

void fb_init_platform(uint64_t base, uint32_t width, uint32_t height,
                      uint32_t stride, uint32_t bpp)
{
    (void)bpp;
    fb_base   = (volatile uint32_t *)base;
    fb_width  = width;
    fb_height = height;
    fb_pitch  = stride;
    scale     = width >= 1024 ? 2 : 1;
    fb_set_text_top(0);
}

/* Confine the console to pixel rows [y, height): on SM8350 the rows above
 * hold the boot progress bars and panic cells painted by main.c. */
void fb_set_text_top(uint32_t y)
{
    text_y0  = y;
    text_col = 0;
    text_row = 0;
    max_cols = fb_width / (FONT_W * scale);
    max_rows = (fb_height - y) / (FONT_H * scale);
    viewing = 0;
    if (max_cols > TEXT_MAX_COLS)
        max_cols = TEXT_MAX_COLS;
    if (!max_cols) return;
    if (max_cols * max_rows > TEXT_MAX_CELLS)
        max_rows = TEXT_MAX_CELLS / max_cols;
    for (uint32_t i = 0; i < max_cols * max_rows; i++)
        cells[i] = 0;
}

/* Clears the text area only */
void fb_clear(void)
{
    if (!fb_base) return;
    volatile uint32_t *p = fb_base + text_y0 * fb_pitch;
    for (uint32_t i = 0; i < (fb_height - text_y0) * fb_pitch; i++)
        p[i] = BG_COLOR;
    for (uint32_t i = 0; i < max_cols * max_rows; i++)
        cells[i] = CELL_BLANK;
    text_col = 0;
    text_row = 0;
    viewing = 0;
}

/* Anything outside the font draws as a space */
static void fb_draw_char(uint32_t col, uint32_t row, char c,
                         uint32_t fg, uint32_t bg)
{
    if (!fb_base) return;
    if (c < 32 || c > 126) c = ' ';
    uint32_t px = col * FONT_W * scale;
    uint32_t py = text_y0 + row * FONT_H * scale;
    const uint8_t *glyph = font8x16[c - 32];
    if (scale == 2 && !(fb_pitch & 1)) {
        /* The phone panel. Each font bit is two equal pixels, one 64-bit
         * store, and each font row is two pixel rows. A scroll redraws
         * ~1800 glyphs: 8 ms with the generic loop below (a divide and a
         * 32-bit store per pixel), ~4.5 ms with this. */
        uint64_t fg2 = fg | (uint64_t)fg << 32;
        uint64_t bg2 = bg | (uint64_t)bg << 32;
        uint64_t *line = (uint64_t *)(fb_base + py * fb_pitch + px);
        for (uint32_t y = 0; y < FONT_H; y++) {
            uint64_t pair[FONT_W];
            for (uint32_t x = 0; x < FONT_W; x++)
                pair[x] = (glyph[y] & (0x80 >> x)) ? fg2 : bg2;
            for (uint32_t r = 0; r < 2; r++, line += fb_pitch / 2)
                for (uint32_t x = 0; x < FONT_W; x++)
                    line[x] = pair[x];
        }
        return;
    }
    for (uint32_t y = 0; y < FONT_H * scale; y++) {
        uint8_t bits = glyph[y / scale];
        volatile uint32_t *line = fb_base + (py + y) * fb_pitch + px;
        for (uint32_t x = 0; x < FONT_W * scale; x++)
            line[x] = (bits & (0x80 >> (x / scale))) ? fg : bg;
    }
}

/* Set a cell, drawing it only if it changes */
static void fb_set_cell(uint32_t row, uint32_t col, uint32_t v)
{
    uint32_t *cell = &cells[row * max_cols + col];
    if (*cell == v) return;
    *cell = v;
    fb_draw_char(col, row, (char)(v & 0xFF), v >> 8, BG_COLOR);
}

static void fb_blank_row(uint32_t row)
{
    for (uint32_t col = 0; col < max_cols; col++)
        fb_set_cell(row, col, CELL_BLANK);
}

/* Scroll up by `lines`. Each cell takes the one `lines` rows below it and is
 * redrawn only if that differs, so blank runs cost nothing. */
static void fb_scroll(uint32_t lines)
{
    uint32_t shift = lines * max_cols;
    uint32_t n = (max_rows - lines) * max_cols;
    uint32_t row = 0, col = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = cells[i + shift];
        if (cells[i] != v) {
            cells[i] = v;
            fb_draw_char(col, row, (char)(v & 0xFF), v >> 8, BG_COLOR);
        }
        if (++col == max_cols) {
            col = 0;
            row++;
        }
    }
    for (row = max_rows - lines; row < max_rows; row++)
        fb_blank_row(row);
}

/* Jump scroll: at the bottom, move the text up a quarter of the area at
 * once. A scroll costs about a redraw of the visible text whatever the
 * distance (~4.5 ms on the phone, bound by writes to the uncached
 * framebuffer); line by line that made `dmesg` take ~0.25 s. */
static void fb_newline(void)
{
    text_col = 0;
    if (text_row + 1 < max_rows) {
        /* Rows below the cursor still hold whatever ABL left there */
        fb_blank_row(++text_row);
        return;
    }
    uint32_t jump = max_rows / 4 ? max_rows / 4 : 1;
    fb_scroll(jump);
    text_row = max_rows - jump;
}

static void fb_put(char c, uint32_t color)
{
    if (!fb_base || !max_cols) return;
    if (viewing) {
        /* Showing history: the text only reaches the log (serial.c), and
         * going back to the live console redraws from there */
        view_missed = 1;
        return;
    }
    if (c == '\n') {
        fb_newline();
    } else if (c == '\r') {
        text_col = 0;
    } else if (c == '\b') {
        if (text_col > 0) text_col--;
    } else {
        uint32_t v = (c > 32 && c <= 126)
            ? ((color & 0xFFFFFF) << 8) | (uint8_t)c : CELL_BLANK;
        fb_set_cell(text_row, text_col, v);
        if (++text_col >= max_cols)
            fb_newline();
    }
}

void fb_putc(char c)
{
    fb_put(c, FG_COLOR);
}

void fb_puts(const char *s)
{
    while (*s) fb_putc(*s++);
}

void fb_puts_color(const char *s, uint32_t color)
{
    while (*s) fb_put(*s++, color);
}

void fb_puthex(uint64_t val, int digits)
{
    static const char hex[] = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; i--)
        fb_putc(hex[(val >> (i * 4)) & 0xF]);
}

void fb_putchar(char c)
{
    fb_putc(c);
}

/* ── History: the console log on screen ─────────────────────────
 *
 * The text area keeps only what fits; serial.c's log (`dmesg`'s ring) has
 * the rest. fb_history lays the log out the way fb_put lays out live text
 * (wrapping at max_cols, '\r', '\b'), in the default color, and shows it
 * any number of lines back, with the last row saying where. The view is
 * anchored to its top line, so output that arrives meanwhile doesn't move
 * it; scrolling down to the end, or going live, redraws the log's tail with
 * the cursor after it, and that output shows up then. */
#define HIST_LINES      4096                /* line starts kept: the last ones */
#define HIST_COLOR      0x00FFFF00
static uint32_t line_start[HIST_LINES];     /* log offsets, a ring */

/* Breaks the log into lines; returns how many (the last is the cursor's) */
static uint32_t hist_layout(const char *ring, uint32_t size, uint32_t total)
{
    uint32_t i = total > size ? total - size : 0;
    uint32_t n = 0, col = 0;
    line_start[n++ % HIST_LINES] = i;
    for (; i < total; i++) {
        char c = ring[i % size];
        if (c == '\n') {
            col = 0;
            line_start[n++ % HIST_LINES] = i + 1;
        } else if (c == '\r') {
            col = 0;
        } else if (c == '\b') {
            if (col > 0) col--;
        } else if (++col >= max_cols) {
            col = 0;
            line_start[n++ % HIST_LINES] = i + 1;
        }
    }
    return n;
}

/* Draws log line `line` (of n) in screen row `row`; returns the column after
 * its text, where the cursor goes if it's the last line */
static uint32_t hist_row(const char *ring, uint32_t size, uint32_t total,
                         uint32_t line, uint32_t n, uint32_t row)
{
    static uint32_t text[TEXT_MAX_COLS];
    uint32_t to = line + 1 < n ? line_start[(line + 1) % HIST_LINES] : total;
    uint32_t col = 0;
    for (uint32_t c = 0; c < max_cols; c++)
        text[c] = CELL_BLANK;
    for (uint32_t i = line_start[line % HIST_LINES]; i < to; i++) {
        char c = ring[i % size];
        if (c == '\n')
            break;
        if (c == '\r') {
            col = 0;
        } else if (c == '\b') {
            if (col > 0) col--;
        } else if (col < max_cols) {
            text[col++] = (c > 32 && c <= 126)
                ? ((FG_COLOR & 0xFFFFFF) << 8) | (uint8_t)c : CELL_BLANK;
        }
    }
    for (uint32_t c = 0; c < max_cols; c++)
        fb_set_cell(row, c, text[c]);
    return col;
}

static void hist_status(const char *s, uint32_t *col)
{
    for (; *s && *col < max_cols; s++, (*col)++)
        fb_set_cell(max_rows - 1, *col,
                    *s == ' ' ? CELL_BLANK : (HIST_COLOR << 8) | (uint8_t)*s);
}

static void hist_status_dec(uint32_t v, uint32_t *col)
{
    char buf[11];
    int i = sizeof(buf) - 1;
    buf[i] = 0;
    do {
        buf[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v && i > 0);
    hist_status(&buf[i], col);
}

/* The live console: the log's tail, the cursor after it */
static void hist_live(const char *ring, uint32_t size, uint32_t total, uint32_t n)
{
    uint32_t first = n > max_rows ? n - max_rows : 0;
    uint32_t row = 0, col = 0;
    for (uint32_t l = first; l < n; l++, row++)
        col = hist_row(ring, size, total, l, n, row);
    text_row = row - 1;
    text_col = col;
    for (; row < max_rows; row++)
        fb_blank_row(row);
    viewing = view_missed = 0;
}

void fb_redraw_log(const char *ring, uint32_t size, uint32_t total)
{
    if (!fb_base || !max_rows)
        return;
    hist_live(ring, size, total, hist_layout(ring, size, total));
}

int fb_history(const char *ring, uint32_t size, uint32_t total, int lines)
{
    if (!fb_base || max_rows < 2 || (!viewing && lines <= 0))
        return 0;
    uint32_t n = hist_layout(ring, size, total);
    uint32_t rows = max_rows - 1;               /* the last row is the status */
    uint32_t oldest = n > HIST_LINES ? n - HIST_LINES : 0;
    uint32_t end_top = n > rows ? n - rows : 0; /* the top line at the end */

    /* Where the view's top line is now (live: the end) */
    uint32_t top = end_top;
    if (viewing) {
        top = oldest;
        for (uint32_t l = oldest; l < n && line_start[l % HIST_LINES] <= view_top; l++)
            top = l;
    }
    int64_t t = (int64_t)top - lines;
    if (t < (int64_t)oldest)
        t = oldest;

    if (t >= (int64_t)end_top) {
        hist_live(ring, size, total, n);
        return 0;
    }

    viewing = 1;
    view_top = line_start[t % HIST_LINES];
    for (uint32_t r = 0; r < rows; r++)
        hist_row(ring, size, total, (uint32_t)t + r, n, r);
    uint32_t col = 0;
    hist_status("-- history: ", &col);
    hist_status_dec(end_top - (uint32_t)t, &col);
    hist_status(" of ", &col);
    hist_status_dec(end_top - oldest, &col);
    hist_status(" lines back", &col);
    hist_status(view_missed ? ", new output below --" : " --", &col);
    while (col < max_cols)
        fb_set_cell(max_rows - 1, col++, CELL_BLANK);
    return (int)(end_top - (uint32_t)t);
}

/* Solid rectangle, for status markers outside the text area. Any CPU,
 * no lock: markers don't overlap. No-op without a framebuffer. */
void fb_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    if (!fb_base) return;
    for (uint32_t r = y; r < y + h && r < fb_height; r++)
        for (uint32_t c = x; c < x + w && c < fb_width; c++)
            fb_base[r * fb_pitch + c] = color;
}

void fb_putdec(uint64_t val)
{
    char buf[21];
    int i = 0;
    if (val == 0) {
        fb_putc('0');
        return;
    }
    while (val > 0) {
        buf[i++] = '0' + (val % 10);
        val /= 10;
    }
    while (i > 0) fb_putc(buf[--i]);
}
