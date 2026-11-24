/*
 * OsitoK WASM — Display driver
 *
 * Replaces arch/x86/kernel/display.c (GOP double buffer + VBlank).
 * Uses HTML Canvas 2D context for output, malloc for back buffer,
 * and emscripten_sleep for frame pacing.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <emscripten.h>

#include "sys/display_syscalls.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);

/* ── Display state ─────────────────────────────────────────── */

static uint32_t *back_buf;
static uint32_t  disp_w, disp_h, disp_pitch;
static uint64_t  disp_fb_size;
static bool      disp_dirty;
static bool      disp_initialized;
static uint32_t  disp_refresh_hz = 60;

/* ── Display API ───────────────────────────────────────────── */

int display_init(uint32_t *gop_base, uint32_t width, uint32_t height,
                 uint32_t pitch, uint32_t target_fps)
{
    (void)gop_base;
    disp_w     = width;
    disp_h     = height;
    disp_pitch = pitch;
    disp_refresh_hz = target_fps ? target_fps : 60;
    disp_fb_size = (uint64_t)pitch * height * sizeof(uint32_t);

    back_buf = (uint32_t *)malloc((size_t)disp_fb_size);
    if (!back_buf) {
        serial_puts("[DISP] Failed to allocate back buffer\n");
        return -1;
    }
    memset(back_buf, 0, (size_t)disp_fb_size);

    /* Create Canvas element and get 2D context in JS */
    EM_ASM({
        var c = document.getElementById('desktop-canvas');
        if (!c) {
            c = document.createElement('canvas');
            c.id = 'desktop-canvas';
            document.body.appendChild(c);
        }
        c.width  = $0;
        c.height = $1;
        c.style.display = 'block';
        /* Scale canvas to fill viewport while preserving aspect ratio */
        c.style.width = '100%';
        c.style.height = '100%';
        c.style.objectFit = 'contain';
        c.style.background = '#000';
        c.style.imageRendering = 'pixelated';
        window.__dispCtx = c.getContext('2d');
        window.__dispImg = window.__dispCtx.createImageData($0, $1);

        /* Hide text terminal, show canvas */
        var term = document.getElementById('terminal');
        if (term) term.style.display = 'none';
        var inp = document.querySelector('.input-row');
        if (inp) inp.style.display = 'none';
        var bar = document.querySelector('.status-bar');
        if (bar) bar.style.display = 'none';
        /* Prevent page scroll */
        document.body.style.overflow = 'hidden';

        /* Route keyboard events to WASM shell (replaces the hidden input field) */
        c.tabIndex = 0;  /* make canvas focusable */
        c.focus();
        c.addEventListener('keydown', function(e) {
            var key = e.key;
            if (key === 'Enter') {
                Module.ccall('wasm_kb_push', null, ['number'], [13]);
            } else if (key === 'Backspace') {
                Module.ccall('wasm_kb_push', null, ['number'], [8]);
            } else if (key === 'Escape') {
                Module.ccall('wasm_kb_push', null, ['number'], [27]);
            } else if (key === 'Tab') {
                Module.ccall('wasm_kb_push', null, ['number'], [9]);
            } else if (e.ctrlKey && key === 'c') {
                Module.ccall('wasm_kb_push', null, ['number'], [3]);
            } else if (key.length === 1) {
                Module.ccall('wasm_kb_push', null, ['number'], [key.charCodeAt(0)]);
            }
            e.preventDefault();
        });

        /* Route mouse events to compositor input system.
         * input_set_mouse_abs expects tablet coords (0-32767). */
        c.addEventListener('mousemove', function(e) {
            var rect = c.getBoundingClientRect();
            var nx = (e.clientX - rect.left) / rect.width;
            var ny = (e.clientY - rect.top) / rect.height;
            var tx = Math.round(nx * 32767);
            var ty = Math.round(ny * 32767);
            if (tx < 0) tx = 0; if (tx > 32767) tx = 32767;
            if (ty < 0) ty = 0; if (ty > 32767) ty = 32767;
            if (Module._input_set_mouse_abs)
                Module._input_set_mouse_abs(tx, ty);
        });
        c.addEventListener('mousedown', function(e) {
            var btn = 0;
            if (e.button === 0) btn = 1;  /* left */
            if (e.button === 2) btn = 2;  /* right */
            if (e.button === 1) btn = 4;  /* middle */
            if (Module._input_post_mouse_button)
                Module._input_post_mouse_button(btn);
            e.preventDefault();
        });
        c.addEventListener('mouseup', function(e) {
            if (Module._input_post_mouse_button)
                Module._input_post_mouse_button(0);
            e.preventDefault();
        });
        c.addEventListener('contextmenu', function(e) { e.preventDefault(); });
    }, width, height);

    disp_initialized = true;
    disp_dirty = false;

    serial_puts("[DISP] Canvas display: ");
    serial_putdec(width);
    serial_puts("x");
    serial_putdec(height);
    serial_puts("\n");

    return 0;
}

