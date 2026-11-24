/*
 * OsitoK - 4x6 bitmap font
 *
 * 95 printable ASCII glyphs (32-126), 4 pixels wide, 6 rows tall.
 * Each byte stores 4 pixels in bits 7:4 (MSB = leftmost pixel).
 * 6 bytes per glyph, 570 bytes total.
 *
 * Grid: 32 columns x 10 rows = 320 characters on 128x64 screen.
 */
#ifndef OSITO_FONT_H
#define OSITO_FONT_H

#include "osito.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FONT_W      4
#define FONT_H      6
#define TEXT_COLS    (FB_WIDTH / FONT_W)    /* 32 */
#define TEXT_ROWS    (FB_HEIGHT / FONT_H)   /* 10 */

#define FONT_FIRST  32   /* space */
#define FONT_LAST   126  /* tilde */
#define FONT_GLYPHS (FONT_LAST - FONT_FIRST + 1)  /* 95 */

/* 95 glyphs x 6 bytes = 570 bytes, stored in DRAM (const) */
extern const uint8_t font_4x6[FONT_GLYPHS][FONT_H];

#ifdef __cplusplus
}
#endif

#endif /* OSITO_FONT_H */
