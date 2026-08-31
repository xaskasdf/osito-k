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
#include "../include/paging.h"
#include "../include/sys/display_syscalls.h"

/* ── External functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);
extern void fb_putdec(uint64_t val);

extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);
extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);
extern void  mem_free_pages(void *addr, uint64_t count);
extern uint64_t idt_get_ticks(void);
extern void sched_yield(void);

/* GPU display engine (gpu_display.c) — weak so we link without GPU driver */
extern int      gpu_display_is_ready(void)  __attribute__((weak));
extern int      gpu_display_flip(uint64_t fb_addr) __attribute__((weak));
extern uint32_t gpu_display_vblank_count(void) __attribute__((weak));

typedef struct {
    uint32_t pixel_clock_hz;
    uint16_t h_active, h_blank, h_sync_offset, h_sync_width;
    uint16_t v_active, v_blank, v_sync_offset, v_sync_width;
    uint16_t h_total, v_total;
    uint32_t refresh_hz;
    bool     interlaced;
} display_edid_mode_t;

extern int      gpu_display_detect_monitor(display_edid_mode_t *mode) __attribute__((weak));
extern int      gpu_display_set_mode(const display_edid_mode_t *mode,
                                     uint64_t fb_addr,
                                     uint32_t fb_pitch) __attribute__((weak));

/* Virtio-GPU 2D scanout (virtio_gpu.c) — weak so we link without virtio driver */
extern bool     virtio_gpu_ready(void)      __attribute__((weak));
extern bool     virtio_gpu_is_vga_compatible(void) __attribute__((weak));
extern uint32_t *virtio_gpu_get_fb(void)    __attribute__((weak));
extern uint32_t virtio_gpu_get_width(void)  __attribute__((weak));
extern uint32_t virtio_gpu_get_height(void) __attribute__((weak));
extern bool     virtio_gpu_get_native_mode(uint32_t *width,
                                           uint32_t *height) __attribute__((weak));
extern void     virtio_gpu_flush(void)      __attribute__((weak));
extern int      virtio_gpu_resize(uint32_t width, uint32_t height) __attribute__((weak));

/* Intel Gen9 probe (GOP scanout retained, no unsafe RAM page-flip) */
extern int      intel_gfx_is_ready(void)    __attribute__((weak));

static void virtio_blit(const uint32_t *src);
static int display_auto_modeset(void);
static inline uint64_t disp_rdtsc(void);
int display_resize(uint32_t new_width, uint32_t new_height, uint32_t new_pitch);
uint32_t *display_get_back_buffer(void);

/* ── Display state ───────────────────────────────────────────── */

typedef struct {
    uint32_t *gop_fb;        /* Original GOP framebuffer (always visible) */
    uint32_t  gop_width;
    uint32_t  gop_height;
    uint32_t  gop_pitch;
    uint32_t *back;          /* Back buffer (draw here) */
    uint32_t  width;
    uint32_t  height;
    uint32_t  pitch;         /* pixels per scanline */
    uint64_t  fb_size;       /* total bytes per buffer */
    bool      initialized;
    bool      dirty;         /* back buffer has changes */
    bool      gpu_scanout;   /* GPU display engine active (page flip) */
    bool      virtio_scanout; /* virtio-gpu 2D active (copy + flush) */
    uint32_t *virtio_fb;     /* virtio-gpu framebuffer pointer */

    /* Double buffer rotation (GPU scanout only).
     * Two buffers alternate: compositor draws into back while GPU
     * scans out from front. On flip, they swap roles — zero copy. */
    uint32_t *buffers[2];    /* Two page-aligned framebuffers */
    uint8_t   draw_idx;      /* Index compositor draws into (0 or 1) */

    /* VBlank simulation */
    uint64_t  vblank_count;
    uint64_t  last_flip_tick;
    uint32_t  target_fps;    /* target refresh rate */
    uint32_t  ticks_per_frame; /* ticks between frames at target_fps */

    /* TSC-based frame pacing (sub-tick precision, true 60fps) */
    uint64_t  tsc_per_sec;     /* calibrated TSC frequency */
    uint64_t  tsc_per_frame;   /* TSC cycles per frame at target_fps */
    uint64_t  last_flip_tsc;   /* TSC value at last flip */

    /* Stats */
    uint64_t  flip_count;
    uint64_t  frame_drops;   /* frames that took >1 vblank */
} display_t;

