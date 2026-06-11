/*
 * OsitoK Windows Compatibility Layer — gdi32.dll Shim Implementation
 *
 * Real GDI Device Contexts and BitBlt for Win32 compat layer.
 * DCs back onto framebuffer (screen) or kmalloc'd pixel buffers (memory).
 */

#include "gdi32_shim.h"
#include "win32_abi.h"

extern void serial_puts(const char *s);
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);

/* Framebuffer access (weak — may not be linked) */
extern uint32_t *fb_get_base(void)   __attribute__((weak));
extern uint32_t  fb_get_width(void)  __attribute__((weak));
extern uint32_t  fb_get_height(void) __attribute__((weak));
extern uint32_t  fb_get_pitch(void)  __attribute__((weak));

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

static void gdi_memset(void *p, int v, SIZE_T n)
{
    BYTE *d = (BYTE *)p;
    while (n--) *d++ = (BYTE)v;
}

/* ── GDI DC and Bitmap tables ────────────────────────────────── */

#define MAX_GDI_DCS     16
#define MAX_GDI_BITMAPS 32

typedef struct {
    int      in_use;
    void    *surface;       /* pixel buffer (NULL = no bitmap selected yet) */
    int      width, height;
    int      bpp;
    int      pitch;         /* bytes per scanline */
    uint32_t text_color;
    uint32_t bk_color;
    int      bk_mode;       /* TRANSPARENT=1, OPAQUE=2 */
    HGDIOBJ  prev_bitmap;   /* previously selected bitmap handle */
    int      is_screen;     /* DC targets the GOP framebuffer (window/screen DC) */
    int      bottomup;      /* selected bitmap is a bottom-up DIB */
} GDI_DC;

typedef struct {
    int      in_use;
    void    *pixels;
    int      width, height;
    int      bpp;
    int      pitch;
    int      bottomup;      /* DIB with positive biHeight (rows stored bottom-up) */
    uint32_t alloc_pages;   /* >0: pixels from mem_alloc_pages (DIB section) */
} GDI_BITMAP;

static GDI_DC     gdi_dcs[MAX_GDI_DCS];
static GDI_BITMAP gdi_bmps[MAX_GDI_BITMAPS];

/* Handle encoding:
 *   HDC    = 0xDC000000 | index
 *   HBITMAP = 0xBB000000 | index
 * Extract index with & 0xFF.
 */
#define DC_TAG     0xDC000000u
#define BMP_TAG    0xBB000000u
#define TAG_MASK   0xFF000000u
#define IDX_MASK   0x000000FFu

#define IS_DC_HANDLE(h)  (((ULONG_PTR)(h) & TAG_MASK) == DC_TAG)
#define IS_BMP_HANDLE(h) (((ULONG_PTR)(h) & TAG_MASK) == BMP_TAG)
#define DC_INDEX(h)      ((int)((ULONG_PTR)(h) & IDX_MASK))
#define BMP_INDEX(h)     ((int)((ULONG_PTR)(h) & IDX_MASK))

static GDI_DC *dc_from_handle(HDC h)
{
    if (!IS_DC_HANDLE(h)) return NULL;
    int idx = DC_INDEX(h);
    if (idx < 0 || idx >= MAX_GDI_DCS) return NULL;
    if (!gdi_dcs[idx].in_use) return NULL;
    return &gdi_dcs[idx];
}

static GDI_BITMAP *bmp_from_handle(HBITMAP h)
{
    if (!IS_BMP_HANDLE(h)) return NULL;
    int idx = BMP_INDEX(h);
    if (idx < 0 || idx >= MAX_GDI_BITMAPS) return NULL;
    if (!gdi_bmps[idx].in_use) return NULL;
    return &gdi_bmps[idx];
}

static int alloc_dc(void)
{
    for (int i = 0; i < MAX_GDI_DCS; i++) {
        if (!gdi_dcs[i].in_use) {
            gdi_memset(&gdi_dcs[i], 0, sizeof(GDI_DC));
            gdi_dcs[i].in_use = 1;
            gdi_dcs[i].bk_mode = 2; /* OPAQUE */
            return i;
        }
    }
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

/* ── GetDeviceCaps ───────────────────────────────────────────── */

int WINAPI GetDeviceCaps(HDC hdc, int index)
{
    (void)hdc;
    /* Report the real GOP resolution as the device extent so UT99 keeps the
     * larger DirectDraw-enumerated modes (it filters modes bigger than this). */
    int hw = (fb_get_width  && fb_get_width())  ? (int)fb_get_width()  : SCREEN_WIDTH;
    int vh = (fb_get_height && fb_get_height()) ? (int)fb_get_height() : SCREEN_HEIGHT;
    switch (index) {
    case HORZRES:    return hw;
    case VERTRES:    return vh;
    case BITSPIXEL:  return SCREEN_BPP;
    case PLANES:     return 1;
    case RASTERCAPS: return 0;
    case TECHNOLOGY: return 1; /* DT_RASDISPLAY */
    default:         return 0;
    }
}

/* ── DC creation / destruction ───────────────────────────────── */

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

HDC WINAPI CreateCompatibleDC(HDC hdc)
{
    serial_puts("[GDI32] CreateCompatibleDC\n");

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

    return (HDC)(ULONG_PTR)(DC_TAG | (unsigned)idx);
}

BOOL WINAPI DeleteDC(HDC hdc)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (dc) {
        dc->in_use = 0;
        dc->surface = NULL;
    }
    return TRUE;
}

