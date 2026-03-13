/*
 * OsitoK x86-64 — GOP Framebuffer Text Console
 *
 * Simple 32bpp text rendering using embedded 8×16 bitmap font.
 * Initialized from UEFI GOP before ExitBootServices.
 */

#include "../include/types.h"

/* ── Framebuffer state ───────────────────────────────────────── */

static uint32_t *fb_base;    /* active drawing target (shadow or vram) */
static uint32_t *fb_vram;    /* physical VRAM (MMIO, slow uncached access) */
static uint32_t *fb_shadow;  /* RAM shadow buffer (fast cached access) */
static uint32_t  fb_width;
static uint32_t  fb_height;
static uint32_t  fb_pitch;    /* pixels per scanline */

static uint32_t  text_col;
static uint32_t  text_row;
static uint32_t  max_cols;
static uint32_t  max_rows;

/* Dirty region tracking (pixel rows) */
static uint32_t  dirty_top;
static uint32_t  dirty_bot;

#define FONT_W  8
#define FONT_H  16

#define FG_COLOR 0x00CCCCCC   /* Light gray */
#define BG_COLOR 0x00000000   /* Black */
#define HL_COLOR 0x0000FF00   /* Green for highlights */

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

void fb_init(uint32_t *base, uint32_t width, uint32_t height, uint32_t pitch)
{
    fb_base   = base;
    fb_vram   = base;
    fb_shadow = NULL;
    fb_width  = width;
    fb_height = height;
    fb_pitch  = pitch;
    text_col  = 0;
    text_row  = 0;
    max_cols  = width / FONT_W;
    max_rows  = height / FONT_H;
    dirty_top = 0;
    dirty_bot = 0;
}

/* Enable shadow framebuffer (call after memory allocator is ready) */
void fb_enable_shadow(void *buf)
{
    fb_shadow = (uint32_t *)buf;
    /* Copy current VRAM to shadow (slow UC read, but one-time) */
    uint32_t total = fb_height * fb_pitch;
    for (uint32_t i = 0; i < total; i++)
        fb_shadow[i] = fb_vram[i];
    fb_base = fb_shadow;
    dirty_top = 0;
    dirty_bot = 0;
}

/* Flush dirty region from shadow to VRAM */
void fb_flush(void)
{
    if (!fb_shadow || dirty_top >= dirty_bot) return;
    /* Clamp to framebuffer bounds */
    if (dirty_bot > fb_height) dirty_bot = fb_height;
    /* Copy dirty rows: use 64-bit writes for efficiency on WC memory */
    uint64_t *dst = (uint64_t *)(fb_vram + dirty_top * fb_pitch);
    uint64_t *src = (uint64_t *)(fb_shadow + dirty_top * fb_pitch);
    uint32_t qwords = (dirty_bot - dirty_top) * fb_pitch / 2;
    for (uint32_t i = 0; i < qwords; i++)
        dst[i] = src[i];
    dirty_top = fb_height;
    dirty_bot = 0;
}

void fb_flush_all(void)
{
    if (!fb_shadow) return;
    uint64_t *dst = (uint64_t *)fb_vram;
    uint64_t *src = (uint64_t *)fb_shadow;
    uint32_t qwords = fb_height * fb_pitch / 2;
    for (uint32_t i = 0; i < qwords; i++)
        dst[i] = src[i];
}

static void fb_mark_dirty(uint32_t pixel_top, uint32_t pixel_bot)
{
    if (pixel_top < dirty_top) dirty_top = pixel_top;
    if (pixel_bot > dirty_bot) dirty_bot = pixel_bot;
}

void fb_clear(void)
{
    for (uint32_t y = 0; y < fb_height; y++)
        for (uint32_t x = 0; x < fb_width; x++)
            fb_base[y * fb_pitch + x] = BG_COLOR;
    text_col = 0;
    text_row = 0;
}

static void fb_putchar_at(uint32_t col, uint32_t row, char c, uint32_t fg)
{
    if (c < 32 || c > 126) return;
    const uint8_t *glyph = font8x16[c - 32];
    uint32_t px = col * FONT_W;
    uint32_t py = row * FONT_H;

    for (uint32_t y = 0; y < FONT_H; y++) {
        uint8_t bits = glyph[y];
        for (uint32_t x = 0; x < FONT_W; x++) {
            uint32_t color = (bits & (0x80 >> x)) ? fg : BG_COLOR;
            uint32_t sx = px + x;
            uint32_t sy = py + y;
            if (sx < fb_width && sy < fb_height)
                fb_base[sy * fb_pitch + sx] = color;
        }
    }
    fb_mark_dirty(py, py + FONT_H);
}