static display_t disp;

static uint32_t *display_alloc_back_buffer(uint64_t size)
{
    void *physical = mem_alloc_aligned(size, 4096);
    return physical ? (uint32_t *)PHYS_TO_VIRT(physical) : NULL;
}

static void display_free_back_buffer(uint32_t *buffer, uint64_t size)
{
    if (!buffer) return;
    mem_free_pages((void *)(uintptr_t)kvirt_to_phys(buffer),
                   (size + 4095ULL) / 4096ULL);
}

static bool display_gop_mirror_compatible(void)
{
    return disp.gop_fb && disp.width == disp.gop_width &&
           disp.height == disp.gop_height && disp.pitch == disp.gop_pitch;
}

static bool display_virtio_owns_primary_scanout(void)
{
    return disp.virtio_scanout && virtio_gpu_is_vga_compatible &&
           virtio_gpu_is_vga_compatible();
}

/* ── Display mode state (multi-monitor placeholder) ──────────── */

static boot_display_mode_t avail_modes[BOOT_MAX_DISPLAY_MODES];
static uint32_t avail_mode_count;
static uint32_t current_mode_idx;

#define DISP_EINVAL  (-22)
#define DISP_ENOSYS  (-38)
#define DISP_ENOTSUP (-95)

#define DISPLAY_DEFAULT_REFRESH_HZ 60u
#define DISPLAY_MAX_REFRESH_HZ     240u

void display_set_available_modes(const boot_display_mode_t *modes,
                                 uint32_t count, uint32_t current)
{
    if (!modes || count == 0) return;
    if (count > BOOT_MAX_DISPLAY_MODES) count = BOOT_MAX_DISPLAY_MODES;
    if (current >= count) current = 0;
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

    if (disp.initialized && disp.virtio_scanout)
        (void)display_auto_modeset();
}

static uint32_t display_backend_kind(void)
{
    if (disp.gpu_scanout)
        return DISPLAY_BACKEND_NVIDIA;
    if (disp.virtio_scanout)
        return DISPLAY_BACKEND_VIRTIO;
    if (disp.initialized || disp.gop_fb)
        return DISPLAY_BACKEND_GOP;
    return DISPLAY_BACKEND_NONE;
}

static void display_fill_mode_info(display_mode_info_t *out,
                                   uint32_t width, uint32_t height,
                                   uint32_t pitch, uint32_t pixel_format,
                                   uint32_t refresh_hz, uint32_t flags)
{
    if (!out) return;
    out->width = width;
    out->height = height;
    out->pitch = pitch;
    out->pixel_format = pixel_format;
    out->refresh_hz = refresh_hz;
    out->flags = flags;
    out->backend = display_backend_kind();
    out->reserved = 0;
}

static void display_select_current_mode(uint32_t width, uint32_t height)
{
    for (uint32_t i = 0; i < avail_mode_count; i++) {
        if (avail_modes[i].width == width && avail_modes[i].height == height) {
            current_mode_idx = i;
            return;
        }
    }
}

static uint32_t display_default_refresh(uint32_t refresh_hz)
{
    if (refresh_hz == 0) return DISPLAY_DEFAULT_REFRESH_HZ;
    if (refresh_hz > DISPLAY_MAX_REFRESH_HZ) return DISPLAY_MAX_REFRESH_HZ;
    return refresh_hz;
}

