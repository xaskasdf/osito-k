/*
 * OsitoK WASM — Framebuffer driver
 *
 * Two modes:
 *   1. Text mode (default): serial_puts routes to JS terminal div.
 *      fb_puts/putchar are no-ops to prevent double output.
 *   2. Pixel mode (after fb_redirect): text rendering goes to a
 *      pixel surface for the compositor to display on Canvas.
 *
 * fb_redirect() is called by the 'desktop' command to switch modes.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <emscripten.h>

extern void serial_puts(const char *s);
extern void serial_putc(char c);

/* ── Framebuffer state ─────────────────────────────────────── */

static uint32_t *fb_base;        /* pixel buffer (NULL = text mode) */
static uint32_t  fb_w = 800;
static uint32_t  fb_h = 600;
static uint32_t  fb_p = 800;     /* pitch in pixels */
static uint32_t  fb_bg_color = 0xFF000000;  /* clear color (ARGB) */

void fb_init(uint32_t *base, uint32_t w, uint32_t h, uint32_t pitch)
{
    fb_base = base;
    fb_w = w;
    fb_h = h;
    fb_p = pitch;
}

/* ── Framebuffer query API ─────────────────────────────────── */

uint32_t *fb_get_vram(void)   { return fb_base; }
uint32_t  fb_get_width(void)  { return fb_w; }
uint32_t  fb_get_height(void) { return fb_h; }
uint32_t  fb_get_pitch(void)  { return fb_p; }

/* ── Mode switching ────────────────────────────────────────── */

void fb_redirect(uint32_t *new_base, uint32_t tw, uint32_t th, uint32_t pitch)
{
    fb_base = new_base;
    fb_w = tw;
    fb_h = th;
    fb_p = pitch;
    /* terminal.c will detect fb_base and render pixels to it */
}

void fb_set_clear_color(uint32_t color) { fb_bg_color = color; }

/* ── Clear ─────────────────────────────────────────────────── */

void fb_clear(void)
{
    if (fb_base) {
        /* Pixel mode: fill surface with background color */
        uint32_t total = fb_p * fb_h;
        for (uint32_t i = 0; i < total; i++)
            fb_base[i] = fb_bg_color;
    } else {
        /* Text mode: clear JS terminal */
        EM_ASM({ wasm_clear_terminal(); });
    }
}

/* ── Text output (no-ops in text mode — serial handles it) ── */

void fb_puts(const char *s)               { (void)s; }
void fb_putchar(char c)                   { (void)c; }
void fb_putdec(uint64_t val)              { (void)val; }
void fb_puthex(uint64_t val, int d)       { (void)val; (void)d; }
void fb_puts_color(const char *s, uint32_t color) { (void)s; (void)color; }
void fb_enable_shadow(void *buf)          { (void)buf; }
