/*
 * OsitoK x86-64 — Display Subsystem (X-RETINA)
 *
 * Double-buffered display with VBlank synchronization.
 * Manages front/back buffers, page flipping, and surface operations.
 * Foundation for compositor and GPU display engine integration.
 *
 * Phase 1: Software double buffer over GOP framebuffer.
 * Phase 2: GPU display engine scanout (see GPU_DISPLAY.md).
 */

#include "../include/types.h"
#include "../include/boot_info.h"

/* ── External functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);
extern void fb_putdec(uint64_t val);

extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);
extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);
extern uint64_t idt_get_ticks(void);

/* ── Display state ───────────────────────────────────────────── */

typedef struct {
    uint32_t *gop_fb;        /* Original GOP framebuffer (always visible) */
    uint32_t *back;          /* Back buffer (draw here) */
    uint32_t  width;
    uint32_t  height;
    uint32_t  pitch;         /* pixels per scanline */
    uint64_t  fb_size;       /* total bytes per buffer */
    bool      initialized;
    bool      dirty;         /* back buffer has changes */

    /* VBlank simulation */
    uint64_t  vblank_count;
    uint64_t  last_flip_tick;
    uint32_t  target_fps;    /* target refresh rate */
    uint32_t  ticks_per_frame; /* ticks between frames at target_fps */

    /* TSC-based frame pacing (sub-tick precision, true 60fps) */
    uint64_t  tsc_per_frame;   /* TSC cycles per frame at target_fps */
    uint64_t  last_flip_tsc;   /* TSC value at last flip */

    /* Stats */
    uint64_t  flip_count;
    uint64_t  frame_drops;   /* frames that took >1 vblank */
} display_t;

static display_t disp;

/* ── Display mode state (multi-monitor placeholder) ──────────── */

static boot_display_mode_t avail_modes[BOOT_MAX_DISPLAY_MODES];
static uint32_t avail_mode_count;
static uint32_t current_mode_idx;

void display_set_available_modes(const boot_display_mode_t *modes,
                                 uint32_t count, uint32_t current)
{
    if (!modes || count == 0) return;
    if (count > BOOT_MAX_DISPLAY_MODES) count = BOOT_MAX_DISPLAY_MODES;
    avail_mode_count = count;
    current_mode_idx = current;
    for (uint32_t i = 0; i < count; i++)
        avail_modes[i] = modes[i];

    serial_puts("[DISP] Available modes: ");
    serial_putdec(count);
    serial_puts(", current=");
    serial_putdec(current);
    serial_puts(" (");
    serial_putdec(modes[current].width);
    serial_puts("x");
    serial_putdec(modes[current].height);
    serial_puts(")\n");
}

uint32_t display_get_mode_count(void)
{
    return avail_mode_count;
}

const boot_display_mode_t *display_get_mode(uint32_t idx)
{
    if (idx >= avail_mode_count) return (void *)0;
    return &avail_modes[idx];
}

/* ── Surface: a drawable rectangle ───────────────────────────── */

typedef struct {
    uint32_t *pixels;
    uint32_t  width;
    uint32_t  height;
    uint32_t  pitch;         /* pixels per scanline */
    uint64_t  size;          /* total bytes */
    bool      allocated;     /* true if we own the memory */
} surface_t;

/* ── TSC helper ──────────────────────────────────────────────── */

static inline uint64_t disp_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Calibrate TSC frequency using APIC ticks as reference.
 * Measures over N_CAL ticks (~100ms at 100Hz). */
#define TSC_CAL_TICKS 10

static uint64_t calibrate_tsc(void)
{
    /* Align to tick boundary */
    uint64_t t0 = idt_get_ticks();
    while (idt_get_ticks() == t0) __asm__ volatile ("hlt");

    uint64_t tick_start = idt_get_ticks();
    uint64_t tsc_start  = disp_rdtsc();

    /* Wait for N_CAL more ticks */
    while (idt_get_ticks() - tick_start < TSC_CAL_TICKS)
        __asm__ volatile ("hlt");

    uint64_t tsc_end  = disp_rdtsc();
    uint64_t elapsed_tsc = tsc_end - tsc_start;

    /* APIC fires at ~100Hz; TSC cycles per second = elapsed_tsc * (100 / N_CAL) */
    return elapsed_tsc * 100 / TSC_CAL_TICKS;
}