static int display_apply_refresh(uint32_t refresh_hz)
{
    if (refresh_hz == 0)
        return 0;
    if (refresh_hz > DISPLAY_MAX_REFRESH_HZ)
        return DISP_EINVAL;

    disp.target_fps = refresh_hz;
    disp.ticks_per_frame = (100 + refresh_hz - 1) / refresh_hz;
    if (disp.ticks_per_frame == 0) disp.ticks_per_frame = 1;
    disp.tsc_per_frame = disp.tsc_per_sec ? disp.tsc_per_sec / refresh_hz : 0;
    disp.last_flip_tick = idt_get_ticks();
    disp.last_flip_tsc  = disp_rdtsc();

    serial_puts("[DISP] Refresh pacing: ");
    serial_putdec(refresh_hz);
    serial_puts("Hz\n");
    return 0;
}

static int display_set_virtio_mode(uint32_t width, uint32_t height)
{
    if (!disp.virtio_scanout || !virtio_gpu_resize)
        return DISP_ENOTSUP;

    uint32_t old_width = disp.width;
    uint32_t old_height = disp.height;
    uint32_t old_pitch = disp.pitch;
    int rc = virtio_gpu_resize(width, height);
    if (rc < 0) {
        if (rc == DISP_EINVAL) return DISP_EINVAL;
        if (rc == DISP_ENOSYS) return DISP_ENOSYS;
        return DISP_ENOTSUP;
    }

    if (display_resize(width, height, width) < 0) {
        (void)virtio_gpu_resize(old_width, old_height);
        (void)display_resize(old_width, old_height, old_pitch);
        return DISP_ENOTSUP;
    }

    disp.virtio_fb = virtio_gpu_get_fb ? virtio_gpu_get_fb() : NULL;
    if (!disp.virtio_fb) {
        (void)virtio_gpu_resize(old_width, old_height);
        (void)display_resize(old_width, old_height, old_pitch);
        disp.virtio_fb = virtio_gpu_get_fb ? virtio_gpu_get_fb() : NULL;
        disp.virtio_scanout = disp.virtio_fb != NULL;
        return DISP_ENOTSUP;
    }

    display_select_current_mode(width, height);
    virtio_blit(disp.back);
    return 0;
}

static int display_auto_modeset(void)
{
    if (!disp.initialized || !disp.virtio_scanout ||
        !virtio_gpu_get_native_mode) {
        return DISP_ENOTSUP;
    }

    if (!display_virtio_owns_primary_scanout()) {
        serial_puts("[DISP] Automodeset skipped: virtio is a secondary display\n");
        return DISP_ENOTSUP;
    }

    uint32_t native_w = 0, native_h = 0;
    if (!virtio_gpu_get_native_mode(&native_w, &native_h))
        return DISP_ENOTSUP;
    if (native_w == disp.width && native_h == disp.height)
        return 0;

    int rc = display_set_virtio_mode(native_w, native_h);
    if (rc == 0) {
        serial_puts("[DISP] Automodeset: virtio native ");
        serial_putdec(native_w);
        serial_puts("x");
        serial_putdec(native_h);
        serial_puts("\n");
    } else {
        serial_puts("[DISP] Automodeset: virtio native failed\n");
    }
    return rc;
}

uint32_t display_get_mode_count(void)
{
    if (avail_mode_count)
        return avail_mode_count;
    return disp.initialized ? 1 : 0;
}

const boot_display_mode_t *display_get_mode(uint32_t idx)
{
    if (idx >= avail_mode_count) return (void *)0;
    return &avail_modes[idx];
}

int display_modeset_get_mode(uint32_t idx, display_mode_info_t *out)
{
    if (!out) return DISP_EINVAL;

    if (avail_mode_count) {
        if (idx >= avail_mode_count) return DISP_EINVAL;
        const boot_display_mode_t *m = &avail_modes[idx];
        uint32_t flags = DISPLAY_MODE_BOOT;
        if (idx == current_mode_idx ||
            (disp.initialized && m->width == disp.width &&
             m->height == disp.height)) {
            flags |= DISPLAY_MODE_CURRENT;
        }
        display_fill_mode_info(out, m->width, m->height, m->pitch,
                               m->pixel_format, 0, flags);
        return 0;
    }

    if (idx == 0 && disp.initialized) {
        display_fill_mode_info(out, disp.width, disp.height, disp.pitch,
                               0, disp.target_fps, DISPLAY_MODE_CURRENT);
        return 0;
    }

    return DISP_EINVAL;
}