/* Blit back buffer to Canvas (no sleep) */
static void display_blit(void)
{
    if (!disp_initialized || !disp_dirty) return;

    /* Copy BGRA back buffer → Canvas RGBA ImageData.
     * OsitoK uses 0xAARRGGBB (BGRA in memory on little-endian).
     * Canvas ImageData expects [R,G,B,A] byte order. */
    EM_ASM({
        var src  = $0 >>> 0;
        var npx  = $1;
        var data = window.__dispImg.data;
        var heap = HEAPU8;
        for (var i = 0; i < npx; i++) {
            var off = src + i * 4;
            var b = heap[off];
            var g = heap[off + 1];
            var r = heap[off + 2];
            var a = heap[off + 3];
            var d = i * 4;
            data[d]     = r;
            data[d + 1] = g;
            data[d + 2] = b;
            data[d + 3] = a ? a : 255;
        }
        window.__dispCtx.putImageData(window.__dispImg, 0, 0);
    }, back_buf, disp_w * disp_h);

    disp_dirty = false;
}

void display_flip(void)          { display_blit(); }
void display_flip_nowait(void)   { display_blit(); }
void display_force_refresh(void) { disp_dirty = true; display_blit(); }

void display_wait_vblank(void)
{
    emscripten_sleep((int)(1000 / disp_refresh_hz));
}

uint32_t *display_get_back_buffer(void) { return back_buf; }
void      display_mark_dirty(void)      { disp_dirty = true; }
uint32_t  display_get_width(void)       { return disp_w; }
uint32_t  display_get_height(void)      { return disp_h; }
uint32_t  display_get_pitch(void)       { return disp_pitch; }

typedef struct { uint32_t w, h, p, f; } boot_display_mode_t;
void display_set_available_modes(const boot_display_mode_t *m, uint32_t n, uint32_t c)
{ (void)m; (void)n; (void)c; }

static int display_fill_mode(display_mode_info_t *out)
{
    if (!out || !disp_initialized) return -1;
    out->width = disp_w;
    out->height = disp_h;
    out->pitch = disp_pitch;
    out->pixel_format = 0;
    out->refresh_hz = disp_refresh_hz;
    out->flags = DISPLAY_MODE_CURRENT | DISPLAY_MODE_HARDWARE;
    out->backend = DISPLAY_BACKEND_NONE;
    out->reserved = 0;
    return 0;
}

uint32_t display_get_mode_count(void)
{
    return disp_initialized ? 1 : 0;
}

int display_modeset_get_mode(uint32_t index, display_mode_info_t *out)
{
    return index == 0 ? display_fill_mode(out) : -1;
}

int display_modeset_get_current(display_mode_info_t *out)
{
    return display_fill_mode(out);
}

int display_modeset_set(uint32_t width, uint32_t height,
                        uint32_t refresh_hz, uint32_t flags)
{
    if (!disp_initialized || refresh_hz > 1000) return -1;
    if (flags & DISPLAY_SET_NATIVE) {
        width = disp_w;
        height = disp_h;
    }
    if (!(flags & DISPLAY_SET_REFRESH_ONLY) &&
            (width != disp_w || height != disp_h)) {
        return -1;
    }
    if (refresh_hz) disp_refresh_hz = refresh_hz;
    return 0;
}