/* ── VBlank simulation ───────────────────────────────────────── */

/* TSC-based frame pacing: precise to microseconds, not limited by 100Hz APIC.
 * Falls back to APIC-tick method if TSC calibration looks unreasonable. */

void display_wait_vblank(void)
{
    if (disp.tsc_per_frame) {
        /* Spin-wait until target TSC value.
         * Use HLT only if we're more than ~5ms away (5M cycles at 1GHz). */
        uint64_t target = disp.last_flip_tsc + disp.tsc_per_frame;
        while (disp_rdtsc() < target) {
            uint64_t remaining = target - disp_rdtsc();
            if (remaining > disp.tsc_per_frame / 4)
                __asm__ volatile ("hlt");
            /* else tight spin for last ~4ms */
        }
    } else {
        /* Fallback: APIC tick-based (50fps cap at 100Hz) */
        while ((idt_get_ticks() - disp.last_flip_tick) < disp.ticks_per_frame)
            __asm__ volatile ("hlt");
    }
    disp.vblank_count++;
}

/* ── Initialization ──────────────────────────────────────────── */

int display_init(uint32_t *gop_base, uint32_t width, uint32_t height,
                 uint32_t pitch, uint32_t target_fps)
{
    serial_puts("[DISP] Initializing display subsystem...\n");

    disp.gop_fb = gop_base;
    disp.width  = width;
    disp.height = height;
    disp.pitch  = pitch;
    disp.fb_size = (uint64_t)pitch * height * sizeof(uint32_t);

    /* 0 means default 60fps; clamp to sane range */
    if (target_fps == 0) target_fps = 60;
    if (target_fps > 240) target_fps = 240;
    disp.target_fps = target_fps;

    /* Compute ticks per frame from 100Hz APIC tick rate.
     * Round up so we never flip faster than the target. */
    disp.ticks_per_frame = (100 + target_fps - 1) / target_fps;
    if (disp.ticks_per_frame == 0) disp.ticks_per_frame = 1;
    disp.last_flip_tick = idt_get_ticks();
    disp.flip_count = 0;
    disp.frame_drops = 0;
    disp.vblank_count = 0;
    disp.dirty = false;

    /* Calibrate TSC for sub-tick frame pacing (~100ms measurement window) */
    serial_puts("[DISP] Calibrating TSC...\n");
    uint64_t tsc_per_sec = calibrate_tsc();
    /* Sanity check: expect 100MHz – 5GHz for any CPU we'll run on */
    if (tsc_per_sec >= 100000000ULL && tsc_per_sec <= 5000000000ULL) {
        disp.tsc_per_frame = tsc_per_sec / target_fps;
        serial_puts("[DISP] TSC: ");
        serial_putdec(tsc_per_sec / 1000000);
        serial_puts(" MHz, ");
        serial_putdec(disp.tsc_per_frame / 1000);
        serial_puts(" kcy/frame\n");
    } else {
        disp.tsc_per_frame = 0;  /* use APIC fallback */
        serial_puts("[DISP] TSC calibration out of range, using APIC ticks\n");
    }
    disp.last_flip_tsc = disp_rdtsc();

    /* Allocate back buffer (page-aligned for potential GPU use) */
    disp.back = (uint32_t *)mem_alloc_aligned(disp.fb_size, 4096);
    if (!disp.back) {
        serial_puts("[DISP] ERROR: Failed to allocate back buffer\n");
        return -1;
    }

    /* Copy current GOP contents to back buffer */
    memcpy(disp.back, disp.gop_fb, disp.fb_size);

    disp.initialized = true;

    serial_puts("[DISP] Double buffer: ");
    serial_putdec(width);
    serial_puts("x");
    serial_putdec(height);
    serial_puts(" @ ");
    serial_putdec(disp.target_fps);
    serial_puts("fps, back=0x");
    serial_puthex((uint64_t)disp.back, 16);
    serial_puts(" (");
    serial_putdec(disp.fb_size / 1024);
    serial_puts(" KB)\n");

    return 0;
}