static void fb_scroll(void)
{
    /* Scroll up by one row */
    uint32_t row_pixels = FONT_H * fb_pitch;
    uint32_t total_pixels = fb_height * fb_pitch;

    /* Move rows up (works on shadow if available, else VRAM) */
    uint64_t *dst = (uint64_t *)fb_base;
    uint64_t *src = (uint64_t *)(fb_base + row_pixels);
    uint32_t qwords = (total_pixels - row_pixels) / 2;
    for (uint32_t i = 0; i < qwords; i++)
        dst[i] = src[i];

    /* Clear last row */
    uint64_t *last = (uint64_t *)(fb_base + (total_pixels - row_pixels));
    uint32_t clear_qwords = row_pixels / 2;
    for (uint32_t i = 0; i < clear_qwords; i++)
        last[i] = 0;

    /* Mark entire screen dirty */
    fb_mark_dirty(0, fb_height);
    fb_flush();
}

/* ── ANSI CSI escape sequence parser ────────────────────────── */

#define ANSI_STATE_NORMAL  0
#define ANSI_STATE_ESC     1   /* received ESC */
#define ANSI_STATE_CSI     2   /* received ESC [ */
#define ANSI_STATE_QMARK   3   /* received ESC [ ? */

static int    ansi_state;
static int    ansi_params[8];
static int    ansi_nparams;
static int    ansi_cur_param;
static uint32_t ansi_fg_color = FG_COLOR;

/* Basic ANSI color palette (SGR 30-37) */
static const uint32_t ansi_colors[8] = {
    0x00000000, /* 0: black */
    0x00CC0000, /* 1: red */
    0x0000CC00, /* 2: green */
    0x00CCCC00, /* 3: yellow */
    0x000000CC, /* 4: blue */
    0x00CC00CC, /* 5: magenta */
    0x0000CCCC, /* 6: cyan */
    0x00CCCCCC, /* 7: white/gray */
};

/* Bright ANSI colors (SGR 90-97) */
static const uint32_t ansi_bright[8] = {
    0x00666666, /* 0: bright black (dark gray) */
    0x00FF4444, /* 1: bright red */
    0x0044FF44, /* 2: bright green */
    0x00FFFF44, /* 3: bright yellow */
    0x004444FF, /* 4: bright blue */
    0x00FF44FF, /* 5: bright magenta */
    0x0044FFFF, /* 6: bright cyan */
    0x00FFFFFF, /* 7: bright white */
};

static void fb_clear_line_from(uint32_t row, uint32_t col)
{
    /* Clear from (col, row) to end of line */
    for (uint32_t c = col; c < max_cols; c++)
        fb_putchar_at(c, row, ' ', BG_COLOR);
}

static void fb_clear_line(uint32_t row)
{
    fb_clear_line_from(row, 0);
}

static void ansi_execute(char cmd)
{
    int p0 = (ansi_nparams > 0) ? ansi_params[0] : 0;
    int p1 = (ansi_nparams > 1) ? ansi_params[1] : 0;

    switch (cmd) {
    case 'A': /* Cursor Up */
        if (p0 == 0) p0 = 1;
        text_row = (text_row >= (uint32_t)p0) ? text_row - p0 : 0;
        break;
    case 'B': /* Cursor Down */
        if (p0 == 0) p0 = 1;
        text_row += p0;
        if (text_row >= max_rows) text_row = max_rows - 1;
        break;
    case 'C': /* Cursor Forward */
        if (p0 == 0) p0 = 1;
        text_col += p0;
        if (text_col >= max_cols) text_col = max_cols - 1;
        break;
    case 'D': /* Cursor Back */
        if (p0 == 0) p0 = 1;
        text_col = (text_col >= (uint32_t)p0) ? text_col - p0 : 0;
        break;
    case 'H': /* Cursor Position (row;col, 1-based) */
    case 'f':
        text_row = (p0 > 0) ? (uint32_t)(p0 - 1) : 0;
        text_col = (p1 > 0) ? (uint32_t)(p1 - 1) : 0;
        if (text_row >= max_rows) text_row = max_rows - 1;
        if (text_col >= max_cols) text_col = max_cols - 1;
        break;
    case 'J': /* Erase in Display */
        if (p0 == 0) {
            /* Clear from cursor to end of screen */
            fb_clear_line_from(text_row, text_col);
            for (uint32_t r = text_row + 1; r < max_rows; r++)
                fb_clear_line(r);
        } else if (p0 == 2 || p0 == 3) {
            /* Clear entire screen */
            fb_clear();
        }
        break;
    case 'K': /* Erase in Line */
        if (p0 == 0) {
            fb_clear_line_from(text_row, text_col);
        } else if (p0 == 2) {
            fb_clear_line(text_row);
        }
        break;
    case 'm': /* SGR — Select Graphic Rendition */
        if (ansi_nparams == 0) {
            ansi_fg_color = FG_COLOR; /* Reset */
        }
        for (int i = 0; i < ansi_nparams; i++) {
            int p = ansi_params[i];
            if (p == 0) ansi_fg_color = FG_COLOR;
            else if (p == 1) { /* Bold — use bright version if available */ }
            else if (p == 7) { /* Reverse — swap fg/bg (simplified) */ }
            else if (p >= 30 && p <= 37) ansi_fg_color = ansi_colors[p - 30];
            else if (p == 39) ansi_fg_color = FG_COLOR; /* Default fg */
            else if (p >= 90 && p <= 97) ansi_fg_color = ansi_bright[p - 90];
        }
        break;
    case 'n': /* Device Status Report */
        /* 6n = cursor position report: we can't send back to the app
         * from framebuffer. The syscall layer handles this via keyboard
         * injection. Just ignore here. */
        break;
    case 'l': /* Reset Mode (used for ?25l = hide cursor) */
    case 'h': /* Set Mode (used for ?25h = show cursor) */
        /* Cursor visibility — we don't draw a cursor, so ignore */
        break;
    }
}