/* ── Screen DC helpers (called from user32 GetDC/ReleaseDC) ──── */

HDC gdi32_alloc_screen_dc(void)
{
    int idx = alloc_dc();
    if (idx < 0) return NULL;

    int w, h, p;
    void *base = get_screen_surface(&w, &h, &p);

    gdi_dcs[idx].surface = base;
    gdi_dcs[idx].width   = w;
    gdi_dcs[idx].height  = h;
    gdi_dcs[idx].bpp     = 32;
    gdi_dcs[idx].pitch   = p;
    gdi_dcs[idx].is_screen = 1;  /* window/screen DC: BitBlt here = present to GOP */

    return (HDC)(ULONG_PTR)(DC_TAG | (unsigned)idx);
}

void gdi32_free_screen_dc(HDC hdc)
{
    GDI_DC *dc = dc_from_handle(hdc);
    if (dc) {
        dc->in_use = 0;
        dc->surface = NULL;
    }
}

/* ── Pixel format (passthrough stubs — DirectDraw uses these) ── */

int WINAPI ChoosePixelFormat(HDC hdc, const PIXELFORMATDESCRIPTOR *ppfd)
{
    (void)hdc; (void)ppfd;
    return 1; /* format #1 */
}

BOOL WINAPI SetPixelFormat(HDC hdc, int format, const PIXELFORMATDESCRIPTOR *ppfd)
{
    (void)hdc; (void)format; (void)ppfd;
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
    (void)hdc; (void)iPixelFormat;
    if (ppfd && nBytes >= sizeof(PIXELFORMATDESCRIPTOR)) {
        BYTE *p = (BYTE *)ppfd;
        for (SIZE_T i = 0; i < sizeof(PIXELFORMATDESCRIPTOR); i++) p[i] = 0;
        ppfd->nSize       = sizeof(PIXELFORMATDESCRIPTOR);
        ppfd->nVersion    = 1;
        ppfd->cColorBits  = 32;
        ppfd->cRedBits    = 8;
        ppfd->cGreenBits  = 8;
        ppfd->cBlueBits   = 8;
        ppfd->cDepthBits  = 24;
    }
    return 1; /* 1 pixel format available */
}

BOOL WINAPI SwapBuffers(HDC hdc)
{
    (void)hdc;
    return TRUE;
}

/* ── Bitmap creation ─────────────────────────────────────────── */

HBITMAP WINAPI CreateCompatibleBitmap(HDC hdc, int cx, int cy)
{
    serial_puts("[GDI32] CreateCompatibleBitmap\n");

    if (cx <= 0 || cy <= 0) return NULL;

    int idx = alloc_bmp();
    if (idx < 0) return NULL;

    int bpp = 32;
    GDI_DC *dc = dc_from_handle(hdc);
    if (dc) bpp = dc->bpp;

    int pitch = cx * (bpp / 8);
    void *pixels = kmalloc((uint64_t)pitch * cy);
    if (!pixels) {
        gdi_bmps[idx].in_use = 0;
        return NULL;
    }
    gdi_memset(pixels, 0, (SIZE_T)pitch * cy);

    gdi_bmps[idx].pixels = pixels;
    gdi_bmps[idx].width  = cx;
    gdi_bmps[idx].height = cy;
    gdi_bmps[idx].bpp    = bpp;
    gdi_bmps[idx].pitch  = pitch;

    return (HBITMAP)(ULONG_PTR)(BMP_TAG | (unsigned)idx);
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

    /* For fonts, brushes, pens — just return the input (fake passthrough) */
    return h;
}