int display_modeset_get_current(display_mode_info_t *out)
{
    if (!out) return DISP_EINVAL;

    if (disp.initialized) {
        uint32_t flags = DISPLAY_MODE_CURRENT | DISPLAY_MODE_HARDWARE;
        display_fill_mode_info(out, disp.width, disp.height, disp.pitch,
                               0, disp.target_fps, flags);
        return 0;
    }

    if (avail_mode_count && current_mode_idx < avail_mode_count) {
        const boot_display_mode_t *m = &avail_modes[current_mode_idx];
        display_fill_mode_info(out, m->width, m->height, m->pitch,
                               m->pixel_format, 0,
                               DISPLAY_MODE_CURRENT | DISPLAY_MODE_BOOT);
        return 0;
    }

    return DISP_ENOSYS;
}

int display_modeset_set(uint32_t width, uint32_t height,
                        uint32_t refresh_hz, uint32_t flags)
{
    if (!disp.initialized)
        return DISP_ENOSYS;
    if (refresh_hz > DISPLAY_MAX_REFRESH_HZ)
        return DISP_EINVAL;

    if (flags & DISPLAY_SET_REFRESH_ONLY) {
        if (width || height || (flags & DISPLAY_SET_NATIVE) || !refresh_hz)
            return DISP_EINVAL;
        return display_apply_refresh(refresh_hz);
    }

    if (flags & DISPLAY_SET_NATIVE) {
        if (display_virtio_owns_primary_scanout() &&
            virtio_gpu_get_native_mode) {
            uint32_t native_w = 0, native_h = 0;
            if (!virtio_gpu_get_native_mode(&native_w, &native_h))
                return DISP_ENOTSUP;
            if (width && height &&
                (native_w != width || native_h != height)) {
                return DISP_EINVAL;
            }
            int rc = display_set_virtio_mode(native_w, native_h);
            if (rc < 0) return rc;
            return display_apply_refresh(refresh_hz);
        }

        if (!disp.gpu_scanout || !gpu_display_detect_monitor ||
            !gpu_display_set_mode) {
            return DISP_ENOTSUP;
        }

        display_edid_mode_t native;
        if (gpu_display_detect_monitor(&native) < 0)
            return DISP_ENOTSUP;

        if (width && height &&
            (native.h_active != width || native.v_active != height)) {
            return DISP_EINVAL;
        }

        if (display_resize(native.h_active, native.v_active,
                           native.h_active) < 0) {
            return DISP_ENOTSUP;
        }

        uint32_t *bb = display_get_back_buffer();
        if (!bb)
            return DISP_ENOTSUP;

        if (gpu_display_set_mode(&native, (uint64_t)(uintptr_t)bb,
                                 (uint32_t)native.h_active * 4) < 0) {
            return DISP_ENOTSUP;
        }

        return display_apply_refresh(refresh_hz);
    }

    if (!width || !height)
        return DISP_EINVAL;

    if (width == disp.width && height == disp.height)
        return display_apply_refresh(refresh_hz);

    if (display_virtio_owns_primary_scanout() && virtio_gpu_resize) {
        int rc = display_set_virtio_mode(width, height);
        if (rc < 0) return rc;
        return display_apply_refresh(refresh_hz);
    }

    return DISP_ENOTSUP;
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
    /* TSC was already calibrated at boot against the PIT in idt_init
     * (idt.c:2240-2257). Reuse that value rather than spinning on
     * APIC ticks here — the APIC LVT may be masked by compat32 for
     * the lifetime of a Win32 PE process (compat32.c:1163), in which
     * case `hlt` would never wake up and this function would hang. */
    extern uint64_t idt_get_tsc_freq(void);
    return idt_get_tsc_freq();
}

/* ── VBlank synchronization ──────────────────────────────────── */

/* Three methods in priority order:
 * 1. GPU hardware VBlank counter (real display timing, ~0 drift)
 * 2. TSC-based frame pacing (precise to microseconds)
 * 3. APIC tick-based (50fps cap at 100Hz, coarsest) */

