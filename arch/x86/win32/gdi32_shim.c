/*
 * OsitoK Windows Compatibility Layer — gdi32.dll Shim Implementation
 *
 * Real GDI Device Contexts and BitBlt for Win32 compat layer.
 * DCs back onto framebuffer (screen) or kmalloc'd pixel buffers (memory).
 */

#include "gdi32_shim.h"
#include "compat32.h"
#include "dllloader.h"
#include "kernel32_shim.h"
#include "opengl32_shim.h"
#include "win32_abi.h"
#include "../fs/vfs.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern void serial_puthex(uint64_t val, int digits);
extern void *kmalloc(uint64_t size);
extern void *krealloc(void *ptr, uint64_t size);
extern void  kfree(void *ptr);
extern const uint8_t gui_font8x16[95][16];

/* Framebuffer access (weak — may not be linked) */
extern uint32_t *fb_get_base(void)   __attribute__((weak));
extern uint32_t  fb_get_width(void)  __attribute__((weak));
extern uint32_t  fb_get_height(void) __attribute__((weak));
extern uint32_t  fb_get_pitch(void)  __attribute__((weak));
extern uint32_t  display_get_width(void)  __attribute__((weak));
extern uint32_t  display_get_height(void) __attribute__((weak));
extern BOOL user32_get_window_surface(HANDLE window, void **pixels, int *width,
                                      int *height, int *pitch)
    __attribute__((weak));
extern void user32_mark_window_dirty(HANDLE window) __attribute__((weak));
extern BOOL user32_get_current_display_mode(uint32_t *width, uint32_t *height,
                                            uint32_t *bpp, uint32_t *frequency)
    __attribute__((weak));

#define SCREEN_WIDTH  800
#define SCREEN_HEIGHT 600
#define SCREEN_BPP    32

/* ── Local helpers ───────────────────────────────────────────── */

static void gdi_memcpy(void *dst, const void *src, SIZE_T n)
{
    BYTE *d = (BYTE *)dst;
    const BYTE *s = (const BYTE *)src;
    while (n--) *d++ = *s++;
}

static void gdi_memmove(void *dst, const void *src, SIZE_T n)
{
    BYTE *d = (BYTE *)dst;
    const BYTE *s = (const BYTE *)src;
    if (d <= s || d >= s + n) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
}

static void gdi_memset(void *p, int v, SIZE_T n)
{
    BYTE *d = (BYTE *)p;
    while (n--) *d++ = (BYTE)v;
}

/* ── GDI DC and Bitmap tables ────────────────────────────────── */

#define MAX_GDI_DCS     4096
#define MAX_GDI_BITMAPS 512
#define MAX_GDI_FONTS   256
#define MAX_GDI_REGIONS 512
#define MAX_REGION_RECTS 64

typedef struct {
    int      in_use;
    void    *surface;       /* pixel buffer (NULL = no bitmap selected yet) */
    int      width, height;
    int      bpp;
    int      pitch;         /* bytes per scanline */
    uint32_t text_color;
    uint32_t bk_color;
    uint32_t brush_color;
    int      bk_mode;       /* TRANSPARENT=1, OPAQUE=2 */
    UINT     text_align;
    int      map_mode;
    int      stretch_mode;
    int      viewport_x, viewport_y;
    int      brush_x, brush_y;
    int      graphics_mode; /* GM_COMPATIBLE or GM_ADVANCED */
    float    world_transform[6];
    HGDIOBJ  prev_bitmap;   /* currently selected bitmap handle */
    HGDIOBJ  current_font;  /* currently selected font handle */
    int      is_screen;     /* DC targets the GOP framebuffer (window/screen DC) */
    int      pooled_window; /* GetDC/BeginPaint DC, retained across ReleaseDC */
    ULONG_PTR owner_window; /* HWND associated with a pooled window DC */
    int      bottomup;      /* selected bitmap is a bottom-up DIB */
} GDI_DC;

static BOOL gdi_read_dc_pixel(const GDI_DC *dc, int x, int y,
                              uint32_t *color);
static BOOL gdi_write_dc_pixel(GDI_DC *dc, int x, int y, uint32_t color);

typedef struct {
    int      in_use;
    void    *pixels;
    int      width, height;
    int      bpp;
    int      pitch;
    int      bottomup;      /* DIB with positive biHeight (rows stored bottom-up) */
    int      virtual_alloc; /* pixels from VirtualAlloc (DIB section) */
    void    *mapping_base;  /* MapViewOfFile base for section-backed DIBs */
} GDI_BITMAP;

typedef struct {
    LONG lfHeight;
    LONG lfWidth;
    LONG lfEscapement;
    LONG lfOrientation;
    LONG lfWeight;
    BYTE lfItalic;
    BYTE lfUnderline;
    BYTE lfStrikeOut;
    BYTE lfCharSet;
    BYTE lfOutPrecision;
    BYTE lfClipPrecision;
    BYTE lfQuality;
    BYTE lfPitchAndFamily;
    WCHAR lfFaceName[32];
} GDI_LOGFONTW;

typedef struct {
    LONG lfHeight;
    LONG lfWidth;
    LONG lfEscapement;
    LONG lfOrientation;
    LONG lfWeight;
    BYTE lfItalic;
    BYTE lfUnderline;
    BYTE lfStrikeOut;
    BYTE lfCharSet;
    BYTE lfOutPrecision;
    BYTE lfClipPrecision;
    BYTE lfQuality;
    BYTE lfPitchAndFamily;
    char lfFaceName[32];
} GDI_LOGFONTA;

typedef struct {
    int in_use;
    GDI_LOGFONTW logfont;
} GDI_FONT;

typedef struct {
    int in_use;
    UINT count;
    GDI_RECT rects[MAX_REGION_RECTS];
} GDI_REGION;

_Static_assert(sizeof(GDI_LOGFONTW) == 92, "LOGFONTW layout changed");
_Static_assert(sizeof(GDI_LOGFONTA) == 60, "LOGFONTA layout changed");

static GDI_DC     gdi_dcs[MAX_GDI_DCS];
static GDI_BITMAP gdi_bmps[MAX_GDI_BITMAPS];
static GDI_FONT   gdi_fonts[MAX_GDI_FONTS];
static GDI_REGION gdi_regions[MAX_GDI_REGIONS];
static volatile int gdi_region_lock;
static volatile int gdi_gamma_lock;
static int gdi_gamma_initialized;
static WORD gdi_gamma_ramp[3][256];
static UINT       gdi_blit_trace_count;

/* Handle encoding:
 *   HDC    = 0xDC000000 | index
 *   HBITMAP = 0xBB000000 | index
 * DC indices use 12 bits; bitmap and font tables use the same decoder but
 * retain their smaller table-specific bounds.
 */
#define DC_TAG     0xDC000000u
#define BMP_TAG    0xBB000000u
#define FONT_TAG   0xF0000000u
#define REGION_TAG 0xCC000000u
#define TAG_MASK   0xFF000000u
#define IDX_MASK   0x00000FFFu
#define STOCK_BITMAP ((HGDIOBJ)(ULONG_PTR)(BMP_TAG | 0xFEu))
#define STOCK_SYSTEM_FONT ((HGDIOBJ)(ULONG_PTR)0xAA00000Du)
#define OBJ_BITMAP 7
#define OBJ_FONT   6

#define GDI_BI_RGB            0u
#define GDI_BI_BITFIELDS      3u
#define GDI_BI_ALPHABITFIELDS 6u
#define GDI_DIB_RGB_COLORS    0u

#define IS_DC_HANDLE(h)  (((ULONG_PTR)(h) & TAG_MASK) == DC_TAG)
#define IS_BMP_HANDLE(h) (((ULONG_PTR)(h) & TAG_MASK) == BMP_TAG)
#define IS_FONT_HANDLE(h) (((ULONG_PTR)(h) & TAG_MASK) == FONT_TAG)
#define IS_REGION_HANDLE(h) (((ULONG_PTR)(h) & TAG_MASK) == REGION_TAG)
#define IS_STOCK_FONT_HANDLE(h) \
    ((((ULONG_PTR)(h) & TAG_MASK) == 0xAA000000u) && \
     (((ULONG_PTR)(h) & IDX_MASK) >= 10u) && \
     (((ULONG_PTR)(h) & IDX_MASK) <= 17u) && \
     (((ULONG_PTR)(h) & IDX_MASK) != 15u))
#define DC_INDEX(h)      ((int)((ULONG_PTR)(h) & IDX_MASK))
#define BMP_INDEX(h)     ((int)((ULONG_PTR)(h) & IDX_MASK))
#define FONT_INDEX(h)    ((int)((ULONG_PTR)(h) & IDX_MASK))
#define REGION_INDEX(h)  ((int)((ULONG_PTR)(h) & IDX_MASK))

static GDI_DC *dc_from_handle(HDC h)
{
    if (!IS_DC_HANDLE(h)) return NULL;
    int idx = DC_INDEX(h);
    if (idx < 0 || idx >= MAX_GDI_DCS) return NULL;
    if (!gdi_dcs[idx].in_use) return NULL;
    GDI_DC *dc = &gdi_dcs[idx];

    /* A window DC survives ReleaseDC and may be retained by EGL for the
     * lifetime of a window.  Keep it attached to user32's current backing
     * store when that store is replaced by a resize. */
    if (dc->pooled_window && dc->owner_window &&
        user32_get_window_surface) {
        void *surface = NULL;
        int width = 0, height = 0, pitch = 0;
        if (user32_get_window_surface((HANDLE)dc->owner_window, &surface,
                                      &width, &height, &pitch)) {
            if (dc->surface != surface || dc->width != width ||
                dc->height != height || dc->pitch != pitch) {
                static uint32_t refresh_trace_count;
                if (refresh_trace_count++ < 64) {
                    serial_puts("[GDI-DC-REFRESH] hwnd=0x");
                    serial_puthex(dc->owner_window, 8);
                    serial_puts(" size=");
                    serial_putdec((uint32_t)width);
                    serial_puts("x");
                    serial_putdec((uint32_t)height);
                    serial_puts(" pitch=");
                    serial_putdec((uint32_t)pitch);
                    serial_puts("\n");
                }
            }
            dc->surface = surface;
            dc->width = width;
            dc->height = height;
            dc->pitch = pitch;
            dc->is_screen = 0;
        } else {
            dc->surface = NULL;
            dc->width = 0;
            dc->height = 0;
            dc->pitch = 0;
        }
    }
    return dc;
}

static GDI_BITMAP *bmp_from_handle(HBITMAP h)
{
    if (!IS_BMP_HANDLE(h)) return NULL;
    int idx = BMP_INDEX(h);
    if (idx < 0 || idx >= MAX_GDI_BITMAPS) return NULL;
    if (!gdi_bmps[idx].in_use) return NULL;
    return &gdi_bmps[idx];
}

static GDI_FONT *font_from_handle(HGDIOBJ h)
{
    if (!IS_FONT_HANDLE(h)) return NULL;
    int idx = FONT_INDEX(h);
    if (idx < 0 || idx >= MAX_GDI_FONTS) return NULL;
    if (!gdi_fonts[idx].in_use) return NULL;
    return &gdi_fonts[idx];
}

static GDI_REGION *region_from_handle(HGDIOBJ h)
{
    if (!IS_REGION_HANDLE(h)) return NULL;
    int idx = REGION_INDEX(h);
    if (idx < 0 || idx >= MAX_GDI_REGIONS) return NULL;
    if (!gdi_regions[idx].in_use) return NULL;
    return &gdi_regions[idx];
}

static void regions_lock(void)
{
    while (__atomic_exchange_n(&gdi_region_lock, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile ("pause");
}

static void regions_unlock(void)
{
    __atomic_store_n(&gdi_region_lock, 0, __ATOMIC_RELEASE);
}

static void gamma_lock(void)
{
    while (__atomic_exchange_n(&gdi_gamma_lock, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile ("pause");
}

static void gamma_unlock(void)
{
    __atomic_store_n(&gdi_gamma_lock, 0, __ATOMIC_RELEASE);
}

static void gamma_initialize(void)
{
    if (gdi_gamma_initialized) return;
    for (int channel = 0; channel < 3; channel++)
        for (int value = 0; value < 256; value++)
            gdi_gamma_ramp[channel][value] = (WORD)(value * 257U);
    gdi_gamma_initialized = 1;
}

static int alloc_region(void)
{
    for (int i = 0; i < MAX_GDI_REGIONS; i++) {
        if (gdi_regions[i].in_use) continue;
        gdi_memset(&gdi_regions[i], 0, sizeof(gdi_regions[i]));
        gdi_regions[i].in_use = 1;
        return i;
    }
    return -1;
}

static void gdi_stock_font_logfont(GDI_LOGFONTW *lf)
{
    static const char face[] = "System";
    gdi_memset(lf, 0, sizeof(*lf));
    lf->lfHeight = -16;
    lf->lfWeight = 400;
    lf->lfCharSet = 1; /* DEFAULT_CHARSET */
    for (int i = 0; face[i] && i < 31; i++)
        lf->lfFaceName[i] = (WCHAR)(BYTE)face[i];
}

static int alloc_dc(void)
{
    for (int i = 0; i < MAX_GDI_DCS; i++) {
        if (!gdi_dcs[i].in_use) {
            gdi_memset(&gdi_dcs[i], 0, sizeof(GDI_DC));
            gdi_dcs[i].in_use = 1;
            gdi_dcs[i].text_color = 0x00000000u; /* COLORREF black */
            gdi_dcs[i].bk_color = 0x00FFFFFFu;   /* COLORREF white */
            gdi_dcs[i].bk_mode = 2; /* OPAQUE */
            gdi_dcs[i].map_mode = 1; /* MM_TEXT */
            gdi_dcs[i].stretch_mode = 1; /* BLACKONWHITE */
            gdi_dcs[i].graphics_mode = 1; /* GM_COMPATIBLE */
            gdi_dcs[i].world_transform[0] = 1.0f;
            gdi_dcs[i].world_transform[3] = 1.0f;
            gdi_dcs[i].current_font = STOCK_SYSTEM_FONT;
            return i;
        }
    }
    serial_puts("[GDI32] DC table exhausted capacity=");
    serial_putdec(MAX_GDI_DCS);
    serial_puts("\n");
    return -1;
}

static int alloc_bmp(void)
{
    for (int i = 0; i < MAX_GDI_BITMAPS; i++) {
        if (!gdi_bmps[i].in_use) {
            gdi_memset(&gdi_bmps[i], 0, sizeof(GDI_BITMAP));
            gdi_bmps[i].in_use = 1;
            return i;
        }
    }
    return -1;
}

static int alloc_font(void)
{
    for (int i = 0; i < MAX_GDI_FONTS; i++) {
        if (!gdi_fonts[i].in_use) {
            gdi_memset(&gdi_fonts[i], 0, sizeof(GDI_FONT));
            gdi_fonts[i].in_use = 1;
            return i;
        }
    }
    return -1;
}

static int bitmap_slots_used(void)
{
    int used = 0;
    for (int i = 0; i < MAX_GDI_BITMAPS; i++)
        if (gdi_bmps[i].in_use) used++;
    return used;
}

/* Get framebuffer info, falling back to compile-time constants */
static void *get_screen_surface(int *out_w, int *out_h, int *out_pitch)
{
    void *base = NULL;
    int w = SCREEN_WIDTH, h = SCREEN_HEIGHT, p = SCREEN_WIDTH * 4;

    if (fb_get_base) {
        uint32_t *fb = fb_get_base();
        if (fb) {
            base = fb;
            if (fb_get_width)  w = (int)fb_get_width();
            if (fb_get_height) h = (int)fb_get_height();
            if (fb_get_pitch)  p = (int)fb_get_pitch();
        }
    }

    if (out_w)     *out_w = w;
    if (out_h)     *out_h = h;
    if (out_pitch) *out_pitch = p;
    return base;
}

static void mark_dc_dirty(GDI_DC *dc)
{
    if (dc && dc->pooled_window && dc->owner_window &&
        user32_mark_window_dirty)
        user32_mark_window_dirty((HANDLE)dc->owner_window);
}

/* ── GetDeviceCaps ───────────────────────────────────────────── */

int WINAPI GetDeviceCaps(HDC hdc, int index)
{
    (void)hdc;
    /* Report the real GOP resolution as the device extent so UT99 keeps the
     * larger DirectDraw-enumerated modes (it filters modes bigger than this).
     * BUT once a fullscreen-exclusive SetDisplayMode has happened, NT reports
     * the CURRENT mode here (HORZRES/VERTRES track the desktop mode) — gate on
     * ddraw_display_mode_active() so startup enumeration still sees the GOP
     * size while in-game consumers see the truth after a SetRes. */
    extern int  ddraw_display_mode_active(void) __attribute__((weak));
    extern void ddraw_get_display_mode(uint32_t *w, uint32_t *h, uint32_t *bpp)
                __attribute__((weak));
    uint32_t physical_w = display_get_width ? display_get_width() : 0;
    uint32_t physical_h = display_get_height ? display_get_height() : 0;
    if (!physical_w && fb_get_width) physical_w = fb_get_width();
    if (!physical_h && fb_get_height) physical_h = fb_get_height();
    int hw = physical_w ? (int)physical_w : SCREEN_WIDTH;
    int vh = physical_h ? (int)physical_h : SCREEN_HEIGHT;
    uint32_t mode_bpp = SCREEN_BPP;
    if (user32_get_current_display_mode) {
        uint32_t mw = 0, mh = 0;
        user32_get_current_display_mode(&mw, &mh, &mode_bpp, NULL);
        if (mw && mh) { hw = (int)mw; vh = (int)mh; }
    } else if (ddraw_display_mode_active && ddraw_get_display_mode &&
               ddraw_display_mode_active()) {
        uint32_t mw = 0, mh = 0;
        ddraw_get_display_mode(&mw, &mh, &mode_bpp);
        if (mw && mh) { hw = (int)mw; vh = (int)mh; }
    }
    switch (index) {
    case HORZRES:    return hw;
    case VERTRES:    return vh;
    case BITSPIXEL:  return (int)mode_bpp;
    case PLANES:     return 1;
    case RASTERCAPS: return 0;
    case TECHNOLOGY: return 1; /* DT_RASDISPLAY */
    case LOGPIXELSX:
    case LOGPIXELSY: return 96;
    default:         return 0;
    }
}

/* ── DC creation / destruction ───────────────────────────────── */

BOOL WINAPI GetDeviceGammaRamp(HDC hdc, PVOID ramp)
{
    if (!dc_from_handle(hdc)) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    if (!ramp) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    gamma_lock();
    gamma_initialize();
    gdi_memcpy(ramp, gdi_gamma_ramp, sizeof(gdi_gamma_ramp));
    gamma_unlock();
    SetLastError(0);
    return TRUE;
}

BOOL WINAPI SetDeviceGammaRamp(HDC hdc, PCVOID ramp)
{
    if (!dc_from_handle(hdc)) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    if (!ramp) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    gamma_lock();
    gamma_initialize();
    gdi_memcpy(gdi_gamma_ramp, ramp, sizeof(gdi_gamma_ramp));
    gamma_unlock();
    SetLastError(0);
    return TRUE;
}

HDC WINAPI CreateDCA(PCSTR lpszDriver, PCSTR lpszDevice,
                     PCSTR lpszOutput, PCVOID lpInitData)
{
    (void)lpszDriver; (void)lpszDevice;
    (void)lpszOutput; (void)lpInitData;
    serial_puts("[GDI32] CreateDCA\n");

    /* Allocate a screen DC */
    int idx = alloc_dc();
    if (idx < 0) return NULL;

    int w, h, p;
    void *base = get_screen_surface(&w, &h, &p);

    gdi_dcs[idx].surface = base;
    gdi_dcs[idx].width   = w;
    gdi_dcs[idx].height  = h;
    gdi_dcs[idx].bpp     = 32;
    gdi_dcs[idx].pitch   = p;
    gdi_dcs[idx].is_screen = 1;  /* display DC: BitBlt here = present to GOP */

    return (HDC)(ULONG_PTR)(DC_TAG | (unsigned)idx);
}

static HDC WINAPI CreateDCW_k32(PCWSTR driver, PCWSTR device,
                                 PCWSTR output, PCVOID init_data)
{
    (void)driver;
    (void)device;
    (void)output;
    return CreateDCA(NULL, NULL, NULL, init_data);
}

static BOOL WINAPI GetICMProfileW_stub(HDC hdc, DWORD *name_chars,
                                        PWSTR filename)
{
    if (!dc_from_handle(hdc) || !name_chars) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    if (filename && *name_chars)
        filename[0] = 0;
    *name_chars = 0;
    SetLastError(50); /* ERROR_NOT_SUPPORTED */
    return FALSE;
}

HDC WINAPI CreateCompatibleDC(HDC hdc)
{
#ifndef OK_QUIET
    serial_puts("[GDI32] CreateCompatibleDC\n");
#endif

    int idx = alloc_dc();
    if (idx < 0) return NULL;

    /* Inherit dimensions from source DC (or screen defaults) */
    GDI_DC *src = dc_from_handle(hdc);
    if (src) {
        gdi_dcs[idx].width  = src->width;
        gdi_dcs[idx].height = src->height;
        gdi_dcs[idx].bpp    = src->bpp;
        gdi_dcs[idx].pitch  = src->pitch;
    } else {
        /* Default to screen dimensions */
        gdi_dcs[idx].width  = SCREEN_WIDTH;
        gdi_dcs[idx].height = SCREEN_HEIGHT;
        gdi_dcs[idx].bpp    = 32;
        gdi_dcs[idx].pitch  = SCREEN_WIDTH * 4;
    }
    /* surface = NULL — no bitmap selected yet */

    gdi_dcs[idx].prev_bitmap = STOCK_BITMAP;
    return (HDC)(ULONG_PTR)(DC_TAG | (unsigned)idx);
}

BOOL WINAPI DeleteDC(HDC hdc)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (dc && dc->pooled_window)
        return FALSE;
    if (dc) {
        dc->in_use = 0;
        dc->surface = NULL;
    }
    return TRUE;
}

/* ── Screen DC helpers (called from user32 GetDC/ReleaseDC) ──── */

HDC gdi32_alloc_window_dc(HANDLE window)
{
    int idx = -1;
    for (int i = 0; i < MAX_GDI_DCS; i++) {
        if (gdi_dcs[i].in_use && gdi_dcs[i].pooled_window &&
            gdi_dcs[i].owner_window == (ULONG_PTR)window) {
            idx = i;
            break;
        }
    }
    if (idx < 0) idx = alloc_dc();
    if (idx < 0) return NULL;

    GDI_DC *dc = &gdi_dcs[idx];

    int w = 0, h = 0, p = 0;
    void *base = NULL;
    int is_screen = 1;

    if (window && user32_get_window_surface &&
        user32_get_window_surface(window, &base, &w, &h, &p)) {
        is_screen = 0;
    } else {
        base = get_screen_surface(&w, &h, &p);
    }

    dc->surface = base;
    dc->width   = w;
    dc->height  = h;
    dc->bpp     = 32;
    dc->pitch   = p;
    dc->is_screen = is_screen;
    dc->pooled_window = 1;
    dc->owner_window = (ULONG_PTR)window;
    dc->current_font = STOCK_SYSTEM_FONT;

    return (HDC)(ULONG_PTR)(DC_TAG | (unsigned)idx);
}

HDC gdi32_alloc_screen_dc(void)
{
    return gdi32_alloc_window_dc(NULL);
}

HANDLE gdi32_window_from_dc(HDC hdc)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc || !dc->pooled_window) return NULL;
    return (HANDLE)dc->owner_window;
}

void gdi32_free_screen_dc(HDC hdc)
{
    GDI_DC *dc = dc_from_handle(hdc);
    /* ReleaseDC releases a pooled borrow; the DC remains associated with its
     * window so WindowFromDC stays deterministic across concurrent windows. */
    if (dc && dc->pooled_window) return;
    if (dc) {
        dc->in_use = 0;
        dc->surface = NULL;
    }
}

/* ── Pixel format (passthrough stubs — DirectDraw uses these) ── */

int WINAPI ChoosePixelFormat(HDC hdc, const PIXELFORMATDESCRIPTOR *ppfd)
{
    serial_puts("[GDI32-PFD] ChoosePixelFormat hdc=0x");
    serial_puthex((uint64_t)(ULONG_PTR)hdc, 8);
    serial_puts(" pfd=0x");
    serial_puthex((uint64_t)(ULONG_PTR)ppfd, 8);
    if (ppfd) {
        serial_puts(" size=");
        serial_putdec(ppfd->nSize);
        serial_puts(" flags=0x");
        serial_puthex(ppfd->dwFlags, 8);
        serial_puts(" rgba/depth/stencil=");
        serial_putdec(ppfd->cColorBits);
        serial_puts("/");
        serial_putdec(ppfd->cAlphaBits);
        serial_puts("/");
        serial_putdec(ppfd->cDepthBits);
        serial_puts("/");
        serial_putdec(ppfd->cStencilBits);
    }
    serial_puts(" -> 1\n");
    return 1; /* format #1 */
}

BOOL WINAPI SetPixelFormat(HDC hdc, int format, const PIXELFORMATDESCRIPTOR *ppfd)
{
    serial_puts("[GDI32-PFD] SetPixelFormat hdc=0x");
    serial_puthex((uint64_t)(ULONG_PTR)hdc, 8);
    serial_puts(" format=");
    serial_putdec((uint64_t)(uint32_t)format);
    serial_puts(" pfd=0x");
    serial_puthex((uint64_t)(ULONG_PTR)ppfd, 8);
    serial_puts(" -> TRUE\n");
    return TRUE;
}

int WINAPI GetPixelFormat(HDC hdc)
{
    (void)hdc;
    return 1;
}

int WINAPI DescribePixelFormat(HDC hdc, int iPixelFormat, DWORD nBytes,
                                LPPIXELFORMATDESCRIPTOR ppfd)
{
    serial_puts("[GDI32-PFD] DescribePixelFormat hdc=0x");
    serial_puthex((uint64_t)(ULONG_PTR)hdc, 8);
    serial_puts(" format=");
    serial_putdec((uint64_t)(uint32_t)iPixelFormat);
    serial_puts(" bytes=");
    serial_putdec(nBytes);
    serial_puts(" out=0x");
    serial_puthex((uint64_t)(ULONG_PTR)ppfd, 8);
    serial_puts(" -> 1\n");
    if (ppfd && nBytes >= sizeof(PIXELFORMATDESCRIPTOR)) {
        BYTE *p = (BYTE *)ppfd;
        for (SIZE_T i = 0; i < sizeof(PIXELFORMATDESCRIPTOR); i++) p[i] = 0;
        ppfd->nSize       = sizeof(PIXELFORMATDESCRIPTOR);
        ppfd->nVersion    = 1;
        ppfd->dwFlags     = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL |
                            PFD_DOUBLEBUFFER;
        if (!opengl32_has_accelerated_backend())
            ppfd->dwFlags |= PFD_GENERIC_FORMAT;
        ppfd->iPixelType  = PFD_TYPE_RGBA;
        ppfd->cColorBits  = 32;
        ppfd->cRedBits    = 8;
        ppfd->cGreenBits  = 8;
        ppfd->cBlueBits   = 8;
        ppfd->cAlphaBits  = 8;
        ppfd->cDepthBits  = 24;
        ppfd->cStencilBits = 8;
        ppfd->iLayerType  = PFD_MAIN_PLANE;
    }
    return 1; /* 1 pixel format available */
}

BOOL WINAPI SwapBuffers(HDC hdc)
{
    return opengl32_swap_buffers(hdc);
}

/* ── Bitmap creation ─────────────────────────────────────────── */

HBITMAP WINAPI CreateCompatibleBitmap(HDC hdc, int cx, int cy)
{
    serial_puts("[GDI32] CreateCompatibleBitmap hdc=0x");
    serial_puthex((uint64_t)(ULONG_PTR)hdc, 8);
    serial_puts(" size=");
    serial_putdec((uint64_t)(uint32_t)cx);
    serial_puts("x");
    serial_putdec((uint64_t)(uint32_t)cy);
    serial_puts(" slots=");
    serial_putdec((uint64_t)bitmap_slots_used());
    serial_puts("\n");

    if (cx <= 0 || cy <= 0) {
        serial_puts("[GDI32] CreateCompatibleBitmap failed: invalid dimensions\n");
        return NULL;
    }

    int idx = alloc_bmp();
    if (idx < 0) {
        serial_puts("[GDI32] CreateCompatibleBitmap failed: bitmap table full\n");
        return NULL;
    }

    int bpp = 32;
    GDI_DC *dc = dc_from_handle(hdc);
    if (dc) bpp = dc->bpp;

    int pitch = cx * (bpp / 8);
    void *pixels = kmalloc((uint64_t)pitch * cy);
    if (!pixels) {
        gdi_bmps[idx].in_use = 0;
        serial_puts("[GDI32] CreateCompatibleBitmap failed: allocation bytes=0x");
        serial_puthex((uint64_t)(uint32_t)pitch * (uint64_t)(uint32_t)cy, 16);
        serial_puts("\n");
        return NULL;
    }
    gdi_memset(pixels, 0, (SIZE_T)pitch * cy);

    gdi_bmps[idx].pixels = pixels;
    gdi_bmps[idx].width  = cx;
    gdi_bmps[idx].height = cy;
    gdi_bmps[idx].bpp    = bpp;
    gdi_bmps[idx].pitch  = pitch;

    HBITMAP bitmap = (HBITMAP)(ULONG_PTR)(BMP_TAG | (unsigned)idx);
    serial_puts("[GDI32] CreateCompatibleBitmap -> 0x");
    serial_puthex((uint64_t)(ULONG_PTR)bitmap, 8);
    serial_puts("\n");
    return bitmap;
}

static BOOL gdi_native_color_masks(int bpp, DWORD *red, DWORD *green,
                                   DWORD *blue)
{
    if (!red || !green || !blue) return FALSE;

    if (bpp == 16) {
        *red = 0x0000F800u;
        *green = 0x000007E0u;
        *blue = 0x0000001Fu;
        return TRUE;
    }
    if (bpp == 32) {
        *red = 0x00FF0000u;
        *green = 0x0000FF00u;
        *blue = 0x000000FFu;
        return TRUE;
    }
    return FALSE;
}

int WINAPI GetDIBits(HDC hdc, HBITMAP bitmap, UINT start_scan,
                     UINT scan_lines, PVOID bits, PVOID bitmap_info,
                     UINT usage)
{
    (void)hdc;
    (void)usage;
    GDI_BITMAP *bmp = bmp_from_handle(bitmap);
    if (!bmp || !bitmap_info || start_scan >= (UINT)bmp->height)
        return 0;

    BYTE *header = (BYTE *)bitmap_info;
    DWORD header_size = *(DWORD *)(header + 0);
    if (header_size < 40)
        return 0;

    USHORT requested_bpp = *(USHORT *)(header + 14);
    DWORD requested_compression = *(DWORD *)(header + 16);
    BOOL format_query = !bits && requested_bpp == 0;
    USHORT output_bpp = format_query ? (USHORT)bmp->bpp : requested_bpp;
    DWORD output_compression = format_query ? GDI_BI_RGB
                                            : requested_compression;

    if (!output_bpp)
        output_bpp = (USHORT)bmp->bpp;
    if (output_bpp != (USHORT)bmp->bpp)
        return 0; /* Pixel-format conversion is not implemented yet. */
    if (output_compression != GDI_BI_RGB &&
        output_compression != GDI_BI_BITFIELDS)
        return 0;

    DWORD red_mask = 0, green_mask = 0, blue_mask = 0;
    if (output_compression == GDI_BI_BITFIELDS &&
        !gdi_native_color_masks(output_bpp, &red_mask, &green_mask,
                                &blue_mask))
        return 0;

    *(DWORD *)(header + 0) = header_size;        /* biSize */
    *(LONG *)(header + 4) = bmp->width;         /* biWidth */
    *(LONG *)(header + 8) = bmp->height;        /* biHeight */
    *(USHORT *)(header + 12) = 1;               /* biPlanes */
    *(USHORT *)(header + 14) = output_bpp;       /* biBitCount */
    *(DWORD *)(header + 16) = output_compression;
    *(DWORD *)(header + 20) = (DWORD)(bmp->pitch * bmp->height);

    if (output_compression == GDI_BI_BITFIELDS) {
        *(DWORD *)(header + 40) = red_mask;
        *(DWORD *)(header + 44) = green_mask;
        *(DWORD *)(header + 48) = blue_mask;

        static UINT bitfields_trace_count;
        if (bitfields_trace_count++ < 8) {
            serial_puts("[GDI32] GetDIBits BI_BITFIELDS bpp=");
            serial_putdec(output_bpp);
            serial_puts(" masks=0x");
            serial_puthex(red_mask, 8);
            serial_puts("/0x");
            serial_puthex(green_mask, 8);
            serial_puts("/0x");
            serial_puthex(blue_mask, 8);
            serial_puts("\n");
        }
    }

    UINT available = (UINT)bmp->height - start_scan;
    UINT copied = scan_lines < available ? scan_lines : available;
    if (bits && copied) {
        gdi_memcpy(bits, (BYTE *)bmp->pixels + start_scan * bmp->pitch,
                   (SIZE_T)copied * bmp->pitch);
    }
    return (int)copied;
}

HBITMAP WINAPI CreateBitmap(int nWidth, int nHeight, UINT nPlanes,
                             UINT nBitCount, PVOID lpBits)
{
    (void)nPlanes;
    serial_puts("[GDI32] CreateBitmap\n");

    if (nWidth <= 0 || nHeight <= 0) return NULL;

    int idx = alloc_bmp();
    if (idx < 0) return NULL;

    int bpp = (int)nBitCount;
    if (bpp < 8) bpp = 32; /* default to 32bpp for monochrome requests */
    int pitch = nWidth * (bpp / 8);
    void *pixels = kmalloc((uint64_t)pitch * nHeight);
    if (!pixels) {
        gdi_bmps[idx].in_use = 0;
        return NULL;
    }

    if (lpBits) {
        gdi_memcpy(pixels, lpBits, (SIZE_T)pitch * nHeight);
    } else {
        gdi_memset(pixels, 0, (SIZE_T)pitch * nHeight);
    }

    gdi_bmps[idx].pixels = pixels;
    gdi_bmps[idx].width  = nWidth;
    gdi_bmps[idx].height = nHeight;
    gdi_bmps[idx].bpp    = bpp;
    gdi_bmps[idx].pitch  = pitch;

    return (HBITMAP)(ULONG_PTR)(BMP_TAG | (unsigned)idx);
}

HBITMAP gdi32_clone_bitmap(HBITMAP bitmap)
{
    GDI_BITMAP *source = bmp_from_handle(bitmap);
    if (!source || !source->pixels || source->pitch <= 0 ||
        source->height <= 0)
        return NULL;

    uint64_t size = (uint64_t)(uint32_t)source->pitch *
                    (uint64_t)(uint32_t)source->height;
    if (!size || size > (uint64_t)(SIZE_T)-1)
        return NULL;

    int idx = alloc_bmp();
    if (idx < 0) return NULL;
    void *pixels = kmalloc(size);
    if (!pixels) {
        gdi_bmps[idx].in_use = 0;
        return NULL;
    }

    gdi_memcpy(pixels, source->pixels, (SIZE_T)size);
    gdi_bmps[idx].pixels = pixels;
    gdi_bmps[idx].width = source->width;
    gdi_bmps[idx].height = source->height;
    gdi_bmps[idx].bpp = source->bpp;
    gdi_bmps[idx].pitch = source->pitch;
    gdi_bmps[idx].bottomup = source->bottomup;
    return (HBITMAP)(ULONG_PTR)(BMP_TAG | (unsigned)idx);
}

static BOOL gdi_read_bitmap_pixel(const GDI_BITMAP *bitmap, int x, int y,
                                  uint32_t *color)
{
    if (!bitmap || !bitmap->pixels || !color || x < 0 || y < 0 ||
        x >= bitmap->width || y >= bitmap->height)
        return FALSE;

    int stored_y = bitmap->bottomup ? bitmap->height - 1 - y : y;
    const BYTE *row = (const BYTE *)bitmap->pixels +
                      (SIZE_T)stored_y * bitmap->pitch;
    if (bitmap->bpp == 32) {
        *color = ((const uint32_t *)row)[x];
    } else if (bitmap->bpp == 24) {
        const BYTE *pixel = row + (SIZE_T)x * 3;
        *color = 0xFF000000u | ((uint32_t)pixel[2] << 16) |
                 ((uint32_t)pixel[1] << 8) | pixel[0];
    } else if (bitmap->bpp == 16) {
        USHORT value = ((const USHORT *)row)[x];
        DWORD r = (value >> 11) & 0x1F;
        DWORD g = (value >> 5) & 0x3F;
        DWORD b = value & 0x1F;
        *color = 0xFF000000u | ((r * 255 / 31) << 16) |
                 ((g * 255 / 63) << 8) | (b * 255 / 31);
    } else if (bitmap->bpp == 8) {
        BYTE value = row[x];
        *color = 0xFF000000u | ((uint32_t)value << 16) |
                 ((uint32_t)value << 8) | value;
    } else if (bitmap->bpp == 1) {
        BYTE bit = (BYTE)((row[x >> 3] >> (7 - (x & 7))) & 1);
        *color = bit ? 0xFFFFFFFFu : 0xFF000000u;
    } else {
        return FALSE;
    }
    return TRUE;
}

BOOL gdi32_draw_icon_bitmap(HDC hdc, HBITMAP color_handle,
                            HBITMAP mask_handle, int x, int y,
                            int width, int height)
{
    GDI_DC *dc = dc_from_handle(hdc);
    GDI_BITMAP *color = bmp_from_handle(color_handle);
    GDI_BITMAP *mask = bmp_from_handle(mask_handle);
    GDI_BITMAP *source = color ? color : mask;
    if (!dc || !dc->surface || !source)
        return FALSE;

    if (width <= 0) width = source->width;
    if (height <= 0) height = source->height;
    if (width <= 0 || height <= 0)
        return FALSE;

    BOOL color_has_alpha = FALSE;
    if (color && color->bpp == 32) {
        for (int sy = 0; sy < color->height && !color_has_alpha; sy++) {
            for (int sx = 0; sx < color->width; sx++) {
                uint32_t sample;
                if (gdi_read_bitmap_pixel(color, sx, sy, &sample) &&
                    (sample & 0xFF000000u)) {
                    color_has_alpha = TRUE;
                    break;
                }
            }
        }
    }

    BOOL drew = FALSE;
    for (int dy = 0; dy < height; dy++) {
        int sy = (int)((uint64_t)(uint32_t)dy *
                       (uint32_t)source->height / (uint32_t)height);
        for (int dx = 0; dx < width; dx++) {
            int sx = (int)((uint64_t)(uint32_t)dx *
                           (uint32_t)source->width / (uint32_t)width);
            uint32_t source_color;
            if (!gdi_read_bitmap_pixel(source, sx, sy, &source_color))
                continue;

            BYTE alpha = color ? (BYTE)(source_color >> 24) : 0xFF;
            if (color && !color_has_alpha) {
                uint32_t mask_color = 0;
                if (mask && gdi_read_bitmap_pixel(mask, sx, sy, &mask_color) &&
                    (mask_color & 0x00FFFFFFu) == 0x00FFFFFFu)
                    continue;
                alpha = 0xFF;
            } else if (color && alpha == 0) {
                continue;
            }

            uint32_t output = 0xFF000000u | (source_color & 0x00FFFFFFu);
            if (alpha != 0xFF) {
                uint32_t dest;
                if (gdi_read_dc_pixel(dc, x + dx, y + dy, &dest)) {
                    DWORD inv = 255u - alpha;
                    DWORD r = ((((source_color >> 16) & 0xFFu) * alpha) +
                               (((dest >> 16) & 0xFFu) * inv) + 127u) / 255u;
                    DWORD g = ((((source_color >> 8) & 0xFFu) * alpha) +
                               (((dest >> 8) & 0xFFu) * inv) + 127u) / 255u;
                    DWORD b = (((source_color & 0xFFu) * alpha) +
                               ((dest & 0xFFu) * inv) + 127u) / 255u;
                    output = 0xFF000000u | (r << 16) | (g << 8) | b;
                }
            }
            drew |= gdi_write_dc_pixel(dc, x + dx, y + dy, output);
        }
    }
    if (drew) mark_dc_dirty(dc);
    return drew;
}

HBITMAP WINAPI CreateDIBitmap(HDC hdc, PVOID pbmih, DWORD flInit,
                              PVOID pjBits, PVOID pbmi, UINT iUsage)
{
    (void)hdc; (void)pbmih; (void)flInit;
    (void)pjBits; (void)pbmi; (void)iUsage;
    serial_puts("[GDI32] CreateDIBitmap (stub)\n");
    return (HBITMAP)(ULONG_PTR)(BMP_TAG | 0xFE); /* fake — no slot */
}

/* ── Object selection / deletion ─────────────────────────────── */

HGDIOBJ WINAPI SelectObject(HDC hdc, HGDIOBJ h)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc) return h; /* unknown DC — passthrough */

    if (h == STOCK_BITMAP) {
        HGDIOBJ prev = dc->prev_bitmap;
        dc->surface = NULL;
        dc->bottomup = 0;
        dc->prev_bitmap = STOCK_BITMAP;
        return prev;
    }

    /* Selecting a bitmap into the DC */
    if (IS_BMP_HANDLE(h)) {
        GDI_BITMAP *bmp = bmp_from_handle((HBITMAP)h);
        if (bmp) {
            HGDIOBJ prev = dc->prev_bitmap;
            dc->surface     = bmp->pixels;
            dc->width       = bmp->width;
            dc->height      = bmp->height;
            dc->bpp         = bmp->bpp;
            dc->pitch       = bmp->pitch;
            dc->bottomup    = bmp->bottomup;
            dc->prev_bitmap = h;
            return prev ? prev : h;
        }
    }

    if (font_from_handle(h) || IS_STOCK_FONT_HANDLE(h)) {
        HGDIOBJ prev = dc->current_font;
        dc->current_font = h;
        return prev ? prev : STOCK_SYSTEM_FONT;
    }

    /* For brushes and pens, keep the existing passthrough behavior. */
    return h;
}