/* ── Non-temporal MMIO blit ──────────────────────────────────── */

/* Write-combining MMIO memory benefits from non-temporal stores:
 * - movntdq bypasses the CPU cache entirely (no cache pollution)
 * - The WC buffer aggregates writes into full cache-line bursts
 * - sfence ensures all writes are flushed before returning
 * 64-byte chunks match the cache line / WC buffer size exactly. */
static void memcpy_nt(void *dst, const void *src, uint64_t size)
{
    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d = (uint8_t *)dst;
    uint64_t chunks = size / 64;

    for (uint64_t i = 0; i < chunks; i++) {
        __asm__ volatile (
            "movdqa   (%0), %%xmm0\n\t"
            "movdqa 16(%0), %%xmm1\n\t"
            "movdqa 32(%0), %%xmm2\n\t"
            "movdqa 48(%0), %%xmm3\n\t"
            "movntdq %%xmm0,   (%1)\n\t"
            "movntdq %%xmm1, 16(%1)\n\t"
            "movntdq %%xmm2, 32(%1)\n\t"
            "movntdq %%xmm3, 48(%1)\n\t"
            : : "r"(s), "r"(d) : "memory",
                "xmm0", "xmm1", "xmm2", "xmm3"
        );
        s += 64; d += 64;
    }
    __asm__ volatile ("sfence" ::: "memory");

    uint64_t rem = size % 64;
    if (rem) memcpy(d, s, rem);
}

/* ── Page Flip ───────────────────────────────────────────────── */

/* Flip back buffer to screen. Waits for VBlank to avoid tearing.
 * Uses non-temporal stores for Phase 1 (GOP MMIO write-combining).
 * Phase 2 (GPU) would change the scanout address via SET_OFFSET. */

void display_flip(void)
{
    if (!disp.initialized || !disp.dirty) return;

    /* Wait for VBlank */
    display_wait_vblank();

    /* Blit back buffer → GOP MMIO using non-temporal stores.
     * Bypasses CPU cache for WC memory; avoids cache pollution. */
    memcpy_nt(disp.gop_fb, disp.back, disp.fb_size);

    /* Track timing */
    uint64_t now = idt_get_ticks();
    uint64_t elapsed = now - disp.last_flip_tick;
    if (elapsed > disp.ticks_per_frame * 2)
        disp.frame_drops++;

    disp.last_flip_tick = now;
    disp.last_flip_tsc  = disp_rdtsc();
    disp.flip_count++;
    disp.dirty = false;
}

/* Flip without waiting (for compositor that manages its own timing) */
void display_flip_nowait(void)
{
    if (!disp.initialized || !disp.dirty) return;
    memcpy_nt(disp.gop_fb, disp.back, disp.fb_size);
    disp.last_flip_tick = idt_get_ticks();
    disp.last_flip_tsc  = disp_rdtsc();
    disp.flip_count++;
    disp.dirty = false;
}

/* Force-refresh: regular memcpy (not NT stores) to ensure QEMU/HVF
 * dirty-page tracking detects the framebuffer write. Used after
 * fullscreen→desktop transitions where NT stores may not trigger
 * QEMU's Cocoa display refresh. */
void display_force_refresh(void)
{
    if (!disp.initialized || !disp.back) return;
    memcpy(disp.gop_fb, disp.back, disp.fb_size);
    disp.last_flip_tick = idt_get_ticks();
    disp.last_flip_tsc  = disp_rdtsc();
    disp.flip_count++;
    disp.dirty = false;
}

/* ── Back Buffer Access ──────────────────────────────────────── */

uint32_t *display_get_back_buffer(void)
{
    return disp.back;
}

void display_mark_dirty(void)
{
    disp.dirty = true;
}

uint32_t display_get_width(void)  { return disp.width; }
uint32_t display_get_height(void) { return disp.height; }
uint32_t display_get_pitch(void)  { return disp.pitch; }

