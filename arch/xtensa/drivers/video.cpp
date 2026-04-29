/*
 * OsitoK - Framebuffer video driver
 *
 * 128x64 monochrome framebuffer (1KB RAM).
 * Bresenham line drawing for wireframe graphics (Elite, etc).
 * Output via UART "video bridge" — sync header + raw pixels.
 */

#include "drivers/video.h"
#include "drivers/font.h"
#include "drivers/uart.h"

extern "C" {

/* ====== Framebuffer ====== */

static uint8_t fb[FB_SIZE];  /* 128x64 pixels, 1 bit per pixel, row-major */

/* ====== Init ====== */

void video_init(void)
{
    fb_clear();
    uart_puts("video: framebuffer ");
    uart_put_dec(FB_WIDTH);
    uart_putc('x');
    uart_put_dec(FB_HEIGHT);
    uart_puts(" (");
    uart_put_dec(FB_SIZE);
    uart_puts(" bytes)\n");
}

/* ====== Pixel operations ====== */

void fb_clear(void)
{
    for (int i = 0; i < FB_SIZE; i++)
        fb[i] = 0;
}

void fb_set_pixel(int x, int y)
{
    if ((unsigned)x >= FB_WIDTH || (unsigned)y >= FB_HEIGHT)
        return;
    fb[y * FB_STRIDE + (x >> 3)] |= (0x80 >> (x & 7));
}

void fb_clear_pixel(int x, int y)
{
    if ((unsigned)x >= FB_WIDTH || (unsigned)y >= FB_HEIGHT)
        return;
    fb[y * FB_STRIDE + (x >> 3)] &= ~(0x80 >> (x & 7));
}

/* ====== Bresenham line drawing ====== */

void fb_line(int x0, int y0, int x1, int y1)
{
    int dx = x1 - x0;
    int dy = y1 - y0;
    int sx = 1;
    int sy = 1;

    if (dx < 0) { dx = -dx; sx = -1; }
    if (dy < 0) { dy = -dy; sy = -1; }

    int err;

    if (dx >= dy) {
        /* Shallow line (more horizontal) */
        err = dx >> 1;
        for (int i = 0; i <= dx; i++) {
            fb_set_pixel(x0, y0);
            err -= dy;
            if (err < 0) {
                y0 += sy;
                err += dx;
            }
            x0 += sx;
        }
    } else {
        /* Steep line (more vertical) */
        err = dy >> 1;
        for (int i = 0; i <= dy; i++) {
            fb_set_pixel(x0, y0);
            err -= dx;
            if (err < 0) {
                x0 += sx;
                err += dy;
            }
            y0 += sy;
        }
    }
}

/* ====== Text rendering ====== */

void fb_putchar(int x, int y, char c)
{
    if (c < FONT_FIRST || c > FONT_LAST)
        c = '?';
    const uint8_t *glyph = font_4x6[c - FONT_FIRST];

    for (int row = 0; row < FONT_H; row++) {
        uint8_t bits = glyph[row];  /* pixels in bits 7:4 */
        for (int col = 0; col < FONT_W; col++) {
            if (bits & (0x80 >> col))
                fb_set_pixel(x + col, y + row);
        }
    }
}

void fb_puts_at(int x, int y, const char *str)
{
    while (*str) {
        fb_putchar(x, y, *str++);
        x += FONT_W;
    }
}

void fb_text_putc(int col, int row, char c)
{
    fb_putchar(col * FONT_W, row * FONT_H, c);
}

void fb_text_puts(int col, int row, const char *str)
{
    while (*str) {
        if (col >= TEXT_COLS) {
            col = 0;
            row++;
            if (row >= TEXT_ROWS)
                return;
        }
        fb_putchar(col * FONT_W, row * FONT_H, *str++);
        col++;
    }
}

/* ====== UART video bridge flush ====== */

void fb_flush(void)
{
    static const uint8_t sync[] = {
        VIDEO_SYNC_0, VIDEO_SYNC_1,
        VIDEO_SYNC_2, VIDEO_SYNC_3
    };

    uart_write_raw(sync, 4);
    uart_write_raw(fb, FB_SIZE);
}

} /* extern "C" */