HGDIOBJ WINAPI GetCurrentObject(HDC hdc, UINT type)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc) return NULL;
    if (type == OBJ_BITMAP)
        return dc->prev_bitmap ? dc->prev_bitmap : STOCK_BITMAP;
    if (type == OBJ_FONT)
        return dc->current_font ? dc->current_font : STOCK_SYSTEM_FONT;
    return NULL;
}

#define GDI_RGN_ERROR         0
#define GDI_NULLREGION        1
#define GDI_SIMPLEREGION      2
#define GDI_COMPLEXREGION     3
#define GDI_RGN_AND           1
#define GDI_RGN_OR            2
#define GDI_RGN_XOR           3
#define GDI_RGN_DIFF          4
#define GDI_RGN_COPY          5

static void region_normalize_rect(GDI_RECT *rect)
{
    if (rect->left > rect->right) {
        LONG value = rect->left;
        rect->left = rect->right;
        rect->right = value;
    }
    if (rect->top > rect->bottom) {
        LONG value = rect->top;
        rect->top = rect->bottom;
        rect->bottom = value;
    }
}

static BOOL region_rect_empty(const GDI_RECT *rect)
{
    return rect->left >= rect->right || rect->top >= rect->bottom;
}

static BOOL region_rect_contains(const GDI_RECT *outer,
                                 const GDI_RECT *inner)
{
    return outer->left <= inner->left && outer->top <= inner->top &&
           outer->right >= inner->right && outer->bottom >= inner->bottom;
}

static BOOL region_intersect_rects(const GDI_RECT *first,
                                   const GDI_RECT *second,
                                   GDI_RECT *result)
{
    result->left = first->left > second->left ? first->left : second->left;
    result->top = first->top > second->top ? first->top : second->top;
    result->right = first->right < second->right
        ? first->right : second->right;
    result->bottom = first->bottom < second->bottom
        ? first->bottom : second->bottom;
    return !region_rect_empty(result);
}

static void region_remove_rect(GDI_REGION *region, UINT index)
{
    if (index >= region->count) return;
    for (UINT i = index + 1; i < region->count; i++)
        region->rects[i - 1] = region->rects[i];
    region->count--;
}

static BOOL region_append_rect(GDI_REGION *region, GDI_RECT rect)
{
    region_normalize_rect(&rect);
    if (region_rect_empty(&rect)) return TRUE;

    for (UINT i = 0; i < region->count; ) {
        GDI_RECT *current = &region->rects[i];
        if (region_rect_contains(current, &rect)) return TRUE;
        if (region_rect_contains(&rect, current)) {
            region_remove_rect(region, i);
            continue;
        }

        if (current->top == rect.top && current->bottom == rect.bottom &&
            current->left <= rect.right && rect.left <= current->right) {
            if (current->left < rect.left) rect.left = current->left;
            if (current->right > rect.right) rect.right = current->right;
            region_remove_rect(region, i);
            continue;
        }
        if (current->left == rect.left && current->right == rect.right &&
            current->top <= rect.bottom && rect.top <= current->bottom) {
            if (current->top < rect.top) rect.top = current->top;
            if (current->bottom > rect.bottom) rect.bottom = current->bottom;
            region_remove_rect(region, i);
            continue;
        }
        i++;
    }

    if (region->count >= MAX_REGION_RECTS) return FALSE;
    region->rects[region->count++] = rect;
    return TRUE;
}

static BOOL region_subtract_rect(const GDI_RECT *source,
                                 const GDI_RECT *cut,
                                 GDI_REGION *result)
{
    GDI_RECT overlap;
    if (!region_intersect_rects(source, cut, &overlap))
        return region_append_rect(result, *source);

    GDI_RECT piece = {
        source->left, source->top, source->right, overlap.top
    };
    if (!region_append_rect(result, piece)) return FALSE;
    piece = (GDI_RECT){
        source->left, overlap.bottom, source->right, source->bottom
    };
    if (!region_append_rect(result, piece)) return FALSE;
    piece = (GDI_RECT){
        source->left, overlap.top, overlap.left, overlap.bottom
    };
    if (!region_append_rect(result, piece)) return FALSE;
    piece = (GDI_RECT){
        overlap.right, overlap.top, source->right, overlap.bottom
    };
    return region_append_rect(result, piece);
}

static BOOL region_difference(const GDI_REGION *first,
                              const GDI_REGION *second,
                              GDI_REGION *result)
{
    result->count = 0;
    for (UINT i = 0; i < first->count; i++) {
        GDI_REGION pieces;
        gdi_memset(&pieces, 0, sizeof(pieces));
        if (!region_append_rect(&pieces, first->rects[i])) return FALSE;

        for (UINT j = 0; j < second->count && pieces.count; j++) {
            GDI_REGION next;
            gdi_memset(&next, 0, sizeof(next));
            for (UINT k = 0; k < pieces.count; k++) {
                if (!region_subtract_rect(&pieces.rects[k],
                                          &second->rects[j], &next))
                    return FALSE;
            }
            pieces = next;
        }
        for (UINT j = 0; j < pieces.count; j++) {
            if (!region_append_rect(result, pieces.rects[j])) return FALSE;
        }
    }
    return TRUE;
}

static BOOL region_intersection(const GDI_REGION *first,
                                const GDI_REGION *second,
                                GDI_REGION *result)
{
    result->count = 0;
    for (UINT i = 0; i < first->count; i++) {
        for (UINT j = 0; j < second->count; j++) {
            GDI_RECT overlap;
            if (region_intersect_rects(&first->rects[i], &second->rects[j],
                                       &overlap) &&
                !region_append_rect(result, overlap))
                return FALSE;
        }
    }
    return TRUE;
}

static BOOL region_union(const GDI_REGION *first,
                         const GDI_REGION *second,
                         GDI_REGION *result)
{
    result->count = 0;
    for (UINT i = 0; i < first->count; i++) {
        if (!region_append_rect(result, first->rects[i])) return FALSE;
    }

    GDI_REGION unique;
    if (!region_difference(second, first, &unique)) return FALSE;
    for (UINT i = 0; i < unique.count; i++) {
        if (!region_append_rect(result, unique.rects[i])) return FALSE;
    }
    return TRUE;
}

static BOOL region_combine(const GDI_REGION *first,
                           const GDI_REGION *second,
                           int mode, GDI_REGION *result)
{
    gdi_memset(result, 0, sizeof(*result));
    result->in_use = 1;
    switch (mode) {
    case GDI_RGN_AND:
        return region_intersection(first, second, result);
    case GDI_RGN_OR:
        return region_union(first, second, result);
    case GDI_RGN_XOR: {
        GDI_REGION left;
        GDI_REGION right;
        if (!region_difference(first, second, &left) ||
            !region_difference(second, first, &right))
            return FALSE;
        return region_union(&left, &right, result);
    }
    case GDI_RGN_DIFF:
        return region_difference(first, second, result);
    case GDI_RGN_COPY:
        for (UINT i = 0; i < first->count; i++) {
            if (!region_append_rect(result, first->rects[i])) return FALSE;
        }
        return TRUE;
    default:
        return FALSE;
    }
}

static int region_type(const GDI_REGION *region)
{
    if (!region->count) return GDI_NULLREGION;
    return region->count == 1 ? GDI_SIMPLEREGION : GDI_COMPLEXREGION;
}

HGDIOBJ WINAPI CreateRectRgn(int left, int top, int right, int bottom)
{
    regions_lock();
    int index = alloc_region();
    if (index < 0) {
        regions_unlock();
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return NULL;
    }
    GDI_RECT rect = { left, top, right, bottom };
    if (!region_append_rect(&gdi_regions[index], rect)) {
        gdi_regions[index].in_use = 0;
        regions_unlock();
        SetLastError(8);
        return NULL;
    }
    HGDIOBJ handle = (HGDIOBJ)(ULONG_PTR)(REGION_TAG | (unsigned)index);
    regions_unlock();
    return handle;
}

HGDIOBJ WINAPI CreateRectRgnIndirect(const GDI_RECT *rect)
{
    if (!rect) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return NULL;
    }
    return CreateRectRgn(rect->left, rect->top, rect->right, rect->bottom);
}

BOOL WINAPI SetRectRgn(HGDIOBJ rgn, int left, int top, int right, int bottom)
{
    regions_lock();
    GDI_REGION *region = region_from_handle(rgn);
    if (!region) {
        regions_unlock();
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    region->count = 0;
    GDI_RECT rect = { left, top, right, bottom };
    BOOL result = region_append_rect(region, rect);
    regions_unlock();
    return result;
}

int WINAPI CombineRgn(HGDIOBJ dest_handle, HGDIOBJ first_handle,
                      HGDIOBJ second_handle, int mode)
{
    regions_lock();
    GDI_REGION *dest = region_from_handle(dest_handle);
    GDI_REGION *first = region_from_handle(first_handle);
    GDI_REGION *second = mode == GDI_RGN_COPY
        ? first : region_from_handle(second_handle);
    if (!dest || !first || !second || mode < GDI_RGN_AND ||
        mode > GDI_RGN_COPY) {
        regions_unlock();
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return GDI_RGN_ERROR;
    }

    GDI_REGION first_copy = *first;
    GDI_REGION second_copy = *second;
    GDI_REGION result;
    if (!region_combine(&first_copy, &second_copy, mode, &result)) {
        regions_unlock();
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return GDI_RGN_ERROR;
    }
    *dest = result;
    int type = region_type(dest);
    regions_unlock();
    return type;
}

BOOL WINAPI EqualRgn(HGDIOBJ first_handle, HGDIOBJ second_handle)
{
    regions_lock();
    GDI_REGION *first = region_from_handle(first_handle);
    GDI_REGION *second = region_from_handle(second_handle);
    GDI_REGION difference;
    if (!first || !second ||
        !region_combine(first, second, GDI_RGN_XOR, &difference)) {
        regions_unlock();
        return FALSE;
    }
    BOOL equal = difference.count == 0;
    regions_unlock();
    return equal;
}

BOOL WINAPI PtInRegion(HGDIOBJ rgn, int x, int y)
{
    regions_lock();
    GDI_REGION *region = region_from_handle(rgn);
    if (!region) {
        regions_unlock();
        return FALSE;
    }
    for (UINT i = 0; i < region->count; i++) {
        GDI_RECT *rect = &region->rects[i];
        if (x >= rect->left && x < rect->right &&
            y >= rect->top && y < rect->bottom) {
            regions_unlock();
            return TRUE;
        }
    }
    regions_unlock();
    return FALSE;
}

BOOL WINAPI RectInRegion(HGDIOBJ rgn, const GDI_RECT *input)
{
    if (!input) return FALSE;
    regions_lock();
    GDI_REGION *region = region_from_handle(rgn);
    if (!region) {
        regions_unlock();
        return FALSE;
    }
    GDI_RECT rect = *input;
    region_normalize_rect(&rect);
    for (UINT i = 0; i < region->count; i++) {
        GDI_RECT overlap;
        if (region_intersect_rects(&region->rects[i], &rect, &overlap)) {
            regions_unlock();
            return TRUE;
        }
    }
    regions_unlock();
    return FALSE;
}

int WINAPI GetRgnBox(HGDIOBJ rgn, GDI_RECT *rect)
{
    if (!rect) return GDI_RGN_ERROR;
    regions_lock();
    GDI_REGION *region = region_from_handle(rgn);
    if (!region) {
        regions_unlock();
        return GDI_RGN_ERROR;
    }
    if (!region->count) {
        *rect = (GDI_RECT){ 0, 0, 0, 0 };
        regions_unlock();
        return GDI_NULLREGION;
    }

    *rect = region->rects[0];
    for (UINT i = 1; i < region->count; i++) {
        GDI_RECT *part = &region->rects[i];
        if (part->left < rect->left) rect->left = part->left;
        if (part->top < rect->top) rect->top = part->top;
        if (part->right > rect->right) rect->right = part->right;
        if (part->bottom > rect->bottom) rect->bottom = part->bottom;
    }
    int type = region_type(region);
    regions_unlock();
    return type;
}

int WINAPI OffsetRgn(HGDIOBJ rgn, int x, int y)
{
    regions_lock();
    GDI_REGION *region = region_from_handle(rgn);
    if (!region) {
        regions_unlock();
        return GDI_RGN_ERROR;
    }
    for (UINT i = 0; i < region->count; i++) {
        region->rects[i].left += x;
        region->rects[i].right += x;
        region->rects[i].top += y;
        region->rects[i].bottom += y;
    }
    int type = region_type(region);
    regions_unlock();
    return type;
}

int WINAPI SelectClipRgn(HDC hdc, HGDIOBJ rgn)
{
    if (!dc_from_handle(hdc)) return GDI_RGN_ERROR;
    if (!rgn) return GDI_NULLREGION;
    regions_lock();
    GDI_REGION *region = region_from_handle(rgn);
    int type = region ? region_type(region) : GDI_RGN_ERROR;
    regions_unlock();
    return type;
}

int gdi32_region_selftest(void)
{
    int checks = 0;
    int failures = 0;
#define REGION_CHECK(condition) do { \
    checks++; \
    if (!(condition)) failures++; \
} while (0)

    HGDIOBJ first = CreateRectRgn(0, 0, 10, 10);
    HGDIOBJ second = CreateRectRgn(5, 5, 15, 15);
    HGDIOBJ result = CreateRectRgn(0, 0, 0, 0);
    REGION_CHECK(first != NULL && second != NULL && result != NULL);
    REGION_CHECK(PtInRegion(first, 0, 0));
    REGION_CHECK(PtInRegion(first, 9, 9));
    REGION_CHECK(!PtInRegion(first, 10, 9));
    REGION_CHECK(!PtInRegion(first, 9, 10));

    GDI_RECT probe = { 9, 9, 12, 12 };
    REGION_CHECK(RectInRegion(first, &probe));
    probe = (GDI_RECT){ 10, 0, 12, 2 };
    REGION_CHECK(!RectInRegion(first, &probe));

    REGION_CHECK(CombineRgn(result, first, second, GDI_RGN_AND) ==
                 GDI_SIMPLEREGION);
    REGION_CHECK(PtInRegion(result, 5, 5));
    REGION_CHECK(!PtInRegion(result, 4, 5));
    REGION_CHECK(!PtInRegion(result, 10, 5));

    REGION_CHECK(CombineRgn(result, first, second, GDI_RGN_OR) ==
                 GDI_COMPLEXREGION);
    REGION_CHECK(PtInRegion(result, 1, 1));
    REGION_CHECK(PtInRegion(result, 14, 14));
    REGION_CHECK(!PtInRegion(result, 14, 1));
    GDI_RECT bounds;
    REGION_CHECK(GetRgnBox(result, &bounds) == GDI_COMPLEXREGION);
    REGION_CHECK(bounds.left == 0 && bounds.top == 0 &&
                 bounds.right == 15 && bounds.bottom == 15);

    REGION_CHECK(CombineRgn(result, first, second, GDI_RGN_DIFF) ==
                 GDI_COMPLEXREGION);
    REGION_CHECK(PtInRegion(result, 1, 1));
    REGION_CHECK(PtInRegion(result, 9, 1));
    REGION_CHECK(!PtInRegion(result, 5, 5));

    REGION_CHECK(CombineRgn(result, first, second, GDI_RGN_XOR) ==
                 GDI_COMPLEXREGION);
    REGION_CHECK(PtInRegion(result, 1, 1));
    REGION_CHECK(PtInRegion(result, 14, 14));
    REGION_CHECK(!PtInRegion(result, 7, 7));

    REGION_CHECK(CombineRgn(result, first, NULL, GDI_RGN_COPY) ==
                 GDI_SIMPLEREGION);
    REGION_CHECK(EqualRgn(result, first));
    REGION_CHECK(OffsetRgn(result, 2, -3) == GDI_SIMPLEREGION);
    REGION_CHECK(PtInRegion(result, 2, -3));
    REGION_CHECK(!PtInRegion(result, 1, -3));
    REGION_CHECK(CombineRgn(result, result, second, GDI_RGN_AND) ==
                 GDI_SIMPLEREGION);
    REGION_CHECK(PtInRegion(result, 5, 5));

    HGDIOBJ empty = CreateRectRgn(4, 4, 4, 9);
    REGION_CHECK(empty != NULL);
    REGION_CHECK(GetRgnBox(empty, &bounds) == GDI_NULLREGION);
    REGION_CHECK(bounds.left == 0 && bounds.top == 0 &&
                 bounds.right == 0 && bounds.bottom == 0);

    REGION_CHECK(DeleteObject(first));
    REGION_CHECK(!PtInRegion(first, 1, 1));
    REGION_CHECK(DeleteObject(second));
    REGION_CHECK(DeleteObject(result));
    REGION_CHECK(DeleteObject(empty));

    serial_puts("[GDI-REGION-TEST] checks=");
    serial_putdec((uint64_t)checks);
    serial_puts(" failures=");
    serial_putdec((uint64_t)failures);
    serial_puts("\n");
#undef REGION_CHECK
    return failures;
}

UINT WINAPI SetTextAlign(HDC hdc, UINT align)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc) return 0xFFFFFFFFu;
    UINT prev = dc->text_align;
    dc->text_align = align;
    return prev;
}

BOOL WINAPI DeleteObject(HGDIOBJ ho)
{
    if (IS_REGION_HANDLE(ho)) {
        regions_lock();
        GDI_REGION *region = region_from_handle(ho);
        if (!region) {
            regions_unlock();
            SetLastError(6); /* ERROR_INVALID_HANDLE */
            return FALSE;
        }
        gdi_memset(region, 0, sizeof(*region));
        regions_unlock();
        return TRUE;
    }
    /* If it's a bitmap handle, free the pixels with the matching allocator. */
    if (IS_BMP_HANDLE(ho)) {
        GDI_BITMAP *bmp = bmp_from_handle((HBITMAP)ho);
        if (bmp) {
            if (bmp->pixels) {
                if (bmp->mapping_base)
                    UnmapViewOfFile(bmp->mapping_base);
                else if (bmp->virtual_alloc)
                    VirtualFree(bmp->pixels, 0, MEM_RELEASE);
                else
                    kfree(bmp->pixels);
                bmp->pixels = NULL;
            }
            bmp->mapping_base = NULL;
            bmp->in_use = 0;
        }
        return TRUE;
    }
    if (IS_FONT_HANDLE(ho)) {
        GDI_FONT *font = font_from_handle(ho);
        if (!font) return FALSE;
        font->in_use = 0;
        return TRUE;
    }
    /* Other objects (pens and brushes) — just succeed */
    return TRUE;
}

typedef struct {
    LONG bmType;
    LONG bmWidth;
    LONG bmHeight;
    LONG bmWidthBytes;
    USHORT bmPlanes;
    USHORT bmBitsPixel;
    uint32_t bmBits;
} GDI_BITMAP_INFO32;

typedef struct {
    LONG bmType;
    LONG bmWidth;
    LONG bmHeight;
    LONG bmWidthBytes;
    USHORT bmPlanes;
    USHORT bmBitsPixel;
    PVOID bmBits;
} GDI_BITMAP_INFO64;

_Static_assert(sizeof(GDI_BITMAP_INFO32) == 24,
               "32-bit BITMAP layout changed");
_Static_assert(sizeof(GDI_BITMAP_INFO64) == 32,
               "64-bit BITMAP layout changed");

static int gdi_get_object(HGDIOBJ h, int c, PVOID pv, BOOL wide)
{
    GDI_FONT *font = font_from_handle(h);
    GDI_LOGFONTW stock_font;
    const GDI_LOGFONTW *logfont = font ? &font->logfont : NULL;
    GDI_BITMAP *bmp = bmp_from_handle((HBITMAP)h);
    int width, height, pitch, bpp, virtual_alloc;
    int required = g_compat32_mode ? (int)sizeof(GDI_BITMAP_INFO32)
                                   : (int)sizeof(GDI_BITMAP_INFO64);

    if (!logfont && IS_STOCK_FONT_HANDLE(h)) {
        gdi_stock_font_logfont(&stock_font);
        logfont = &stock_font;
    }

    if (logfont) {
        required = wide ? (int)sizeof(GDI_LOGFONTW)
                        : (int)sizeof(GDI_LOGFONTA);
        if (!pv) return required;
        if (c < required) return 0;

        if (wide) {
            gdi_memcpy(pv, logfont, sizeof(*logfont));
        } else {
            GDI_LOGFONTA *out = (GDI_LOGFONTA *)pv;
            gdi_memset(out, 0, sizeof(*out));
            gdi_memcpy(out, logfont, 28);
            for (int i = 0; i < 31; i++) {
                WCHAR ch = logfont->lfFaceName[i];
                out->lfFaceName[i] = ch < 0x80 ? (char)ch : '?';
                if (!ch) break;
            }
        }
        return required;
    }

    if (bmp) {
        width = bmp->width;
        height = bmp->height;
        pitch = bmp->pitch;
        bpp = bmp->bpp;
        virtual_alloc = bmp->virtual_alloc;
    } else if (h == STOCK_BITMAP) {
        width = 1;
        height = 1;
        pitch = 2;
        bpp = 1;
        virtual_alloc = 0;
    } else {
        if (h) {
            serial_puts("[GDI32] GetObject invalid handle=0x");
            serial_puthex((uint64_t)(ULONG_PTR)h, 8);
            serial_puts("\n");
        }
        return 0;
    }

    serial_puts("[GDI32] GetObject bitmap=0x");
    serial_puthex((uint64_t)(ULONG_PTR)h, 8);
    serial_puts(" bytes=");
    serial_putdec((uint64_t)(uint32_t)c);
    serial_puts(" need=");
    serial_putdec((uint64_t)(uint32_t)required);
    serial_puts("\n");

    if (!pv) return required;
    if (c < required) return 0;

    if (g_compat32_mode) {
        GDI_BITMAP_INFO32 *out = (GDI_BITMAP_INFO32 *)pv;
        gdi_memset(out, 0, sizeof(*out));
        out->bmWidth = width;
        out->bmHeight = height;
        out->bmWidthBytes = pitch;
        out->bmPlanes = 1;
        out->bmBitsPixel = (USHORT)bpp;
        out->bmBits = (bmp && virtual_alloc)
            ? (uint32_t)(ULONG_PTR)bmp->pixels : 0;
    } else {
        GDI_BITMAP_INFO64 *out = (GDI_BITMAP_INFO64 *)pv;
        gdi_memset(out, 0, sizeof(*out));
        out->bmWidth = width;
        out->bmHeight = height;
        out->bmWidthBytes = pitch;
        out->bmPlanes = 1;
        out->bmBitsPixel = (USHORT)bpp;
        out->bmBits = (bmp && virtual_alloc) ? bmp->pixels : NULL;
    }
    return required;
}

int WINAPI GetObjectA(HGDIOBJ h, int c, PVOID pv)
{
    return gdi_get_object(h, c, pv, FALSE);
}

static int WINAPI GetObjectW_k32(HGDIOBJ h, int c, PVOID pv)
{
    return gdi_get_object(h, c, pv, TRUE);
}

static DWORD WINAPI GetObjectType_k32(HGDIOBJ h)
{
    if (bmp_from_handle((HBITMAP)h) || h == STOCK_BITMAP) return OBJ_BITMAP;
    if (font_from_handle(h)) return 6; /* OBJ_FONT */
    if (dc_from_handle((HDC)h)) return 3; /* OBJ_DC */
    if (((ULONG_PTR)h & TAG_MASK) == 0xBE000000u) return 2; /* OBJ_BRUSH */
    if (((ULONG_PTR)h & TAG_MASK) == 0xEE000000u) return 1; /* OBJ_PEN */
    if (((ULONG_PTR)h & TAG_MASK) == 0xAA000000u) return 6; /* OBJ_FONT */
    if (IS_REGION_HANDLE(h)) {
        regions_lock();
        BOOL valid = region_from_handle(h) != NULL;
        regions_unlock();
        if (valid) return 8; /* OBJ_REGION */
    }
    return 0;
}

/* ── GDI object creation (brushes, pens, stock objects) ──────── */

HBRUSH_GDI WINAPI CreateSolidBrush(DWORD color)
{
    (void)color;
    return (HBRUSH_GDI)(ULONG_PTR)0xBE000001;
}

HBRUSH_GDI WINAPI CreatePatternBrush(HBITMAP hBitmap)
{
    (void)hBitmap;
    return (HBRUSH_GDI)(ULONG_PTR)0xBE000002;
}

HPEN WINAPI CreatePen(int iStyle, int cWidth, DWORD color)
{
    (void)iStyle; (void)cWidth; (void)color;
    return (HPEN)(ULONG_PTR)0xEE000001;
}

HGDIOBJ WINAPI GetStockObject(int i)
{
    return (HGDIOBJ)(ULONG_PTR)(0xAA000000 + i);
}

/* ── BitBlt / PatBlt ─────────────────────────────────────────── */

/* ROP codes */
#define ROP_SRCCOPY    0x00CC0020
#define ROP_BLACKNESS  0x00000042
#define ROP_WHITENESS  0x00FF0062
#define ROP_PATCOPY    0x00F00021