/* ── Surface Operations ──────────────────────────────────────── */

int surface_create(surface_t *s, uint32_t width, uint32_t height)
{
    s->width  = width;
    s->height = height;
    s->pitch  = width;
    s->size   = (uint64_t)width * height * sizeof(uint32_t);
    s->pixels = (uint32_t *)kmalloc(s->size);
    if (!s->pixels) return -1;
    memset(s->pixels, 0, s->size);
    s->allocated = true;
    return 0;
}

void surface_destroy(surface_t *s)
{
    if (s->allocated && s->pixels) {
        kfree(s->pixels);
        s->pixels = NULL;
    }
    s->allocated = false;
}

/* Wrap existing buffer as surface (no ownership) */
void surface_wrap(surface_t *s, uint32_t *pixels, uint32_t width,
                  uint32_t height, uint32_t pitch)
{
    s->pixels = pixels;
    s->width  = width;
    s->height = height;
    s->pitch  = pitch;
    s->size   = (uint64_t)pitch * height * sizeof(uint32_t);
    s->allocated = false;
}

void surface_clear(surface_t *s, uint32_t color)
{
    for (uint32_t y = 0; y < s->height; y++)
        for (uint32_t x = 0; x < s->width; x++)
            s->pixels[y * s->pitch + x] = color;
}

/* ── Blit Operations ─────────────────────────────────────────── */

/* Opaque blit: fast memcpy per scanline */
void blit_opaque(surface_t *dst, const surface_t *src,
                 int32_t dx, int32_t dy)
{
    /* Clip source to destination bounds */
    int32_t sx = 0, sy = 0;
    int32_t w = (int32_t)src->width;
    int32_t h = (int32_t)src->height;

    if (dx < 0) { sx = -dx; w += dx; dx = 0; }
    if (dy < 0) { sy = -dy; h += dy; dy = 0; }
    if (dx + w > (int32_t)dst->width)  w = (int32_t)dst->width  - dx;
    if (dy + h > (int32_t)dst->height) h = (int32_t)dst->height - dy;
    if (w <= 0 || h <= 0) return;

    for (int32_t y = 0; y < h; y++) {
        uint32_t *d = dst->pixels + ((uint32_t)(dy + y)) * dst->pitch + (uint32_t)dx;
        const uint32_t *s = src->pixels + ((uint32_t)(sy + y)) * src->pitch + (uint32_t)sx;
        memcpy(d, s, (uint64_t)w * sizeof(uint32_t));
    }
}

/* Alpha blit: per-pixel blending (A8R8G8B8 format) */
void blit_alpha(surface_t *dst, const surface_t *src,
                int32_t dx, int32_t dy)
{
    int32_t sx = 0, sy = 0;
    int32_t w = (int32_t)src->width;
    int32_t h = (int32_t)src->height;

    if (dx < 0) { sx = -dx; w += dx; dx = 0; }
    if (dy < 0) { sy = -dy; h += dy; dy = 0; }
    if (dx + w > (int32_t)dst->width)  w = (int32_t)dst->width  - dx;
    if (dy + h > (int32_t)dst->height) h = (int32_t)dst->height - dy;
    if (w <= 0 || h <= 0) return;

    for (int32_t y = 0; y < h; y++) {
        uint32_t *d = dst->pixels + ((uint32_t)(dy + y)) * dst->pitch + (uint32_t)dx;
        const uint32_t *s = src->pixels + ((uint32_t)(sy + y)) * src->pitch + (uint32_t)sx;
        for (int32_t x = 0; x < w; x++) {
            uint32_t sp = s[x];
            uint8_t a = sp >> 24;
            if (a == 0) continue;        /* Fully transparent */
            if (a == 0xFF) {
                d[x] = sp;               /* Fully opaque: direct copy */
                continue;
            }
            /* Alpha blend */
            uint8_t inv_a = 255 - a;
            uint32_t dp = d[x];
            uint8_t r = (uint8_t)(((sp >> 16 & 0xFF) * a + (dp >> 16 & 0xFF) * inv_a) >> 8);
            uint8_t g = (uint8_t)(((sp >> 8  & 0xFF) * a + (dp >> 8  & 0xFF) * inv_a) >> 8);
            uint8_t b = (uint8_t)(((sp       & 0xFF) * a + (dp       & 0xFF) * inv_a) >> 8);
            d[x] = 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }
}

/* Fill rectangle on surface */
void surface_fill_rect(surface_t *s, int32_t x, int32_t y,
                       int32_t w, int32_t h, uint32_t color)
{
    /* Clip */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int32_t)s->width)  w = (int32_t)s->width  - x;
    if (y + h > (int32_t)s->height) h = (int32_t)s->height - y;
    if (w <= 0 || h <= 0) return;

    for (int32_t ry = 0; ry < h; ry++) {
        uint32_t *row = s->pixels + ((uint32_t)(y + ry)) * s->pitch + (uint32_t)x;
        for (int32_t rx = 0; rx < w; rx++)
            row[rx] = color;
    }
}

