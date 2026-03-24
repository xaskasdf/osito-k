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

/* ── Retina-quality dark palette (macOS-inspired depth) ─────── */

/* Desktop background: subtle warm-cool gradient */
#define GUI_BG_DARK         0xFF1A1A1E  /* Top: dark with cool undertone */
#define GUI_BG_LIGHT        0xFF28262E  /* Bottom: slightly warmer, purple tint */

/* Panel / Dock: frosted glass feel */
#define GUI_PANEL_BG        0xD8202028  /* 85% opaque, blue-tinted dark */
#define GUI_PANEL_TEXT      0xFFE5E5EA  /* Primary text (higher contrast) */
#define GUI_DOCK_BG         0xB0282830  /* 69% opaque, matches panel */
#define GUI_DOCK_HOVER      0xFF404048  /* Dock item hover */

/* Window chrome */
#define GUI_TITLEBAR        0xFF2D2D32  /* Unfocused: neutral dark */
#define GUI_TITLEBAR_FOCUS  0xFF383840  /* Focused: slightly lighter */
#define GUI_TITLEBAR_TEXT   0xFFE5E5EA  /* Title text (focused) */
#define GUI_WINDOW_BG       0xFF1C1C20  /* Deep content area */
#define GUI_BORDER          0xFF404048  /* Subtle visible border */
#define GUI_SHADOW_COLOR    0x50000000  /* Shadow base (50/255 max alpha) */

/* Traffic lights (macOS exact) */
#define GUI_CLOSE           0xFFFF5F57  /* macOS red */
#define GUI_MINIMIZE        0xFFFEBC2E  /* macOS yellow */
#define GUI_MAXIMIZE        0xFF28C840  /* macOS green */
#define GUI_BTN_DIM         0xFF555558  /* Unfocused button gray */

/* Accent + text */
#define GUI_ACCENT          0xFF007AFF  /* System blue (more saturated) */
#define GUI_TEXT_DIM        0xFF8E8E93  /* Secondary text */

/* Terminal */
#define GUI_TERM_BG         0xFF1A1A2E  /* Dark terminal blue */
#define GUI_TERM_FG         0xFFE0E0E0  /* Slightly warm white */
#define GUI_TERM_CURSOR     0xFF00FF88  /* Green cursor */

/* Legacy compat */
#define GUI_SHADOW          GUI_SHADOW_COLOR

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

/* ── Debug info (populated by kernel, rendered by panel) ──── */

typedef struct {
    uint64_t fps;
    uint64_t mem_used_mb;
    uint64_t mem_free_mb;
    uint64_t mem_total_mb;   /* total physical memory (UEFI map sum) */
    uint64_t ticks;
    uint32_t procs;
    uint8_t  rtc_hour;
    uint8_t  rtc_min;
    uint8_t  rtc_sec;
} gui_debug_info_t;

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

/* Anti-aliased primitives (Phase 2.1) */
void gui_rounded_rect_aa(gui_surface_t *s, int32_t x, int32_t y,
                         int32_t w, int32_t h, int32_t r, uint32_t color);
void gui_fill_circle_aa(gui_surface_t *s, int32_t cx, int32_t cy,
                        int32_t r, uint32_t color);

/* Multi-layer soft shadow (Phase 2.2) */
void gui_box_shadow(gui_surface_t *s, int32_t x, int32_t y,
                    int32_t w, int32_t h, int32_t offset_x, int32_t offset_y,
                    int32_t blur, uint32_t color);

/* Gradient titlebar (Phase 2.7) */
void gui_gradient_h(gui_surface_t *s, int32_t x, int32_t y,
                    int32_t w, int32_t h,
                    uint32_t left_color, uint32_t right_color);

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
void gui_panel_set_debug(const gui_debug_info_t *info);
const gui_debug_info_t *gui_panel_get_debug(void);

/* ── Dock (gui_dock.c) ────────────────────────────────────── */

void gui_dock_render(gui_surface_t *s, uint32_t screen_w, uint32_t screen_h);

/* ── Window decorations (gui_window.c) ────────────────────── */

void gui_window_render(gui_surface_t *s, const gui_win_desc_t *win, bool focused);

/* ── Animation engine (gui_anim.c) ───────────────────────── */

/* Easing functions — input/output in 16.16 fixed-point (0..0x10000) */
typedef int32_t (*gui_ease_fn)(int32_t t);

int32_t gui_ease_linear(int32_t t);
int32_t gui_ease_out_cubic(int32_t t);    /* 1-(1-t)^3: macOS deceleration */
int32_t gui_ease_out_quad(int32_t t);     /* 1-(1-t)^2: soft deceleration */
int32_t gui_ease_in_out_quad(int32_t t);  /* smooth step */
int32_t gui_ease_spring(int32_t t);       /* slight overshoot / bounce */

/* Animate a single int32_t value over time.
 * Call gui_anim_tick() each frame; the target is updated in-place.
 * on_complete is called once when the animation finishes (may be NULL). */
int  gui_anim_start(int32_t *target, int32_t end_val, uint32_t duration_ms,
                    gui_ease_fn ease,
                    void (*on_complete)(void *ctx), void *ctx);
void gui_anim_cancel(int32_t *target);

/* Called by compositor each frame.  Returns true if any animation is running. */
bool gui_anim_tick(uint64_t now_ms);
bool gui_anim_any_active(void);

/* ── Desktop orchestrator (gui_desktop.c) ─────────────────── */

void gui_desktop_init(uint32_t screen_w, uint32_t screen_h);
void gui_desktop_render(gui_surface_t *screen);
gui_win_desc_t *gui_desktop_get_windows(int *count);
void gui_desktop_raise_window(int idx);
void gui_desktop_show_window(int idx);   /* raise + restore if closed */
int *gui_desktop_get_order(void);

/* Register the terminal shm pixel buffer so gui_desktop blits it at the
 * correct z-order position (inside window 0, not after all windows). */
void gui_desktop_set_terminal_surface(uint32_t *px, uint32_t w, uint32_t h);

#endif /* OSITO_GUI_H */
