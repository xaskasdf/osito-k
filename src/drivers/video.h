/*
 * OsitoK - Framebuffer video driver
 *
 * 128x64 monochrome framebuffer (1KB) with Bresenham line drawing.
 * Output via UART "video bridge" protocol for PC-side rendering.
 */
#ifndef OSITO_VIDEO_H
#define OSITO_VIDEO_H

#include "osito.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Framebuffer dimensions */
#define FB_WIDTH   128
#define FB_HEIGHT  64
#define FB_STRIDE  (FB_WIDTH / 8)   /* 16 bytes per row */
#define FB_SIZE    (FB_STRIDE * FB_HEIGHT)  /* 1024 bytes */

/* Sync header for UART video bridge protocol */
#define VIDEO_SYNC_0  0x00
#define VIDEO_SYNC_1  0xFF
#define VIDEO_SYNC_2  0x00
#define VIDEO_SYNC_3  0xFF

/* Initialize video subsystem (clears framebuffer) */
void video_init(void);

/* Clear entire framebuffer to black */
void fb_clear(void);

/* Set a single pixel (x: 0-127, y: 0-63) */
void fb_set_pixel(int x, int y);

/* Clear a single pixel */
void fb_clear_pixel(int x, int y);

/* Draw a line using Bresenham's algorithm */
void fb_line(int x0, int y0, int x1, int y1);

/* Flush framebuffer to UART (sync header + 1024 bytes) */
void fb_flush(void);

/* ====== Text rendering (requires font.h) ====== */

/* Draw a single character at pixel coordinates (OR-compositing) */
void fb_putchar(int x, int y, char c);

/* Draw a string at pixel coordinates (no wrapping) */
void fb_puts_at(int x, int y, const char *str);

/* Draw a character at grid coordinates (col 0-31, row 0-9) */
void fb_text_putc(int col, int row, char c);

/* Draw a string at grid coordinates (wraps at screen edge) */
void fb_text_puts(int col, int row, const char *str);

#ifdef __cplusplus
}
#endif

#endif /* OSITO_VIDEO_H */