void display_wait_vblank(void)
{
    if (disp.gpu_scanout && gpu_display_vblank_count) {
        /* Hardware VBlank: poll GPU display engine counter.
         * Actual monitor refresh timing — no drift, no simulation. */
        uint32_t start = gpu_display_vblank_count();
        uint64_t deadline = idt_get_ticks() + 100;  /* ~1s at 100Hz */
        while (gpu_display_vblank_count() == start) {
            if (idt_get_ticks() >= deadline) break;
            sched_yield();
        }
    } else if (disp.tsc_per_frame) {
        /* Yield while there is useful time left in the frame.  compat32 can
         * run with interrupts or the APIC timer masked, so HLT is not a safe
         * pacing primitive here. */
        uint64_t target = disp.last_flip_tsc + disp.tsc_per_frame;
        while (disp_rdtsc() < target) {
            uint64_t remaining = target - disp_rdtsc();
            if (remaining > disp.tsc_per_frame / 4)
                sched_yield();
            else
                __asm__ volatile ("pause");
        }
    } else {
        /* Fallback: APIC tick-based (50fps cap at 100Hz) */
        while ((idt_get_ticks() - disp.last_flip_tick) < disp.ticks_per_frame)
            sched_yield();
    }
    disp.vblank_count++;
}

/* ── Initialization ──────────────────────────────────────────── */

