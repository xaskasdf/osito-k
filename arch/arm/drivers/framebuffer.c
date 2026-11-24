/*
 * framebuffer.c -- Splash framebuffer console for SM8350
 *
 * Tested on ASUS ROG Phone 5, 2026-03-03/04.
 * ABL leaves the display pipeline active with splash screen at 0xE5000000.
 * We write pixels directly — no MDSS initialization needed.
 *
 * Format: ARGB8888 (confirmed with R/G/B/W color bar test)
 * Resolution: 1080 x 2448 (Samsung AMS678 ER2 OLED, DSI command mode)
 * Stride: 1080 pixels, linear (no padding)
 *
 * CRITICAL: In dual-output (FB + UART), always call fb_puts() BEFORE
 * uart_puts(). UART TX can hang and block framebuffer output.
 */

#include "sm8350.h"

/* 8x16 VGA bitmap font (include the font data header) */
/* extern const uint8_t font8x16[]; */

#define FB_BASE     ((volatile uint32_t *)SPLASH_FB_BASE)
#define FB_W        DISPLAY_WIDTH
#define FB_H        DISPLAY_HEIGHT
#define CHAR_W      8
#define CHAR_H      16
#define COLS        (FB_W / CHAR_W)     /* 135 */
#define ROWS        (FB_H / CHAR_H)     /* 153 */

#define COLOR_BG    0xFF000000  /* Black */
#define COLOR_FG    0xFF00FF00  /* Green */

static int cur_col, cur_row;

static inline void fb_pixel(int x, int y, uint32_t color) {
    if (x >= 0 && x < FB_W && y >= 0 && y < FB_H)
        FB_BASE[y * FB_W + x] = color;
}

void fb_fill_rect(int x, int y, int w, int h, uint32_t color) {
    for (int row = y; row < y + h && row < FB_H; row++)
        for (int col = x; col < x + w && col < FB_W; col++)
            FB_BASE[row * FB_W + col] = color;
}

void fb_init(void) {
    /* Clear screen to background color */
    volatile uint32_t *p = FB_BASE;
    for (int i = 0; i < FB_W * FB_H; i++)
        *p++ = COLOR_BG;
    cur_col = 0;
    cur_row = 0;
}

/*
 * Scroll screen up by one character row (16 pixels).
 * Copies pixel rows upward and clears the bottom row.
 */
static void fb_scroll(void) {
    volatile uint32_t *dst = FB_BASE;
    volatile uint32_t *src = FB_BASE + CHAR_H * FB_W;
    int copy_pixels = FB_W * (FB_H - CHAR_H);

    for (int i = 0; i < copy_pixels; i++)
        dst[i] = src[i];

    /* Clear bottom row */
    volatile uint32_t *bottom = FB_BASE + (FB_H - CHAR_H) * FB_W;
    for (int i = 0; i < FB_W * CHAR_H; i++)
        bottom[i] = COLOR_BG;
}

/*
 * Draw a character at (col, row) using the 8x16 font.
 * Font data must be provided (font8x16.h from sm8350-boot or VGA ROM).
 */
/* void fb_draw_char(int col, int row, char c, uint32_t fg, uint32_t bg) {
    int px = col * CHAR_W;
    int py = row * CHAR_H;
    const uint8_t *glyph = &font8x16[(unsigned char)c * CHAR_H];

    for (int y = 0; y < CHAR_H; y++) {
        uint8_t bits = glyph[y];
        for (int x = 0; x < CHAR_W; x++) {
            fb_pixel(px + x, py + y, (bits & (0x80 >> x)) ? fg : bg);
        }
    }
} */

void fb_putc(char c) {
    if (c == '\n') {
        cur_col = 0;
        cur_row++;
    } else if (c == '\r') {
        cur_col = 0;
    } else if (c == '\b') {
        if (cur_col > 0) cur_col--;
    } else {
        /* fb_draw_char(cur_col, cur_row, c, COLOR_FG, COLOR_BG); */
        cur_col++;
        if (cur_col >= COLS) {
            cur_col = 0;
            cur_row++;
        }
    }

    if (cur_row >= ROWS) {
        fb_scroll();
        cur_row = ROWS - 1;
    }
}

void fb_puts(const char *s) {
    while (*s)
        fb_putc(*s++);
}

void fb_puts_color(const char *s, uint32_t color) {
    /* Temporarily change foreground for colored output.
     * In real implementation, pass color to fb_draw_char. */
    (void)color;
    fb_puts(s);
}