/* ── Gamma-Correct Text Blending (X-RETINA) ─────────────────── */

/* sRGB ↔ Linear lookup tables for gamma-correct blending.
 * This makes text look "retina quality" — sharp and properly weighted.
 *
 * Without gamma correction, dark text on light backgrounds appears
 * too thin, and light text on dark backgrounds appears too thick.
 * This is because sRGB is a nonlinear space; blending must happen
 * in linear space for physically correct results. */

static uint16_t srgb_to_linear_lut[256];  /* 8-bit sRGB → 16-bit linear */
static uint8_t  linear_to_srgb_lut[4096]; /* 12-bit linear → 8-bit sRGB */
static bool     gamma_lut_ready;

/* Initialize gamma LUTs (called once) */
static void gamma_lut_init(void)
{
    if (gamma_lut_ready) return;

    /* sRGB → linear: x^2.2 approximated with integer math.
     * Using (x/255)^2 * 65535 as fast approximation of gamma 2.2.
     * True gamma 2.2 would need pow(), but x^2 is close enough for
     * text rendering and avoids floating point in kernel. */
    for (int i = 0; i < 256; i++) {
        /* Approximate gamma 2.2 as: linear = (i/255)^2 * 65535
         * = i * i / 255 * 255 * 65535 / 65025 ≈ i*i*256/255 */
        uint32_t sq = (uint32_t)i * i;
        srgb_to_linear_lut[i] = (uint16_t)((sq * 257 + 128) >> 8);
    }

    /* Linear → sRGB: sqrt(x) * 255 approximated.
     * For 12-bit input (0-4095) → 8-bit output (0-255). */
    for (int i = 0; i < 4096; i++) {
        /* Approximate gamma 1/2.2 as sqrt: srgb = sqrt(i/4095) * 255 */
        /* Integer sqrt via Newton's method */
        uint32_t val = (uint32_t)i * 65025;  /* scale to 0-266,347,375 */
        uint32_t x = val;
        if (x > 0) {
            uint32_t r = x;
            /* 8 iterations of Newton's method for integer sqrt */
            r = (r + val / r) >> 1;
            r = (r + val / r) >> 1;
            r = (r + val / r) >> 1;
            r = (r + val / r) >> 1;
            r = (r + val / r) >> 1;
            r = (r + val / r) >> 1;
            r = (r + val / r) >> 1;
            r = (r + val / r) >> 1;
            linear_to_srgb_lut[i] = (uint8_t)((r * 255 + 32512) / 65025);
        } else {
            linear_to_srgb_lut[i] = 0;
        }
    }

    /* Fix endpoints */
    srgb_to_linear_lut[0] = 0;
    srgb_to_linear_lut[255] = 65535;
    linear_to_srgb_lut[0] = 0;
    linear_to_srgb_lut[4095] = 255;

    gamma_lut_ready = true;
    serial_puts("[DISP] Gamma LUTs initialized (sRGB ↔ linear)\n");
}

/* Blend a single text glyph with gamma correction.
 * glyph: 8-bit alpha coverage map (width × height bytes).
 * text_color: A8R8G8B8 color for the text. */