int display_init(uint32_t *gop_base, uint32_t width, uint32_t height,
                 uint32_t pitch, uint32_t target_fps)
{
    serial_puts("[DISP] Initializing display subsystem...\n");

    disp.gop_fb = gop_base;
    disp.gop_width = width;
    disp.gop_height = height;
    disp.gop_pitch = pitch;
    disp.width  = width;
    disp.height = height;
    disp.pitch  = pitch;
    disp.fb_size = (uint64_t)pitch * height * sizeof(uint32_t);

    /* 0 means default 60fps; clamp to sane range */
    target_fps = display_default_refresh(target_fps);
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
    /* Sanity check: accept 10MHz – 8GHz (covers VMs, old CPUs, modern Xeons) */
    if (tsc_per_sec >= 10000000ULL && tsc_per_sec <= 8000000000ULL) {
        disp.tsc_per_sec = tsc_per_sec;
        disp.tsc_per_frame = disp.tsc_per_sec / target_fps;
        serial_puts("[DISP] TSC: ");
        serial_putdec(disp.tsc_per_sec / 1000000);
        serial_puts(" MHz, ");
        serial_putdec(disp.tsc_per_frame / 1000);
        serial_puts(" kcy/frame\n");
    } else {
        disp.tsc_per_sec = 0;
        disp.tsc_per_frame = 0;  /* use APIC fallback */
        serial_puts("[DISP] TSC calibration out of range, using APIC ticks\n");
    }
    disp.last_flip_tsc = disp_rdtsc();

    /* Allocate back buffers (page-aligned for GPU scanout) */
    disp.buffers[0] = display_alloc_back_buffer(disp.fb_size);
    disp.buffers[1] = display_alloc_back_buffer(disp.fb_size);
    if (!disp.buffers[0] || !disp.buffers[1]) {
        serial_puts("[DISP] ERROR: Failed to allocate back buffer(s)\n");
        return -1;
    }
    disp.draw_idx = 0;
    disp.back = disp.buffers[0];

    /* Copy current GOP contents to both buffers */
    memcpy(disp.buffers[0], disp.gop_fb, disp.fb_size);
    memcpy(disp.buffers[1], disp.gop_fb, disp.fb_size);

    disp.initialized = true;
    disp.gpu_scanout = false;

    /* Check if GPU display engine is available for page flipping */
    if (gpu_display_is_ready && gpu_display_flip &&
        gpu_display_is_ready()) {
        disp.gpu_scanout = true;
    }

    /* Check if virtio-GPU 2D is available for scanout */
    disp.virtio_scanout = false;
    disp.virtio_fb = NULL;
    if (virtio_gpu_ready && virtio_gpu_ready()) {
        disp.virtio_fb = virtio_gpu_get_fb();
        if (disp.virtio_fb) {
            disp.virtio_scanout = true;
            /* Initial blit: copy current GOP contents to virtio-gpu */
            virtio_blit(disp.gop_fb);
            serial_puts("[DISP] virtio-GPU scanout enabled\n");
        }
    }

    serial_puts("[DISP] Double buffer: ");
    serial_putdec(width);
    serial_puts("x");
    serial_putdec(height);
    serial_puts(" @ ");
    serial_putdec(disp.target_fps);
    serial_puts("fps, buf[0]=0x");
    serial_puthex((uint64_t)disp.buffers[0], 16);
    serial_puts(" buf[1]=0x");
    serial_puthex((uint64_t)disp.buffers[1], 16);
    serial_puts(" (");
    serial_putdec(disp.fb_size / 1024);
    serial_puts(" KB each)\n");
    serial_puts("[DISP] Scanout: ");
    if (disp.gpu_scanout)
        serial_puts("GPU page flip\n");
    else if (disp.virtio_scanout)
        serial_puts("virtio blit + flush\n");
    else if (intel_gfx_is_ready && intel_gfx_is_ready())
        serial_puts("Intel Gen9 GOP-retained\n");
    else
        serial_puts("CPU memcpy (GOP)\n");

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

/* ── Virtio-GPU blit helper ──────────────────────────────────── */

static void virtio_blit(const uint32_t *src)
{
    extern uint64_t idt_get_ticks(void);

    uint32_t vw = virtio_gpu_get_width();
    uint32_t vh = virtio_gpu_get_height();
    uint32_t cw = disp.width < vw ? disp.width : vw;
    uint32_t ch = disp.height < vh ? disp.height : vh;
    uint32_t *dst = disp.virtio_fb;
    for (uint32_t y = 0; y < ch; y++)
        memcpy(dst + y * vw, src + y * disp.pitch, cw * 4);

    /* Throttle after the first present. The initial blit must flush even
     * during early boot, otherwise QEMU never receives SET_SCANOUT and the
     * virtio window can stay black until a later SHM/GPU present. */
    static uint64_t last_flush_tick;
    uint64_t now = idt_get_ticks();
    if (last_flush_tick == 0 || now - last_flush_tick >= 3) {
        virtio_gpu_flush();
        last_flush_tick = now;
    }
}

/* ── Page Flip ───────────────────────────────────────────────── */

/* Flip back buffer to screen. Waits for VBlank to avoid tearing.
 * Uses non-temporal stores for Phase 1 (GOP MMIO write-combining).
 * Phase 2 (GPU) would change the scanout address via SET_OFFSET. */

void display_flip(void)
{
    /* The dirty-flag early-return optimisation skipped 59 of 60 flips
     * on a static desktop, which means the host display backend
     * (Cocoa, HVF) only saw a single refresh after the compositor
     * started — every subsequent frame produced the same gop_fb
     * memcpy that the host had already seen, but with dirty=true only
     * being set when the compositor explicitly invalidates, most
     * frames were skipped. Cheap memcpy ≪ losing user-visible refresh
     * cadence in QEMU/HVF. */
    if (!disp.initialized) return;

    /* Wait for VBlank */
    display_wait_vblank();

    if (disp.gpu_scanout) {
        /* GPU page flip: tell display engine to scan out from current draw buffer.
         * Then swap to the other buffer for next frame's drawing.
         * Compositor draws into buffers[draw_idx], GPU reads from the one we just flipped. */
        gpu_display_flip(kvirt_to_phys(disp.buffers[disp.draw_idx]));
        disp.draw_idx ^= 1;
        disp.back = disp.buffers[disp.draw_idx];
    } else if (display_gop_mirror_compatible()) {
        /* Back buffer → GOP framebuffer (QEMU / no GPU scanout).
         *
         * Use regular memcpy, NOT memcpy_nt: non-temporal stores
         * bypass cache and *also* bypass QEMU/HVF dirty-page tracking
         * — the host display backend only refreshes pages it observes
         * being written via normal cacheable stores. The mfence then
         * drains the write buffers before the host's next dirty-page
         * scan, so the new pixels actually become visible. */
        memcpy(disp.gop_fb, disp.back, disp.fb_size);
        __asm__ volatile ("mfence" ::: "memory");
    }

    /* Virtio-GPU: also blit to virtio framebuffer and flush to host */
    if (disp.virtio_scanout)
        virtio_blit(disp.back);

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
    if (disp.gpu_scanout) {
        gpu_display_flip(kvirt_to_phys(disp.buffers[disp.draw_idx]));
        disp.draw_idx ^= 1;
        disp.back = disp.buffers[disp.draw_idx];
    } else if (display_gop_mirror_compatible()) {
        memcpy(disp.gop_fb, disp.back, disp.fb_size);
    }
    if (disp.virtio_scanout)
        virtio_blit(disp.back);
    disp.last_flip_tick = idt_get_ticks();
    disp.last_flip_tsc  = disp_rdtsc();
    disp.flip_count++;
    disp.dirty = false;
}

/* Force-refresh: ensure the current back buffer is displayed.
 * With GPU scanout: page flip to current buffer.
 * Without GPU: regular memcpy to GOP (QEMU/HVF dirty-page tracking). */
void display_force_refresh(void)
{
    if (!disp.initialized || !disp.back) return;
    if (disp.gpu_scanout)
        gpu_display_flip(kvirt_to_phys(disp.back));
    else if (display_gop_mirror_compatible())
        memcpy(disp.gop_fb, disp.back, disp.fb_size);
    if (disp.virtio_scanout)
        virtio_blit(disp.back);
    disp.last_flip_tick = idt_get_ticks();
    disp.last_flip_tsc  = disp_rdtsc();
    disp.flip_count++;
    disp.dirty = false;
}

/* Resize display buffers after modeset.
 * Re-allocates back buffers for the new resolution. */
int display_resize(uint32_t new_width, uint32_t new_height, uint32_t new_pitch)
{
    if (!disp.initialized) return -1;

    uint64_t new_size = (uint64_t)new_pitch * new_height * sizeof(uint32_t);
    uint64_t old_size = disp.fb_size;

    uint32_t *b0 = display_alloc_back_buffer(new_size);
    uint32_t *b1 = display_alloc_back_buffer(new_size);
    if (!b0 || !b1) {
        serial_puts("[DISP] Resize failed: cannot allocate buffers\n");
        display_free_back_buffer(b0, new_size);
        display_free_back_buffer(b1, new_size);
        return -1;
    }

    /* Clear new buffers */
    memset(b0, 0, new_size);
    memset(b1, 0, new_size);

    /* Free old buffers */
    display_free_back_buffer(disp.buffers[0], old_size);
    display_free_back_buffer(disp.buffers[1], old_size);

    disp.width  = new_width;
    disp.height = new_height;
    disp.pitch  = new_pitch;
    disp.fb_size = new_size;
    disp.buffers[0] = b0;
    disp.buffers[1] = b1;
    disp.draw_idx = 0;
    disp.back = disp.buffers[0];

    serial_puts("[DISP] Resized to ");
    serial_putdec(new_width);
    serial_puts("x");
    serial_putdec(new_height);
    serial_puts(" (");
    serial_putdec(new_size / 1024);
    serial_puts(" KB per buffer)\n");

    return 0;
}

/* Enable GPU scanout (called after gpu_display_init succeeds) */
void display_enable_gpu_scanout(void)
{
    if (!disp.initialized) return;
    if (gpu_display_is_ready && gpu_display_flip &&
        gpu_display_is_ready()) {
        disp.gpu_scanout = true;
        serial_puts("[DISP] GPU scanout enabled (page flip)\n");
    }
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

/* ── Stats ───────────────────────────────────────────────────── */

uint64_t display_get_flip_count(void) { return disp.flip_count; }
uint64_t display_get_vblank_count(void) { return disp.vblank_count; }
uint64_t display_get_frame_drops(void) { return disp.frame_drops; }
