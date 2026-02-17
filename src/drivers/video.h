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

#ifdef __cplusplus
}
#endif

#endif /* OSITO_VIDEO_H */