BOOL WINAPI BitBlt(HDC hdcDest, int x, int y, int cx, int cy,
                   HDC hdcSrc, int x1, int y1, DWORD rop)
{
    GDI_DC *dst = dc_from_handle(hdcDest);
    GDI_DC *trace_src = dc_from_handle(hdcSrc);
    if (gdi_blit_trace_count++ < 64) {
        serial_puts("[GDI-BLIT] dst=0x");
        serial_puthex((uint64_t)(ULONG_PTR)hdcDest, 8);
        serial_puts(dst && dst->surface ? " ready" : " invalid");
        serial_puts(" src=0x");
        serial_puthex((uint64_t)(ULONG_PTR)hdcSrc, 8);
        serial_puts(trace_src && trace_src->surface ? " ready" : " invalid");
        serial_puts(" size=");
        serial_putdec((uint64_t)(uint32_t)cx);
        serial_puts("x");
        serial_putdec((uint64_t)(uint32_t)cy);
        serial_puts(" rop=0x");
        serial_puthex(rop, 8);
        serial_puts("\n");
    }
    if (!dst || !dst->surface) return FALSE;

    /* Handle fill-only raster ops (no source needed) */
    if (rop == ROP_BLACKNESS || rop == ROP_WHITENESS || rop == ROP_PATCOPY) {
        uint32_t fill;
        if (rop == ROP_BLACKNESS)     fill = 0x00000000;
        else if (rop == ROP_WHITENESS) fill = 0xFFFFFFFF;
        else                           fill = 0x00000000; /* PATCOPY: black brush */

        /* Clip destination rectangle */
        int dx = x, dy = y;
        int dw = cx, dh = cy;
        if (dx < 0) { dw += dx; dx = 0; }
        if (dy < 0) { dh += dy; dy = 0; }
        if (dx + dw > dst->width)  dw = dst->width - dx;
        if (dy + dh > dst->height) dh = dst->height - dy;
        if (dw <= 0 || dh <= 0) return TRUE;

        BYTE *dst_base = (BYTE *)dst->surface;
        int dst_pitch = dst->pitch;

        for (int row = 0; row < dh; row++) {
            uint32_t *dp = (uint32_t *)(dst_base + (dy + row) * dst_pitch) + dx;
            for (int col = 0; col < dw; col++)
                dp[col] = fill;
        }
        mark_dc_dirty(dst);
        return TRUE;
    }

    /* SRCCOPY: requires valid source */
    if (rop == ROP_SRCCOPY) {
        GDI_DC *src = dc_from_handle(hdcSrc);
        if (!src || !src->surface) return FALSE;

        /* Blit to the window/screen DC = PRESENT. This is UT99 SoftDrv's
         * windowed frame present (render into a DIB section, BitBlt the memory
         * DC to the window DC). Route it through the shared scaled present
         * (bpp-convert + nearest-neighbor fill of the GOP + shadow flush) —
         * the same pipeline DirectDraw Flip/Blt uses — instead of a raw
         * unscaled, unflushed memcpy. Bottom-up DIBs present inverted via a
         * negative pitch starting at the last memory row. */
        if (dst->is_screen) {
            extern void ddraw_present_pixels(const void *pixels, uint32_t sw,
                                             uint32_t sh, uint32_t bpp,
                                             int32_t pitch_bytes,
                                             const uint32_t *pal256);
            extern void ddraw_suspend_present_hook(void);
            ddraw_suspend_present_hook();
            const uint8_t *px = (const uint8_t *)src->surface;
            int32_t p = src->pitch;
            if (src->bottomup && src->height > 1) {
                px += (size_t)(src->height - 1) * (size_t)src->pitch;
                p = -p;
            }
            ddraw_present_pixels(px, (uint32_t)src->width, (uint32_t)src->height,
                                 (uint32_t)src->bpp, p, NULL);
            return TRUE;
        }

        /* Clip source coordinates */
        int sx = x1, sy = y1;
        int dx = x, dy = y;
        int w = cx, h = cy;

        /* Clip to source bounds */
        if (sx < 0) { w += sx; dx -= sx; sx = 0; }
        if (sy < 0) { h += sy; dy -= sy; sy = 0; }
        if (sx + w > src->width)  w = src->width - sx;
        if (sy + h > src->height) h = src->height - sy;

        /* Clip to dest bounds */
        if (dx < 0) { w += dx; sx -= dx; dx = 0; }
        if (dy < 0) { h += dy; sy -= dy; dy = 0; }
        if (dx + w > dst->width)  w = dst->width - dx;
        if (dy + h > dst->height) h = dst->height - dy;

        if (w <= 0 || h <= 0) return TRUE;

        int src_bytes = src->bpp / 8;
        int dst_bytes = dst->bpp / 8;
        BOOL identical_format = src->bpp == dst->bpp && src_bytes > 0 &&
                                src_bytes <= 4;

        if (identical_format) {
            BYTE *src_base = (BYTE *)src->surface;
            BYTE *dst_base = (BYTE *)dst->surface;
            int row_bytes = w * src_bytes;
            int first = 0;
            int last = h;
            int step = 1;
            if (src->surface == dst->surface && dy > sy) {
                first = h - 1;
                last = -1;
                step = -1;
            }
            for (int row = first; row != last; row += step) {
                int src_y = src->bottomup ? src->height - 1 - (sy + row)
                                          : sy + row;
                int dst_y = dst->bottomup ? dst->height - 1 - (dy + row)
                                          : dy + row;
                BYTE *sp = src_base + src_y * src->pitch + sx * src_bytes;
                BYTE *dp = dst_base + dst_y * dst->pitch + dx * dst_bytes;
                gdi_memmove(dp, sp, (SIZE_T)row_bytes);
            }
        } else {
            for (int row = 0; row < h; row++) {
                for (int col = 0; col < w; col++) {
                    uint32_t color;
                    if (!gdi_read_dc_pixel(src, sx + col, sy + row, &color) ||
                        !gdi_write_dc_pixel(dst, dx + col, dy + row, color))
                        return FALSE;
                }
            }
        }
        mark_dc_dirty(dst);
        return TRUE;
    }

    /* Unknown ROP — succeed silently */
    return TRUE;
}

BOOL WINAPI PatBlt(HDC hdc, int x, int y, int w, int h, DWORD rop)
{
    /* PatBlt is BitBlt with no source */
    return BitBlt(hdc, x, y, w, h, NULL, 0, 0, rop);
}

/* ── Line drawing (stubs) ────────────────────────────────────── */

BOOL WINAPI MoveToEx(HDC hdc, int x, int y, PVOID lppt)
{
    (void)hdc; (void)x; (void)y; (void)lppt;
    return TRUE;
}

BOOL WINAPI LineTo(HDC hdc, int x, int y)
{
    (void)hdc; (void)x; (void)y;
    return TRUE;
}

/* ── Text ────────────────────────────────────────────────────── */

DWORD WINAPI SetTextColor(HDC hdc, DWORD color)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (dc) {
        DWORD prev = dc->text_color;
        dc->text_color = color;
        return prev;
    }
    return 0;
}

DWORD WINAPI SetBkColor(HDC hdc, DWORD color)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (dc) {
        DWORD prev = dc->bk_color;
        dc->bk_color = color;
        return prev;
    }
    return 0;
}

int WINAPI SetBkMode(HDC hdc, int mode)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (dc) {
        int prev = dc->bk_mode;
        dc->bk_mode = mode;
        return prev;
    }
    return 0;
}

static uint32_t gdi_colorref_to_argb(DWORD color)
{
    return 0xFF000000u | ((color & 0x000000FFu) << 16) |
           (color & 0x0000FF00u) | ((color & 0x00FF0000u) >> 16);
}

static BOOL gdi_text_out(HDC hdc, int x, int y, PCVOID text, int count,
                         BOOL wide)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc || count < 0 || (count && !text)) {
        SetLastError(!dc ? 6 : 87); /* ERROR_INVALID_HANDLE/PARAMETER */
        return FALSE;
    }
    if (!count) return TRUE;

    uint32_t foreground = gdi_colorref_to_argb(dc->text_color);
    uint32_t background = gdi_colorref_to_argb(dc->bk_color);
    BOOL wrote = FALSE;
    x += dc->viewport_x;
    y += dc->viewport_y;

    for (int index = 0; index < count; index++) {
        UINT codepoint = wide ? ((const WCHAR *)text)[index]
                              : ((const BYTE *)text)[index];
        if (codepoint < 32 || codepoint > 126) codepoint = '?';
        const uint8_t *glyph = gui_font8x16[codepoint - 32];
        int glyph_x = x + index * 8;
        for (int row = 0; row < 16; row++) {
            for (int column = 0; column < 8; column++) {
                BOOL set = (glyph[row] & (0x80u >> column)) != 0;
                if (set || dc->bk_mode == 2) {
                    wrote |= gdi_write_dc_pixel(
                        dc, glyph_x + column, y + row,
                        set ? foreground : background);
                }
            }
        }
    }
    if (wrote) mark_dc_dirty(dc);
    SetLastError(0);
    return TRUE;
}

BOOL WINAPI TextOutW(HDC hdc, int x, int y, PCWSTR lpString, int c)
{
    return gdi_text_out(hdc, x, y, lpString, c, TRUE);
}

static BOOL WINAPI TextOutA_k32(HDC hdc, int x, int y, PCSTR text, int count)
{
    return gdi_text_out(hdc, x, y, text, count, FALSE);
}

BOOL WINAPI ExtTextOutA(HDC hdc, int x, int y, UINT options,
                        PVOID lprect, PCSTR lpString, UINT c, PVOID lpDx)
{
    (void)hdc; (void)x; (void)y; (void)options;
    (void)lprect; (void)lpString; (void)c; (void)lpDx;
    return TRUE;
}

static BOOL WINAPI ExtTextOutW_stub(HDC hdc, int x, int y, UINT options,
                                     PVOID lprect, PCWSTR text, UINT count,
                                     PVOID spacing)
{
    (void)options; (void)lprect; (void)spacing;
    return TextOutW(hdc, x, y, text, (int)count);
}

static int WINAPI SetMapMode_stub(HDC hdc, int mode)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc || mode < 1 || mode > 8) return 0;
    int previous = dc->map_mode;
    dc->map_mode = mode;
    return previous;
}

typedef struct {
    LONG x;
    LONG y;
} GDI_POINT;

static BOOL WINAPI SetViewportOrgEx_k32(HDC hdc, int x, int y,
                                         GDI_POINT *previous)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    if (previous) {
        previous->x = dc->viewport_x;
        previous->y = dc->viewport_y;
    }
    dc->viewport_x = x;
    dc->viewport_y = y;
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI SetBrushOrgEx_k32(HDC hdc, int x, int y,
                                      GDI_POINT *previous)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    if (previous) {
        previous->x = dc->brush_x;
        previous->y = dc->brush_y;
    }
    dc->brush_x = x;
    dc->brush_y = y;
    SetLastError(0);
    return TRUE;
}

static int WINAPI SetStretchBltMode_k32(HDC hdc, int mode)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc || mode < 1 || mode > 4) return 0;
    int previous = dc->stretch_mode;
    dc->stretch_mode = mode;
    return previous;
}

static DWORD WINAPI SetDCBrushColor_k32(HDC hdc, DWORD color)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc) return 0xFFFFFFFFu; /* CLR_INVALID */
    DWORD previous = dc->brush_color;
    dc->brush_color = color;
    return previous;
}

static BOOL WINAPI GdiFlush_k32(void)
{
    return TRUE;
}

static BOOL WINAPI Polyline_k32(HDC hdc, const GDI_POINT *points, int count)
{
    return dc_from_handle(hdc) && points && count >= 2;
}

typedef struct {
    const BYTE *bits;
    const BYTE *colors;
    int width;
    int height;
    int top_down;
    int bpp;
    int pitch;
    int core_header;
    DWORD compression;
    DWORD color_count;
    DWORD red_mask;
    DWORD green_mask;
    DWORD blue_mask;
} GDI_DIB_VIEW;

static UINT gdi_dib_trace_count;
static UINT gdi_frame_probe_count;

static USHORT gdi_read_u16(const BYTE *p)
{
    return (USHORT)((USHORT)p[0] | ((USHORT)p[1] << 8));
}

static DWORD gdi_read_u32(const BYTE *p)
{
    return (DWORD)p[0] | ((DWORD)p[1] << 8) |
           ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24);
}

static BOOL gdi_abs_i32(int value, int *absolute)
{
    if (!absolute || value == (-2147483647 - 1)) return FALSE;
    *absolute = value < 0 ? -value : value;
    return TRUE;
}

static BOOL gdi_parse_dib(PCVOID bitmap_info, PCVOID bits, UINT usage,
                          GDI_DIB_VIEW *view)
{
    if (!bitmap_info || !bits || !view || usage != GDI_DIB_RGB_COLORS)
        return FALSE;

    const BYTE *info = (const BYTE *)bitmap_info;
    DWORD header_size = gdi_read_u32(info);
    gdi_memset(view, 0, sizeof(*view));
    view->bits = (const BYTE *)bits;

    int raw_height;
    if (header_size == 12) {
        view->core_header = 1;
        view->width = (int)gdi_read_u16(info + 4);
        raw_height = (int)gdi_read_u16(info + 6);
        if (gdi_read_u16(info + 8) != 1) return FALSE;
        view->bpp = (int)gdi_read_u16(info + 10);
        view->compression = GDI_BI_RGB;
        view->colors = info + 12;
    } else if (header_size >= 40) {
        view->width = (int)(LONG)gdi_read_u32(info + 4);
        raw_height = (int)(LONG)gdi_read_u32(info + 8);
        if (gdi_read_u16(info + 12) != 1) return FALSE;
        view->bpp = (int)gdi_read_u16(info + 14);
        view->compression = gdi_read_u32(info + 16);
        view->color_count = gdi_read_u32(info + 32);
        view->colors = info + header_size;

        if (view->compression == GDI_BI_BITFIELDS ||
            view->compression == GDI_BI_ALPHABITFIELDS) {
            const BYTE *masks;
            if (header_size >= 52) {
                masks = info + 40;
            } else if (header_size == 40) {
                masks = info + 40;
                view->colors += view->compression == GDI_BI_ALPHABITFIELDS
                              ? 16 : 12;
            } else {
                return FALSE;
            }
            view->red_mask = gdi_read_u32(masks);
            view->green_mask = gdi_read_u32(masks + 4);
            view->blue_mask = gdi_read_u32(masks + 8);
            if (!view->red_mask || !view->green_mask || !view->blue_mask)
                return FALSE;
        } else if (view->compression != GDI_BI_RGB) {
            return FALSE;
        }
    } else {
        return FALSE;
    }

    if (view->width <= 0 || !raw_height ||
        !gdi_abs_i32(raw_height, &view->height))
        return FALSE;
    view->top_down = raw_height < 0;

    if (view->bpp != 1 && view->bpp != 4 && view->bpp != 8 &&
        view->bpp != 16 && view->bpp != 24 && view->bpp != 32)
        return FALSE;
    if ((view->compression == GDI_BI_BITFIELDS ||
         view->compression == GDI_BI_ALPHABITFIELDS) &&
        view->bpp != 16 && view->bpp != 32)
        return FALSE;

    uint64_t row_bits = (uint64_t)(uint32_t)view->width *
                        (uint64_t)(uint32_t)view->bpp;
    uint64_t pitch = ((row_bits + 31) / 32) * 4;
    if (!pitch || pitch > 0x7FFFFFFFULL) return FALSE;
    view->pitch = (int)pitch;

    if (view->bpp <= 8) {
        DWORD maximum = 1u << view->bpp;
        if (!view->color_count || view->color_count > maximum)
            view->color_count = maximum;
    }
    if (view->compression == GDI_BI_RGB && view->bpp == 16) {
        view->red_mask = 0x7C00;
        view->green_mask = 0x03E0;
        view->blue_mask = 0x001F;
    }
    return TRUE;
}

static BYTE gdi_expand_mask(DWORD pixel, DWORD mask)
{
    if (!mask) return 0;
    unsigned shift = 0;
    while (shift < 32 && !(mask & (1u << shift))) shift++;
    DWORD maximum = mask >> shift;
    DWORD component = (pixel & mask) >> shift;
    return maximum ? (BYTE)(((uint64_t)component * 255 + maximum / 2) /
                            maximum) : 0;
}

static uint32_t gdi_palette_pixel(const GDI_DIB_VIEW *view, DWORD index)
{
    if (!view->colors || index >= view->color_count) return 0xFF000000u;
    const BYTE *entry = view->colors + index * (view->core_header ? 3 : 4);
    return 0xFF000000u | ((uint32_t)entry[2] << 16) |
           ((uint32_t)entry[1] << 8) | entry[0];
}

static BOOL gdi_read_dib_pixel(const GDI_DIB_VIEW *view, int x, int y,
                               uint32_t *color)
{
    if (!view || !color || x < 0 || y < 0 ||
        x >= view->width || y >= view->height)
        return FALSE;

    int stored_y = view->top_down ? y : view->height - 1 - y;
    const BYTE *row = view->bits + (SIZE_T)stored_y * view->pitch;
    DWORD value;
    switch (view->bpp) {
    case 1:
        value = (row[x >> 3] >> (7 - (x & 7))) & 1u;
        *color = gdi_palette_pixel(view, value);
        return TRUE;
    case 4:
        value = row[x >> 1];
        value = (x & 1) ? (value & 0x0Fu) : (value >> 4);
        *color = gdi_palette_pixel(view, value);
        return TRUE;
    case 8:
        *color = gdi_palette_pixel(view, row[x]);
        return TRUE;
    case 16:
        value = gdi_read_u16(row + (SIZE_T)x * 2);
        *color = 0xFF000000u |
                 ((uint32_t)gdi_expand_mask(value, view->red_mask) << 16) |
                 ((uint32_t)gdi_expand_mask(value, view->green_mask) << 8) |
                 gdi_expand_mask(value, view->blue_mask);
        return TRUE;
    case 24: {
        const BYTE *pixel = row + (SIZE_T)x * 3;
        *color = 0xFF000000u | ((uint32_t)pixel[2] << 16) |
                 ((uint32_t)pixel[1] << 8) | pixel[0];
        return TRUE;
    }
    case 32:
        value = gdi_read_u32(row + (SIZE_T)x * 4);
        if (view->compression == GDI_BI_RGB) {
            *color = 0xFF000000u | (value & 0x00FFFFFFu);
        } else {
            *color = 0xFF000000u |
                     ((uint32_t)gdi_expand_mask(value, view->red_mask) << 16) |
                     ((uint32_t)gdi_expand_mask(value, view->green_mask) << 8) |
                     gdi_expand_mask(value, view->blue_mask);
        }
        return TRUE;
    default:
        return FALSE;
    }
}

static void gdi_trace_dib_frame(const GDI_DIB_VIEW *view)
{
    if (!view || gdi_frame_probe_count >= 4 ||
        view->width < 256 || view->height < 256)
        return;

    uint64_t hash = 1469598103934665603ULL;
    uint64_t nonblack = 0;
    uint32_t first = 0, center = 0, last = 0;
    for (int y = 0; y < view->height; y++) {
        for (int x = 0; x < view->width; x++) {
            uint32_t color = 0;
            if (!gdi_read_dib_pixel(view, x, y, &color))
                continue;
            uint32_t rgb = color & 0x00FFFFFFu;
            if (rgb) nonblack++;
            hash ^= rgb;
            hash *= 1099511628211ULL;
            if (!x && !y) first = color;
            if (x == view->width / 2 && y == view->height / 2)
                center = color;
            if (x == view->width - 1 && y == view->height - 1)
                last = color;
        }
    }

    serial_puts("[GDI-FRAME] index=");
    serial_putdec(gdi_frame_probe_count++);
    serial_puts(" nonblack=");
    serial_putdec(nonblack);
    serial_puts(" hash=0x");
    serial_puthex(hash, 16);
    serial_puts(" first=0x");
    serial_puthex(first, 8);
    serial_puts(" center=0x");
    serial_puthex(center, 8);
    serial_puts(" last=0x");
    serial_puthex(last, 8);
    serial_puts("\n");
}

static BOOL gdi_read_dc_pixel(const GDI_DC *dc, int x, int y,
                              uint32_t *color)
{
    if (!dc || !dc->surface || !color || x < 0 || y < 0 ||
        x >= dc->width || y >= dc->height)
        return FALSE;

    int stored_y = dc->bottomup ? dc->height - 1 - y : y;
    const BYTE *pixel = (const BYTE *)dc->surface +
                        (SIZE_T)stored_y * dc->pitch;
    if (dc->bpp == 32) {
        *color = 0xFF000000u | (((const uint32_t *)pixel)[x] & 0x00FFFFFFu);
    } else if (dc->bpp == 24) {
        pixel += (SIZE_T)x * 3;
        *color = 0xFF000000u | ((uint32_t)pixel[2] << 16) |
                 ((uint32_t)pixel[1] << 8) | pixel[0];
    } else if (dc->bpp == 16) {
        USHORT value = ((const USHORT *)pixel)[x];
        DWORD r = (value >> 11) & 0x1F;
        DWORD g = (value >> 5) & 0x3F;
        DWORD b = value & 0x1F;
        *color = 0xFF000000u | ((r * 255 / 31) << 16) |
                 ((g * 255 / 63) << 8) | (b * 255 / 31);
    } else {
        return FALSE;
    }
    return TRUE;
}

static BOOL gdi_write_dc_pixel(GDI_DC *dc, int x, int y, uint32_t color)
{
    if (!dc || !dc->surface || x < 0 || y < 0 ||
        x >= dc->width || y >= dc->height)
        return FALSE;
    int stored_y = dc->bottomup ? dc->height - 1 - y : y;
    BYTE *pixel = (BYTE *)dc->surface + (SIZE_T)stored_y * dc->pitch;
    if (dc->bpp == 32) {
        ((uint32_t *)pixel)[x] = color;
    } else if (dc->bpp == 24) {
        pixel += (SIZE_T)x * 3;
        pixel[0] = (BYTE)color;
        pixel[1] = (BYTE)(color >> 8);
        pixel[2] = (BYTE)(color >> 16);
    } else if (dc->bpp == 16) {
        DWORD r = (color >> 16) & 0xFF;
        DWORD g = (color >> 8) & 0xFF;
        DWORD b = color & 0xFF;
        ((USHORT *)pixel)[x] = (USHORT)(((r * 31 + 127) / 255) << 11 |
                                       ((g * 63 + 127) / 255) << 5 |
                                       ((b * 31 + 127) / 255));
    } else {
        return FALSE;
    }
    return TRUE;
}

static int gdi_stretch_dib(GDI_DC *dc, int x, int y, int width, int height,
                           int source_x, int source_y, int source_width,
                           int source_height, const GDI_DIB_VIEW *view,
                           DWORD rop)
{
    int dest_width, dest_height, src_width, src_height;
    if (!dc || !dc->surface || rop != ROP_SRCCOPY ||
        !gdi_abs_i32(width, &dest_width) ||
        !gdi_abs_i32(height, &dest_height) ||
        !gdi_abs_i32(source_width, &src_width) ||
        !gdi_abs_i32(source_height, &src_height) ||
        !dest_width || !dest_height || !src_width || !src_height)
        return 0;

    int left = width > 0 ? x : x - dest_width;
    int top = height > 0 ? y : y - dest_height;
    int clip_left = left < 0 ? 0 : left;
    int clip_top = top < 0 ? 0 : top;
    int clip_right = left + dest_width;
    int clip_bottom = top + dest_height;
    if (clip_right > dc->width) clip_right = dc->width;
    if (clip_bottom > dc->height) clip_bottom = dc->height;
    if (clip_left >= clip_right || clip_top >= clip_bottom) return 0;

    int rows_written = 0;
    for (int dy = clip_top; dy < clip_bottom; dy++) {
        int dest_v = dy - top;
        if (height < 0) dest_v = dest_height - 1 - dest_v;
        int src_v = (int)(((uint64_t)(uint32_t)dest_v *
                           (uint64_t)(uint32_t)src_height) /
                          (uint64_t)(uint32_t)dest_height);
        int sy = source_height > 0 ? source_y + src_v
                                   : source_y - src_v - 1;
        BOOL wrote_row = FALSE;
        for (int dx = clip_left; dx < clip_right; dx++) {
            int dest_u = dx - left;
            if (width < 0) dest_u = dest_width - 1 - dest_u;
            int src_u = (int)(((uint64_t)(uint32_t)dest_u *
                               (uint64_t)(uint32_t)src_width) /
                              (uint64_t)(uint32_t)dest_width);
            int sx = source_width > 0 ? source_x + src_u
                                      : source_x - src_u - 1;
            uint32_t color;
            if (gdi_read_dib_pixel(view, sx, sy, &color) &&
                gdi_write_dc_pixel(dc, dx, dy, color))
                wrote_row = TRUE;
        }
        if (wrote_row) rows_written++;
    }
    if (rows_written) mark_dc_dirty(dc);
    return rows_written;
}

static BOOL WINAPI StretchBlt_k32(HDC destination, int x, int y,
                                   int width, int height, HDC source,
                                   int source_x, int source_y,
                                   int source_width, int source_height,
                                   DWORD rop)
{
    if (width == source_width && height == source_height)
        return BitBlt(destination, x, y, width, height, source,
                      source_x, source_y, rop);
    return dc_from_handle(destination) && dc_from_handle(source);
}

static int WINAPI StretchDIBits_k32(HDC hdc, int x, int y, int width,
                                     int height, int source_x, int source_y,
                                     int source_width, int source_height,
                                     PCVOID bits, PCVOID bitmap_info,
                                     UINT usage, DWORD rop)
{
    GDI_DC *dc = dc_from_handle(hdc);
    GDI_DIB_VIEW view;
    if (!dc || !gdi_parse_dib(bitmap_info, bits, usage, &view)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    gdi_trace_dib_frame(&view);
    if (gdi_dib_trace_count++ < 64) {
        serial_puts("[GDI-DIB] StretchDIBits dst=");
        serial_putdec((uint64_t)(uint32_t)width);
        serial_puts("x");
        serial_putdec((uint64_t)(uint32_t)height);
        serial_puts(" src=");
        serial_putdec((uint64_t)(uint32_t)source_width);
        serial_puts("x");
        serial_putdec((uint64_t)(uint32_t)source_height);
        serial_puts(" dib=");
        serial_putdec((uint64_t)(uint32_t)view.width);
        serial_puts("x");
        serial_putdec((uint64_t)(uint32_t)view.height);
        serial_puts("x");
        serial_putdec((uint64_t)(uint32_t)view.bpp);
        serial_puts(view.top_down ? " top-down\n" : " bottom-up\n");
    }
    int rows = gdi_stretch_dib(dc, x + dc->viewport_x,
                               y + dc->viewport_y, width, height,
                               source_x, source_y, source_width,
                               source_height, &view, rop);
    SetLastError(rows ? 0 : 87);
    return rows;
}

static int WINAPI SetDIBitsToDevice_k32(HDC hdc, int x, int y, DWORD width,
                                        DWORD height, int source_x,
                                        int source_y, UINT start_scan,
                                        UINT scan_lines, PCVOID bits,
                                        PCVOID bitmap_info, UINT usage)
{
    GDI_DC *dc = dc_from_handle(hdc);
    GDI_DIB_VIEW view;
    if (!dc || !width || !height || !scan_lines ||
        width > 0x7FFFFFFFu || height > 0x7FFFFFFFu ||
        !gdi_parse_dib(bitmap_info, bits, usage, &view) ||
        start_scan >= (UINT)view.height) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }

    UINT available = (UINT)view.height - start_scan;
    UINT rows = scan_lines < available ? scan_lines : available;
    if (rows > height) rows = height;
    if (gdi_dib_trace_count++ < 64) {
        serial_puts("[GDI-DIB] SetDIBitsToDevice size=");
        serial_putdec(width);
        serial_puts("x");
        serial_putdec(height);
        serial_puts(" start=");
        serial_putdec(start_scan);
        serial_puts(" lines=");
        serial_putdec(rows);
        serial_puts(" bpp=");
        serial_putdec((uint64_t)(uint32_t)view.bpp);
        serial_puts("\n");
    }

    int copied = gdi_stretch_dib(dc, x + dc->viewport_x,
                                 y + dc->viewport_y, (int)width, (int)rows,
                                 source_x, source_y + (int)start_scan,
                                 (int)width, (int)rows, &view, ROP_SRCCOPY);
    SetLastError(copied ? 0 : 87);
    return copied;
}

int gdi32_dib_selftest(void)
{
    int failures = 0;
    int checks = 0;
#define DIB_CHECK(condition) do { checks++; if (!(condition)) failures++; } while (0)

    int index = alloc_dc();
    if (index < 0) return 1;
    GDI_DC *dc = &gdi_dcs[index];
    uint32_t destination[16];
    HDC handle = (HDC)(ULONG_PTR)(DC_TAG | (unsigned)index);
    dc->surface = destination;
    dc->width = 4;
    dc->height = 4;
    dc->bpp = 32;
    dc->pitch = 4 * 4;
    dc->bottomup = 0;

    BYTE info[64];
    gdi_memset(info, 0, sizeof(info));
    info[0] = 40;
    info[4] = 2;                         /* biWidth = 2 */
    info[8] = 0xFE; info[9] = 0xFF;      /* biHeight = -2 */
    info[10] = 0xFF; info[11] = 0xFF;
    info[12] = 1;                        /* biPlanes = 1 */
    info[14] = 32;                       /* biBitCount = 32 */
    uint32_t top_down[4] = {
        0x00FF0000, 0x0000FF00,
        0x000000FF, 0x00FFFFFF,
    };

    gdi_memset(destination, 0, sizeof(destination));
    int rows = StretchDIBits_k32(handle, 1, 1, 2, 2, 0, 0, 2, 2,
                                  top_down, info, GDI_DIB_RGB_COLORS,
                                  ROP_SRCCOPY);
    DIB_CHECK(rows == 2);
    DIB_CHECK(destination[5] == 0xFFFF0000u);
    DIB_CHECK(destination[6] == 0xFF00FF00u);
    DIB_CHECK(destination[9] == 0xFF0000FFu);
    DIB_CHECK(destination[10] == 0xFFFFFFFFu);

    info[8] = 2; info[9] = 0;            /* bottom-up biHeight = 2 */
    info[10] = 0; info[11] = 0;
    uint32_t bottom_up[4] = {
        0x000000FF, 0x00FFFFFF,
        0x00FF0000, 0x0000FF00,
    };
    gdi_memset(destination, 0, sizeof(destination));
    rows = StretchDIBits_k32(handle, 0, 0, 2, 2, 0, 0, 2, 2,
                             bottom_up, info, GDI_DIB_RGB_COLORS,
                             ROP_SRCCOPY);
    DIB_CHECK(rows == 2);
    DIB_CHECK(destination[0] == 0xFFFF0000u);
    DIB_CHECK(destination[1] == 0xFF00FF00u);
    DIB_CHECK(destination[4] == 0xFF0000FFu);
    DIB_CHECK(destination[5] == 0xFFFFFFFFu);

    info[8] = 0xFE; info[9] = 0xFF;
    info[10] = 0xFF; info[11] = 0xFF;
    gdi_memset(destination, 0, sizeof(destination));
    rows = StretchDIBits_k32(handle, 0, 0, 4, 4, 0, 0, 2, 2,
                             top_down, info, GDI_DIB_RGB_COLORS,
                             ROP_SRCCOPY);
    DIB_CHECK(rows == 4);
    DIB_CHECK(destination[0] == 0xFFFF0000u &&
              destination[5] == 0xFFFF0000u);
    DIB_CHECK(destination[3] == 0xFF00FF00u);
    DIB_CHECK(destination[12] == 0xFF0000FFu);
    DIB_CHECK(destination[15] == 0xFFFFFFFFu);

    info[8] = 0xFF; info[9] = 0xFF;      /* top-down biHeight = -1 */
    info[14] = 24;
    BYTE rgb24[8] = { 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0, 0 };
    gdi_memset(destination, 0, sizeof(destination));
    rows = StretchDIBits_k32(handle, 0, 0, 2, 1, 0, 0, 2, 1,
                             rgb24, info, GDI_DIB_RGB_COLORS,
                             ROP_SRCCOPY);
    DIB_CHECK(rows == 1);
    DIB_CHECK(destination[0] == 0xFFFF0000u);
    DIB_CHECK(destination[1] == 0xFF00FF00u);

    info[8] = 0xFE; info[14] = 32;
    gdi_memset(destination, 0, sizeof(destination));
    rows = SetDIBitsToDevice_k32(handle, 1, 0, 2, 2, 0, 0, 0, 2,
                                 top_down, info, GDI_DIB_RGB_COLORS);
    DIB_CHECK(rows == 2);
    DIB_CHECK(destination[1] == 0xFFFF0000u);
    DIB_CHECK(destination[2] == 0xFF00FF00u);
    DIB_CHECK(destination[5] == 0xFF0000FFu);
    DIB_CHECK(destination[6] == 0xFFFFFFFFu);

    dc->in_use = 0;
    dc->surface = NULL;
    serial_puts("[GDI-DIB-TEST] checks=");
    serial_putdec((uint64_t)checks);
    serial_puts(" failures=");
    serial_putdec((uint64_t)failures);
    serial_puts("\n");
#undef DIB_CHECK
    return failures;
}

typedef struct {
    LONG x;
    LONG y;
    USHORT red;
    USHORT green;
    USHORT blue;
    USHORT alpha;
} GDI_TRIVERTEX;

typedef struct {
    ULONG upper_left;
    ULONG lower_right;
} GDI_GRADIENT_RECT;

#define GRADIENT_FILL_RECT_H 0u
#define GRADIENT_FILL_RECT_V 1u

static uint32_t *gdi_dc_row32(GDI_DC *dc, int y)
{
    if (!dc || !dc->surface || dc->bpp != 32 ||
        y < 0 || y >= dc->height)
        return NULL;
    if (dc->bottomup)
        y = dc->height - 1 - y;
    return (uint32_t *)((BYTE *)dc->surface + (SIZE_T)y * dc->pitch);
}

static USHORT gradient_channel(USHORT first, USHORT second,
                               int position, int span)
{
    if (span <= 0) return first;
    return (USHORT)(((uint64_t)first * (uint64_t)(span - position) +
                     (uint64_t)second * (uint64_t)position) /
                    (uint64_t)span);
}

static BOOL WINAPI GradientFill_k32(HDC hdc, const GDI_TRIVERTEX *vertices,
                                     ULONG vertex_count, PCVOID mesh,
                                     ULONG mesh_count, ULONG mode)
{
    GDI_DC *dc = dc_from_handle(hdc);
    const GDI_GRADIENT_RECT *rects = (const GDI_GRADIENT_RECT *)mesh;
    if (!dc || !dc->surface || dc->bpp != 32 || !vertices || !rects ||
        (mode != GRADIENT_FILL_RECT_H && mode != GRADIENT_FILL_RECT_V))
        return FALSE;

    for (ULONG item = 0; item < mesh_count; item++) {
        if (rects[item].upper_left >= vertex_count ||
            rects[item].lower_right >= vertex_count)
            return FALSE;

        const GDI_TRIVERTEX *first = &vertices[rects[item].upper_left];
        const GDI_TRIVERTEX *second = &vertices[rects[item].lower_right];
        if ((mode == GRADIENT_FILL_RECT_H && first->x > second->x) ||
            (mode == GRADIENT_FILL_RECT_V && first->y > second->y)) {
            const GDI_TRIVERTEX *swap = first;
            first = second;
            second = swap;
        }

        int x0 = first->x < second->x ? first->x : second->x;
        int y0 = first->y < second->y ? first->y : second->y;
        int x1 = first->x > second->x ? first->x : second->x;
        int y1 = first->y > second->y ? first->y : second->y;
        x0 += dc->viewport_x;
        x1 += dc->viewport_x;
        y0 += dc->viewport_y;
        y1 += dc->viewport_y;
        if (x0 == x1 || y0 == y1) continue;

        int clip_x0 = x0 < 0 ? 0 : x0;
        int clip_y0 = y0 < 0 ? 0 : y0;
        int clip_x1 = x1 > dc->width ? dc->width : x1;
        int clip_y1 = y1 > dc->height ? dc->height : y1;
        int span = mode == GRADIENT_FILL_RECT_H ? x1 - x0 : y1 - y0;

        for (int y = clip_y0; y < clip_y1; y++) {
            uint32_t *row = gdi_dc_row32(dc, y);
            if (!row) continue;
            for (int x = clip_x0; x < clip_x1; x++) {
                int position = mode == GRADIENT_FILL_RECT_H ? x - x0 : y - y0;
                USHORT red = gradient_channel(first->red, second->red,
                                              position, span);
                USHORT green = gradient_channel(first->green, second->green,
                                                position, span);
                USHORT blue = gradient_channel(first->blue, second->blue,
                                               position, span);
                USHORT alpha = gradient_channel(first->alpha, second->alpha,
                                                position, span);
                row[x] = ((uint32_t)(alpha >> 8) << 24) |
                         ((uint32_t)(red >> 8) << 16) |
                         ((uint32_t)(green >> 8) << 8) |
                         (uint32_t)(blue >> 8);
            }
        }
    }
    mark_dc_dirty(dc);
    return TRUE;
}