void display_blend_glyph_linear(surface_t *dst, const uint8_t *glyph,
                                int32_t gx, int32_t gy,
                                int32_t gw, int32_t gh,
                                uint32_t text_color)
{
    if (!gamma_lut_ready) gamma_lut_init();

    uint16_t tr = srgb_to_linear_lut[(text_color >> 16) & 0xFF];
    uint16_t tg = srgb_to_linear_lut[(text_color >> 8)  & 0xFF];
    uint16_t tb = srgb_to_linear_lut[ text_color        & 0xFF];

    for (int32_t y = 0; y < gh; y++) {
        int32_t py = gy + y;
        if (py < 0 || py >= (int32_t)dst->height) continue;
        for (int32_t x = 0; x < gw; x++) {
            int32_t px = gx + x;
            if (px < 0 || px >= (int32_t)dst->width) continue;

            uint8_t alpha = glyph[y * gw + x];
            if (alpha == 0) continue;

            uint32_t *pixel = &dst->pixels[(uint32_t)py * dst->pitch + (uint32_t)px];

            if (alpha == 255) {
                *pixel = 0xFF000000 | (text_color & 0x00FFFFFF);
                continue;
            }

            /* Gamma-correct blend in linear space */
            uint32_t bg = *pixel;
            uint16_t br = srgb_to_linear_lut[(bg >> 16) & 0xFF];
            uint16_t bg_ = srgb_to_linear_lut[(bg >> 8) & 0xFF];
            uint16_t bb = srgb_to_linear_lut[bg & 0xFF];

            /* Blend: out = text * alpha + bg * (1-alpha) */
            uint16_t a = (uint16_t)alpha;
            uint16_t inv_a = 255 - a;
            uint16_t or_ = (uint16_t)((tr * a + br * inv_a) / 255);
            uint16_t og  = (uint16_t)((tg * a + bg_ * inv_a) / 255);
            uint16_t ob  = (uint16_t)((tb * a + bb * inv_a) / 255);

            /* Convert back to sRGB (scale 16-bit linear to 12-bit LUT index) */
            uint8_t r = linear_to_srgb_lut[or_ >> 4];
            uint8_t g = linear_to_srgb_lut[og >> 4];
            uint8_t b = linear_to_srgb_lut[ob >> 4];

            *pixel = 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }
}

/* ── Dithering (X-RETINA) ────────────────────────────────────── */

/* Ordered 4×4 Bayer dithering for 6-bit panels.
 * Many TN panels only display 6 bits per channel (262K colors).
 * Dithering eliminates visible color banding in gradients. */

static const int8_t bayer4x4[4][4] = {
    { -8,  0, -6,  2},
    {  4, -4,  6, -2},
    { -5,  3, -7,  1},
    {  7, -1,  5, -3}
};

/* Apply dithering to the back buffer (in-place).
 * Call before display_flip() for panels with <8-bit color depth. */
void display_apply_dither(void)
{
    if (!disp.initialized) return;

    for (uint32_t y = 0; y < disp.height; y++) {
        for (uint32_t x = 0; x < disp.width; x++) {
            uint32_t *p = &disp.back[y * disp.pitch + x];
            uint32_t px = *p;

            int8_t d = bayer4x4[y & 3][x & 3];

            int r = ((px >> 16) & 0xFF) + d;
            int g = ((px >> 8)  & 0xFF) + d;
            int b = ( px        & 0xFF) + d;

            /* Clamp */
            if (r < 0) r = 0;
            if (r > 255) r = 255;
            if (g < 0) g = 0;
            if (g > 255) g = 255;
            if (b < 0) b = 0;
            if (b > 255) b = 255;

            /* Quantize to 6-bit (mask lower 2 bits) */
            r &= 0xFC; g &= 0xFC; b &= 0xFC;

            *p = 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}

/* ── Stats ───────────────────────────────────────────────────── */

uint64_t display_get_flip_count(void) { return disp.flip_count; }
uint64_t display_get_vblank_count(void) { return disp.vblank_count; }
uint64_t display_get_frame_drops(void) { return disp.frame_drops; }