BOOL WINAPI DeleteObject(HGDIOBJ ho)
{
    /* If it's a bitmap handle, free the pixels with the MATCHING allocator:
     * DIB sections come from mem_alloc_pages (PE32-visible low pages), regular
     * bitmaps from kmalloc — kfree on a page allocation corrupts the heap. */
    if (IS_BMP_HANDLE(ho)) {
        GDI_BITMAP *bmp = bmp_from_handle((HBITMAP)ho);
        if (bmp) {
            if (bmp->pixels) {
                if (bmp->alloc_pages) {
                    extern void mem_free_pages(void *addr, uint64_t count);
                    mem_free_pages(bmp->pixels, bmp->alloc_pages);
                } else {
                    kfree(bmp->pixels);
                }
                bmp->pixels = NULL;
            }
            bmp->in_use = 0;
        }
        return TRUE;
    }
    /* Other objects (fonts, pens, brushes) — just succeed */
    return TRUE;
}

int WINAPI GetObjectA(HGDIOBJ h, int c, PVOID pv)
{
    (void)h; (void)c; (void)pv;
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

        BYTE *src_base = (BYTE *)src->surface;
        BYTE *dst_base = (BYTE *)dst->surface;
        int src_pitch = src->pitch;
        int dst_pitch = dst->pitch;
        int bpp_bytes = dst->bpp / 8;
        int row_bytes = w * bpp_bytes;

        for (int row = 0; row < h; row++) {
            BYTE *sp = src_base + (sy + row) * src_pitch + sx * bpp_bytes;
            BYTE *dp = dst_base + (dy + row) * dst_pitch + dx * bpp_bytes;
            gdi_memcpy(dp, sp, (SIZE_T)row_bytes);
        }
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

BOOL WINAPI TextOutW(HDC hdc, int x, int y, PCWSTR lpString, int c)
{
    (void)hdc; (void)x; (void)y; (void)lpString; (void)c;
    return TRUE;
}

BOOL WINAPI ExtTextOutA(HDC hdc, int x, int y, UINT options,
                        PVOID lprect, PCSTR lpString, UINT c, PVOID lpDx)
{
    (void)hdc; (void)x; (void)y; (void)options;
    (void)lprect; (void)lpString; (void)c; (void)lpDx;
    return TRUE;
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

/* ── Stubs for additional GDI32 imports ───────────────────────── */

static PVOID WINAPI CreateFontA_stub(int h, int w, int esc, int orient, int weight,
                                      DWORD italic, DWORD underline, DWORD strikeout,
                                      DWORD charset, DWORD outprec, DWORD clipprec,
                                      DWORD quality, DWORD pitch, const char *face)
{
    (void)h; (void)w; (void)esc; (void)orient; (void)weight;
    (void)italic; (void)underline; (void)strikeout;
    (void)charset; (void)outprec; (void)clipprec;
    (void)quality; (void)pitch; (void)face;
    return (PVOID)(ULONG_PTR)0xF0F0F001;  /* fake HFONT */
}

static PVOID WINAPI CreateFontW_stub(int h, int w, int esc, int orient, int weight,
                                      DWORD italic, DWORD underline, DWORD strikeout,
                                      DWORD charset, DWORD outprec, DWORD clipprec,
                                      DWORD quality, DWORD pitch, const WCHAR *face)
{
    (void)h; (void)w; (void)esc; (void)orient; (void)weight;
    (void)italic; (void)underline; (void)strikeout;
    (void)charset; (void)outprec; (void)clipprec;
    (void)quality; (void)pitch; (void)face;
    return (PVOID)(ULONG_PTR)0xF0F0F002;  /* fake HFONT */
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
    extern void serial_putdec(uint64_t val);
    extern void serial_puthex(uint64_t val, int digits);
    (void)hdc; (void)usage; (void)hSection; (void)offset;
    if (ppvBits) *(uint32_t *)ppvBits = 0;
    if (!pbmi) return NULL;

    const uint8_t *bi = (const uint8_t *)pbmi;     /* BITMAPINFOHEADER (32-bit) */
    int32_t  w    = *(const int32_t  *)(bi + 4);   /* biWidth */
    int32_t  hraw = *(const int32_t  *)(bi + 8);   /* biHeight (<0 = top-down) */
    uint16_t bpp  = *(const uint16_t *)(bi + 14);  /* biBitCount */
    int32_t  h    = hraw < 0 ? -hraw : hraw;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return NULL;
    if (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return NULL;

    int idx = alloc_bmp();
    if (idx < 0) return NULL;

    uint32_t pitch = (((uint32_t)w * bpp + 31) / 32) * 4;  /* DWORD-aligned */
    uint64_t size  = (uint64_t)pitch * (uint64_t)h;
    extern void *mem_alloc_pages(uint64_t count);
    void *pixels = mem_alloc_pages((size + 4095) / 4096);  /* PE32-visible low mem */
    if (!pixels) { gdi_bmps[idx].in_use = 0; return NULL; }
    gdi_memset(pixels, 0, size);

    gdi_bmps[idx].pixels = pixels;
    gdi_bmps[idx].width  = w;
    gdi_bmps[idx].height = h;
    gdi_bmps[idx].bpp    = bpp;
    gdi_bmps[idx].pitch  = (int)pitch;
    gdi_bmps[idx].bottomup    = (hraw > 0);  /* positive biHeight = bottom-up */
    gdi_bmps[idx].alloc_pages = (uint32_t)((size + 4095) / 4096);

    if (ppvBits) *(uint32_t *)ppvBits = (uint32_t)(uintptr_t)pixels;

    serial_puts("[GDI32] CreateDIBSection ");
    serial_putdec((uint64_t)w); serial_puts("x"); serial_putdec((uint64_t)h);
    serial_puts("x"); serial_putdec(bpp);
    serial_puts(" bits=0x"); serial_puthex((uint64_t)(uintptr_t)pixels, 8);
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

/* ── Export table ──────────────────────────────────────────────── */

typedef struct { const char *name; PVOID func; uint8_t argc; uint8_t cc; } SHIM_EXPORT;

static const SHIM_EXPORT gdi32_exports[] = {
    { "GetDeviceCaps",       (PVOID)GetDeviceCaps, 2, CC_STDCALL },
    { "CreateDCA",           (PVOID)CreateDCA, 4, CC_STDCALL },
    { "DeleteDC",            (PVOID)DeleteDC, 1, CC_STDCALL },
    { "ChoosePixelFormat",   (PVOID)ChoosePixelFormat, 2, CC_STDCALL },
    { "SetPixelFormat",      (PVOID)SetPixelFormat, 3, CC_STDCALL },
    { "GetPixelFormat",      (PVOID)GetPixelFormat, 1, CC_STDCALL },
    { "DescribePixelFormat", (PVOID)DescribePixelFormat, 4, CC_STDCALL },
    { "SwapBuffers",         (PVOID)SwapBuffers, 1, CC_STDCALL },
    { "SelectObject",        (PVOID)SelectObject, 2, CC_STDCALL },
    { "DeleteObject",        (PVOID)DeleteObject, 1, CC_STDCALL },
    { "GetObjectA",          (PVOID)GetObjectA, 3, CC_STDCALL },
    /* GDI object creation */
    { "CreateCompatibleDC",       (PVOID)CreateCompatibleDC, 1, CC_STDCALL },
    { "CreateCompatibleBitmap",   (PVOID)CreateCompatibleBitmap, 3, CC_STDCALL },
    { "CreateSolidBrush",         (PVOID)CreateSolidBrush, 1, CC_STDCALL },
    { "CreatePatternBrush",       (PVOID)CreatePatternBrush, 1, CC_STDCALL },
    { "CreatePen",                (PVOID)CreatePen, 3, CC_STDCALL },
    { "CreateBitmap",             (PVOID)CreateBitmap, 5, CC_STDCALL },
    { "CreateDIBitmap",           (PVOID)CreateDIBitmap, 6, CC_STDCALL },
    { "GetStockObject",           (PVOID)GetStockObject, 1, CC_STDCALL },
    /* Drawing */
    { "BitBlt",                   (PVOID)BitBlt, 9, CC_STDCALL },
    { "PatBlt",                   (PVOID)PatBlt, 6, CC_STDCALL },
    { "MoveToEx",                 (PVOID)MoveToEx, 4, CC_STDCALL },
    { "LineTo",                   (PVOID)LineTo, 3, CC_STDCALL },
    /* Text */
    { "SetTextColor",             (PVOID)SetTextColor, 2, CC_STDCALL },
    { "SetBkColor",               (PVOID)SetBkColor, 2, CC_STDCALL },
    { "SetBkMode",                (PVOID)SetBkMode, 2, CC_STDCALL },
    { "TextOutW",                 (PVOID)TextOutW, 5, CC_STDCALL },
    { "ExtTextOutA",              (PVOID)ExtTextOutA, 8, CC_STDCALL },
    { "GetTextExtentPoint32A",    (PVOID)GetTextExtentPoint32A, 4, CC_STDCALL },
    { "GetTextExtentPoint32W",    (PVOID)GetTextExtentPoint32W, 4, CC_STDCALL },
    /* Additional stubs */
    { "CreateFontA",              (PVOID)CreateFontA_stub, 14, CC_STDCALL },
    { "CreateFontW",              (PVOID)CreateFontW_stub, 14, CC_STDCALL },
    { "CreateDIBSection",         (PVOID)CreateDIBSection_impl, 6, CC_STDCALL },
    { "GetPixel",                 (PVOID)GetPixel_stub, 3, CC_STDCALL },
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
    return (PVOID)gdi32_exports;
}