static BOOL WINAPI TransparentBlt_k32(HDC destination, int x, int y,
                                       int width, int height, HDC source,
                                       int source_x, int source_y,
                                       int source_width, int source_height,
                                       UINT transparent)
{
    GDI_DC *dst = dc_from_handle(destination);
    GDI_DC *src = dc_from_handle(source);
    if (!dst || !src || dst->bpp != 32 || src->bpp != 32 ||
        width <= 0 || height <= 0 || source_width <= 0 || source_height <= 0)
        return FALSE;

    x += dst->viewport_x;
    y += dst->viewport_y;
    source_x += src->viewport_x;
    source_y += src->viewport_y;
    for (int dy = 0; dy < height; dy++) {
        int dst_y = y + dy;
        int src_y = source_y + (int)((int64_t)dy * source_height / height);
        uint32_t *dst_row = gdi_dc_row32(dst, dst_y);
        uint32_t *src_row = gdi_dc_row32(src, src_y);
        if (!dst_row || !src_row) continue;
        for (int dx = 0; dx < width; dx++) {
            int dst_x = x + dx;
            int src_x = source_x + (int)((int64_t)dx * source_width / width);
            if (dst_x < 0 || dst_x >= dst->width ||
                src_x < 0 || src_x >= src->width)
                continue;
            uint32_t pixel = src_row[src_x];
            if ((pixel & 0x00FFFFFFu) != (transparent & 0x00FFFFFFu))
                dst_row[dst_x] = pixel;
        }
    }
    mark_dc_dirty(dst);
    return TRUE;
}

static BOOL WINAPI AlphaBlend_k32(HDC destination, int x, int y,
                                   int width, int height, HDC source,
                                   int source_x, int source_y,
                                   int source_width, int source_height,
                                   DWORD blend_function)
{
    GDI_DC *dst = dc_from_handle(destination);
    GDI_DC *src = dc_from_handle(source);
    if (!dst || !src || dst->bpp != 32 || src->bpp != 32 ||
        width <= 0 || height <= 0 || source_width <= 0 || source_height <= 0)
        return FALSE;

    UINT constant_alpha = (blend_function >> 16) & 0xFFu;
    BOOL use_source_alpha = ((blend_function >> 24) & 0x01u) != 0;
    x += dst->viewport_x;
    y += dst->viewport_y;
    source_x += src->viewport_x;
    source_y += src->viewport_y;
    for (int dy = 0; dy < height; dy++) {
        int dst_y = y + dy;
        int src_y = source_y + (int)((int64_t)dy * source_height / height);
        uint32_t *dst_row = gdi_dc_row32(dst, dst_y);
        uint32_t *src_row = gdi_dc_row32(src, src_y);
        if (!dst_row || !src_row) continue;
        for (int dx = 0; dx < width; dx++) {
            int dst_x = x + dx;
            int src_x = source_x + (int)((int64_t)dx * source_width / width);
            if (dst_x < 0 || dst_x >= dst->width ||
                src_x < 0 || src_x >= src->width)
                continue;

            uint32_t source_pixel = src_row[src_x];
            uint32_t destination_pixel = dst_row[dst_x];
            UINT alpha = constant_alpha;
            if (use_source_alpha)
                alpha = alpha * ((source_pixel >> 24) & 0xFFu) / 255u;
            UINT inverse = 255u - alpha;
            UINT blue = ((source_pixel & 0xFFu) * alpha +
                         (destination_pixel & 0xFFu) * inverse + 127u) / 255u;
            UINT green = (((source_pixel >> 8) & 0xFFu) * alpha +
                          ((destination_pixel >> 8) & 0xFFu) * inverse + 127u) /
                         255u;
            UINT red = (((source_pixel >> 16) & 0xFFu) * alpha +
                        ((destination_pixel >> 16) & 0xFFu) * inverse + 127u) /
                       255u;
            dst_row[dst_x] = 0xFF000000u | (red << 16) | (green << 8) | blue;
        }
    }
    mark_dc_dirty(dst);
    return TRUE;
}

static HANDLE WINAPI AddFontMemResourceEx_k32(PVOID data, DWORD size,
                                               PVOID reserved,
                                               DWORD *font_count)
{
    (void)reserved;
    if (!data || !size) return NULL;
    if (font_count) *font_count = 1;
    return (HANDLE)(ULONG_PTR)0xFA000001u;
}

static int WINAPI AddFontResourceA_k32(PCSTR path)
{
    return path && *path ? 1 : 0;
}

static int WINAPI AddFontResourceExA_k32(PCSTR path, DWORD flags,
                                          PVOID reserved)
{
    (void)flags; (void)reserved;
    return AddFontResourceA_k32(path);
}

static BOOL WINAPI RemoveFontResourceA_k32(PCSTR path)
{
    return path && *path;
}

static int WINAPI AddFontResourceExW_k32(PCWSTR path, DWORD flags,
                                          PVOID reserved)
{
    (void)flags;
    (void)reserved;
    return path && *path ? 1 : 0;
}

static BOOL WINAPI RemoveFontResourceExW_k32(PCWSTR path, DWORD flags,
                                              PVOID reserved)
{
    (void)flags;
    (void)reserved;
    return path && *path;
}

static int WINAPI SetGraphicsMode_k32(HDC hdc, int mode)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc || (mode != 1 && mode != 2)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    int previous = dc->graphics_mode;
    dc->graphics_mode = mode;
    SetLastError(0);
    return previous;
}

static int WINAPI GetGraphicsMode_k32(HDC hdc)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return 0;
    }
    return dc->graphics_mode;
}

static void world_transform_identity(float transform[6])
{
    transform[0] = 1.0f;
    transform[1] = 0.0f;
    transform[2] = 0.0f;
    transform[3] = 1.0f;
    transform[4] = 0.0f;
    transform[5] = 0.0f;
}

static void world_transform_multiply(float result[6], const float left[6],
                                     const float right[6])
{
    float composed[6];
    composed[0] = left[0] * right[0] + left[1] * right[2];
    composed[1] = left[0] * right[1] + left[1] * right[3];
    composed[2] = left[2] * right[0] + left[3] * right[2];
    composed[3] = left[2] * right[1] + left[3] * right[3];
    composed[4] = left[4] * right[0] + left[5] * right[2] + right[4];
    composed[5] = left[4] * right[1] + left[5] * right[3] + right[5];
    for (int i = 0; i < 6; i++) result[i] = composed[i];
}

static BOOL WINAPI SetWorldTransform_k32(HDC hdc, const float *transform)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc || !transform || dc->graphics_mode != 2) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    for (int i = 0; i < 6; i++) dc->world_transform[i] = transform[i];
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI GetWorldTransform_k32(HDC hdc, float *transform)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc || !transform) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    for (int i = 0; i < 6; i++) transform[i] = dc->world_transform[i];
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI ModifyWorldTransform_k32(HDC hdc, const float *transform,
                                             DWORD mode)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc || dc->graphics_mode != 2 ||
        (mode != 1 && (!transform || mode < 2 || mode > 4))) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    if (mode == 1) { /* MWT_IDENTITY */
        world_transform_identity(dc->world_transform);
    } else if (mode == 2) { /* MWT_LEFTMULTIPLY */
        world_transform_multiply(dc->world_transform, transform,
                                 dc->world_transform);
    } else if (mode == 3) { /* MWT_RIGHTMULTIPLY */
        world_transform_multiply(dc->world_transform, dc->world_transform,
                                 transform);
    } else { /* MWT_SET */
        for (int i = 0; i < 6; i++) dc->world_transform[i] = transform[i];
    }
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI GetTextMetricsA_stub(HDC hdc, PVOID metrics)
{
    (void)hdc;
    if (!metrics) return FALSE;

    gdi_memset(metrics, 0, 56); /* sizeof(TEXTMETRICA) */
    LONG *m = (LONG *)metrics;
    m[0] = 16;  /* height */
    m[1] = 12;  /* ascent */
    m[2] = 4;   /* descent */
    m[5] = 8;   /* average width */
    m[6] = 8;   /* maximum width */
    m[7] = 400; /* normal weight */
    return TRUE;
}

static BOOL WINAPI GetTextMetricsW_stub(HDC hdc, PVOID metrics)
{
    LONG *m;
    WCHAR *chars;

    (void)hdc;
    if (!metrics) return FALSE;

    gdi_memset(metrics, 0, 60); /* sizeof(TEXTMETRICW) */
    m = (LONG *)metrics;
    m[0] = 16;  /* height */
    m[1] = 12;  /* ascent */
    m[2] = 4;   /* descent */
    m[5] = 8;   /* average width */
    m[6] = 8;   /* maximum width */
    m[7] = 400; /* normal weight */
    chars = (WCHAR *)((BYTE *)metrics + 44);
    chars[0] = L' ';
    chars[1] = 0x00FF;
    chars[2] = L'?';
    chars[3] = L' ';
    return TRUE;
}

typedef int (WINAPI *FONTENUMPROCA)(PVOID, PVOID, DWORD, LONG_PTR);
typedef int (WINAPI *FONTENUMPROCW)(PVOID, PVOID, DWORD, LONG_PTR);

typedef struct {
    GDI_LOGFONTW logfont;
    WCHAR full_name[64];
    WCHAR style[32];
    WCHAR script[32];
} GDI_ENUMLOGFONTEXW;

typedef struct {
    LONG height;
    LONG ascent;
    LONG descent;
    LONG internal_leading;
    LONG external_leading;
    LONG average_width;
    LONG maximum_width;
    LONG weight;
    LONG overhang;
    LONG digitized_aspect_x;
    LONG digitized_aspect_y;
    WCHAR first_char;
    WCHAR last_char;
    WCHAR default_char;
    WCHAR break_char;
    BYTE italic;
    BYTE underlined;
    BYTE struck_out;
    BYTE pitch_and_family;
    BYTE charset;
    BYTE padding[3];
    DWORD flags;
    UINT size_em;
    UINT cell_height;
    UINT average_width_em;
    DWORD unicode_subsets[4];
    DWORD codepage_subsets[2];
} GDI_NEWTEXTMETRICEXW;

_Static_assert(sizeof(GDI_ENUMLOGFONTEXW) == 348,
               "ENUMLOGFONTEXW layout changed");
_Static_assert(sizeof(GDI_NEWTEXTMETRICEXW) == 100,
               "NEWTEXTMETRICEXW layout changed");

static void gdi_copy_wide(WCHAR *destination, UINT capacity,
                          const WCHAR *source)
{
    UINT index = 0;
    if (!capacity) return;
    while (source && source[index] && index + 1 < capacity) {
        destination[index] = source[index];
        index++;
    }
    destination[index] = 0;
}

static void gdi_ascii_to_wide(WCHAR *destination, UINT capacity,
                              const char *source)
{
    UINT index = 0;
    if (!capacity) return;
    while (source && source[index] && index + 1 < capacity) {
        destination[index] = (WCHAR)(BYTE)source[index];
        index++;
    }
    destination[index] = 0;
}

static int WINAPI EnumFontFamiliesExA_stub(HDC hdc, PVOID logfont,
                                            PVOID callback, LONG_PTR param,
                                            DWORD flags)
{
    (void)hdc; (void)flags;
    if (!callback) return 0;
    if (g_compat32_mode) {
        uint32_t args[4] = {
            (uint32_t)(ULONG_PTR)logfont, (uint32_t)(ULONG_PTR)logfont,
            4, (uint32_t)param
        };
        return (int)compat32_callback_args((uint32_t)(ULONG_PTR)callback, 4, args);
    }
    return ((FONTENUMPROCA)callback)(logfont, logfont, 4, param);
}

static int WINAPI EnumFontFamiliesExW_k32(HDC hdc, PVOID requested_font,
                                           PVOID callback, LONG_PTR param,
                                           DWORD flags)
{
    if (!dc_from_handle(hdc) || !requested_font || !callback) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    (void)flags;

    BYTE *wire = (BYTE *)VirtualAlloc(NULL, 0x1000,
                                      MEM_RESERVE | MEM_COMMIT,
                                      PAGE_READWRITE);
    if (!wire || (g_compat32_mode &&
                  (ULONG_PTR)wire + 0x1000 > UINT32_MAX)) {
        if (wire) VirtualFree(wire, 0, MEM_RELEASE);
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return 0;
    }

    GDI_ENUMLOGFONTEXW *font = (GDI_ENUMLOGFONTEXW *)wire;
    GDI_NEWTEXTMETRICEXW *metrics =
        (GDI_NEWTEXTMETRICEXW *)(wire + 512);
    const GDI_LOGFONTW *request = (const GDI_LOGFONTW *)requested_font;
    gdi_memset(font, 0, sizeof(*font));
    gdi_memset(metrics, 0, sizeof(*metrics));
    gdi_stock_font_logfont(&font->logfont);
    if (request->lfFaceName[0])
        gdi_copy_wide(font->logfont.lfFaceName, 32, request->lfFaceName);
    gdi_copy_wide(font->full_name, 64, font->logfont.lfFaceName);
    gdi_ascii_to_wide(font->style, 32, "Regular");
    gdi_ascii_to_wide(font->script, 32, "Western");

    metrics->height = 16;
    metrics->ascent = 12;
    metrics->descent = 4;
    metrics->average_width = 8;
    metrics->maximum_width = 8;
    metrics->weight = font->logfont.lfWeight;
    metrics->digitized_aspect_x = 96;
    metrics->digitized_aspect_y = 96;
    metrics->first_char = L' ';
    metrics->last_char = 0x00FF;
    metrics->default_char = L'?';
    metrics->break_char = L' ';
    metrics->italic = font->logfont.lfItalic;
    metrics->underlined = font->logfont.lfUnderline;
    metrics->struck_out = font->logfont.lfStrikeOut;
    metrics->pitch_and_family = font->logfont.lfPitchAndFamily;
    metrics->charset = font->logfont.lfCharSet;
    metrics->flags = 0x40; /* NTM_REGULAR */
    metrics->size_em = 2048;
    metrics->cell_height = 2048;
    metrics->average_width_em = 1024;
    metrics->unicode_subsets[0] = 1;  /* Basic Latin */
    metrics->codepage_subsets[0] = 1; /* Latin-1 */

    int result;
    if (g_compat32_mode) {
        uint32_t args[4] = {
            (uint32_t)(ULONG_PTR)font,
            (uint32_t)(ULONG_PTR)metrics,
            4, /* TRUETYPE_FONTTYPE */
            (uint32_t)param
        };
        result = (int)compat32_callback_args(
            (uint32_t)(ULONG_PTR)callback, 4, args);
    } else {
        result = ((FONTENUMPROCW)callback)(font, metrics, 4, param);
    }
    VirtualFree(wire, 0, MEM_RELEASE);
    SetLastError(0);
    return result;
}

static BOOL WINAPI GetCharABCWidthsW_stub(HDC hdc, UINT first, UINT last,
                                           PVOID widths)
{
    (void)hdc;
    if (!widths || last < first || last - first > 0x10FFFF) return FALSE;
    LONG *abc = (LONG *)widths;
    UINT count = last - first + 1;
    for (UINT i = 0; i < count; i++, abc += 3) {
        abc[0] = 0;
        abc[1] = 8;
        abc[2] = 0;
    }
    return TRUE;
}

static BOOL WINAPI GetCharWidthW_k32(HDC hdc, UINT first, UINT last,
                                      int *widths)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc || !widths || last < first || last - first > 0x10FFFFu) {
        SetLastError(!dc ? 6 : 87);
        return FALSE;
    }

    int width = 8;
    GDI_FONT *font = font_from_handle(dc->current_font);
    if (font && font->logfont.lfWidth) {
        width = font->logfont.lfWidth;
        if (width < 0) width = -width;
    }
    UINT count = last - first + 1;
    for (UINT index = 0; index < count; index++) widths[index] = width;
    SetLastError(0);
    return TRUE;
}

static DWORD WINAPI GetFontData_k32(HDC hdc, DWORD table, DWORD offset,
                                     PVOID buffer, DWORD size)
{
    (void)table;
    (void)offset;
    (void)buffer;
    (void)size;
    if (!dc_from_handle(hdc)) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return 0xFFFFFFFFu; /* GDI_ERROR */
    }

    /* GDI fonts currently expose metrics but do not retain an sfnt backing
     * stream. Report the documented failure value so callers can fall back to
     * Java/DirectWrite font files instead of treating a missing export as a
     * delay-load exception. */
    SetLastError(50); /* ERROR_NOT_SUPPORTED */
    return 0xFFFFFFFFu; /* GDI_ERROR */
}

BOOL WINAPI GetTextExtentPoint32A(HDC hdc, PCSTR lpString, int c, PVOID lpSize)
{
    (void)hdc; (void)lpString;
    if (lpSize) {
        LONG *sz = (LONG *)lpSize;
        sz[0] = c * 8;   /* cx */
        sz[1] = 16;      /* cy */
    }
    return TRUE;
}

BOOL WINAPI GetTextExtentPoint32W(HDC hdc, PCWSTR lpString, int c, PVOID lpSize)
{
    (void)hdc; (void)lpString;
    if (lpSize) {
        LONG *sz = (LONG *)lpSize;
        sz[0] = c * 8;   /* cx */
        sz[1] = 16;      /* cy */
    }
    return TRUE;
}

/* ── Additional GDI32 imports ─────────────────────────────────── */

static int WINAPI GetTextFaceW_k32(HDC hdc, int count, PWSTR name)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (!dc) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return 0;
    }

    GDI_LOGFONTW stock_font;
    const GDI_LOGFONTW *logfont = NULL;
    GDI_FONT *font = font_from_handle(dc->current_font);
    if (font && font->logfont.lfFaceName[0]) {
        logfont = &font->logfont;
    } else {
        gdi_stock_font_logfont(&stock_font);
        logfont = &stock_font;
    }

    int length = 0;
    while (length < 31 && logfont->lfFaceName[length]) length++;
    if (!name)
        return length + 1; /* Query includes the terminating NUL. */
    if (count <= 0)
        return 0;

    int copied = length;
    if (copied >= count) copied = count - 1;
    for (int i = 0; i < copied; i++)
        name[i] = logfont->lfFaceName[i];
    name[copied] = 0;
    return copied;
}

static PVOID gdi_create_font(const GDI_LOGFONTW *logfont)
{
    int idx = alloc_font();
    if (idx < 0) return NULL;
    gdi_memcpy(&gdi_fonts[idx].logfont, logfont, sizeof(*logfont));
    return (PVOID)(ULONG_PTR)(FONT_TAG | (unsigned)idx);
}

static PVOID WINAPI CreateFontA_stub(int h, int w, int esc, int orient, int weight,
                                      DWORD italic, DWORD underline, DWORD strikeout,
                                      DWORD charset, DWORD outprec, DWORD clipprec,
                                      DWORD quality, DWORD pitch, const char *face)
{
    GDI_LOGFONTW lf;
    gdi_memset(&lf, 0, sizeof(lf));
    lf.lfHeight = h;
    lf.lfWidth = w;
    lf.lfEscapement = esc;
    lf.lfOrientation = orient;
    lf.lfWeight = weight;
    lf.lfItalic = (BYTE)italic;
    lf.lfUnderline = (BYTE)underline;
    lf.lfStrikeOut = (BYTE)strikeout;
    lf.lfCharSet = (BYTE)charset;
    lf.lfOutPrecision = (BYTE)outprec;
    lf.lfClipPrecision = (BYTE)clipprec;
    lf.lfQuality = (BYTE)quality;
    lf.lfPitchAndFamily = (BYTE)pitch;
    if (face) {
        for (int i = 0; i < 31 && face[i]; i++)
            lf.lfFaceName[i] = (WCHAR)(BYTE)face[i];
    }
    return gdi_create_font(&lf);
}

static PVOID WINAPI CreateFontW_stub(int h, int w, int esc, int orient, int weight,
                                      DWORD italic, DWORD underline, DWORD strikeout,
                                      DWORD charset, DWORD outprec, DWORD clipprec,
                                      DWORD quality, DWORD pitch, const WCHAR *face)
{
    GDI_LOGFONTW lf;
    gdi_memset(&lf, 0, sizeof(lf));
    lf.lfHeight = h;
    lf.lfWidth = w;
    lf.lfEscapement = esc;
    lf.lfOrientation = orient;
    lf.lfWeight = weight;
    lf.lfItalic = (BYTE)italic;
    lf.lfUnderline = (BYTE)underline;
    lf.lfStrikeOut = (BYTE)strikeout;
    lf.lfCharSet = (BYTE)charset;
    lf.lfOutPrecision = (BYTE)outprec;
    lf.lfClipPrecision = (BYTE)clipprec;
    lf.lfQuality = (BYTE)quality;
    lf.lfPitchAndFamily = (BYTE)pitch;
    if (face) {
        for (int i = 0; i < 31 && face[i]; i++)
            lf.lfFaceName[i] = face[i];
    }
    return gdi_create_font(&lf);
}

static PVOID WINAPI CreateFontIndirectW_stub(const void *logfont)
{
    if (!logfont) return NULL;
    return gdi_create_font((const GDI_LOGFONTW *)logfont);
}

/* Real CreateDIBSection. UT99's SoftDrv windowed path recreates its render DIB
 * on every SetRes (incl. color-depth changes) and then check()s the returned
 * bits pointer (UWindowsViewport::ResizeViewport, WinDrv line 2348) — a NULL
 * here is a FATAL engine assert ("Critical Error" → exit). NT semantics: parse
 * the 32-bit BITMAPINFOHEADER, allocate the pixel buffer, return a real HBITMAP
 * and write the bits pointer through ppvBits (a 4-byte slot in the 32-bit
 * caller — write it as uint32, not a 64-bit store). */
static PVOID WINAPI CreateDIBSection_impl(HDC hdc, PVOID pbmi, UINT usage,
                                           void **ppvBits, PVOID hSection, DWORD offset)
{
    (void)hdc; (void)usage;
    serial_puts("[GDI32] CreateDIBSection request pbmi=0x");
    serial_puthex((uint64_t)(ULONG_PTR)pbmi, 16);
    serial_puts(" section=0x");
    serial_puthex((uint64_t)(ULONG_PTR)hSection, 16);
    serial_puts(" slots=");
    serial_putdec((uint64_t)bitmap_slots_used());
    serial_puts("\n");
    if (ppvBits) {
        if (g_compat32_mode) *(uint32_t *)ppvBits = 0;
        else *ppvBits = NULL;
    }
    if (!pbmi) {
        serial_puts("[GDI32] CreateDIBSection failed: null BITMAPINFO\n");
        return NULL;
    }

    const uint8_t *bi = (const uint8_t *)pbmi;     /* BITMAPINFOHEADER (32-bit) */
    int32_t  w    = *(const int32_t  *)(bi + 4);   /* biWidth */
    int32_t  hraw = *(const int32_t  *)(bi + 8);   /* biHeight (<0 = top-down) */
    uint16_t bpp  = *(const uint16_t *)(bi + 14);  /* biBitCount */
    int32_t  h    = hraw < 0 ? -hraw : hraw;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) {
        serial_puts("[GDI32] CreateDIBSection failed: dimensions w=0x");
        serial_puthex((uint64_t)(uint32_t)w, 8);
        serial_puts(" h=0x");
        serial_puthex((uint64_t)(uint32_t)hraw, 8);
        serial_puts("\n");
        return NULL;
    }
    if (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) {
        serial_puts("[GDI32] CreateDIBSection failed: bpp=");
        serial_putdec(bpp);
        serial_puts("\n");
        return NULL;
    }

    int idx = alloc_bmp();
    if (idx < 0) {
        serial_puts("[GDI32] CreateDIBSection failed: bitmap table full\n");
        return NULL;
    }

    uint32_t pitch = (((uint32_t)w * bpp + 31) / 32) * 4;  /* DWORD-aligned */
    uint64_t size  = (uint64_t)pitch * (uint64_t)h;
    void *pixels = NULL;
    void *mapping_base = NULL;

    if (hSection) {
        /* CreateDIBSection may expose an existing file mapping as its pixel
         * storage. Chromium uses this path to let its renderer write a frame
         * that the browser process later BitBlts. Keep the view base separate
         * because the DIB offset only needs DWORD alignment, while the NT
         * section mapper requires a page-aligned offset. */
        if (offset & 3u) {
            gdi_bmps[idx].in_use = 0;
            SetLastError(87); /* ERROR_INVALID_PARAMETER */
            serial_puts("[GDI32] CreateDIBSection failed: unaligned offset\n");
            return NULL;
        }
        DWORD aligned_offset = offset & ~0xFFFu;
        SIZE_T delta = (SIZE_T)(offset - aligned_offset);
        if (size > (uint64_t)(SIZE_T)-1 - delta) {
            gdi_bmps[idx].in_use = 0;
            SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
            return NULL;
        }
        mapping_base = MapViewOfFile((HANDLE)hSection, 0x0002u,
                                     0, aligned_offset,
                                     (SIZE_T)size + delta);
        if (mapping_base)
            pixels = (BYTE *)mapping_base + delta;
    } else {
        pixels = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT,
                              PAGE_READWRITE);
    }
    if (!pixels) {
        gdi_bmps[idx].in_use = 0;
        serial_puts("[GDI32] CreateDIBSection failed: allocation bytes=0x");
        serial_puthex(size, 16);
        serial_puts("\n");
        return NULL;
    }
    if (!mapping_base)
        gdi_memset(pixels, 0, size);

    gdi_bmps[idx].pixels = pixels;
    gdi_bmps[idx].width  = w;
    gdi_bmps[idx].height = h;
    gdi_bmps[idx].bpp    = bpp;
    gdi_bmps[idx].pitch  = (int)pitch;
    gdi_bmps[idx].bottomup    = (hraw > 0);  /* positive biHeight = bottom-up */
    gdi_bmps[idx].virtual_alloc = mapping_base ? 0 : 1;
    gdi_bmps[idx].mapping_base  = mapping_base;

    if (ppvBits) {
        if (g_compat32_mode)
            *(uint32_t *)ppvBits = (uint32_t)(uintptr_t)pixels;
        else
            *ppvBits = pixels;
    }

    serial_puts("[GDI32] CreateDIBSection ");
    serial_putdec((uint64_t)w); serial_puts("x"); serial_putdec((uint64_t)h);
    serial_puts("x"); serial_putdec(bpp);
    serial_puts(" bits=0x"); serial_puthex((uint64_t)(uintptr_t)pixels, 8);
    if (mapping_base) {
        serial_puts(" shared-base=0x");
        serial_puthex((uint64_t)(uintptr_t)mapping_base, 16);
    }
    serial_puts("\n");
    return (PVOID)(ULONG_PTR)(BMP_TAG | (uint32_t)idx);
}

static DWORD WINAPI GetPixel_stub(HDC hdc, int x, int y)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (dc && dc->surface && x >= 0 && y >= 0 &&
        x < dc->width && y < dc->height) {
        BYTE *base = (BYTE *)dc->surface;
        uint32_t *row = (uint32_t *)(base + y * dc->pitch);
        return row[x];
    }
    return 0x00000000;  /* black pixel */
}

typedef struct { PVOID *lpVtbl; } DWRITE_OBJECT_STUB;
typedef struct _DWRITE_COLLECTION_STUB DWRITE_COLLECTION_STUB;
typedef struct _DWRITE_FAMILY_STUB DWRITE_FAMILY_STUB;
typedef struct {
    PVOID *lpVtbl;
    DWRITE_FAMILY_STUB *family;
} DWRITE_FONT_STUB;
#define DWRITE_GSUB_CACHE_CAPACITY 64
typedef struct {
    USHORT glyph;
    USHORT source_glyph;
} DWRITE_GSUB_CACHE_ENTRY;
struct _DWRITE_FAMILY_STUB {
    PVOID *lpVtbl;
    DWRITE_COLLECTION_STUB *collection;
    DWRITE_FONT_STUB font;
};
struct _DWRITE_COLLECTION_STUB {
    PVOID *lpVtbl;
    volatile ULONG refs;
    UINT family_count;
    UINT font_file_count;
    BOOL heap_owned;
    PVOID *font_files;
    DWRITE_FAMILY_STUB family;
};
typedef struct {
    PVOID *lpVtbl;
    volatile ULONG refs;
    PVOID loader;
    UINT key_size;
    volatile ULONG gsub_cache_lock;
    UINT gsub_cache_count;
    DWRITE_GSUB_CACHE_ENTRY gsub_cache[DWRITE_GSUB_CACHE_CAPACITY];
    BYTE key[];
} DWRITE_FONT_FILE_STUB;
typedef struct {
    PVOID *lpVtbl;
    volatile ULONG refs;
    vfs_node_t node;
} DWRITE_LOCAL_FONT_STREAM;
typedef struct {
    PVOID *lpVtbl;
} DWRITE_LOCAL_FONT_LOADER;
typedef struct {
    PVOID *lpVtbl;
    volatile ULONG refs;
    UINT face_type;
    UINT face_index;
    UINT simulations;
    UINT font_file_count;
    PVOID font_files[];
} DWRITE_FONT_FACE_STUB;
typedef struct {
    PVOID stream;
    PVOID fragment_context;
} DWRITE_FONT_TABLE_CONTEXT;
typedef struct {
    PVOID *lpVtbl;
    const WCHAR *value;
} DWRITE_STRINGS_STUB;

typedef struct {
    USHORT design_units_per_em;
    USHORT ascent;
    USHORT descent;
    int16_t line_gap;
    USHORT cap_height;
    USHORT x_height;
    int16_t underline_position;
    USHORT underline_thickness;
    int16_t strikethrough_position;
    USHORT strikethrough_thickness;
} DWRITE_FONT_METRICS_STUB;

typedef struct {
    int32_t left_side_bearing;
    UINT advance_width;
    int32_t right_side_bearing;
    int32_t top_side_bearing;
    UINT advance_height;
    int32_t bottom_side_bearing;
    int32_t vertical_origin_y;
} DWRITE_GLYPH_METRICS_STUB;

typedef struct {
    float advance_offset;
    float ascender_offset;
} DWRITE_GLYPH_OFFSET_STUB;

typedef struct {
    PVOID font_face;
    float font_em_size;
    UINT glyph_count;
    const USHORT *glyph_indices;
    const float *glyph_advances;
    const DWRITE_GLYPH_OFFSET_STUB *glyph_offsets;
    BOOL sideways;
    UINT bidi_level;
} DWRITE_GLYPH_RUN_STUB;

typedef struct {
    float m11, m12, m21, m22, dx, dy;
} DWRITE_MATRIX_STUB;

typedef struct {
    LONG left, top, right, bottom;
} DWRITE_RECT_STUB;

typedef struct {
    PVOID *lpVtbl;
    volatile ULONG refs;
    DWRITE_RECT_STUB bounds;
    UINT glyph_count;
    USHORT raster_chars[];
} DWRITE_GLYPH_ANALYSIS_STUB;

#define DWRITE_E_NOINTERFACE         ((LONG)0x80004002)
#define DWRITE_E_POINTER             ((LONG)0x80004003)
#define DWRITE_E_NOTIMPL             ((LONG)0x80004001)
#define DWRITE_E_INVALIDARG          ((LONG)0x80070057)
#define DWRITE_E_OUTOFMEMORY         ((LONG)0x8007000E)
#define DWRITE_E_INSUFFICIENT_BUFFER ((LONG)0x8007007A)
#define DWRITE_E_FILEFORMAT          ((LONG)0x88985000)
#define DWRITE_E_NOFONT              ((LONG)0x88985002)
#define DWRITE_E_FILENOTFOUND        ((LONG)0x88985003)
#define DWRITE_E_FILEACCESS          ((LONG)0x88985004)
#define DWRITE_E_ALREADYREGISTERED   ((LONG)0x88985006)

#define DWRITE_FONT_FILE_TYPE_UNKNOWN             0
#define DWRITE_FONT_FILE_TYPE_CFF                 1
#define DWRITE_FONT_FILE_TYPE_TRUETYPE            2
#define DWRITE_FONT_FILE_TYPE_OPENTYPE_COLLECTION 3
#define DWRITE_FONT_FACE_TYPE_CFF                 0
#define DWRITE_FONT_FACE_TYPE_TRUETYPE            1
#define DWRITE_FONT_FACE_TYPE_OPENTYPE_COLLECTION 2
#define DWRITE_FONT_FACE_TYPE_UNKNOWN             6
#define DWRITE_OPENTYPE_TAG_CMAP                  0x70616D63U
#define DWRITE_OPENTYPE_TAG_GSUB                  0x42555347U
#define DWRITE_OPENTYPE_TAG_MAXP                  0x7078616DU