void fb_putc(char c, uint32_t color)
{
    if (!fb_base) return;

    /* ANSI state machine */
    switch (ansi_state) {
    case ANSI_STATE_ESC:
        if (c == '[') {
            ansi_state = ANSI_STATE_CSI;
            ansi_nparams = 0;
            ansi_cur_param = 0;
            for (int i = 0; i < 8; i++) ansi_params[i] = 0;
            return;
        }
        ansi_state = ANSI_STATE_NORMAL;
        /* Fall through to render the character */
        break;
    case ANSI_STATE_CSI:
    case ANSI_STATE_QMARK:
        if (c == '?') {
            ansi_state = ANSI_STATE_QMARK;
            return;
        }
        if (c >= '0' && c <= '9') {
            ansi_cur_param = ansi_cur_param * 10 + (c - '0');
            return;
        }
        if (c == ';') {
            if (ansi_nparams < 8)
                ansi_params[ansi_nparams++] = ansi_cur_param;
            ansi_cur_param = 0;
            return;
        }
        /* Command character — finalize params and execute */
        if (ansi_nparams < 8)
            ansi_params[ansi_nparams++] = ansi_cur_param;
        ansi_execute(c);
        ansi_state = ANSI_STATE_NORMAL;
        return;
    }

    /* Normal character processing */
    if (c == 27) {  /* ESC */
        ansi_state = ANSI_STATE_ESC;
        return;
    }

    /* Use ANSI fg color if no explicit color override */
    uint32_t fg = (color == FG_COLOR) ? ansi_fg_color : color;

    if (c == '\n') {
        text_col = 0;
        text_row++;
    } else if (c == '\r') {
        text_col = 0;
    } else if (c == '\b') {
        if (text_col > 0) text_col--;
    } else if (c == '\t') {
        text_col = (text_col + 4) & ~3;
    } else {
        fb_putchar_at(text_col, text_row, c, fg);
        text_col++;
    }

    if (text_col >= max_cols) {
        text_col = 0;
        text_row++;
    }
    if (text_row >= max_rows) {
        fb_scroll();
        text_row = max_rows - 1;
    }
}

/* ── Query framebuffer dimensions ──────────────────────────── */

uint32_t fb_get_cols(void) { return max_cols; }
uint32_t fb_get_rows(void) { return max_rows; }
uint32_t *fb_get_base(void)   { return fb_base; }
uint32_t  fb_get_width(void)  { return fb_width; }
uint32_t  fb_get_height(void) { return fb_height; }
uint32_t  fb_get_pitch(void)  { return fb_pitch; }

void fb_puts(const char *s)
{
    while (*s) fb_putc(*s++, FG_COLOR);
    fb_flush();
}

void fb_puts_color(const char *s, uint32_t color)
{
    while (*s) fb_putc(*s++, color);
    fb_flush();
}

void fb_puthex(uint64_t val, int digits)
{
    static const char hex[] = "0123456789ABCDEF";
    fb_puts("0x");
    for (int i = (digits - 1) * 4; i >= 0; i -= 4)
        fb_putc(hex[(val >> i) & 0xF], FG_COLOR);
}

void fb_putdec(uint64_t val)
{
    char buf[20];
    int i = 0;
    if (val == 0) { fb_putc('0', FG_COLOR); return; }
    while (val > 0) {
        buf[i++] = '0' + (val % 10);
        val /= 10;
    }
    while (--i >= 0) fb_putc(buf[i], FG_COLOR);
}

void fb_putchar(char c)
{
    fb_putc(c, FG_COLOR);
    fb_flush();
}
