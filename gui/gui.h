/*
 * OsitoK GUI — Shared graphical desktop library
 *
 * Architecture-independent rendering to pixel buffers.
 * Inspired by elementaryOS Pantheon dark mode.
 * Used by both x86-64 and AArch64 kernels.
 */

#ifndef OSITO_GUI_H
#define OSITO_GUI_H

#ifdef __KERNEL_X86__
#include "types.h"
#elif defined(__KERNEL_ARM__)
#include "types.h"
#else
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#endif

/* ── Surface: a drawable pixel buffer ──────────────────────── */

typedef struct {
    uint32_t *pixels;
    uint32_t  width;
    uint32_t  height;
    uint32_t  pitch;    /* pixels per scanline */
} gui_surface_t;

/* ── elementaryOS Pantheon dark palette ────────────────────── */

#define GUI_BG_DARK         0xFF2D2D2D  /* Desktop gradient top */
#define GUI_BG_LIGHT        0xFF333333  /* Desktop gradient bottom */
#define GUI_PANEL_BG        0xE6262626  /* Panel (semi-transparent) */
#define GUI_PANEL_TEXT      0xFFDADADA  /* Panel text */
#define GUI_DOCK_BG         0xCC303030  /* Dock background */
#define GUI_DOCK_HOVER      0xFF404040  /* Dock item hover */
#define GUI_ACCENT          0xFF3689E6  /* elementary blue */
#define GUI_TITLEBAR        0xFF383838  /* Window title bar */
#define GUI_TITLEBAR_TEXT   0xFFD4D4D4  /* Title text */
#define GUI_CLOSE           0xFFED5353  /* Close button red */
#define GUI_MINIMIZE        0xFFF0C040  /* Minimize button yellow */
#define GUI_MAXIMIZE        0xFF68B723  /* Maximize button green */
#define GUI_WINDOW_BG       0xFF2B2B2B  /* Window content area */
#define GUI_BORDER          0xFF1A1A1A  /* 1px window border */
#define GUI_SHADOW          0x40000000  /* Drop shadow */
#define GUI_TEXT_DIM        0xFF888888  /* Dimmed text */

/* ── Layout constants ─────────────────────────────────────── */

#define GUI_PANEL_HEIGHT    28
#define GUI_DOCK_HEIGHT     48
#define GUI_DOCK_ICON_SIZE  36
#define GUI_DOCK_PADDING    8
#define GUI_TITLEBAR_H      28
#define GUI_BORDER_W        1
#define GUI_CORNER_RADIUS   4
#define GUI_SHADOW_OFFSET   3
#define GUI_BTN_RADIUS      6
#define GUI_FONT_W          8
#define GUI_FONT_H          16

/* ── Window descriptor ────────────────────────────────────── */

typedef struct {
    int32_t  x, y;
    int32_t  w, h;          /* content area dimensions */
    const char *title;
    uint32_t content_color;
} gui_win_desc_t;

/* ── Drawing primitives (gui_draw.c) ──────────────────────── */

void gui_fill_rect(gui_surface_t *s, int32_t x, int32_t y,
                   int32_t w, int32_t h, uint32_t color);
void gui_fill_rect_alpha(gui_surface_t *s, int32_t x, int32_t y,
                         int32_t w, int32_t h, uint32_t color);
void gui_hline(gui_surface_t *s, int32_t x, int32_t y,
               int32_t w, uint32_t color);
void gui_vline(gui_surface_t *s, int32_t x, int32_t y,
               int32_t h, uint32_t color);
void gui_gradient_v(gui_surface_t *s, int32_t x, int32_t y,
                    int32_t w, int32_t h,
                    uint32_t top_color, uint32_t bot_color);
void gui_rounded_rect(gui_surface_t *s, int32_t x, int32_t y,
                      int32_t w, int32_t h, int32_t r, uint32_t color);
void gui_rounded_rect_alpha(gui_surface_t *s, int32_t x, int32_t y,
                            int32_t w, int32_t h, int32_t r, uint32_t color);
void gui_fill_circle(gui_surface_t *s, int32_t cx, int32_t cy,
                     int32_t r, uint32_t color);

/* ── Text rendering (gui_text.c) ──────────────────────────── */

extern const uint8_t gui_font8x16[95][16];

void gui_draw_char(gui_surface_t *s, int32_t x, int32_t y,
                   char c, uint32_t fg, uint32_t bg);
void gui_draw_text(gui_surface_t *s, int32_t x, int32_t y,
                   const char *str, uint32_t fg, uint32_t bg);
int  gui_text_width(const char *str);
void gui_draw_text_centered(gui_surface_t *s, int32_t x, int32_t y,
                            int32_t w, const char *str,
                            uint32_t fg, uint32_t bg);

/* ── Panel (gui_panel.c) ──────────────────────────────────── */

void gui_panel_render(gui_surface_t *s, uint32_t screen_w);

/* ── Dock (gui_dock.c) ────────────────────────────────────── */

void gui_dock_render(gui_surface_t *s, uint32_t screen_w, uint32_t screen_h);

/* ── Window decorations (gui_window.c) ────────────────────── */

void gui_window_render(gui_surface_t *s, const gui_win_desc_t *win);

/* ── Desktop orchestrator (gui_desktop.c) ─────────────────── */

void gui_desktop_init(uint32_t screen_w, uint32_t screen_h);
void gui_desktop_render(gui_surface_t *screen);

#endif /* OSITO_GUI_H */