static const GUID dwrite_iid_iunknown = {
    0x00000000, 0x0000, 0x0000,
    { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 }
};
static const GUID dwrite_iid_factory = {
    0xB859EE5A, 0xD838, 0x4B5B,
    { 0xA2, 0xE8, 0x1A, 0xDC, 0x7D, 0x93, 0xDB, 0x48 }
};
static const GUID dwrite_iid_factory1 = {
    0x30572F99, 0xDAC6, 0x41DB,
    { 0xA1, 0x6E, 0x04, 0x86, 0x30, 0x7E, 0x60, 0x6A }
};
static const GUID dwrite_iid_factory2 = {
    0x0439FC60, 0xCA44, 0x4994,
    { 0x8D, 0xEE, 0x3A, 0x9A, 0xF7, 0xB7, 0x32, 0xEC }
};
static const GUID dwrite_iid_factory3 = {
    0x9A1B41C3, 0xD3BB, 0x466A,
    { 0x87, 0xFC, 0xFE, 0x67, 0x55, 0x6A, 0x3B, 0x65 }
};
static const GUID dwrite_iid_collection = {
    0xA84CEE02, 0x3EEA, 0x4EEE,
    { 0xA8, 0x27, 0x87, 0xC1, 0xA0, 0x2A, 0x0F, 0xCC }
};
static const GUID dwrite_iid_family = {
    0xDA20D8EF, 0x812A, 0x4C43,
    { 0x98, 0x02, 0x62, 0xEC, 0x4A, 0xBD, 0x7A, 0xDD }
};
static const GUID dwrite_iid_font_list = {
    0x1A0D8438, 0x1D97, 0x4EC1,
    { 0xAE, 0xF9, 0xA2, 0xFB, 0x86, 0xED, 0x6A, 0xCB }
};
static const GUID dwrite_iid_font = {
    0xACD16696, 0x8C14, 0x4F5D,
    { 0x87, 0x7E, 0xFE, 0x3F, 0xC1, 0xD3, 0x27, 0x37 }
};
static const GUID dwrite_iid_face = {
    0x5F49804D, 0x7024, 0x4D43,
    { 0xBF, 0xA9, 0xD2, 0x59, 0x84, 0xF5, 0x38, 0x49 }
};
static const GUID dwrite_iid_font_file = {
    0x739D886A, 0xCEF5, 0x47DC,
    { 0x87, 0x69, 0x1A, 0x8B, 0x41, 0xBE, 0xBB, 0xB0 }
};
static const GUID dwrite_iid_font_file_stream = {
    0x6D4865FE, 0x0AB8, 0x4D91,
    { 0x8F, 0x62, 0x5D, 0xD6, 0xBE, 0x34, 0xA3, 0xE0 }
};
static const GUID dwrite_iid_font_file_loader = {
    0x727CAD4E, 0xD6AF, 0x4C9E,
    { 0x8A, 0x08, 0xD6, 0x95, 0xB1, 0x1C, 0xAA, 0x49 }
};
static const GUID dwrite_iid_local_font_file_loader = {
    0xB2D9F3EC, 0xC9FE, 0x4A11,
    { 0xA2, 0xEC, 0xD8, 0x62, 0x08, 0xF7, 0xC0, 0xA2 }
};
static const GUID dwrite_iid_strings = {
    0x08256209, 0x099A, 0x4B34,
    { 0xB8, 0x6D, 0xC2, 0x2B, 0x11, 0x0E, 0x77, 0x71 }
};
static const GUID dwrite_iid_glyph_analysis = {
    0x7D97DBF7, 0xE085, 0x42D4,
    { 0x81, 0xE3, 0x6A, 0x88, 0x3B, 0xDE, 0xD1, 0x18 }
};
static const GUID dwrite_iid_font_fallback = {
    0xEFA008F9, 0xF7A1, 0x48BF,
    { 0xB0, 0x5C, 0xF2, 0x24, 0x71, 0x3C, 0xC0, 0xFF }
};

static int dwrite_guid_equal(LPCGUID left, LPCGUID right)
{
    const BYTE *a = (const BYTE *)left;
    const BYTE *b = (const BYTE *)right;
    if (!a || !b) return 0;
    for (int i = 0; i < 16; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

static LONG WINAPI dwrite_factory_QueryInterface(PVOID self, LPCGUID iid,
                                                  PVOID *object)
{
    if (!object) return (LONG)0x80004003; /* E_POINTER */
    *object = NULL;
    if (!dwrite_guid_equal(iid, &dwrite_iid_iunknown) &&
        !dwrite_guid_equal(iid, &dwrite_iid_factory) &&
        !dwrite_guid_equal(iid, &dwrite_iid_factory1) &&
        !dwrite_guid_equal(iid, &dwrite_iid_factory2) &&
        !dwrite_guid_equal(iid, &dwrite_iid_factory3))
        return (LONG)0x80004002; /* E_NOINTERFACE */
    *object = self;
    return 0;
}

static ULONG WINAPI dwrite_factory_AddRef(PVOID self)
{
    (void)self;
    return 2;
}

static ULONG WINAPI dwrite_factory_Release(PVOID self)
{
    (void)self;
    return 1;
}

static void dwrite_trace(const char *line);

static LONG WINAPI dwrite_factory_notimpl(PVOID self)
{
    (void)self;
    PVOID caller = __builtin_return_address(0);
    dwrite_trace("[DWRITE] unsupported factory method\n");
    dll_debug_log_address(caller);
    return DWRITE_E_NOTIMPL;
}

/* IDWriteFactory3 extends the 24-entry base interface. Keep every inherited
 * slot callable so unsupported extension methods fail with E_NOTIMPL. */
static PVOID dwrite_factory_vtbl[40];
static PVOID dwrite_collection_vtbl[7];
static PVOID dwrite_family_vtbl[9];
static PVOID dwrite_font_vtbl[14];
static PVOID dwrite_face_vtbl[18];
static PVOID dwrite_font_file_vtbl[6];
static PVOID dwrite_local_font_stream_vtbl[7];
static PVOID dwrite_local_font_loader_vtbl[7];
static PVOID dwrite_strings_vtbl[9];
static PVOID dwrite_analysis_vtbl[6];
static PVOID dwrite_fallback_vtbl[4];

static LONG WINAPI dwrite_factory_CreateFontFace(
    PVOID self, UINT requested_face_type, UINT file_count,
    PVOID const *font_files, UINT face_index, UINT simulations,
    PVOID *font_face);
static LONG WINAPI dwrite_face_TryGetFontTable(
    PVOID self, UINT table_tag, const void **table_data, UINT *table_size,
    PVOID *context, BOOL *exists);
static void WINAPI dwrite_face_ReleaseFontTable(PVOID self, PVOID context);

static DWRITE_OBJECT_STUB dwrite_factory = { dwrite_factory_vtbl };
static DWRITE_COLLECTION_STUB dwrite_collection = {
    dwrite_collection_vtbl, 1, 0, 0, FALSE, NULL,
    { dwrite_family_vtbl, &dwrite_collection,
      { dwrite_font_vtbl, NULL } }
};
static DWRITE_LOCAL_FONT_LOADER dwrite_local_font_loader = {
    dwrite_local_font_loader_vtbl
};
static DWRITE_OBJECT_STUB dwrite_fallback = { dwrite_fallback_vtbl };

typedef ULONG (WINAPI *DWRITE_ADDREF_FN)(PVOID);
typedef ULONG (WINAPI *DWRITE_RELEASE_FN)(PVOID);

static ULONG dwrite_object_addref(PVOID object)
{
    if (!object || !*(PVOID **)object || !(*(PVOID **)object)[1])
        return 0;
    return ((DWRITE_ADDREF_FN)(*(PVOID **)object)[1])(object);
}

static ULONG dwrite_object_release(PVOID object)
{
    if (!object || !*(PVOID **)object || !(*(PVOID **)object)[2])
        return 0;
    return ((DWRITE_RELEASE_FN)(*(PVOID **)object)[2])(object);
}

static LONG WINAPI dwrite_local_font_loader_QueryInterface(
    PVOID self, LPCGUID iid, PVOID *object)
{
    if (!object) return DWRITE_E_POINTER;
    *object = NULL;
    if (!dwrite_guid_equal(iid, &dwrite_iid_iunknown) &&
        !dwrite_guid_equal(iid, &dwrite_iid_font_file_loader) &&
        !dwrite_guid_equal(iid, &dwrite_iid_local_font_file_loader))
        return DWRITE_E_NOINTERFACE;
    *object = self;
    dwrite_factory_AddRef(self);
    if (dwrite_guid_equal(iid, &dwrite_iid_local_font_file_loader))
        dwrite_trace("[DWRITE] local loader QueryInterface\n");
    return 0;
}

static LONG WINAPI dwrite_local_font_stream_QueryInterface(
    PVOID self, LPCGUID iid, PVOID *object)
{
    if (!object) return DWRITE_E_POINTER;
    *object = NULL;
    if (!dwrite_guid_equal(iid, &dwrite_iid_iunknown) &&
        !dwrite_guid_equal(iid, &dwrite_iid_font_file_stream))
        return DWRITE_E_NOINTERFACE;
    *object = self;
    (void)__atomic_add_fetch(&((DWRITE_LOCAL_FONT_STREAM *)self)->refs, 1,
                             __ATOMIC_RELAXED);
    return 0;
}

static ULONG WINAPI dwrite_local_font_stream_AddRef(PVOID self)
{
    DWRITE_LOCAL_FONT_STREAM *stream = (DWRITE_LOCAL_FONT_STREAM *)self;
    if (!stream) return 0;
    return __atomic_add_fetch(&stream->refs, 1, __ATOMIC_RELAXED);
}

static ULONG WINAPI dwrite_local_font_stream_Release(PVOID self)
{
    DWRITE_LOCAL_FONT_STREAM *stream = (DWRITE_LOCAL_FONT_STREAM *)self;
    if (!stream) return 0;
    ULONG refs = __atomic_sub_fetch(&stream->refs, 1, __ATOMIC_ACQ_REL);
    if (!refs) kfree(stream);
    return refs;
}

static LONG WINAPI dwrite_local_font_stream_ReadFileFragment(
    PVOID self, const void **fragment_start, uint64_t file_offset,
    uint64_t fragment_size, PVOID *fragment_context)
{
    DWRITE_LOCAL_FONT_STREAM *stream = (DWRITE_LOCAL_FONT_STREAM *)self;
    if (!stream || !fragment_start || !fragment_context)
        return DWRITE_E_POINTER;
    *fragment_start = NULL;
    *fragment_context = NULL;
    if (file_offset > stream->node.size ||
        fragment_size > stream->node.size - file_offset ||
        fragment_size > 0x7FFFFFFFU)
        return DWRITE_E_FILEACCESS;

    BYTE *fragment = (BYTE *)kmalloc(fragment_size ? fragment_size : 1);
    if (!fragment) return DWRITE_E_OUTOFMEMORY;
    if (fragment_size) {
        int read = vfs_read(&stream->node, file_offset, fragment,
                            fragment_size);
        if (read < 0 || (uint64_t)read != fragment_size) {
            kfree(fragment);
            return DWRITE_E_FILEACCESS;
        }
    }
    *fragment_start = fragment;
    *fragment_context = fragment;
    return 0;
}

static void WINAPI dwrite_local_font_stream_ReleaseFileFragment(
    PVOID self, PVOID fragment_context)
{
    (void)self;
    kfree(fragment_context);
}

static LONG WINAPI dwrite_local_font_stream_GetFileSize(PVOID self,
                                                         uint64_t *size)
{
    DWRITE_LOCAL_FONT_STREAM *stream = (DWRITE_LOCAL_FONT_STREAM *)self;
    if (!stream || !size) return DWRITE_E_POINTER;
    *size = stream->node.size;
    return 0;
}

static LONG WINAPI dwrite_local_font_stream_GetLastWriteTime(
    PVOID self, uint64_t *last_write_time)
{
    if (!self || !last_write_time) return DWRITE_E_POINTER;
    *last_write_time = 0;
    return 0;
}

static LONG WINAPI dwrite_local_font_loader_CreateStreamFromKey(
    PVOID self, const void *font_file_reference_key, UINT key_size,
    PVOID *font_file_stream)
{
    (void)self;
    if (!font_file_stream) return DWRITE_E_POINTER;
    *font_file_stream = NULL;
    if (!font_file_reference_key || key_size < 2 || key_size > 4096)
        return DWRITE_E_INVALIDARG;

    const char *key = (const char *)font_file_reference_key;
    if (key[key_size - 1] != '\0') return DWRITE_E_INVALIDARG;
    for (UINT i = 0; i + 1 < key_size; i++)
        if (!key[i]) return DWRITE_E_INVALIDARG;

    vfs_node_t node;
    if (!vfs_find(key, VFS_MODE_WIN32, &node))
        return DWRITE_E_FILENOTFOUND;

    DWRITE_LOCAL_FONT_STREAM *stream =
        (DWRITE_LOCAL_FONT_STREAM *)kmalloc(sizeof(*stream));
    if (!stream) return DWRITE_E_OUTOFMEMORY;
    stream->lpVtbl = dwrite_local_font_stream_vtbl;
    stream->refs = 1;
    stream->node = node;
    *font_file_stream = stream;
    return 0;
}

static LONG dwrite_local_font_key_to_utf16(
    const void *font_file_reference_key, UINT key_size, WCHAR *path,
    UINT capacity, UINT *path_length)
{
    if (!font_file_reference_key || !path_length || key_size < 2 ||
        key_size > 4096)
        return DWRITE_E_INVALIDARG;

    const BYTE *key = (const BYTE *)font_file_reference_key;
    if (key[key_size - 1] != 0) return DWRITE_E_INVALIDARG;

    UINT input = 0;
    UINT output = 0;
    while (input + 1 < key_size) {
        UINT codepoint;
        BYTE first = key[input++];
        if (!first) return DWRITE_E_INVALIDARG;

        if (first < 0x80) {
            codepoint = first;
        } else {
            UINT continuation_count;
            if (first >= 0xC2 && first <= 0xDF) {
                continuation_count = 1;
                codepoint = first & 0x1FU;
            } else if (first >= 0xE0 && first <= 0xEF) {
                continuation_count = 2;
                codepoint = first & 0x0FU;
            } else if (first >= 0xF0 && first <= 0xF4) {
                continuation_count = 3;
                codepoint = first & 0x07U;
            } else {
                return DWRITE_E_INVALIDARG;
            }
            if (input + continuation_count > key_size - 1)
                return DWRITE_E_INVALIDARG;
            for (UINT i = 0; i < continuation_count; i++) {
                BYTE continuation = key[input++];
                if ((continuation & 0xC0U) != 0x80U)
                    return DWRITE_E_INVALIDARG;
                codepoint = (codepoint << 6) | (continuation & 0x3FU);
            }
            if ((continuation_count == 2 && codepoint < 0x800U) ||
                (continuation_count == 3 && codepoint < 0x10000U) ||
                (codepoint >= 0xD800U && codepoint <= 0xDFFFU) ||
                codepoint > 0x10FFFFU)
                return DWRITE_E_INVALIDARG;
        }

        UINT units = codepoint < 0x10000U ? 1 : 2;
        if (path && output + units >= capacity)
            return DWRITE_E_INSUFFICIENT_BUFFER;
        if (path) {
            if (units == 1) {
                path[output] = (WCHAR)codepoint;
            } else {
                codepoint -= 0x10000U;
                path[output] = (WCHAR)(0xD800U + (codepoint >> 10));
                path[output + 1] =
                    (WCHAR)(0xDC00U + (codepoint & 0x3FFU));
            }
        }
        output += units;
    }

    if (path) {
        if (output >= capacity) return DWRITE_E_INSUFFICIENT_BUFFER;
        path[output] = 0;
    }
    *path_length = output;
    return 0;
}

static LONG WINAPI dwrite_local_font_loader_GetFilePathLengthFromKey(
    PVOID self, const void *font_file_reference_key, UINT key_size,
    UINT *path_length)
{
    (void)self;
    LONG status = dwrite_local_font_key_to_utf16(
        font_file_reference_key, key_size, NULL, 0, path_length);
    if (status >= 0)
        dwrite_trace("[DWRITE] local loader.GetFilePathLengthFromKey\n");
    return status;
}

static LONG WINAPI dwrite_local_font_loader_GetFilePathFromKey(
    PVOID self, const void *font_file_reference_key, UINT key_size,
    WCHAR *path, UINT path_capacity)
{
    (void)self;
    if (!path) return DWRITE_E_POINTER;
    UINT path_length = 0;
    LONG status = dwrite_local_font_key_to_utf16(
        font_file_reference_key, key_size, path, path_capacity, &path_length);
    if (status >= 0)
        dwrite_trace("[DWRITE] local loader.GetFilePathFromKey\n");
    return status;
}

static LONG WINAPI dwrite_local_font_loader_GetLastWriteTimeFromKey(
    PVOID self, const void *font_file_reference_key, UINT key_size,
    uint64_t *last_write_time)
{
    (void)self;
    if (!last_write_time) return DWRITE_E_POINTER;
    UINT path_length = 0;
    LONG status = dwrite_local_font_key_to_utf16(
        font_file_reference_key, key_size, NULL, 0, &path_length);
    if (status < 0) return status;
    *last_write_time = 0;
    return 0;
}

static LONG dwrite_utf16_path_to_utf8(PCWSTR input, char *output, UINT capacity)
{
    if (!input || !output || !capacity) return DWRITE_E_INVALIDARG;
    UINT written = 0;
    for (UINT i = 0; input[i]; i++) {
        UINT codepoint = input[i];
        if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
            UINT low = input[++i];
            if (low < 0xDC00 || low > 0xDFFF)
                return DWRITE_E_INVALIDARG;
            codepoint = 0x10000U + ((codepoint - 0xD800U) << 10) +
                        (low - 0xDC00U);
        } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
            return DWRITE_E_INVALIDARG;
        }

        UINT needed = codepoint < 0x80 ? 1 : codepoint < 0x800 ? 2 :
                      codepoint < 0x10000 ? 3 : 4;
        if (written + needed >= capacity) return DWRITE_E_INVALIDARG;
        if (needed == 1) {
            output[written++] = (char)codepoint;
        } else if (needed == 2) {
            output[written++] = (char)(0xC0U | (codepoint >> 6));
            output[written++] = (char)(0x80U | (codepoint & 0x3FU));
        } else if (needed == 3) {
            output[written++] = (char)(0xE0U | (codepoint >> 12));
            output[written++] = (char)(0x80U | ((codepoint >> 6) & 0x3FU));
            output[written++] = (char)(0x80U | (codepoint & 0x3FU));
        } else {
            output[written++] = (char)(0xF0U | (codepoint >> 18));
            output[written++] = (char)(0x80U | ((codepoint >> 12) & 0x3FU));
            output[written++] = (char)(0x80U | ((codepoint >> 6) & 0x3FU));
            output[written++] = (char)(0x80U | (codepoint & 0x3FU));
        }
    }
    output[written] = '\0';
    return 0;
}

static LONG dwrite_create_local_font_file(const char *path, PVOID *font_file)
{
    if (!font_file) return DWRITE_E_POINTER;
    *font_file = NULL;
    if (!path || !*path) return DWRITE_E_INVALIDARG;

    UINT key_size = 1;
    while (path[key_size - 1]) {
        if (key_size >= 4096) return DWRITE_E_INVALIDARG;
        key_size++;
    }
    vfs_node_t node;
    if (!vfs_find(path, VFS_MODE_WIN32, &node))
        return DWRITE_E_FILENOTFOUND;

    DWRITE_FONT_FILE_STUB *file = (DWRITE_FONT_FILE_STUB *)kmalloc(
        sizeof(*file) + key_size);
    if (!file) return DWRITE_E_OUTOFMEMORY;
    file->lpVtbl = dwrite_font_file_vtbl;
    file->refs = 1;
    file->loader = &dwrite_local_font_loader;
    dwrite_factory_AddRef(file->loader);
    file->key_size = key_size;
    file->gsub_cache_lock = 0;
    file->gsub_cache_count = 0;
    gdi_memcpy(file->key, path, key_size);
    *font_file = file;
    return 0;
}

#define DWRITE_MAX_REGISTERED_LOADERS 64
static PVOID dwrite_collection_loaders[DWRITE_MAX_REGISTERED_LOADERS];
static PVOID dwrite_file_loaders[DWRITE_MAX_REGISTERED_LOADERS];
static volatile ULONG dwrite_loader_lock;

static void dwrite_loaders_lock(void)
{
    while (__atomic_exchange_n(&dwrite_loader_lock, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile ("pause");
}

static void dwrite_loaders_unlock(void)
{
    __atomic_store_n(&dwrite_loader_lock, 0, __ATOMIC_RELEASE);
}

static LONG dwrite_register_loader(PVOID slots[], PVOID loader)
{
    if (!loader || !*(PVOID **)loader || !dwrite_object_addref(loader))
        return DWRITE_E_INVALIDARG;

    int empty = -1;
    dwrite_loaders_lock();
    for (int i = 0; i < DWRITE_MAX_REGISTERED_LOADERS; i++) {
        if (slots[i] == loader) {
            dwrite_loaders_unlock();
            dwrite_object_release(loader);
            return DWRITE_E_ALREADYREGISTERED;
        }
        if (!slots[i] && empty < 0)
            empty = i;
    }
    if (empty >= 0)
        slots[empty] = loader;
    dwrite_loaders_unlock();

    if (empty < 0) {
        dwrite_object_release(loader);
        return DWRITE_E_OUTOFMEMORY;
    }
    return 0;
}

static LONG dwrite_unregister_loader(PVOID slots[], PVOID loader)
{
    if (!loader)
        return DWRITE_E_INVALIDARG;

    int found = -1;
    dwrite_loaders_lock();
    for (int i = 0; i < DWRITE_MAX_REGISTERED_LOADERS; i++) {
        if (slots[i] == loader) {
            slots[i] = NULL;
            found = i;
            break;
        }
    }
    dwrite_loaders_unlock();

    if (found < 0)
        return DWRITE_E_INVALIDARG;
    dwrite_object_release(loader);
    return 0;
}

static PVOID dwrite_acquire_registered_loader(PVOID slots[], PVOID loader)
{
    PVOID result = NULL;
    dwrite_loaders_lock();
    for (int i = 0; i < DWRITE_MAX_REGISTERED_LOADERS; i++) {
        if (slots[i] == loader) {
            if (dwrite_object_addref(loader))
                result = loader;
            break;
        }
    }
    dwrite_loaders_unlock();
    return result;
}

static ULONG WINAPI dwrite_collection_AddRef(PVOID self)
{
    DWRITE_COLLECTION_STUB *collection = (DWRITE_COLLECTION_STUB *)self;
    if (!collection || collection == &dwrite_collection)
        return 2;
    return __atomic_add_fetch(&collection->refs, 1, __ATOMIC_RELAXED);
}

static ULONG WINAPI dwrite_collection_Release(PVOID self)
{
    DWRITE_COLLECTION_STUB *collection = (DWRITE_COLLECTION_STUB *)self;
    if (!collection || collection == &dwrite_collection ||
        !collection->heap_owned)
        return 1;

    ULONG refs = __atomic_sub_fetch(&collection->refs, 1, __ATOMIC_ACQ_REL);
    if (!refs) {
        for (UINT i = 0; i < collection->font_file_count; i++)
            dwrite_object_release(collection->font_files[i]);
        kfree(collection->font_files);
        kfree(collection);
    }
    return refs;
}

static ULONG WINAPI dwrite_family_AddRef(PVOID self)
{
    DWRITE_FAMILY_STUB *family = (DWRITE_FAMILY_STUB *)self;
    return family && family->collection
        ? dwrite_collection_AddRef(family->collection) : 0;
}

static ULONG WINAPI dwrite_family_Release(PVOID self)
{
    DWRITE_FAMILY_STUB *family = (DWRITE_FAMILY_STUB *)self;
    return family && family->collection
        ? dwrite_collection_Release(family->collection) : 0;
}

static ULONG WINAPI dwrite_font_AddRef(PVOID self)
{
    DWRITE_FONT_STUB *font = (DWRITE_FONT_STUB *)self;
    return font && font->family ? dwrite_family_AddRef(font->family) : 0;
}

static ULONG WINAPI dwrite_font_Release(PVOID self)
{
    DWRITE_FONT_STUB *font = (DWRITE_FONT_STUB *)self;
    return font && font->family ? dwrite_family_Release(font->family) : 0;
}

static LONG WINAPI dwrite_font_file_QueryInterface(PVOID self, LPCGUID iid,
                                                     PVOID *object)
{
    if (!object) return DWRITE_E_POINTER;
    *object = NULL;
    if (!dwrite_guid_equal(iid, &dwrite_iid_iunknown) &&
        !dwrite_guid_equal(iid, &dwrite_iid_font_file))
        return DWRITE_E_NOINTERFACE;
    *object = self;
    dwrite_object_addref(self);
    return 0;
}

static ULONG WINAPI dwrite_font_file_AddRef(PVOID self)
{
    DWRITE_FONT_FILE_STUB *file = (DWRITE_FONT_FILE_STUB *)self;
    if (!file) return 0;
    return __atomic_add_fetch(&file->refs, 1, __ATOMIC_RELAXED);
}

static ULONG WINAPI dwrite_font_file_Release(PVOID self)
{
    DWRITE_FONT_FILE_STUB *file = (DWRITE_FONT_FILE_STUB *)self;
    if (!file) return 0;
    ULONG refs = __atomic_sub_fetch(&file->refs, 1, __ATOMIC_ACQ_REL);
    if (!refs) {
        dwrite_object_release(file->loader);
        kfree(file);
    }
    return refs;
}

static LONG WINAPI dwrite_font_file_GetReferenceKey(PVOID self,
                                                     const void **key,
                                                     UINT *key_size)
{
    DWRITE_FONT_FILE_STUB *file = (DWRITE_FONT_FILE_STUB *)self;
    if (!file || !key || !key_size) return DWRITE_E_POINTER;
    *key = file->key;
    *key_size = file->key_size;
    return 0;
}

static LONG WINAPI dwrite_font_file_GetLoader(PVOID self, PVOID *loader)
{
    DWRITE_FONT_FILE_STUB *file = (DWRITE_FONT_FILE_STUB *)self;
    if (!file || !loader) return DWRITE_E_POINTER;
    *loader = file->loader;
    dwrite_object_addref(file->loader);
    return 0;
}

static UINT dwrite_read_be32(const BYTE *value)
{
    return ((UINT)value[0] << 24) | ((UINT)value[1] << 16) |
           ((UINT)value[2] << 8) | (UINT)value[3];
}

static USHORT dwrite_read_be16(const BYTE *value)
{
    return (USHORT)(((USHORT)value[0] << 8) | (USHORT)value[1]);
}

static USHORT dwrite_cmap_lookup_format4(const BYTE *table, UINT size,
                                          UINT codepoint)
{
    if (!table || size < 16 || codepoint > 0xFFFFU)
        return 0;

    UINT length = dwrite_read_be16(table + 2);
    UINT seg_count = dwrite_read_be16(table + 6) / 2U;
    if (length < 16 || length > size || !seg_count)
        return 0;

    uint64_t end_offset = 14;
    uint64_t start_offset = end_offset + (uint64_t)seg_count * 2U + 2U;
    uint64_t delta_offset = start_offset + (uint64_t)seg_count * 2U;
    uint64_t range_offset = delta_offset + (uint64_t)seg_count * 2U;
    if (range_offset + (uint64_t)seg_count * 2U > length)
        return 0;

    for (UINT i = 0; i < seg_count; i++) {
        UINT end = dwrite_read_be16(table + end_offset + i * 2U);
        if (codepoint > end)
            continue;
        UINT start = dwrite_read_be16(table + start_offset + i * 2U);
        if (codepoint < start)
            return 0;

        int16_t delta = (int16_t)dwrite_read_be16(
            table + delta_offset + i * 2U);
        UINT range = dwrite_read_be16(table + range_offset + i * 2U);
        if (!range)
            return (USHORT)((codepoint + delta) & 0xFFFFU);

        uint64_t glyph_offset = range_offset + (uint64_t)i * 2U + range +
            (uint64_t)(codepoint - start) * 2U;
        if (glyph_offset + 2U > length)
            return 0;
        USHORT glyph = dwrite_read_be16(table + glyph_offset);
        return glyph ? (USHORT)((glyph + delta) & 0xFFFFU) : 0;
    }
    return 0;
}

static USHORT dwrite_cmap_lookup_format12(const BYTE *table, UINT size,
                                           UINT codepoint)
{
    if (!table || size < 16)
        return 0;
    uint64_t length = dwrite_read_be32(table + 4);
    uint64_t group_count = dwrite_read_be32(table + 12);
    if (length < 16 || length > size || group_count > (length - 16) / 12U)
        return 0;

    uint64_t low = 0;
    uint64_t high = group_count;
    while (low < high) {
        uint64_t middle = low + (high - low) / 2U;
        const BYTE *group = table + 16U + middle * 12U;
        UINT start = dwrite_read_be32(group);
        UINT end = dwrite_read_be32(group + 4);
        if (codepoint < start) {
            high = middle;
        } else if (codepoint > end) {
            low = middle + 1U;
        } else {
            uint64_t glyph = (uint64_t)dwrite_read_be32(group + 8) +
                codepoint - start;
            return glyph <= 0xFFFFU ? (USHORT)glyph : 0;
        }
    }
    return 0;
}

static USHORT dwrite_cmap_lookup(const BYTE *table, UINT size,
                                  UINT codepoint)
{
    if (!table || size < 4)
        return 0;
    UINT record_count = dwrite_read_be16(table + 2);
    if (record_count > (size - 4U) / 8U)
        return 0;

    for (UINT pass = 0; pass < 2; pass++) {
        UINT wanted_format = pass == 0 ? 12U : 4U;
        for (UINT i = 0; i < record_count; i++) {
            const BYTE *record = table + 4U + i * 8U;
            UINT platform = dwrite_read_be16(record);
            UINT encoding = dwrite_read_be16(record + 2);
            if (platform != 0 &&
                !(platform == 3 && (encoding == 1 || encoding == 10)))
                continue;
            UINT offset = dwrite_read_be32(record + 4);
            if (offset > size - 2U)
                continue;
            const BYTE *subtable = table + offset;
            UINT format = dwrite_read_be16(subtable);
            if (format != wanted_format)
                continue;
            USHORT glyph = format == 12
                ? dwrite_cmap_lookup_format12(subtable, size - offset,
                                              codepoint)
                : dwrite_cmap_lookup_format4(subtable, size - offset,
                                             codepoint);
            if (glyph)
                return glyph;
        }
    }
    return 0;
}

static USHORT dwrite_cmap_glyph_to_ascii(const BYTE *table, UINT size,
                                          USHORT glyph)
{
    for (UINT codepoint = 32; codepoint <= 126; codepoint++)
        if (dwrite_cmap_lookup(table, size, codepoint) == glyph)
            return (USHORT)codepoint;
    if (glyph && dwrite_cmap_lookup(table, size, 0x00A0U) == glyph)
        return (USHORT)' ';
    return 0;
}

static BOOL dwrite_coverage_index(const BYTE *coverage, UINT size,
                                  USHORT glyph, UINT *index)
{
    if (!coverage || !index || size < 4) return FALSE;
    UINT format = dwrite_read_be16(coverage);
    UINT count = dwrite_read_be16(coverage + 2);
    if (format == 1) {
        if (count > (size - 4U) / 2U) return FALSE;
        UINT low = 0;
        UINT high = count;
        while (low < high) {
            UINT middle = low + (high - low) / 2U;
            USHORT candidate = dwrite_read_be16(
                coverage + 4U + middle * 2U);
            if (glyph < candidate) high = middle;
            else if (glyph > candidate) low = middle + 1U;
            else {
                *index = middle;
                return TRUE;
            }
        }
        return FALSE;
    }
    if (format != 2 || count > (size - 4U) / 6U) return FALSE;
    for (UINT i = 0; i < count; i++) {
        const BYTE *range = coverage + 4U + i * 6U;
        UINT start = dwrite_read_be16(range);
        UINT end = dwrite_read_be16(range + 2);
        UINT first_index = dwrite_read_be16(range + 4);
        if (start > end) return FALSE;
        if (glyph < start) return FALSE;
        if (glyph <= end) {
            *index = first_index + (UINT)glyph - start;
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL dwrite_coverage_glyph(const BYTE *coverage, UINT size,
                                  UINT index, USHORT *glyph)
{
    if (!coverage || !glyph || size < 4) return FALSE;
    UINT format = dwrite_read_be16(coverage);
    UINT count = dwrite_read_be16(coverage + 2);
    if (format == 1) {
        if (count > (size - 4U) / 2U || index >= count) return FALSE;
        *glyph = dwrite_read_be16(coverage + 4U + index * 2U);
        return TRUE;
    }
    if (format != 2 || count > (size - 4U) / 6U) return FALSE;
    for (UINT i = 0; i < count; i++) {
        const BYTE *range = coverage + 4U + i * 6U;
        UINT start = dwrite_read_be16(range);
        UINT end = dwrite_read_be16(range + 2);
        UINT first_index = dwrite_read_be16(range + 4);
        if (start > end) return FALSE;
        UINT range_count = end - start + 1U;
        if (index >= first_index && index - first_index < range_count) {
            *glyph = (USHORT)(start + index - first_index);
            return TRUE;
        }
    }
    return FALSE;
}

static USHORT dwrite_gsub_reverse_single_subtable(const BYTE *subtable,
                                                   UINT size,
                                                   USHORT glyph)
{
    if (!subtable || size < 6) return 0;
    UINT format = dwrite_read_be16(subtable);
    UINT coverage_offset = dwrite_read_be16(subtable + 2);
    if (coverage_offset > size - 4U) return 0;
    const BYTE *coverage = subtable + coverage_offset;
    UINT coverage_size = size - coverage_offset;

    if (format == 1) {
        int16_t delta = (int16_t)dwrite_read_be16(subtable + 4);
        USHORT source = (USHORT)((int32_t)glyph - delta);
        UINT index;
        return dwrite_coverage_index(coverage, coverage_size, source, &index)
            ? source : 0;
    }
    if (format != 2) return 0;
    UINT count = dwrite_read_be16(subtable + 4);
    if (count > (size - 6U) / 2U) return 0;
    for (UINT i = 0; i < count; i++) {
        if (dwrite_read_be16(subtable + 6U + i * 2U) != glyph)
            continue;
        USHORT source = 0;
        if (dwrite_coverage_glyph(coverage, coverage_size, i, &source))
            return source;
    }
    return 0;
}

static USHORT dwrite_gsub_reverse_single(const BYTE *table, UINT size,
                                         USHORT glyph)
{
    if (!table || size < 12 || dwrite_read_be16(table) != 1) return 0;
    UINT lookup_list_offset = dwrite_read_be16(table + 8);
    if (lookup_list_offset > size - 2U) return 0;
    const BYTE *lookup_list = table + lookup_list_offset;
    UINT lookup_list_size = size - lookup_list_offset;
    UINT lookup_count = dwrite_read_be16(lookup_list);
    if (lookup_count > (lookup_list_size - 2U) / 2U) return 0;

    for (UINT i = 0; i < lookup_count; i++) {
        UINT lookup_offset = dwrite_read_be16(
            lookup_list + 2U + i * 2U);
        if (lookup_offset > lookup_list_size - 6U) continue;
        const BYTE *lookup = lookup_list + lookup_offset;
        UINT lookup_size = lookup_list_size - lookup_offset;
        UINT lookup_type = dwrite_read_be16(lookup);
        UINT subtable_count = dwrite_read_be16(lookup + 4);
        if (subtable_count > (lookup_size - 6U) / 2U) continue;

        for (UINT j = 0; j < subtable_count; j++) {
            UINT subtable_offset = dwrite_read_be16(
                lookup + 6U + j * 2U);
            if (subtable_offset > lookup_size - 6U) continue;
            const BYTE *subtable = lookup + subtable_offset;
            UINT subtable_size = lookup_size - subtable_offset;
            USHORT source = 0;
            if (lookup_type == 1) {
                source = dwrite_gsub_reverse_single_subtable(
                    subtable, subtable_size, glyph);
            } else if (lookup_type == 7 && subtable_size >= 8 &&
                       dwrite_read_be16(subtable) == 1 &&
                       dwrite_read_be16(subtable + 2) == 1) {
                UINT extension_offset = dwrite_read_be32(subtable + 4);
                if (extension_offset <= subtable_size - 6U)
                    source = dwrite_gsub_reverse_single_subtable(
                        subtable + extension_offset,
                        subtable_size - extension_offset, glyph);
            }
            if (source) return source;
        }
    }
    return 0;
}

static LONG WINAPI dwrite_font_file_Analyze(PVOID self, BOOL *supported,
                                             UINT *file_type,
                                             UINT *face_type,
                                             UINT *face_count)
{
    DWRITE_FONT_FILE_STUB *file = (DWRITE_FONT_FILE_STUB *)self;
    if (!file || !supported || !file_type || !face_type || !face_count)
        return DWRITE_E_POINTER;
    *supported = FALSE;
    *file_type = DWRITE_FONT_FILE_TYPE_UNKNOWN;
    *face_type = DWRITE_FONT_FACE_TYPE_UNKNOWN;
    *face_count = 0;

    PVOID *loader_vtbl = file->loader ? *(PVOID **)file->loader : NULL;
    if (!loader_vtbl || !loader_vtbl[3])
        return DWRITE_E_INVALIDARG;
    typedef LONG (WINAPI *CREATE_STREAM_FN)(PVOID, const void *, UINT,
                                            PVOID *);
    PVOID stream = NULL;
    LONG status = ((CREATE_STREAM_FN)loader_vtbl[3])(
        file->loader, file->key, file->key_size, &stream);
    if (status < 0 || !stream)
        return status < 0 ? status : DWRITE_E_FILEFORMAT;

    PVOID *stream_vtbl = *(PVOID **)stream;
    if (!stream_vtbl || !stream_vtbl[3] || !stream_vtbl[4] ||
        !stream_vtbl[5]) {
        dwrite_object_release(stream);
        return DWRITE_E_FILEFORMAT;
    }

    typedef LONG (WINAPI *GET_SIZE_FN)(PVOID, uint64_t *);
    typedef LONG (WINAPI *READ_FRAGMENT_FN)(PVOID, const void **, uint64_t,
                                            uint64_t, PVOID *);
    typedef void (WINAPI *RELEASE_FRAGMENT_FN)(PVOID, PVOID);
    uint64_t size = 0;
    status = ((GET_SIZE_FN)stream_vtbl[5])(stream, &size);
    if (status < 0 || size < 4) {
        dwrite_object_release(stream);
        return status < 0 ? status : DWRITE_E_FILEFORMAT;
    }

    uint64_t header_size = size < 12 ? size : 12;
    const BYTE *header = NULL;
    PVOID fragment_context = NULL;
    status = ((READ_FRAGMENT_FN)stream_vtbl[3])(
        stream, (const void **)&header, 0, header_size, &fragment_context);
    if (status >= 0 && header) {
        UINT tag = dwrite_read_be32(header);
        if (tag == 0x00010000U || tag == 0x74727565U ||
            tag == 0x74797031U) {
            *supported = TRUE;
            *file_type = DWRITE_FONT_FILE_TYPE_TRUETYPE;
            *face_type = DWRITE_FONT_FACE_TYPE_TRUETYPE;
            *face_count = 1;
        } else if (tag == 0x4F54544FU) { /* OTTO */
            *supported = TRUE;
            *file_type = DWRITE_FONT_FILE_TYPE_CFF;
            *face_type = DWRITE_FONT_FACE_TYPE_CFF;
            *face_count = 1;
        } else if (tag == 0x74746366U && header_size >= 12) { /* ttcf */
            UINT count = dwrite_read_be32(header + 8);
            if (count && count <= 4096) {
                *supported = TRUE;
                *file_type = DWRITE_FONT_FILE_TYPE_OPENTYPE_COLLECTION;
                *face_type = DWRITE_FONT_FACE_TYPE_OPENTYPE_COLLECTION;
                *face_count = count;
            }
        }
        ((RELEASE_FRAGMENT_FN)stream_vtbl[4])(stream, fragment_context);
    }
    dwrite_object_release(stream);
    return status < 0 ? status : 0;
}

static LONG WINAPI dwrite_fallback_QueryInterface(PVOID self, LPCGUID iid,
                                                   PVOID *object)
{
    if (!object) return DWRITE_E_POINTER;
    *object = NULL;
    if (!dwrite_guid_equal(iid, &dwrite_iid_iunknown) &&
        !dwrite_guid_equal(iid, &dwrite_iid_font_fallback))
        return DWRITE_E_NOINTERFACE;
    *object = self;
    return 0;
}

static LONG WINAPI dwrite_fallback_MapCharacters(
    PVOID self, PVOID analysis_source, UINT text_position, UINT text_length,
    PVOID base_collection, PCWSTR base_family_name, UINT base_weight,
    UINT base_style, UINT base_stretch, UINT *mapped_length,
    PVOID *mapped_font, float *scale)
{
    (void)self;
    (void)analysis_source;
    (void)text_position;
    (void)base_collection;
    (void)base_family_name;
    (void)base_weight;
    (void)base_style;
    (void)base_stretch;
    if (!mapped_length || !mapped_font || !scale) return DWRITE_E_POINTER;
    *mapped_length = text_length;
    *mapped_font = text_length && dwrite_collection.family_count
        ? &dwrite_collection.family.font : NULL;
    if (*mapped_font) dwrite_font_AddRef(*mapped_font);
    *scale = 1.0f;
    dwrite_trace("[DWRITE] fallback.MapCharacters\n");
    return 0;
}

static const WCHAR dwrite_family_name[] = {
    'S', 'e', 'g', 'o', 'e', ' ', 'U', 'I', 0
};
static const WCHAR dwrite_face_name[] = {
    'R', 'e', 'g', 'u', 'l', 'a', 'r', 0
};
static const WCHAR dwrite_locale_name[] = {
    'e', 'n', '-', 'U', 'S', 0
};
static DWRITE_STRINGS_STUB dwrite_family_names = {
    dwrite_strings_vtbl, dwrite_family_name
};
static DWRITE_STRINGS_STUB dwrite_face_names = {
    dwrite_strings_vtbl, dwrite_face_name
};

static UINT dwrite_trace_count;
static UINT dwrite_alpha_trace_count;
static UINT dwrite_glyph_trace_count;

static void dwrite_trace(const char *line)
{
#if defined(OK_QUIET) && OK_QUIET
    (void)line;
#else
    if (dwrite_trace_count++ < 320)
        serial_puts(line);
#endif
}

static LONG dwrite_floor_to_long(float value)
{
    LONG whole = (LONG)value;
    return value < (float)whole ? whole - 1 : whole;
}

static LONG dwrite_ceil_to_long(float value)
{
    LONG whole = (LONG)value;
    return value > (float)whole ? whole + 1 : whole;
}

static void dwrite_transform_point(const DWRITE_MATRIX_STUB *matrix,
                                   float x, float y, float *out_x,
                                   float *out_y)
{
    if (!matrix) {
        *out_x = x;
        *out_y = y;
        return;
    }
    *out_x = x * matrix->m11 + y * matrix->m21 + matrix->dx;
    *out_y = x * matrix->m12 + y * matrix->m22 + matrix->dy;
}

static void dwrite_gsub_cache_lock(DWRITE_FONT_FILE_STUB *file)
{
    while (__atomic_exchange_n(&file->gsub_cache_lock, 1,
                               __ATOMIC_ACQUIRE))
        __asm__ volatile ("pause");
}

static void dwrite_gsub_cache_unlock(DWRITE_FONT_FILE_STUB *file)
{
    __atomic_store_n(&file->gsub_cache_lock, 0, __ATOMIC_RELEASE);
}

static BOOL dwrite_gsub_cache_lookup(DWRITE_FONT_FILE_STUB *file,
                                     USHORT glyph, USHORT *source)
{
    if (!file || !source) return FALSE;
    BOOL found = FALSE;
    dwrite_gsub_cache_lock(file);
    UINT count = file->gsub_cache_count;
    if (count > DWRITE_GSUB_CACHE_CAPACITY)
        count = DWRITE_GSUB_CACHE_CAPACITY;
    for (UINT i = 0; i < count; i++) {
        if (file->gsub_cache[i].glyph != glyph) continue;
        *source = file->gsub_cache[i].source_glyph;
        found = TRUE;
        break;
    }
    dwrite_gsub_cache_unlock(file);
    return found;
}

static void dwrite_gsub_cache_insert(DWRITE_FONT_FILE_STUB *file,
                                     USHORT glyph, USHORT source)
{
    if (!file) return;
    dwrite_gsub_cache_lock(file);
    UINT count = file->gsub_cache_count;
    if (count > DWRITE_GSUB_CACHE_CAPACITY)
        count = DWRITE_GSUB_CACHE_CAPACITY;
    for (UINT i = 0; i < count; i++) {
        if (file->gsub_cache[i].glyph != glyph) continue;
        file->gsub_cache[i].source_glyph = source;
        dwrite_gsub_cache_unlock(file);
        return;
    }
    UINT slot;
    if (count < DWRITE_GSUB_CACHE_CAPACITY) {
        slot = count;
        file->gsub_cache_count = count + 1U;
    } else {
        slot = (UINT)glyph % DWRITE_GSUB_CACHE_CAPACITY;
    }
    file->gsub_cache[slot].glyph = glyph;
    file->gsub_cache[slot].source_glyph = source;
    dwrite_gsub_cache_unlock(file);
}

static USHORT dwrite_reverse_substituted_glyph(DWRITE_FONT_FACE_STUB *face,
                                                USHORT glyph)
{
    if (!face || !face->font_file_count || !face->font_files[0]) return 0;
    DWRITE_FONT_FILE_STUB *file =
        (DWRITE_FONT_FILE_STUB *)face->font_files[0];
    if (file->lpVtbl != dwrite_font_file_vtbl) return 0;

    USHORT source = 0;
    if (dwrite_gsub_cache_lookup(file, glyph, &source)) return source;

    const void *gsub = NULL;
    UINT gsub_size = 0;
    PVOID context = NULL;
    BOOL exists = FALSE;
    LONG status = dwrite_face_TryGetFontTable(
        face, DWRITE_OPENTYPE_TAG_GSUB, &gsub, &gsub_size, &context,
        &exists);
    if (status >= 0 && exists && gsub)
        source = dwrite_gsub_reverse_single(
            (const BYTE *)gsub, gsub_size, glyph);
    if (context) dwrite_face_ReleaseFontTable(face, context);
    dwrite_gsub_cache_insert(file, glyph, source);
    return source;
}

static LONG WINAPI dwrite_analysis_QueryInterface(PVOID self, LPCGUID iid,
                                                    PVOID *object)
{
    if (!object) return DWRITE_E_POINTER;
    *object = NULL;
    if (!dwrite_guid_equal(iid, &dwrite_iid_iunknown) &&
        !dwrite_guid_equal(iid, &dwrite_iid_glyph_analysis))
        return DWRITE_E_NOINTERFACE;
    *object = self;
    (void)__atomic_add_fetch(&((DWRITE_GLYPH_ANALYSIS_STUB *)self)->refs, 1,
                             __ATOMIC_RELAXED);
    return 0;
}

static ULONG WINAPI dwrite_analysis_AddRef(PVOID self)
{
    if (!self) return 0;
    return __atomic_add_fetch(&((DWRITE_GLYPH_ANALYSIS_STUB *)self)->refs, 1,
                              __ATOMIC_RELAXED);
}

static ULONG WINAPI dwrite_analysis_Release(PVOID self)
{
    DWRITE_GLYPH_ANALYSIS_STUB *analysis =
        (DWRITE_GLYPH_ANALYSIS_STUB *)self;
    if (!analysis) return 0;
    ULONG refs = __atomic_sub_fetch(&analysis->refs, 1, __ATOMIC_ACQ_REL);
    if (!refs) kfree(analysis);
    return refs;
}

static LONG WINAPI dwrite_analysis_GetAlphaTextureBounds(
    PVOID self, UINT texture_type, DWRITE_RECT_STUB *bounds)
{
    DWRITE_GLYPH_ANALYSIS_STUB *analysis =
        (DWRITE_GLYPH_ANALYSIS_STUB *)self;
    if (!analysis || !bounds) return DWRITE_E_POINTER;
    if (texture_type > 1) return DWRITE_E_INVALIDARG;
    *bounds = analysis->bounds;
    if (dwrite_alpha_trace_count++ < 64) {
        serial_puts("[DWRITE-ALPHA] bounds type=");
        serial_putdec(texture_type);
        serial_puts(" rect=");
        serial_putdec((uint64_t)(int64_t)bounds->left);
        serial_puts(",");
        serial_putdec((uint64_t)(int64_t)bounds->top);
        serial_puts("-");
        serial_putdec((uint64_t)(int64_t)bounds->right);
        serial_puts(",");
        serial_putdec((uint64_t)(int64_t)bounds->bottom);
        serial_puts(" glyphs=");
        serial_putdec(analysis->glyph_count);
        serial_puts("\n");
    }
    dwrite_trace("[DWRITE] analysis.GetAlphaTextureBounds\n");
    return 0;
}

static LONG WINAPI dwrite_analysis_CreateAlphaTexture(
    PVOID self, UINT texture_type, const DWRITE_RECT_STUB *bounds,
    BYTE *alpha_values, UINT buffer_size)
{
    DWRITE_GLYPH_ANALYSIS_STUB *analysis =
        (DWRITE_GLYPH_ANALYSIS_STUB *)self;
    if (!analysis || !bounds || !alpha_values) return DWRITE_E_POINTER;
    if (texture_type > 1 || bounds->right < bounds->left ||
        bounds->bottom < bounds->top)
        return DWRITE_E_INVALIDARG;

    uint64_t width = (uint64_t)(bounds->right - bounds->left);
    uint64_t height = (uint64_t)(bounds->bottom - bounds->top);
    uint64_t channels = texture_type == 1 ? 3 : 1;
    uint64_t required = width * height * channels;
    if (width > 4096 || height > 4096 || required > buffer_size)
        return DWRITE_E_INSUFFICIENT_BUFFER;

    if (dwrite_alpha_trace_count++ < 64) {
        serial_puts("[DWRITE-ALPHA] create type=");
        serial_putdec(texture_type);
        serial_puts(" size=");
        serial_putdec(width);
        serial_puts("x");
        serial_putdec(height);
        serial_puts(" bytes=");
        serial_putdec(required);
        serial_puts("\n");
    }

    gdi_memset(alpha_values, 0, (SIZE_T)required);
    if (!width || !height || !analysis->glyph_count) return 0;

    LONG run_width = analysis->bounds.right - analysis->bounds.left;
    LONG run_height = analysis->bounds.bottom - analysis->bounds.top;
    if (run_width < 1 || run_height < 1) return 0;
    for (uint64_t y = 0; y < height; y++) {
        LONG global_y = bounds->top + (LONG)y;
        if (global_y < analysis->bounds.top ||
            global_y >= analysis->bounds.bottom)
            continue;
        UINT glyph_y = (UINT)(((uint64_t)(global_y - analysis->bounds.top) *
                               16) / (uint64_t)run_height);
        if (glyph_y > 15) glyph_y = 15;
        for (uint64_t x = 0; x < width; x++) {
            LONG global_x = bounds->left + (LONG)x;
            if (global_x < analysis->bounds.left ||
                global_x >= analysis->bounds.right)
                continue;

            uint64_t relative_x =
                (uint64_t)(global_x - analysis->bounds.left);
            UINT glyph_slot = (UINT)((relative_x * analysis->glyph_count) /
                                     (uint64_t)run_width);
            if (glyph_slot >= analysis->glyph_count)
                glyph_slot = analysis->glyph_count - 1;
            LONG cell_left = analysis->bounds.left +
                (LONG)(((uint64_t)run_width * glyph_slot) /
                       analysis->glyph_count);
            LONG cell_right = analysis->bounds.left +
                (LONG)(((uint64_t)run_width * (glyph_slot + 1)) /
                       analysis->glyph_count);
            LONG cell_width = cell_right - cell_left;
            if (cell_width < 1)
                continue;
            UINT glyph_x = (UINT)(((uint64_t)(global_x - cell_left) * 8) /
                                  (uint64_t)cell_width);
            if (glyph_x > 7) glyph_x = 7;

            USHORT glyph_index = analysis->raster_chars[glyph_slot];
            const uint8_t *glyph =
                (glyph_index >= 32 && glyph_index <= 126)
                    ? gui_font8x16[glyph_index - 32]
                    : gui_font8x16['?' - 32];
            if (!(glyph[glyph_y] & (0x80u >> glyph_x)))
                continue;

            uint64_t pixel = (y * width + x) * channels;
            for (uint64_t channel = 0; channel < channels; channel++)
                alpha_values[pixel + channel] = 255;
        }
    }
    dwrite_trace("[DWRITE] analysis.CreateAlphaTexture\n");
    return 0;
}

static LONG WINAPI dwrite_analysis_GetAlphaBlendParams(
    PVOID self, PVOID rendering_params, float *gamma, float *contrast,
    float *cleartype_level)
{
    (void)self;
    (void)rendering_params;
    if (!gamma || !contrast || !cleartype_level) return DWRITE_E_POINTER;
    *gamma = 2.2f;
    *contrast = 1.0f;
    *cleartype_level = 1.0f;
    return 0;
}

static LONG WINAPI dwrite_factory_CreateGlyphRunAnalysis(
    PVOID self, const DWRITE_GLYPH_RUN_STUB *run, float pixels_per_dip,
    const DWRITE_MATRIX_STUB *transform, UINT rendering_mode,
    UINT measuring_mode, float baseline_x, float baseline_y, PVOID *result)
{
    (void)self;
    (void)rendering_mode;
    (void)measuring_mode;
    if (!run || !result) return DWRITE_E_POINTER;
    *result = NULL;
    if (!(run->font_em_size > 0.0f) || !(pixels_per_dip > 0.0f) ||
        run->glyph_count > 65536)
        return DWRITE_E_INVALIDARG;

    float advance = 0.0f;
    for (UINT i = 0; i < run->glyph_count; i++) {
        float value = run->glyph_advances
            ? run->glyph_advances[i] : run->font_em_size * 0.5f;
        if (!((value > 0.01f || value < -0.01f) &&
              value > -4096.0f && value < 4096.0f))
            value = run->font_em_size * 0.5f;
        if (value < 0.0f) value = -value;
        advance += value;
    }
    if (advance > 4096.0f) advance = 4096.0f;

    float x0 = baseline_x * pixels_per_dip;
    float x1 = (baseline_x + advance) * pixels_per_dip;
    float y0 = (baseline_y - run->font_em_size * 0.8f) * pixels_per_dip;
    float y1 = (baseline_y + run->font_em_size * 0.2f) * pixels_per_dip;
    float px[4], py[4];
    dwrite_transform_point(transform, x0, y0, &px[0], &py[0]);
    dwrite_transform_point(transform, x1, y0, &px[1], &py[1]);
    dwrite_transform_point(transform, x0, y1, &px[2], &py[2]);
    dwrite_transform_point(transform, x1, y1, &px[3], &py[3]);

    float min_x = px[0], max_x = px[0], min_y = py[0], max_y = py[0];
    for (int i = 1; i < 4; i++) {
        if (px[i] < min_x) min_x = px[i];
        if (px[i] > max_x) max_x = px[i];
        if (py[i] < min_y) min_y = py[i];
        if (py[i] > max_y) max_y = py[i];
    }
    if (!(min_x > -1048576.0f && max_x < 1048576.0f &&
          min_y > -1048576.0f && max_y < 1048576.0f))
        return DWRITE_E_INVALIDARG;

    const BYTE *cmap = NULL;
    UINT cmap_size = 0;
    PVOID cmap_context = NULL;
    BOOL cmap_exists = FALSE;
    DWRITE_FONT_FACE_STUB *face =
        (DWRITE_FONT_FACE_STUB *)run->font_face;
    if (face && face->lpVtbl == dwrite_face_vtbl) {
        LONG cmap_status = dwrite_face_TryGetFontTable(
            face, DWRITE_OPENTYPE_TAG_CMAP, (const void **)&cmap,
            &cmap_size, &cmap_context, &cmap_exists);
        if (cmap_status < 0 || !cmap_exists) {
            cmap = NULL;
            cmap_size = 0;
            cmap_context = NULL;
        }
    }

    SIZE_T analysis_size = sizeof(DWRITE_GLYPH_ANALYSIS_STUB) +
        (SIZE_T)run->glyph_count * sizeof(USHORT);
    DWRITE_GLYPH_ANALYSIS_STUB *analysis =
        (DWRITE_GLYPH_ANALYSIS_STUB *)kmalloc(analysis_size);
    if (!analysis) {
        if (cmap_context)
            dwrite_face_ReleaseFontTable(face, cmap_context);
        return (LONG)0x8007000E; /* E_OUTOFMEMORY */
    }
    analysis->lpVtbl = dwrite_analysis_vtbl;
    analysis->refs = 1;
    analysis->bounds.left = dwrite_floor_to_long(min_x);
    analysis->bounds.top = dwrite_floor_to_long(min_y);
    analysis->bounds.right = dwrite_ceil_to_long(max_x);
    analysis->bounds.bottom = dwrite_ceil_to_long(max_y);
    if (analysis->bounds.right - analysis->bounds.left > 4096)
        analysis->bounds.right = analysis->bounds.left + 4096;
    if (analysis->bounds.bottom - analysis->bounds.top > 4096)
        analysis->bounds.bottom = analysis->bounds.top + 4096;
    analysis->glyph_count = run->glyph_count;
    for (UINT i = 0; i < run->glyph_count; i++) {
        USHORT glyph = run->glyph_indices
            ? run->glyph_indices[i] : (USHORT)'?';
        USHORT raster_char = cmap
            ? dwrite_cmap_glyph_to_ascii(cmap, cmap_size, glyph) : 0;
        USHORT source_glyph = glyph;
        for (UINT depth = 0; !raster_char && cmap && depth < 4; depth++) {
            USHORT source = dwrite_reverse_substituted_glyph(
                face, source_glyph);
            if (!source || source == source_glyph) break;
            source_glyph = source;
            raster_char = dwrite_cmap_glyph_to_ascii(
                cmap, cmap_size, source_glyph);
        }
        if (!raster_char && glyph >= 32 && glyph <= 126)
            raster_char = glyph;
        if (!raster_char && dwrite_glyph_trace_count++ < 32) {
            serial_puts("[DWRITE-UNMAPPED] id=");
            serial_putdec(glyph);
            serial_puts(cmap ? " cmap=1\n" : " cmap=0\n");
        }
        analysis->raster_chars[i] = raster_char ? raster_char : (USHORT)'?';
    }
    if (cmap_context)
        dwrite_face_ReleaseFontTable(face, cmap_context);
    *result = analysis;
    dwrite_trace("[DWRITE] factory.CreateGlyphRunAnalysis\n");
    return 0;
}

static LONG dwrite_static_QueryInterface(PVOID self, LPCGUID iid,
                                         LPCGUID primary, LPCGUID secondary,
                                         PVOID *object)
{
    if (!object) return DWRITE_E_POINTER;
    *object = NULL;
    if (!dwrite_guid_equal(iid, &dwrite_iid_iunknown) &&
        !dwrite_guid_equal(iid, primary) &&
        (!secondary || !dwrite_guid_equal(iid, secondary)))
        return DWRITE_E_NOINTERFACE;
    *object = self;
    return 0;
}

static LONG WINAPI dwrite_collection_QueryInterface(PVOID self, LPCGUID iid,
                                                     PVOID *object)
{
    LONG status = dwrite_static_QueryInterface(
        self, iid, &dwrite_iid_collection, NULL, object);
    if (status >= 0)
        dwrite_collection_AddRef(self);
    return status;
}

static UINT WINAPI dwrite_collection_GetFontFamilyCount(PVOID self)
{
    DWRITE_COLLECTION_STUB *collection = (DWRITE_COLLECTION_STUB *)self;
    dwrite_trace("[DWRITE] collection.GetFontFamilyCount\n");
    return collection ? collection->family_count : 0;
}

static LONG WINAPI dwrite_collection_GetFontFamily(PVOID self, UINT index,
                                                    PVOID *family)
{
    DWRITE_COLLECTION_STUB *collection = (DWRITE_COLLECTION_STUB *)self;
    dwrite_trace("[DWRITE] collection.GetFontFamily\n");
    if (!family) return DWRITE_E_POINTER;
    *family = NULL;
    if (!collection || index >= collection->family_count)
        return DWRITE_E_INVALIDARG;
    *family = &collection->family;
    dwrite_family_AddRef(*family);
    return 0;
}

static LONG WINAPI dwrite_collection_FindFamilyName(PVOID self,
                                                     PCWSTR name,
                                                     UINT *index,
                                                     BOOL *exists)
{
    DWRITE_COLLECTION_STUB *collection = (DWRITE_COLLECTION_STUB *)self;
    dwrite_trace("[DWRITE] collection.FindFamilyName -> family 0\n");
    if (!name || !index || !exists) return DWRITE_E_POINTER;
    *index = 0;
    *exists = collection && collection->family_count != 0;
    return 0;
}

static LONG WINAPI dwrite_collection_GetFontFromFontFace(PVOID self,
                                                         PVOID face,
                                                         PVOID *font)
{
    DWRITE_COLLECTION_STUB *collection = (DWRITE_COLLECTION_STUB *)self;
    DWRITE_FONT_FACE_STUB *font_face = (DWRITE_FONT_FACE_STUB *)face;
    dwrite_trace("[DWRITE] collection.GetFontFromFontFace\n");
    if (!font) return DWRITE_E_POINTER;
    *font = NULL;
    if (!collection || !font_face || font_face->lpVtbl != dwrite_face_vtbl ||
        font_face->font_file_count != collection->font_file_count)
        return DWRITE_E_INVALIDARG;
    for (UINT i = 0; i < collection->font_file_count; i++)
        if (font_face->font_files[i] != collection->font_files[i])
            return DWRITE_E_INVALIDARG;
    *font = &collection->family.font;
    dwrite_font_AddRef(*font);
    return 0;
}

static LONG WINAPI dwrite_family_QueryInterface(PVOID self, LPCGUID iid,
                                                 PVOID *object)
{
    LONG status = dwrite_static_QueryInterface(
        self, iid, &dwrite_iid_family, &dwrite_iid_font_list, object);
    if (status >= 0) dwrite_family_AddRef(self);
    return status;
}

static LONG WINAPI dwrite_family_GetFontCollection(PVOID self,
                                                    PVOID *collection)
{
    DWRITE_FAMILY_STUB *family = (DWRITE_FAMILY_STUB *)self;
    if (!collection) return DWRITE_E_POINTER;
    *collection = family ? family->collection : NULL;
    if (!*collection) return DWRITE_E_INVALIDARG;
    dwrite_collection_AddRef(*collection);
    return 0;
}

static UINT WINAPI dwrite_family_GetFontCount(PVOID self)
{
    DWRITE_FAMILY_STUB *family = (DWRITE_FAMILY_STUB *)self;
    UINT count = family && family->collection &&
                 family->collection->font_file_count ? 1 : 0;
    dwrite_trace(count ? "[DWRITE] family.GetFontCount -> 1\n" :
                         "[DWRITE] family.GetFontCount -> 0\n");
    return count;
}

static LONG WINAPI dwrite_family_GetFont(PVOID self, UINT index, PVOID *font)
{
    DWRITE_FAMILY_STUB *family = (DWRITE_FAMILY_STUB *)self;
    dwrite_trace("[DWRITE] family.GetFont\n");
    if (!font) return DWRITE_E_POINTER;
    *font = NULL;
    if (!family || !family->collection ||
        !family->collection->font_file_count || index != 0)
        return DWRITE_E_INVALIDARG;
    *font = &family->font;
    dwrite_font_AddRef(*font);
    return 0;
}

static LONG WINAPI dwrite_family_GetFamilyNames(PVOID self, PVOID *names)
{
    (void)self;
    dwrite_trace("[DWRITE] family.GetFamilyNames\n");
    if (!names) return DWRITE_E_POINTER;
    *names = &dwrite_family_names;
    return 0;
}

static LONG WINAPI dwrite_family_GetFirstMatchingFont(PVOID self, UINT weight,
                                                       UINT stretch, UINT style,
                                                       PVOID *font)
{
    DWRITE_FAMILY_STUB *family = (DWRITE_FAMILY_STUB *)self;
    (void)weight;
    (void)stretch;
    (void)style;
    dwrite_trace("[DWRITE] family.GetFirstMatchingFont\n");
    if (!font) return DWRITE_E_POINTER;
    *font = NULL;
    if (!family || !family->collection ||
        !family->collection->font_file_count)
        return DWRITE_E_NOFONT;
    *font = &family->font;
    dwrite_font_AddRef(*font);
    return 0;
}

static LONG WINAPI dwrite_family_GetMatchingFonts(PVOID self, UINT weight,
                                                   UINT stretch, UINT style,
                                                   PVOID *fonts)
{
    (void)weight;
    (void)stretch;
    (void)style;
    if (!fonts) return DWRITE_E_POINTER;
    *fonts = self;
    dwrite_family_AddRef(self);
    return 0;
}

static LONG WINAPI dwrite_font_QueryInterface(PVOID self, LPCGUID iid,
                                               PVOID *object)
{
    LONG status = dwrite_static_QueryInterface(
        self, iid, &dwrite_iid_font, NULL, object);
    if (status >= 0) dwrite_font_AddRef(self);
    return status;
}

static LONG WINAPI dwrite_font_GetFontFamily(PVOID self, PVOID *family)
{
    DWRITE_FONT_STUB *font = (DWRITE_FONT_STUB *)self;
    if (!family) return DWRITE_E_POINTER;
    *family = font ? font->family : NULL;
    if (!*family) return DWRITE_E_INVALIDARG;
    dwrite_family_AddRef(*family);
    return 0;
}

static UINT WINAPI dwrite_font_GetWeight(PVOID self)
{
    (void)self;
    return 400;
}

static UINT WINAPI dwrite_font_GetStretch(PVOID self)
{
    (void)self;
    return 5;
}

static UINT WINAPI dwrite_font_GetStyle(PVOID self)
{
    (void)self;
    return 0;
}

static BOOL WINAPI dwrite_font_IsSymbolFont(PVOID self)
{
    (void)self;
    return FALSE;
}

static LONG WINAPI dwrite_font_GetFaceNames(PVOID self, PVOID *names)
{
    (void)self;
    if (!names) return DWRITE_E_POINTER;
    *names = &dwrite_face_names;
    return 0;
}

static LONG WINAPI dwrite_font_GetInformationalStrings(PVOID self, UINT id,
                                                        PVOID *strings,
                                                        BOOL *exists)
{
    (void)self;
    (void)id;
    if (!strings || !exists) return DWRITE_E_POINTER;
    *strings = NULL;
    *exists = FALSE;
    return 0;
}

static UINT WINAPI dwrite_font_GetSimulations(PVOID self)
{
    (void)self;
    return 0;
}

static void dwrite_fill_font_metrics(DWRITE_FONT_METRICS_STUB *metrics)
{
    if (!metrics) return;
    metrics->design_units_per_em = 2048;
    metrics->ascent = 1900;
    metrics->descent = 500;
    metrics->line_gap = 0;
    metrics->cap_height = 1462;
    metrics->x_height = 1024;
    metrics->underline_position = -200;
    metrics->underline_thickness = 100;
    metrics->strikethrough_position = 600;
    metrics->strikethrough_thickness = 100;
}

static void WINAPI dwrite_font_GetMetrics(PVOID self,
                                          DWRITE_FONT_METRICS_STUB *metrics)
{
    (void)self;
    dwrite_fill_font_metrics(metrics);
}

static LONG WINAPI dwrite_font_HasCharacter(PVOID self, UINT value,
                                             BOOL *exists)
{
    (void)self;
    (void)value;
    if (!exists) return DWRITE_E_POINTER;
    *exists = TRUE;
    return 0;
}

static LONG WINAPI dwrite_font_CreateFontFace(PVOID self, PVOID *face)
{
    DWRITE_FONT_STUB *font = (DWRITE_FONT_STUB *)self;
    dwrite_trace("[DWRITE] font.CreateFontFace\n");
    if (!face) return DWRITE_E_POINTER;
    *face = NULL;
    if (!font || !font->family || !font->family->collection)
        return DWRITE_E_INVALIDARG;
    DWRITE_COLLECTION_STUB *collection = font->family->collection;
    if (!collection->font_file_count) return DWRITE_E_NOFONT;
    return dwrite_factory_CreateFontFace(
        &dwrite_factory, DWRITE_FONT_FACE_TYPE_UNKNOWN,
        collection->font_file_count, (PVOID const *)collection->font_files,
        0, 0, face);
}

static LONG dwrite_open_font_stream(PVOID font_file, PVOID *stream)
{
    if (!stream) return DWRITE_E_POINTER;
    *stream = NULL;
    if (!font_file || !*(PVOID **)font_file)
        return DWRITE_E_INVALIDARG;
    PVOID *file_vtbl = *(PVOID **)font_file;
    if (!file_vtbl[3] || !file_vtbl[4]) return DWRITE_E_INVALIDARG;

    typedef LONG (WINAPI *GET_KEY_FN)(PVOID, const void **, UINT *);
    typedef LONG (WINAPI *GET_LOADER_FN)(PVOID, PVOID *);
    typedef LONG (WINAPI *CREATE_STREAM_FN)(PVOID, const void *, UINT,
                                             PVOID *);
    const void *key = NULL;
    UINT key_size = 0;
    LONG status = ((GET_KEY_FN)file_vtbl[3])(
        font_file, &key, &key_size);
    if (status < 0 || !key || !key_size)
        return status < 0 ? status : DWRITE_E_FILEFORMAT;

    PVOID loader = NULL;
    status = ((GET_LOADER_FN)file_vtbl[4])(font_file, &loader);
    if (status < 0 || !loader || !*(PVOID **)loader)
        return status < 0 ? status : DWRITE_E_FILEFORMAT;
    PVOID *loader_vtbl = *(PVOID **)loader;
    if (!loader_vtbl[3]) {
        dwrite_object_release(loader);
        return DWRITE_E_FILEFORMAT;
    }
    status = ((CREATE_STREAM_FN)loader_vtbl[3])(
        loader, key, key_size, stream);
    dwrite_object_release(loader);
    if (status < 0 || !*stream)
        return status < 0 ? status : DWRITE_E_FILEFORMAT;
    return 0;
}

static ULONG WINAPI dwrite_face_AddRef(PVOID self)
{
    DWRITE_FONT_FACE_STUB *face = (DWRITE_FONT_FACE_STUB *)self;
    if (!face) return 0;
    return __atomic_add_fetch(&face->refs, 1, __ATOMIC_RELAXED);
}

static ULONG WINAPI dwrite_face_Release(PVOID self)
{
    DWRITE_FONT_FACE_STUB *face = (DWRITE_FONT_FACE_STUB *)self;
    if (!face) return 0;
    ULONG refs = __atomic_sub_fetch(&face->refs, 1, __ATOMIC_ACQ_REL);
    if (!refs) {
        for (UINT i = 0; i < face->font_file_count; i++)
            dwrite_object_release(face->font_files[i]);
        kfree(face);
    }
    return refs;
}

static LONG WINAPI dwrite_face_QueryInterface(PVOID self, LPCGUID iid,
                                               PVOID *object)
{
    LONG status = dwrite_static_QueryInterface(
        self, iid, &dwrite_iid_face, NULL, object);
    if (status >= 0) dwrite_face_AddRef(self);
    return status;
}

static UINT WINAPI dwrite_face_GetType(PVOID self)
{
    DWRITE_FONT_FACE_STUB *face = (DWRITE_FONT_FACE_STUB *)self;
    return face ? face->face_type : DWRITE_FONT_FACE_TYPE_UNKNOWN;
}

static LONG WINAPI dwrite_face_GetFiles(PVOID self, UINT *file_count,
                                        PVOID *files)
{
    DWRITE_FONT_FACE_STUB *face = (DWRITE_FONT_FACE_STUB *)self;
    if (!file_count) return DWRITE_E_POINTER;
    if (!face || face->lpVtbl != dwrite_face_vtbl)
        return DWRITE_E_INVALIDARG;
    UINT required = face->font_file_count;
    if (!files) {
        *file_count = required;
        dwrite_trace("[DWRITE] face.GetFiles query\n");
        return 0;
    }
    UINT capacity = *file_count;
    *file_count = required;
    if (capacity < required) return DWRITE_E_INSUFFICIENT_BUFFER;
    for (UINT i = 0; i < required; i++) {
        files[i] = face->font_files[i];
        dwrite_object_addref(files[i]);
    }
    dwrite_trace("[DWRITE] face.GetFiles returned files\n");
    return 0;
}

static UINT WINAPI dwrite_face_GetIndex(PVOID self)
{
    DWRITE_FONT_FACE_STUB *face = (DWRITE_FONT_FACE_STUB *)self;
    return face ? face->face_index : 0;
}

static UINT WINAPI dwrite_face_GetSimulations(PVOID self)
{
    DWRITE_FONT_FACE_STUB *face = (DWRITE_FONT_FACE_STUB *)self;
    return face ? face->simulations : 0;
}

static BOOL WINAPI dwrite_face_IsSymbolFont(PVOID self)
{
    (void)self;
    return FALSE;
}

static void WINAPI dwrite_face_GetMetrics(PVOID self,
                                          DWRITE_FONT_METRICS_STUB *metrics)
{
    (void)self;
    dwrite_fill_font_metrics(metrics);
}

static USHORT WINAPI dwrite_face_GetGlyphCount(PVOID self)
{
    const void *maxp = NULL;
    UINT maxp_size = 0;
    PVOID context = NULL;
    BOOL exists = FALSE;
    USHORT glyph_count = 256;

    LONG status = dwrite_face_TryGetFontTable(
        self, DWRITE_OPENTYPE_TAG_MAXP, &maxp, &maxp_size, &context,
        &exists);
    if (status >= 0 && exists && maxp && maxp_size >= 6)
        glyph_count = dwrite_read_be16((const BYTE *)maxp + 4);
    if (context)
        dwrite_face_ReleaseFontTable(self, context);
    return glyph_count;
}

static void dwrite_fill_glyph_metrics(DWRITE_GLYPH_METRICS_STUB *metrics,
                                      UINT count)
{
    if (!metrics) return;
    for (UINT i = 0; i < count; i++) {
        metrics[i].left_side_bearing = 0;
        metrics[i].advance_width = 1024;
        metrics[i].right_side_bearing = 0;
        metrics[i].top_side_bearing = 0;
        metrics[i].advance_height = 2048;
        metrics[i].bottom_side_bearing = 0;
        metrics[i].vertical_origin_y = 1900;
    }
}

static LONG WINAPI dwrite_face_GetDesignGlyphMetrics(
    PVOID self, const USHORT *glyph_indices, UINT glyph_count,
    DWRITE_GLYPH_METRICS_STUB *metrics, BOOL sideways)
{
    (void)self;
    (void)glyph_indices;
    (void)sideways;
    if (!metrics) return DWRITE_E_POINTER;
    dwrite_fill_glyph_metrics(metrics, glyph_count);
    return 0;
}

static LONG WINAPI dwrite_face_GetGlyphIndices(PVOID self,
                                                const UINT *codepoints,
                                                UINT count,
                                                USHORT *glyph_indices)
{
    if (!codepoints || !glyph_indices) return DWRITE_E_POINTER;

    const void *cmap = NULL;
    UINT cmap_size = 0;
    PVOID context = NULL;
    BOOL exists = FALSE;
    LONG status = dwrite_face_TryGetFontTable(
        self, DWRITE_OPENTYPE_TAG_CMAP, &cmap, &cmap_size, &context,
        &exists);
    if (status < 0)
        return status;

    for (UINT i = 0; i < count; i++)
        glyph_indices[i] = exists && cmap
            ? dwrite_cmap_lookup((const BYTE *)cmap, cmap_size,
                                 codepoints[i])
            : (codepoints[i] && codepoints[i] < 256
                ? (USHORT)codepoints[i] : 0);
    if (context)
        dwrite_face_ReleaseFontTable(self, context);
    return 0;
}

static LONG WINAPI dwrite_face_TryGetFontTable(PVOID self, UINT table_tag,
                                               const void **table_data,
                                               UINT *table_size,
                                               PVOID *context, BOOL *exists)
{
    DWRITE_FONT_FACE_STUB *face = (DWRITE_FONT_FACE_STUB *)self;
    if (!table_data || !table_size || !context || !exists)
        return DWRITE_E_POINTER;
    *table_data = NULL;
    *table_size = 0;
    *context = NULL;
    *exists = FALSE;
    if (!face || !face->font_file_count) return DWRITE_E_INVALIDARG;

    PVOID stream = NULL;
    LONG status = dwrite_open_font_stream(face->font_files[0], &stream);
    if (status < 0) return status;
    PVOID *stream_vtbl = *(PVOID **)stream;
    if (!stream_vtbl || !stream_vtbl[3] || !stream_vtbl[4]) {
        dwrite_object_release(stream);
        return DWRITE_E_FILEFORMAT;
    }
    typedef LONG (WINAPI *READ_FRAGMENT_FN)(PVOID, const void **, uint64_t,
                                             uint64_t, PVOID *);
    typedef void (WINAPI *RELEASE_FRAGMENT_FN)(PVOID, PVOID);
    READ_FRAGMENT_FN read_fragment = (READ_FRAGMENT_FN)stream_vtbl[3];
    RELEASE_FRAGMENT_FN release_fragment =
        (RELEASE_FRAGMENT_FN)stream_vtbl[4];

    const BYTE *header = NULL;
    PVOID header_context = NULL;
    PVOID offset_context = NULL;
    PVOID directory_context = NULL;
    PVOID fragment_context = NULL;
    status = read_fragment(stream, (const void **)&header, 0, 12,
                           &header_context);
    if (status < 0 || !header) goto table_error;
    uint64_t face_offset = 0;
    if (dwrite_read_be32(header) == 0x74746366U) { /* ttcf */
        UINT count = dwrite_read_be32(header + 8);
        release_fragment(stream, header_context);
        header = NULL;
        header_context = NULL;
        if (face->face_index >= count) {
            status = DWRITE_E_FILEFORMAT;
            goto table_error;
        }
        const BYTE *offset_data = NULL;
        status = read_fragment(stream, (const void **)&offset_data,
                               12ULL + (uint64_t)face->face_index * 4ULL,
                               4, &offset_context);
        if (status < 0 || !offset_data) goto table_error;
        face_offset = dwrite_read_be32(offset_data);
        release_fragment(stream, offset_context);
        offset_context = NULL;
        status = read_fragment(stream, (const void **)&header, face_offset,
                               12, &header_context);
        if (status < 0 || !header) goto table_error;
    }

    UINT table_count = ((UINT)header[4] << 8) | header[5];
    release_fragment(stream, header_context);
    header = NULL;
    header_context = NULL;
    if (!table_count || table_count > 4096) {
        status = DWRITE_E_FILEFORMAT;
        goto table_error;
    }

    const BYTE *directory = NULL;
    uint64_t directory_size = (uint64_t)table_count * 16ULL;
    status = read_fragment(stream, (const void **)&directory,
                           face_offset + 12ULL, directory_size,
                           &directory_context);
    if (status < 0 || !directory) goto table_error;

    UINT file_tag = ((table_tag & 0x000000FFU) << 24) |
                    ((table_tag & 0x0000FF00U) << 8) |
                    ((table_tag & 0x00FF0000U) >> 8) |
                    ((table_tag & 0xFF000000U) >> 24);
    uint64_t found_offset = 0;
    UINT found_size = 0;
    for (UINT i = 0; i < table_count; i++) {
        const BYTE *record = directory + (uint64_t)i * 16ULL;
        if (dwrite_read_be32(record) == file_tag) {
            found_offset = dwrite_read_be32(record + 8);
            found_size = dwrite_read_be32(record + 12);
            break;
        }
    }
    release_fragment(stream, directory_context);
    directory_context = NULL;
    if (!found_size) {
        dwrite_object_release(stream);
        return 0;
    }

    const void *font_table = NULL;
    status = read_fragment(stream, &font_table, found_offset, found_size,
                           &fragment_context);
    if (status < 0 || !font_table) goto table_error;
    DWRITE_FONT_TABLE_CONTEXT *table_context =
        (DWRITE_FONT_TABLE_CONTEXT *)kmalloc(sizeof(*table_context));
    if (!table_context) {
        release_fragment(stream, fragment_context);
        fragment_context = NULL;
        status = DWRITE_E_OUTOFMEMORY;
        goto table_error;
    }
    table_context->stream = stream;
    table_context->fragment_context = fragment_context;
    *table_data = font_table;
    *table_size = found_size;
    *context = table_context;
    *exists = TRUE;
    return 0;

table_error:
    if (header_context) release_fragment(stream, header_context);
    if (offset_context) release_fragment(stream, offset_context);
    if (directory_context) release_fragment(stream, directory_context);
    if (fragment_context) release_fragment(stream, fragment_context);
    dwrite_object_release(stream);
    return status < 0 ? status : DWRITE_E_FILEFORMAT;
}

static void WINAPI dwrite_face_ReleaseFontTable(PVOID self, PVOID context)
{
    (void)self;
    DWRITE_FONT_TABLE_CONTEXT *table_context =
        (DWRITE_FONT_TABLE_CONTEXT *)context;
    if (!table_context || !table_context->stream) return;
    PVOID *stream_vtbl = *(PVOID **)table_context->stream;
    if (stream_vtbl && stream_vtbl[4]) {
        typedef void (WINAPI *RELEASE_FRAGMENT_FN)(PVOID, PVOID);
        ((RELEASE_FRAGMENT_FN)stream_vtbl[4])(
            table_context->stream, table_context->fragment_context);
    }
    dwrite_object_release(table_context->stream);
    kfree(table_context);
}

static LONG WINAPI dwrite_face_GetGlyphRunOutline(PVOID self, float em_size,
                                                  const USHORT *indices,
                                                  const float *advances,
                                                  const PVOID offsets,
                                                  UINT count, BOOL sideways,
                                                  BOOL rtl, PVOID sink)
{
    (void)self;
    (void)em_size;
    (void)indices;
    (void)advances;
    (void)offsets;
    (void)count;
    (void)sideways;
    (void)rtl;
    if (!sink) return DWRITE_E_POINTER;
    dwrite_trace("[DWRITE] face.GetGlyphRunOutline -> empty geometry\n");
    return 0;
}

static LONG WINAPI dwrite_face_GetRecommendedRenderingMode(
    PVOID self, float em_size, float pixels_per_dip, UINT measuring_mode,
    PVOID params, UINT *rendering_mode)
{
    (void)self;
    (void)em_size;
    (void)pixels_per_dip;
    (void)measuring_mode;
    (void)params;
    if (!rendering_mode) return DWRITE_E_POINTER;
    *rendering_mode = 4;
    return 0;
}

static LONG WINAPI dwrite_face_GetGdiCompatibleMetrics(
    PVOID self, float em_size, float pixels_per_dip, const PVOID transform,
    DWRITE_FONT_METRICS_STUB *metrics)
{
    (void)self;
    (void)em_size;
    (void)pixels_per_dip;
    (void)transform;
    if (!metrics) return DWRITE_E_POINTER;
    dwrite_fill_font_metrics(metrics);
    return 0;
}

static LONG WINAPI dwrite_face_GetGdiCompatibleGlyphMetrics(
    PVOID self, float em_size, float pixels_per_dip, const PVOID transform,
    BOOL use_gdi_natural, const USHORT *indices, UINT count,
    DWRITE_GLYPH_METRICS_STUB *metrics, BOOL sideways)
{
    (void)self;
    (void)em_size;
    (void)pixels_per_dip;
    (void)transform;
    (void)use_gdi_natural;
    (void)indices;
    (void)sideways;
    if (!metrics) return DWRITE_E_POINTER;
    dwrite_fill_glyph_metrics(metrics, count);
    return 0;
}

static LONG WINAPI dwrite_strings_QueryInterface(PVOID self, LPCGUID iid,
                                                  PVOID *object)
{
    return dwrite_static_QueryInterface(self, iid, &dwrite_iid_strings,
                                        NULL, object);
}

static UINT dwrite_wstrlen(PCWSTR value)
{
    UINT len = 0;
    if (value)
        while (value[len]) len++;
    return len;
}

static LONG dwrite_copy_wstring(PCWSTR source, PWSTR dest, UINT size)
{
    UINT len = dwrite_wstrlen(source);
    if (!dest) return DWRITE_E_POINTER;
    if (size <= len) return DWRITE_E_INSUFFICIENT_BUFFER;
    for (UINT i = 0; i <= len; i++) dest[i] = source[i];
    return 0;
}

static UINT WINAPI dwrite_strings_GetCount(PVOID self)
{
    (void)self;
    return 1;
}

static LONG WINAPI dwrite_strings_FindLocaleName(PVOID self, PCWSTR locale,
                                                  UINT *index, BOOL *exists)
{
    (void)self;
    (void)locale;
    if (!index || !exists) return DWRITE_E_POINTER;
    *index = 0;
    *exists = TRUE;
    return 0;
}

static LONG WINAPI dwrite_strings_GetLocaleNameLength(PVOID self, UINT index,
                                                       UINT *length)
{
    (void)self;
    if (!length) return DWRITE_E_POINTER;
    if (index != 0) return DWRITE_E_INVALIDARG;
    *length = dwrite_wstrlen(dwrite_locale_name);
    return 0;
}

static LONG WINAPI dwrite_strings_GetLocaleName(PVOID self, UINT index,
                                                 PWSTR locale, UINT size)
{
    (void)self;
    if (index != 0) return DWRITE_E_INVALIDARG;
    return dwrite_copy_wstring(dwrite_locale_name, locale, size);
}

static LONG WINAPI dwrite_strings_GetStringLength(PVOID self, UINT index,
                                                   UINT *length)
{
    DWRITE_STRINGS_STUB *strings = (DWRITE_STRINGS_STUB *)self;
    if (!length) return DWRITE_E_POINTER;
    if (index != 0) return DWRITE_E_INVALIDARG;
    *length = dwrite_wstrlen(strings->value);
    return 0;
}

static LONG WINAPI dwrite_strings_GetString(PVOID self, UINT index,
                                             PWSTR value, UINT size)
{
    DWRITE_STRINGS_STUB *strings = (DWRITE_STRINGS_STUB *)self;
    if (index != 0) return DWRITE_E_INVALIDARG;
    return dwrite_copy_wstring(strings->value, value, size);
}

static LONG WINAPI dwrite_factory_RegisterFontCollectionLoader(PVOID self,
                                                                 PVOID loader)
{
    (void)self;
    LONG status = dwrite_register_loader(dwrite_collection_loaders, loader);
    if (status >= 0)
        dwrite_trace("[DWRITE] registered font collection loader\n");
    return status;
}

static LONG WINAPI dwrite_factory_UnregisterFontCollectionLoader(PVOID self,
                                                                   PVOID loader)
{
    (void)self;
    LONG status = dwrite_unregister_loader(dwrite_collection_loaders, loader);
    if (status >= 0)
        dwrite_trace("[DWRITE] unregistered font collection loader\n");
    return status;
}

static LONG WINAPI dwrite_factory_RegisterFontFileLoader(PVOID self,
                                                           PVOID loader)
{
    (void)self;
    LONG status = dwrite_register_loader(dwrite_file_loaders, loader);
    if (status >= 0)
        dwrite_trace("[DWRITE] registered font file loader\n");
    return status;
}

static LONG WINAPI dwrite_factory_UnregisterFontFileLoader(PVOID self,
                                                             PVOID loader)
{
    (void)self;
    LONG status = dwrite_unregister_loader(dwrite_file_loaders, loader);
    if (status >= 0)
        dwrite_trace("[DWRITE] unregistered font file loader\n");
    return status;
}

static LONG WINAPI dwrite_factory_CreateFontFileReference(
    PVOID self, PCWSTR file_path, const PVOID last_write_time,
    PVOID *font_file)
{
    (void)self;
    (void)last_write_time;
    if (!font_file) return DWRITE_E_POINTER;
    *font_file = NULL;
    if (!file_path) return DWRITE_E_INVALIDARG;

    char path[4096];
    LONG status = dwrite_utf16_path_to_utf8(file_path, path, sizeof(path));
    if (status < 0) return status;
    status = dwrite_create_local_font_file(path, font_file);
    if (status >= 0)
        dwrite_trace("[DWRITE] factory.CreateFontFileReference\n");
    return status;
}

static LONG WINAPI dwrite_factory_CreateCustomFontFileReference(
    PVOID self, const void *key, UINT key_size, PVOID loader, PVOID *font_file)
{
    (void)self;
    if (!font_file) return DWRITE_E_POINTER;
    *font_file = NULL;
    if (!key || !key_size || key_size > 1024 * 1024 || !loader)
        return DWRITE_E_INVALIDARG;

    PVOID retained_loader = dwrite_acquire_registered_loader(
        dwrite_file_loaders, loader);
    if (!retained_loader)
        return DWRITE_E_INVALIDARG;

    DWRITE_FONT_FILE_STUB *file = (DWRITE_FONT_FILE_STUB *)kmalloc(
        sizeof(*file) + key_size);
    if (!file) {
        dwrite_object_release(retained_loader);
        return DWRITE_E_OUTOFMEMORY;
    }
    file->lpVtbl = dwrite_font_file_vtbl;
    file->refs = 1;
    file->loader = retained_loader;
    file->key_size = key_size;
    file->gsub_cache_lock = 0;
    file->gsub_cache_count = 0;
    gdi_memcpy(file->key, key, key_size);
    *font_file = file;
    return 0;
}

static void dwrite_release_font_files(PVOID *files, UINT count)
{
    if (!files) return;
    for (UINT i = 0; i < count; i++)
        dwrite_object_release(files[i]);
    kfree(files);
}

static LONG WINAPI dwrite_factory_CreateCustomFontCollection(
    PVOID self, PVOID loader, const void *key, UINT key_size,
    PVOID *font_collection)
{
    if (!font_collection) return DWRITE_E_POINTER;
    *font_collection = NULL;
    if (!loader || (key_size && !key) || key_size > 1024 * 1024)
        return DWRITE_E_INVALIDARG;

    PVOID retained_loader = dwrite_acquire_registered_loader(
        dwrite_collection_loaders, loader);
    if (!retained_loader)
        return DWRITE_E_INVALIDARG;

    PVOID *loader_vtbl = *(PVOID **)retained_loader;
    if (!loader_vtbl || !loader_vtbl[3]) {
        dwrite_object_release(retained_loader);
        return DWRITE_E_INVALIDARG;
    }

    typedef LONG (WINAPI *CREATE_ENUMERATOR_FN)(PVOID, PVOID, const void *,
                                                 UINT, PVOID *);
    PVOID enumerator = NULL;
    LONG status = ((CREATE_ENUMERATOR_FN)loader_vtbl[3])(
        retained_loader, self, key, key_size, &enumerator);
    dwrite_object_release(retained_loader);
    if (status < 0 || !enumerator)
        return status < 0 ? status : DWRITE_E_NOFONT;

    PVOID *enum_vtbl = *(PVOID **)enumerator;
    if (!enum_vtbl || !enum_vtbl[3] || !enum_vtbl[4]) {
        dwrite_object_release(enumerator);
        return DWRITE_E_INVALIDARG;
    }

    typedef LONG (WINAPI *MOVE_NEXT_FN)(PVOID, BOOL *);
    typedef LONG (WINAPI *GET_CURRENT_FILE_FN)(PVOID, PVOID *);
    PVOID *files = NULL;
    UINT file_count = 0;
    UINT capacity = 0;

    for (;;) {
        BOOL has_file = FALSE;
        status = ((MOVE_NEXT_FN)enum_vtbl[3])(enumerator, &has_file);
        if (status < 0 || !has_file)
            break;
        if (file_count >= 4096) {
            status = DWRITE_E_OUTOFMEMORY;
            break;
        }

        PVOID file = NULL;
        status = ((GET_CURRENT_FILE_FN)enum_vtbl[4])(enumerator, &file);
        if (status < 0 || !file) {
            if (status >= 0) status = DWRITE_E_FILEFORMAT;
            break;
        }

        PVOID *file_vtbl = *(PVOID **)file;
        if (!file_vtbl || !file_vtbl[5]) {
            dwrite_object_release(file);
            status = DWRITE_E_FILEFORMAT;
            break;
        }
        typedef LONG (WINAPI *ANALYZE_FILE_FN)(PVOID, BOOL *, UINT *,
                                                UINT *, UINT *);
        BOOL supported = FALSE;
        UINT file_type = 0, face_type = 0, face_count = 0;
        status = ((ANALYZE_FILE_FN)file_vtbl[5])(
            file, &supported, &file_type, &face_type, &face_count);
        if (status < 0) {
            dwrite_object_release(file);
            break;
        }
        if (!supported || !face_count) {
            dwrite_object_release(file);
            continue;
        }

        if (file_count == capacity) {
            UINT next_capacity = capacity ? capacity * 2 : 4;
            PVOID *grown = (PVOID *)krealloc(
                files, (uint64_t)next_capacity * sizeof(*files));
            if (!grown) {
                dwrite_object_release(file);
                status = DWRITE_E_OUTOFMEMORY;
                break;
            }
            files = grown;
            capacity = next_capacity;
        }
        files[file_count++] = file;
    }
    dwrite_object_release(enumerator);

    if (status < 0) {
        dwrite_release_font_files(files, file_count);
        return status;
    }
    if (!file_count) {
        kfree(files);
        return DWRITE_E_NOFONT;
    }

    DWRITE_COLLECTION_STUB *collection =
        (DWRITE_COLLECTION_STUB *)kmalloc(sizeof(*collection));
    if (!collection) {
        dwrite_release_font_files(files, file_count);
        return DWRITE_E_OUTOFMEMORY;
    }
    collection->lpVtbl = dwrite_collection_vtbl;
    collection->refs = 1;
    collection->family_count = 1;
    collection->font_file_count = file_count;
    collection->heap_owned = TRUE;
    collection->font_files = files;
    collection->family.lpVtbl = dwrite_family_vtbl;
    collection->family.collection = collection;
    collection->family.font.lpVtbl = dwrite_font_vtbl;
    collection->family.font.family = &collection->family;
    *font_collection = collection;
    dwrite_trace("[DWRITE] factory.CreateCustomFontCollection\n");
    return 0;
}

static LONG WINAPI dwrite_factory_CreateFontFace(
    PVOID self, UINT requested_face_type, UINT file_count,
    PVOID const *font_files, UINT face_index, UINT simulations,
    PVOID *font_face)
{
    (void)self;
    if (!font_face) return DWRITE_E_POINTER;
    *font_face = NULL;
    if (!file_count || !font_files || (simulations & ~3U))
        return DWRITE_E_INVALIDARG;

    UINT available_faces = 0;
    UINT resolved_face_type = DWRITE_FONT_FACE_TYPE_UNKNOWN;
    for (UINT i = 0; i < file_count; i++) {
        if (!font_files[i] || !*(PVOID **)font_files[i] ||
            !(*(PVOID **)font_files[i])[5])
            return DWRITE_E_INVALIDARG;
        typedef LONG (WINAPI *ANALYZE_FILE_FN)(PVOID, BOOL *, UINT *,
                                                UINT *, UINT *);
        BOOL supported = FALSE;
        UINT file_type = 0, face_type = 0, faces = 0;
        LONG status = ((ANALYZE_FILE_FN)(*(PVOID **)font_files[i])[5])(
            font_files[i], &supported, &file_type, &face_type, &faces);
        if (status < 0) return status;
        if (!supported || !faces) return DWRITE_E_FILEFORMAT;
        if (resolved_face_type == DWRITE_FONT_FACE_TYPE_UNKNOWN)
            resolved_face_type = face_type;
        if (requested_face_type != DWRITE_FONT_FACE_TYPE_UNKNOWN &&
            requested_face_type != face_type)
            return DWRITE_E_INVALIDARG;
        if (available_faces > UINT32_MAX - faces)
            return DWRITE_E_INVALIDARG;
        available_faces += faces;
    }
    if (face_index >= available_faces)
        return DWRITE_E_INVALIDARG;

    if (file_count > (UINT32_MAX - sizeof(DWRITE_FONT_FACE_STUB)) /
                     sizeof(PVOID))
        return DWRITE_E_OUTOFMEMORY;
    DWRITE_FONT_FACE_STUB *face = (DWRITE_FONT_FACE_STUB *)kmalloc(
        sizeof(*face) + (uint64_t)file_count * sizeof(PVOID));
    if (!face) return DWRITE_E_OUTOFMEMORY;
    face->lpVtbl = dwrite_face_vtbl;
    face->refs = 1;
    face->face_type = requested_face_type == DWRITE_FONT_FACE_TYPE_UNKNOWN
        ? resolved_face_type : requested_face_type;
    face->face_index = face_index;
    face->simulations = simulations;
    face->font_file_count = file_count;
    for (UINT i = 0; i < file_count; i++) {
        face->font_files[i] = font_files[i];
        dwrite_object_addref(face->font_files[i]);
    }
    *font_face = face;
    dwrite_trace("[DWRITE] factory.CreateFontFace\n");
    return 0;
}

static volatile ULONG dwrite_system_font_lock;
static BOOL dwrite_system_font_missing_reported;

static void dwrite_initialize_system_font_collection(void)
{
    if (dwrite_collection.font_file_count) return;
    while (__atomic_exchange_n(&dwrite_system_font_lock, 1,
                               __ATOMIC_ACQUIRE))
        __asm__ volatile ("pause");
    if (dwrite_collection.font_file_count) {
        __atomic_store_n(&dwrite_system_font_lock, 0, __ATOMIC_RELEASE);
        return;
    }

    static const char *const candidates[] = {
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "C:\\System\\Windows\\Fonts\\segoeui.ttf",
        "C:\\System\\Program Files\\Steam\\clientui\\fonts\\GoNotoKurrent-Regular.ttf",
        "C:\\Program Files\\Steam\\clientui\\fonts\\GoNotoKurrent-Regular.ttf",
        NULL
    };
    for (UINT i = 0; candidates[i]; i++) {
        PVOID file = NULL;
        if (dwrite_create_local_font_file(candidates[i], &file) < 0)
            continue;
        BOOL supported = FALSE;
        UINT file_type = 0, face_type = 0, face_count = 0;
        LONG status = dwrite_font_file_Analyze(
            file, &supported, &file_type, &face_type, &face_count);
        if (status < 0 || !supported || !face_count) {
            dwrite_object_release(file);
            continue;
        }
        PVOID *files = (PVOID *)kmalloc(sizeof(PVOID));
        if (!files) {
            dwrite_object_release(file);
            break;
        }
        files[0] = file;
        dwrite_collection.font_files = files;
        dwrite_collection.font_file_count = 1;
        dwrite_collection.family_count = 1;
        serial_puts("[DWRITE] system font: ");
        serial_puts(candidates[i]);
        serial_puts("\n");
        break;
    }
    if (!dwrite_collection.font_file_count &&
        !dwrite_system_font_missing_reported) {
        dwrite_system_font_missing_reported = TRUE;
        serial_puts("[DWRITE] no usable system font file found\n");
    }
    __atomic_store_n(&dwrite_system_font_lock, 0, __ATOMIC_RELEASE);
}

static LONG WINAPI dwrite_factory_GetSystemFontCollection(PVOID self,
                                                          PVOID *collection,
                                                          BOOL check_updates)
{
    (void)self;
    (void)check_updates;
    if (!collection) return DWRITE_E_POINTER;
    dwrite_initialize_system_font_collection();
    *collection = &dwrite_collection;
    dwrite_collection_AddRef(*collection);
    dwrite_trace("[DWRITE] factory.GetSystemFontCollection\n");
    return 0;
}

static LONG WINAPI dwrite_factory_GetSystemFontFallback(PVOID self,
                                                         PVOID *fallback)
{
    (void)self;
    if (!fallback) return DWRITE_E_POINTER;
    *fallback = &dwrite_fallback;
    dwrite_trace("[DWRITE] factory.GetSystemFontFallback\n");
    return 0;
}

static LONG WINAPI DWriteCreateFactory_stub(UINT factory_type, LPCGUID iid,
                                             PVOID *factory)
{
    (void)factory_type;
    if (!iid || !factory) return DWRITE_E_INVALIDARG;
    (void)gdi32_shim_init();
    if (!dwrite_guid_equal(iid, &dwrite_iid_factory) &&
        !dwrite_guid_equal(iid, &dwrite_iid_iunknown)) {
        *factory = NULL;
        return DWRITE_E_NOINTERFACE;
    }
    *factory = &dwrite_factory;
    dwrite_trace("[DWRITE] DWriteCreateFactory\n");
    return 0;
}

/* ── Export table ──────────────────────────────────────────────── */

typedef struct {
    PVOID *lpVtbl;
    volatile ULONG refs;
} DWRITE_TEST_UNKNOWN;

typedef struct {
    PVOID *lpVtbl;
    volatile ULONG refs;
    UINT reads;
    BYTE data[12];
} DWRITE_TEST_STREAM;

typedef struct {
    PVOID *lpVtbl;
    volatile ULONG refs;
    UINT create_calls;
    DWRITE_TEST_STREAM *stream;
} DWRITE_TEST_FILE_LOADER;

typedef struct {
    PVOID *lpVtbl;
    volatile ULONG refs;
    UINT position;
    PVOID factory;
    DWRITE_TEST_FILE_LOADER *file_loader;
} DWRITE_TEST_ENUMERATOR;

typedef struct {
    PVOID *lpVtbl;
    volatile ULONG refs;
    UINT create_calls;
    DWRITE_TEST_ENUMERATOR *enumerator;
} DWRITE_TEST_COLLECTION_LOADER;

static LONG WINAPI dwrite_test_QueryInterface(PVOID self, LPCGUID iid,
                                               PVOID *object)
{
    (void)iid;
    if (!object) return DWRITE_E_POINTER;
    *object = self;
    dwrite_object_addref(self);
    return 0;
}

static ULONG WINAPI dwrite_test_AddRef(PVOID self)
{
    DWRITE_TEST_UNKNOWN *object = (DWRITE_TEST_UNKNOWN *)self;
    return object
        ? __atomic_add_fetch(&object->refs, 1, __ATOMIC_RELAXED) : 0;
}

static ULONG WINAPI dwrite_test_Release(PVOID self)
{
    DWRITE_TEST_UNKNOWN *object = (DWRITE_TEST_UNKNOWN *)self;
    return object
        ? __atomic_sub_fetch(&object->refs, 1, __ATOMIC_RELAXED) : 0;
}

static LONG WINAPI dwrite_test_stream_ReadFileFragment(
    PVOID self, const void **fragment, uint64_t offset, uint64_t size,
    PVOID *context)
{
    DWRITE_TEST_STREAM *stream = (DWRITE_TEST_STREAM *)self;
    if (!stream || !fragment || !context || offset > sizeof(stream->data) ||
        size > sizeof(stream->data) - offset)
        return DWRITE_E_FILEFORMAT;
    *fragment = stream->data + offset;
    *context = NULL;
    stream->reads++;
    return 0;
}

static void WINAPI dwrite_test_stream_ReleaseFileFragment(PVOID self,
                                                           PVOID context)
{
    (void)self;
    (void)context;
}

static LONG WINAPI dwrite_test_stream_GetFileSize(PVOID self, uint64_t *size)
{
    if (!self || !size) return DWRITE_E_POINTER;
    *size = sizeof(((DWRITE_TEST_STREAM *)self)->data);
    return 0;
}

static LONG WINAPI dwrite_test_stream_GetLastWriteTime(PVOID self,
                                                        uint64_t *time)
{
    (void)self;
    if (!time) return DWRITE_E_POINTER;
    *time = 0;
    return 0;
}

static LONG WINAPI dwrite_test_file_loader_CreateStream(
    PVOID self, const void *key, UINT key_size, PVOID *stream)
{
    DWRITE_TEST_FILE_LOADER *loader = (DWRITE_TEST_FILE_LOADER *)self;
    if (!loader || !key || !key_size || !stream) return DWRITE_E_INVALIDARG;
    loader->create_calls++;
    *stream = loader->stream;
    dwrite_object_addref(*stream);
    return 0;
}

static LONG WINAPI dwrite_test_enumerator_MoveNext(PVOID self,
                                                    BOOL *has_file)
{
    DWRITE_TEST_ENUMERATOR *enumerator = (DWRITE_TEST_ENUMERATOR *)self;
    if (!enumerator || !has_file) return DWRITE_E_POINTER;
    *has_file = enumerator->position++ == 0;
    return 0;
}

static LONG WINAPI dwrite_test_enumerator_GetCurrentFile(PVOID self,
                                                          PVOID *font_file)
{
    DWRITE_TEST_ENUMERATOR *enumerator = (DWRITE_TEST_ENUMERATOR *)self;
    static const UINT file_key = 0x4F534954;
    if (!enumerator || !font_file || enumerator->position != 1)
        return DWRITE_E_INVALIDARG;
    PVOID *factory_vtbl = *(PVOID **)enumerator->factory;
    typedef LONG (WINAPI *CREATE_FILE_FN)(PVOID, const void *, UINT, PVOID,
                                          PVOID *);
    return ((CREATE_FILE_FN)factory_vtbl[8])(
        enumerator->factory, &file_key, sizeof(file_key),
        enumerator->file_loader, font_file);
}

static LONG WINAPI dwrite_test_collection_loader_CreateEnumerator(
    PVOID self, PVOID factory, const void *key, UINT key_size,
    PVOID *enumerator_out)
{
    DWRITE_TEST_COLLECTION_LOADER *loader =
        (DWRITE_TEST_COLLECTION_LOADER *)self;
    if (!loader || !factory || (key_size && !key) || !enumerator_out)
        return DWRITE_E_INVALIDARG;
    loader->create_calls++;
    loader->enumerator->position = 0;
    loader->enumerator->factory = factory;
    *enumerator_out = loader->enumerator;
    dwrite_object_addref(*enumerator_out);
    return 0;
}

static PVOID dwrite_test_stream_vtbl[] = {
    (PVOID)dwrite_test_QueryInterface,
    (PVOID)dwrite_test_AddRef,
    (PVOID)dwrite_test_Release,
    (PVOID)dwrite_test_stream_ReadFileFragment,
    (PVOID)dwrite_test_stream_ReleaseFileFragment,
    (PVOID)dwrite_test_stream_GetFileSize,
    (PVOID)dwrite_test_stream_GetLastWriteTime
};
static PVOID dwrite_test_file_loader_vtbl[] = {
    (PVOID)dwrite_test_QueryInterface,
    (PVOID)dwrite_test_AddRef,
    (PVOID)dwrite_test_Release,
    (PVOID)dwrite_test_file_loader_CreateStream
};
static PVOID dwrite_test_enumerator_vtbl[] = {
    (PVOID)dwrite_test_QueryInterface,
    (PVOID)dwrite_test_AddRef,
    (PVOID)dwrite_test_Release,
    (PVOID)dwrite_test_enumerator_MoveNext,
    (PVOID)dwrite_test_enumerator_GetCurrentFile
};
static PVOID dwrite_test_collection_loader_vtbl[] = {
    (PVOID)dwrite_test_QueryInterface,
    (PVOID)dwrite_test_AddRef,
    (PVOID)dwrite_test_Release,
    (PVOID)dwrite_test_collection_loader_CreateEnumerator
};

static void dwrite_test_check(int condition, const char *name,
                              int *checks, int *failures)
{
    (*checks)++;
    if (condition) return;
    (*failures)++;
    serial_puts("[DWRITE-TEST] FAIL: ");
    serial_puts(name);
    serial_puts("\n");
}

int gdi32_dwrite_selftest(void)
{
    int checks = 0;
    int failures = 0;
    DWRITE_TEST_STREAM stream = {
        dwrite_test_stream_vtbl, 1, 0,
        { 0x00, 0x01, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0 }
    };
    DWRITE_TEST_FILE_LOADER file_loader = {
        dwrite_test_file_loader_vtbl, 1, 0, &stream
    };
    DWRITE_TEST_ENUMERATOR enumerator = {
        dwrite_test_enumerator_vtbl, 1, 0, NULL, &file_loader
    };
    DWRITE_TEST_COLLECTION_LOADER collection_loader = {
        dwrite_test_collection_loader_vtbl, 1, 0, &enumerator
    };
    PVOID *factory_vtbl;
    PVOID collection = NULL;
    static const BYTE cmap_format4[] = {
        0x00, 0x00, 0x00, 0x01,
        0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0c,
        0x00, 0x04, 0x00, 0x20, 0x00, 0x00, 0x00, 0x04,
        0x00, 0x04, 0x00, 0x01, 0x00, 0x00,
        0x00, 0x41, 0xff, 0xff,
        0x00, 0x00,
        0x00, 0x41, 0xff, 0xff,
        0xff, 0xc6, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x00
    };
    static const BYTE gsub_extension_single[] = {
        0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a,
        0x00, 0x01, 0x00, 0x04,
        0x00, 0x07, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08,
        0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x08,
        0x00, 0x02, 0x00, 0x08, 0x00, 0x01, 0x00, 0x64,
        0x00, 0x01, 0x00, 0x01, 0x00, 0x07
    };

    serial_puts("[DWRITE-TEST] starting custom-font pipeline test\n");
    gdi32_shim_init();
    factory_vtbl = dwrite_factory_vtbl;

    dwrite_test_check(
        dwrite_cmap_lookup(cmap_format4, sizeof(cmap_format4), 'A') == 7,
        "format 4 cmap maps a codepoint to its glyph",
        &checks, &failures);
    dwrite_test_check(
        dwrite_cmap_lookup(cmap_format4, sizeof(cmap_format4), 'B') == 0,
        "format 4 cmap reports a missing codepoint",
        &checks, &failures);
    dwrite_test_check(
        dwrite_cmap_glyph_to_ascii(
            cmap_format4, sizeof(cmap_format4), 7) == 'A',
        "cmap reverse lookup recovers the raster character",
        &checks, &failures);
    dwrite_test_check(
        dwrite_gsub_reverse_single(
            gsub_extension_single, sizeof(gsub_extension_single), 100) == 7,
        "GSUB extension reverses a single substitution",
        &checks, &failures);
    dwrite_test_check(
        dwrite_gsub_reverse_single(
            gsub_extension_single, sizeof(gsub_extension_single), 101) == 0,
        "GSUB reverse lookup reports an unmapped glyph",
        &checks, &failures);

    typedef LONG (WINAPI *LOADER_FN)(PVOID, PVOID);
    typedef LONG (WINAPI *CREATE_COLLECTION_FN)(PVOID, PVOID, const void *,
                                                 UINT, PVOID *);
    LONG status = ((LOADER_FN)factory_vtbl[5])(
        &dwrite_factory, &collection_loader);
    dwrite_test_check(status == 0 && collection_loader.refs == 2,
                      "register collection loader retains one reference",
                      &checks, &failures);
    status = ((LOADER_FN)factory_vtbl[5])(
        &dwrite_factory, &collection_loader);
    dwrite_test_check(status == DWRITE_E_ALREADYREGISTERED &&
                      collection_loader.refs == 2,
                      "duplicate collection registration is rejected",
                      &checks, &failures);

    status = ((LOADER_FN)factory_vtbl[13])(&dwrite_factory, &file_loader);
    dwrite_test_check(status == 0 && file_loader.refs == 2,
                      "register file loader retains one reference",
                      &checks, &failures);
    status = ((LOADER_FN)factory_vtbl[13])(&dwrite_factory, &file_loader);
    dwrite_test_check(status == DWRITE_E_ALREADYREGISTERED &&
                      file_loader.refs == 2,
                      "duplicate file registration is rejected",
                      &checks, &failures);

    status = ((CREATE_COLLECTION_FN)factory_vtbl[4])(
        &dwrite_factory, &collection_loader, NULL, 0, &collection);
    dwrite_test_check(status == 0 && collection != NULL,
                      "zero-length custom collection key is accepted",
                      &checks, &failures);
    dwrite_test_check(collection_loader.create_calls == 1 &&
                      file_loader.create_calls == 1 && stream.reads == 1,
                      "collection enumerates and analyzes the font stream",
                      &checks, &failures);
    dwrite_test_check(enumerator.refs == 1 && stream.refs == 1,
                      "temporary enumerator and stream references are released",
                      &checks, &failures);
    dwrite_test_check(file_loader.refs == 3,
                      "font file retains its registered loader",
                      &checks, &failures);

    if (collection) {
        DWRITE_COLLECTION_STUB *created =
            (DWRITE_COLLECTION_STUB *)collection;
        typedef UINT (WINAPI *GET_COUNT_FN)(PVOID);
        UINT families = ((GET_COUNT_FN)created->lpVtbl[3])(created);
        dwrite_test_check(families == 1 && created->font_file_count == 1,
                          "collection publishes one validated family",
                          &checks, &failures);

        PVOID family = NULL;
        typedef LONG (WINAPI *GET_FAMILY_FN)(PVOID, UINT, PVOID *);
        status = ((GET_FAMILY_FN)created->lpVtbl[4])(
            created, 0, &family);
        dwrite_test_check(status == 0 && family == &created->family,
                          "family belongs to the custom collection",
                          &checks, &failures);

        PVOID owning_collection = NULL;
        typedef LONG (WINAPI *GET_COLLECTION_FN)(PVOID, PVOID *);
        if (family)
            status = ((GET_COLLECTION_FN)(*(PVOID **)family)[3])(
                family, &owning_collection);
        else
            status = DWRITE_E_INVALIDARG;
        dwrite_test_check(status == 0 && owning_collection == collection,
                          "family returns its owning collection",
                          &checks, &failures);
        dwrite_object_release(owning_collection);

        PVOID face = NULL;
        typedef LONG (WINAPI *CREATE_FACE_FN)(PVOID, UINT, UINT,
                                               PVOID const *, UINT, UINT,
                                               PVOID *);
        status = ((CREATE_FACE_FN)factory_vtbl[9])(
            &dwrite_factory, DWRITE_FONT_FACE_TYPE_TRUETYPE,
            created->font_file_count, (PVOID const *)created->font_files,
            0, 0, &face);
        dwrite_test_check(status == 0 && face &&
                          ((DWRITE_FONT_FACE_STUB *)face)->lpVtbl ==
                              dwrite_face_vtbl,
                          "font face is created from validated files",
                          &checks, &failures);

        UINT face_file_count = 0;
        typedef LONG (WINAPI *GET_FILES_FN)(PVOID, UINT *, PVOID *);
        if (face)
            status = ((GET_FILES_FN)(*(PVOID **)face)[4])(
                face, &face_file_count, NULL);
        else
            status = DWRITE_E_INVALIDARG;
        dwrite_test_check(status == 0 && face_file_count == 1,
                          "font face reports its retained files",
                          &checks, &failures);

        PVOID returned_file = NULL;
        UINT returned_count = face_file_count;
        if (face)
            status = ((GET_FILES_FN)(*(PVOID **)face)[4])(
                face, &returned_count, &returned_file);
        dwrite_test_check(status == 0 && returned_count == 1 &&
                          returned_file == created->font_files[0],
                          "font face returns and retains its source file",
                          &checks, &failures);
        dwrite_object_release(returned_file);

        UINT short_count = 0;
        PVOID short_file = NULL;
        if (face)
            status = ((GET_FILES_FN)(*(PVOID **)face)[4])(
                face, &short_count, &short_file);
        dwrite_test_check(status == DWRITE_E_INSUFFICIENT_BUFFER &&
                          short_count == 1 && !short_file,
                          "font face rejects an undersized file array",
                          &checks, &failures);

        PVOID font = NULL;
        typedef LONG (WINAPI *GET_FONT_FN)(PVOID, UINT, PVOID *);
        if (family)
            status = ((GET_FONT_FN)(*(PVOID **)family)[5])(
                family, 0, &font);
        dwrite_test_check(status == 0 && font == &created->family.font,
                          "family returns its collection-owned font",
                          &checks, &failures);
        PVOID font_face = NULL;
        if (font)
            status = ((LONG (WINAPI *)(PVOID, PVOID *))
                (*(PVOID **)font)[13])(font, &font_face);
        dwrite_test_check(status == 0 && font_face != NULL,
                          "font creates a face from its collection files",
                          &checks, &failures);
        dwrite_object_release(font_face);
        dwrite_object_release(font);

        dwrite_object_release(face);
        dwrite_object_release(family);

        dwrite_object_release(collection);
        collection = NULL;
    }
    dwrite_test_check(file_loader.refs == 2,
                      "collection release drops retained font files",
                      &checks, &failures);

    status = ((LOADER_FN)factory_vtbl[14])(&dwrite_factory, &file_loader);
    dwrite_test_check(status == 0 && file_loader.refs == 1,
                      "file loader unregister releases factory reference",
                      &checks, &failures);
    status = ((LOADER_FN)factory_vtbl[14])(&dwrite_factory, &file_loader);
    dwrite_test_check(status == DWRITE_E_INVALIDARG,
                      "unregistered file loader cannot be removed twice",
                      &checks, &failures);
    status = ((LOADER_FN)factory_vtbl[6])(
        &dwrite_factory, &collection_loader);
    dwrite_test_check(status == 0 && collection_loader.refs == 1,
                      "collection loader unregister releases factory reference",
                      &checks, &failures);
    status = ((CREATE_COLLECTION_FN)factory_vtbl[4])(
        &dwrite_factory, &collection_loader, NULL, 0, &collection);
    dwrite_test_check(status == DWRITE_E_INVALIDARG && !collection,
                      "unregistered loader cannot create collections",
                      &checks, &failures);

    serial_puts("[DWRITE-TEST] checks=");
    serial_putdec((uint64_t)checks);
    serial_puts(" failures=");
    serial_putdec((uint64_t)failures);
    serial_puts("\n");
    return failures;
}

typedef struct { const char *name; PVOID func; uint8_t argc; uint8_t cc; } SHIM_EXPORT;

static const SHIM_EXPORT gdi32_exports[] = {
    { "GetDeviceCaps",       (PVOID)GetDeviceCaps, 2, CC_STDCALL },
    { "GetDeviceGammaRamp",  (PVOID)GetDeviceGammaRamp, 2, CC_STDCALL },
    { "SetDeviceGammaRamp",  (PVOID)SetDeviceGammaRamp, 2, CC_STDCALL },
    { "CreateDCA",           (PVOID)CreateDCA, 4, CC_STDCALL },
    { "CreateDCW",           (PVOID)CreateDCW_k32, 4, CC_STDCALL },
    { "CreateICW",           (PVOID)CreateDCW_k32, 4, CC_STDCALL },
    { "GetICMProfileW",      (PVOID)GetICMProfileW_stub, 3, CC_STDCALL },
    { "DeleteDC",            (PVOID)DeleteDC, 1, CC_STDCALL },
    { "ChoosePixelFormat",   (PVOID)ChoosePixelFormat, 2, CC_STDCALL },
    { "SetPixelFormat",      (PVOID)SetPixelFormat, 3, CC_STDCALL },
    { "GetPixelFormat",      (PVOID)GetPixelFormat, 1, CC_STDCALL },
    { "DescribePixelFormat", (PVOID)DescribePixelFormat, 4, CC_STDCALL },
    { "SwapBuffers",         (PVOID)SwapBuffers, 1, CC_STDCALL },
    { "SelectObject",        (PVOID)SelectObject, 2, CC_STDCALL },
    { "GetCurrentObject",    (PVOID)GetCurrentObject, 2, CC_STDCALL },
    { "GdiFlush",            (PVOID)GdiFlush_k32, 0, CC_STDCALL },
    { "AddFontMemResourceEx", (PVOID)AddFontMemResourceEx_k32, 4, CC_STDCALL },
    { "AddFontResourceA",    (PVOID)AddFontResourceA_k32, 1, CC_STDCALL },
    { "AddFontResourceExA",  (PVOID)AddFontResourceExA_k32, 3, CC_STDCALL },
    { "RemoveFontResourceA", (PVOID)RemoveFontResourceA_k32, 1, CC_STDCALL },
    { "AddFontResourceExW",  (PVOID)AddFontResourceExW_k32, 3, CC_STDCALL },
    { "RemoveFontResourceExW", (PVOID)RemoveFontResourceExW_k32, 3, CC_STDCALL },
    { "CreateRectRgn",       (PVOID)CreateRectRgn, 4, CC_STDCALL },
    { "CreateRectRgnIndirect", (PVOID)CreateRectRgnIndirect, 1, CC_STDCALL },
    { "SetRectRgn",          (PVOID)SetRectRgn, 5, CC_STDCALL },
    { "CombineRgn",          (PVOID)CombineRgn, 4, CC_STDCALL },
    { "EqualRgn",            (PVOID)EqualRgn, 2, CC_STDCALL },
    { "PtInRegion",          (PVOID)PtInRegion, 3, CC_STDCALL },
    { "RectInRegion",        (PVOID)RectInRegion, 2, CC_STDCALL },
    { "GetRgnBox",           (PVOID)GetRgnBox, 2, CC_STDCALL },
    { "OffsetRgn",           (PVOID)OffsetRgn, 3, CC_STDCALL },
    { "SelectClipRgn",       (PVOID)SelectClipRgn, 2, CC_STDCALL },
    { "SetViewportOrgEx",    (PVOID)SetViewportOrgEx_k32, 4, CC_STDCALL },
    { "SetBrushOrgEx",       (PVOID)SetBrushOrgEx_k32, 4, CC_STDCALL },
    { "SetDCBrushColor",     (PVOID)SetDCBrushColor_k32, 2, CC_STDCALL },
    { "SetTextAlign",        (PVOID)SetTextAlign, 2, CC_STDCALL },
    { "DeleteObject",        (PVOID)DeleteObject, 1, CC_STDCALL },
    { "GetObjectA",          (PVOID)GetObjectA, 3, CC_STDCALL },
    { "GetObjectType",       (PVOID)GetObjectType_k32, 1, CC_STDCALL },
    /* GDI object creation */
    { "CreateCompatibleDC",       (PVOID)CreateCompatibleDC, 1, CC_STDCALL },
    { "CreateCompatibleBitmap",   (PVOID)CreateCompatibleBitmap, 3, CC_STDCALL },
    { "GetDIBits",                (PVOID)GetDIBits, 7, CC_STDCALL },
    { "CreateSolidBrush",         (PVOID)CreateSolidBrush, 1, CC_STDCALL },
    { "CreatePatternBrush",       (PVOID)CreatePatternBrush, 1, CC_STDCALL },
    { "CreatePen",                (PVOID)CreatePen, 3, CC_STDCALL },
    { "CreateBitmap",             (PVOID)CreateBitmap, 5, CC_STDCALL },
    { "CreateDIBitmap",           (PVOID)CreateDIBitmap, 6, CC_STDCALL },
    { "GetStockObject",           (PVOID)GetStockObject, 1, CC_STDCALL },
    /* Drawing */
    { "BitBlt",                   (PVOID)BitBlt, 9, CC_STDCALL },
    { "StretchBlt",               (PVOID)StretchBlt_k32, 11, CC_STDCALL },
    { "StretchDIBits",            (PVOID)StretchDIBits_k32, 13, CC_STDCALL },
    { "SetDIBitsToDevice",        (PVOID)SetDIBitsToDevice_k32, 12, CC_STDCALL },
    { "GradientFill",             (PVOID)GradientFill_k32, 6, CC_STDCALL },
    { "TransparentBlt",           (PVOID)TransparentBlt_k32, 11, CC_STDCALL },
    { "AlphaBlend",               (PVOID)AlphaBlend_k32, 11, CC_STDCALL },
    { "PatBlt",                   (PVOID)PatBlt, 6, CC_STDCALL },
    { "MoveToEx",                 (PVOID)MoveToEx, 4, CC_STDCALL },
    { "LineTo",                   (PVOID)LineTo, 3, CC_STDCALL },
    { "Polyline",                 (PVOID)Polyline_k32, 3, CC_STDCALL },
    /* Text */
    { "SetTextColor",             (PVOID)SetTextColor, 2, CC_STDCALL },
    { "SetBkColor",               (PVOID)SetBkColor, 2, CC_STDCALL },
    { "SetBkMode",                (PVOID)SetBkMode, 2, CC_STDCALL },
    { "TextOutW",                 (PVOID)TextOutW, 5, CC_STDCALL },
    { "TextOutA",                 (PVOID)TextOutA_k32, 5, CC_STDCALL },
    { "ExtTextOutA",              (PVOID)ExtTextOutA, 8, CC_STDCALL },
    { "ExtTextOutW",              (PVOID)ExtTextOutW_stub, 8, CC_STDCALL },
    { "GetTextExtentPoint32A",    (PVOID)GetTextExtentPoint32A, 4, CC_STDCALL },
    { "GetTextExtentPoint32W",    (PVOID)GetTextExtentPoint32W, 4, CC_STDCALL },
    { "GetTextFaceW",             (PVOID)GetTextFaceW_k32, 3, CC_STDCALL },
    { "SetMapMode",               (PVOID)SetMapMode_stub, 2, CC_STDCALL },
    { "SetStretchBltMode",        (PVOID)SetStretchBltMode_k32, 2, CC_STDCALL },
    { "SetGraphicsMode",          (PVOID)SetGraphicsMode_k32, 2, CC_STDCALL },
    { "GetGraphicsMode",          (PVOID)GetGraphicsMode_k32, 1, CC_STDCALL },
    { "SetWorldTransform",        (PVOID)SetWorldTransform_k32, 2, CC_STDCALL },
    { "GetWorldTransform",        (PVOID)GetWorldTransform_k32, 2, CC_STDCALL },
    { "ModifyWorldTransform",     (PVOID)ModifyWorldTransform_k32, 3, CC_STDCALL },
    { "GetTextMetricsA",          (PVOID)GetTextMetricsA_stub, 2, CC_STDCALL },
    { "GetTextMetricsW",          (PVOID)GetTextMetricsW_stub, 2, CC_STDCALL },
    { "EnumFontFamiliesExA",      (PVOID)EnumFontFamiliesExA_stub, 5, CC_STDCALL },
    { "EnumFontFamiliesExW",      (PVOID)EnumFontFamiliesExW_k32, 5, CC_STDCALL },
    { "GetCharABCWidthsW",        (PVOID)GetCharABCWidthsW_stub, 4, CC_STDCALL },
    { "GetCharWidthW",            (PVOID)GetCharWidthW_k32, 4, CC_STDCALL },
    { "GetFontData",              (PVOID)GetFontData_k32, 5, CC_STDCALL },
    { "GetObjectW",               (PVOID)GetObjectW_k32, 3, CC_STDCALL },
    /* Additional stubs */
    { "CreateFontA",              (PVOID)CreateFontA_stub, 14, CC_STDCALL },
    { "CreateFontW",              (PVOID)CreateFontW_stub, 14, CC_STDCALL },
    { "CreateFontIndirectW",      (PVOID)CreateFontIndirectW_stub, 1, CC_STDCALL },
    { "CreateDIBSection",         (PVOID)CreateDIBSection_impl, 6, CC_STDCALL },
    { "GetPixel",                 (PVOID)GetPixel_stub, 3, CC_STDCALL },
    { "DWriteCreateFactory",      (PVOID)DWriteCreateFactory_stub, 3, CC_STDCALL },
    { NULL, NULL, 0, CC_STDCALL }
};

const WIN32_EXPORT *gdi32_abi_table(int *count)
{
    *count = (int)(sizeof(gdi32_exports) / sizeof(gdi32_exports[0]));
    return (const WIN32_EXPORT *)gdi32_exports;
}

static int gdi_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

PVOID gdi32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    if (by_ordinal) return NULL;
    for (int i = 0; gdi32_exports[i].name; i++) {
        if (gdi_strcmp(func_name, gdi32_exports[i].name) == 0)
            return gdi32_exports[i].func;
    }
    return NULL;
}

PVOID gdi32_shim_init(void)
{
    dwrite_collection.family.collection = &dwrite_collection;
    dwrite_collection.family.font.family = &dwrite_collection.family;
    for (int i = 3; i < 40; i++)
        dwrite_factory_vtbl[i] = (PVOID)dwrite_factory_notimpl;
    dwrite_factory_vtbl[0] = (PVOID)dwrite_factory_QueryInterface;
    dwrite_factory_vtbl[1] = (PVOID)dwrite_factory_AddRef;
    dwrite_factory_vtbl[2] = (PVOID)dwrite_factory_Release;
    dwrite_factory_vtbl[3] = (PVOID)dwrite_factory_GetSystemFontCollection;
    dwrite_factory_vtbl[4] = (PVOID)dwrite_factory_CreateCustomFontCollection;
    dwrite_factory_vtbl[5] = (PVOID)dwrite_factory_RegisterFontCollectionLoader;
    dwrite_factory_vtbl[6] = (PVOID)dwrite_factory_UnregisterFontCollectionLoader;
    dwrite_factory_vtbl[7] = (PVOID)dwrite_factory_CreateFontFileReference;
    dwrite_factory_vtbl[8] = (PVOID)dwrite_factory_CreateCustomFontFileReference;
    dwrite_factory_vtbl[9] = (PVOID)dwrite_factory_CreateFontFace;
    dwrite_factory_vtbl[13] = (PVOID)dwrite_factory_RegisterFontFileLoader;
    dwrite_factory_vtbl[14] = (PVOID)dwrite_factory_UnregisterFontFileLoader;
    dwrite_factory_vtbl[23] = (PVOID)dwrite_factory_CreateGlyphRunAnalysis;
    dwrite_factory_vtbl[26] = (PVOID)dwrite_factory_GetSystemFontFallback;
    for (int i = 3; i < 7; i++)
        dwrite_collection_vtbl[i] = (PVOID)dwrite_factory_notimpl;
    dwrite_collection_vtbl[0] = (PVOID)dwrite_collection_QueryInterface;
    dwrite_collection_vtbl[1] = (PVOID)dwrite_collection_AddRef;
    dwrite_collection_vtbl[2] = (PVOID)dwrite_collection_Release;
    dwrite_collection_vtbl[3] = (PVOID)dwrite_collection_GetFontFamilyCount;
    dwrite_collection_vtbl[4] = (PVOID)dwrite_collection_GetFontFamily;
    dwrite_collection_vtbl[5] = (PVOID)dwrite_collection_FindFamilyName;
    dwrite_collection_vtbl[6] = (PVOID)dwrite_collection_GetFontFromFontFace;

    dwrite_family_vtbl[0] = (PVOID)dwrite_family_QueryInterface;
    dwrite_family_vtbl[1] = (PVOID)dwrite_family_AddRef;
    dwrite_family_vtbl[2] = (PVOID)dwrite_family_Release;
    dwrite_family_vtbl[3] = (PVOID)dwrite_family_GetFontCollection;
    dwrite_family_vtbl[4] = (PVOID)dwrite_family_GetFontCount;
    dwrite_family_vtbl[5] = (PVOID)dwrite_family_GetFont;
    dwrite_family_vtbl[6] = (PVOID)dwrite_family_GetFamilyNames;
    dwrite_family_vtbl[7] = (PVOID)dwrite_family_GetFirstMatchingFont;
    dwrite_family_vtbl[8] = (PVOID)dwrite_family_GetMatchingFonts;

    dwrite_font_vtbl[0] = (PVOID)dwrite_font_QueryInterface;
    dwrite_font_vtbl[1] = (PVOID)dwrite_font_AddRef;
    dwrite_font_vtbl[2] = (PVOID)dwrite_font_Release;
    dwrite_font_vtbl[3] = (PVOID)dwrite_font_GetFontFamily;
    dwrite_font_vtbl[4] = (PVOID)dwrite_font_GetWeight;
    dwrite_font_vtbl[5] = (PVOID)dwrite_font_GetStretch;
    dwrite_font_vtbl[6] = (PVOID)dwrite_font_GetStyle;
    dwrite_font_vtbl[7] = (PVOID)dwrite_font_IsSymbolFont;
    dwrite_font_vtbl[8] = (PVOID)dwrite_font_GetFaceNames;
    dwrite_font_vtbl[9] = (PVOID)dwrite_font_GetInformationalStrings;
    dwrite_font_vtbl[10] = (PVOID)dwrite_font_GetSimulations;
    dwrite_font_vtbl[11] = (PVOID)dwrite_font_GetMetrics;
    dwrite_font_vtbl[12] = (PVOID)dwrite_font_HasCharacter;
    dwrite_font_vtbl[13] = (PVOID)dwrite_font_CreateFontFace;

    dwrite_face_vtbl[0] = (PVOID)dwrite_face_QueryInterface;
    dwrite_face_vtbl[1] = (PVOID)dwrite_face_AddRef;
    dwrite_face_vtbl[2] = (PVOID)dwrite_face_Release;
    dwrite_face_vtbl[3] = (PVOID)dwrite_face_GetType;
    dwrite_face_vtbl[4] = (PVOID)dwrite_face_GetFiles;
    dwrite_face_vtbl[5] = (PVOID)dwrite_face_GetIndex;
    dwrite_face_vtbl[6] = (PVOID)dwrite_face_GetSimulations;
    dwrite_face_vtbl[7] = (PVOID)dwrite_face_IsSymbolFont;
    dwrite_face_vtbl[8] = (PVOID)dwrite_face_GetMetrics;
    dwrite_face_vtbl[9] = (PVOID)dwrite_face_GetGlyphCount;
    dwrite_face_vtbl[10] = (PVOID)dwrite_face_GetDesignGlyphMetrics;
    dwrite_face_vtbl[11] = (PVOID)dwrite_face_GetGlyphIndices;
    dwrite_face_vtbl[12] = (PVOID)dwrite_face_TryGetFontTable;
    dwrite_face_vtbl[13] = (PVOID)dwrite_face_ReleaseFontTable;
    dwrite_face_vtbl[14] = (PVOID)dwrite_face_GetGlyphRunOutline;
    dwrite_face_vtbl[15] = (PVOID)dwrite_face_GetRecommendedRenderingMode;
    dwrite_face_vtbl[16] = (PVOID)dwrite_face_GetGdiCompatibleMetrics;
    dwrite_face_vtbl[17] = (PVOID)dwrite_face_GetGdiCompatibleGlyphMetrics;

    dwrite_font_file_vtbl[0] = (PVOID)dwrite_font_file_QueryInterface;
    dwrite_font_file_vtbl[1] = (PVOID)dwrite_font_file_AddRef;
    dwrite_font_file_vtbl[2] = (PVOID)dwrite_font_file_Release;
    dwrite_font_file_vtbl[3] = (PVOID)dwrite_font_file_GetReferenceKey;
    dwrite_font_file_vtbl[4] = (PVOID)dwrite_font_file_GetLoader;
    dwrite_font_file_vtbl[5] = (PVOID)dwrite_font_file_Analyze;

    dwrite_local_font_stream_vtbl[0] =
        (PVOID)dwrite_local_font_stream_QueryInterface;
    dwrite_local_font_stream_vtbl[1] =
        (PVOID)dwrite_local_font_stream_AddRef;
    dwrite_local_font_stream_vtbl[2] =
        (PVOID)dwrite_local_font_stream_Release;
    dwrite_local_font_stream_vtbl[3] =
        (PVOID)dwrite_local_font_stream_ReadFileFragment;
    dwrite_local_font_stream_vtbl[4] =
        (PVOID)dwrite_local_font_stream_ReleaseFileFragment;
    dwrite_local_font_stream_vtbl[5] =
        (PVOID)dwrite_local_font_stream_GetFileSize;
    dwrite_local_font_stream_vtbl[6] =
        (PVOID)dwrite_local_font_stream_GetLastWriteTime;

    dwrite_local_font_loader_vtbl[0] =
        (PVOID)dwrite_local_font_loader_QueryInterface;
    dwrite_local_font_loader_vtbl[1] = (PVOID)dwrite_factory_AddRef;
    dwrite_local_font_loader_vtbl[2] = (PVOID)dwrite_factory_Release;
    dwrite_local_font_loader_vtbl[3] =
        (PVOID)dwrite_local_font_loader_CreateStreamFromKey;
    dwrite_local_font_loader_vtbl[4] =
        (PVOID)dwrite_local_font_loader_GetFilePathLengthFromKey;
    dwrite_local_font_loader_vtbl[5] =
        (PVOID)dwrite_local_font_loader_GetFilePathFromKey;
    dwrite_local_font_loader_vtbl[6] =
        (PVOID)dwrite_local_font_loader_GetLastWriteTimeFromKey;

    dwrite_strings_vtbl[0] = (PVOID)dwrite_strings_QueryInterface;
    dwrite_strings_vtbl[1] = (PVOID)dwrite_factory_AddRef;
    dwrite_strings_vtbl[2] = (PVOID)dwrite_factory_Release;
    dwrite_strings_vtbl[3] = (PVOID)dwrite_strings_GetCount;
    dwrite_strings_vtbl[4] = (PVOID)dwrite_strings_FindLocaleName;
    dwrite_strings_vtbl[5] = (PVOID)dwrite_strings_GetLocaleNameLength;
    dwrite_strings_vtbl[6] = (PVOID)dwrite_strings_GetLocaleName;
    dwrite_strings_vtbl[7] = (PVOID)dwrite_strings_GetStringLength;
    dwrite_strings_vtbl[8] = (PVOID)dwrite_strings_GetString;

    dwrite_analysis_vtbl[0] = (PVOID)dwrite_analysis_QueryInterface;
    dwrite_analysis_vtbl[1] = (PVOID)dwrite_analysis_AddRef;
    dwrite_analysis_vtbl[2] = (PVOID)dwrite_analysis_Release;
    dwrite_analysis_vtbl[3] = (PVOID)dwrite_analysis_GetAlphaTextureBounds;
    dwrite_analysis_vtbl[4] = (PVOID)dwrite_analysis_CreateAlphaTexture;
    dwrite_analysis_vtbl[5] = (PVOID)dwrite_analysis_GetAlphaBlendParams;

    dwrite_fallback_vtbl[0] = (PVOID)dwrite_fallback_QueryInterface;
    dwrite_fallback_vtbl[1] = (PVOID)dwrite_factory_AddRef;
    dwrite_fallback_vtbl[2] = (PVOID)dwrite_factory_Release;
    dwrite_fallback_vtbl[3] = (PVOID)dwrite_fallback_MapCharacters;

    return (PVOID)gdi32_exports;
}
