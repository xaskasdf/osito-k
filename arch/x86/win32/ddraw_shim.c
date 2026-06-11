/*
 * OsitoK Windows Compatibility Layer — ddraw.dll Shim Implementation
 *
 * Implements IDirectDraw7 and IDirectDrawSurface7 COM interfaces
 * using simple C vtable structs. Surfaces are backed by malloc'd
 * RAM buffers. Blt/Flip copy the surface to OsitoK's framebuffer.
 *
 * UT99 SoftDrv pipeline:
 *   DirectDrawCreate → SetCooperativeLevel → SetDisplayMode →
 *   CreateSurface(primary+back) → Lock(back) → render pixels →
 *   Unlock → Flip/Blt(primary←back) → repeat
 */

#include "ddraw_shim.h"
#include "win32_abi.h"

extern uint32_t compat32_callback_args(uint32_t func_addr, int nargs, const uint32_t *args);

/* Types we need from user32/gdi32 without pulling in the full headers */
typedef struct tagRECT_DD { LONG left, top, right, bottom; } RECT_DD;
typedef RECT_DD *LPRECT;
typedef HANDLE  HDC;

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void *mem_alloc_pages(uint64_t count);
extern void mem_free_pages(void *addr, uint64_t count);

/* Compositor integration — user32 provides the per-window shm info */
extern void    *user32_get_window_shm_pixels(void *hwnd)      __attribute__((weak));
extern uint32_t user32_get_window_compositor_id(void *hwnd)    __attribute__((weak));
extern void compositor_signal_dirty(uint32_t window_id)        __attribute__((weak));

/* ── Memory helpers ────────────────────────────────────────── */

static void dd_memset(void *p, int v, SIZE_T n)
{
    BYTE *b = (BYTE *)p;
    while (n--) *b++ = (BYTE)v;
}

static void dd_memcpy(void *dst, const void *src, SIZE_T n)
{
    BYTE *d = (BYTE *)dst;
    const BYTE *s = (const BYTE *)src;
    while (n--) *d++ = *s++;
}

/* ── Display state ─────────────────────────────────────────── */

static DWORD display_width  = 640;  /* UT99 default WindowedViewportX */
static DWORD display_height = 480;  /* UT99 default WindowedViewportY */
static DWORD display_bpp    = 16;  /* UT99 SoftDrv uses 16-bit (RGB565) */

/* Framebuffer pointer — connect to real GOP LFB on OsitoK bare metal,
 * or allocate a separate buffer in test harness mode. */
static BYTE *framebuffer = NULL;
static SIZE_T fb_size = 0;
static uint32_t gop_pitch = 0;   /* GOP scanline pitch in pixels */
static HANDLE ddraw_hwnd = NULL;  /* game window — saved by SetCooperativeLevel */

/* GOP framebuffer accessors (defined in kernel/framebuffer.c) */
extern uint32_t *fb_get_base(void)   __attribute__((weak));
extern uint32_t  fb_get_width(void)  __attribute__((weak));
extern uint32_t  fb_get_height(void) __attribute__((weak));
extern uint32_t  fb_get_pitch(void)  __attribute__((weak));
/* fb_get_base() returns the cached RAM *shadow*; writes only reach the
 * displayed VRAM after fb_flush_all() copies shadow→vram. */
extern void      fb_flush_all(void)  __attribute__((weak));

static void ensure_framebuffer(void)
{
    /* Try to use the real GOP framebuffer first */
    if (!framebuffer && fb_get_base) {
        uint32_t *gop = fb_get_base();
        if (gop) {
            framebuffer = (BYTE *)gop;
            gop_pitch = fb_get_pitch ? fb_get_pitch() : display_width;
            fb_size = (SIZE_T)gop_pitch * (fb_get_height ? fb_get_height() : display_height) * 4;
            serial_puts("[DDRAW] Using GOP framebuffer at 0x");
            serial_puthex((uint64_t)(ULONG_PTR)framebuffer, 16);
            serial_puts("\n");
            return;
        }
    }

    /* Fallback: allocate system RAM buffer */
    SIZE_T needed = (SIZE_T)display_width * display_height * 4; /* 32bpp output */
    if (framebuffer && fb_size >= needed) return;
    if (framebuffer) mem_free_pages(framebuffer, (fb_size + 4095) / 4096);
    fb_size = needed;
    gop_pitch = display_width;
    framebuffer = (BYTE *)mem_alloc_pages((fb_size + 4095) / 4096);
    if (framebuffer) dd_memset(framebuffer, 0, fb_size);
}

/* ── Forward declarations ──────────────────────────────────── */

/* COM objects have vtable pointer as first member */
typedef struct IDirectDraw7       IDirectDraw7;
typedef struct IDirectDrawSurface7 IDirectDrawSurface7;

/* ── IDirectDrawSurface7 ───────────────────────────────────── */

#define MAX_SURFACES 8

/* ── IDirectDrawPalette ────────────────────────────────────── */

typedef struct DDPalette {
    uint32_t entries[256];   /* RGBQUAD: 0x00RRGGBB per entry */
} DDPalette;

static DDPalette g_ddpalette;  /* single shared palette instance */

/* IDirectDrawPalette COM vtable (7 methods) */
struct IDirectDrawPalette7;
struct IDirectDrawPalette7Vtbl;

struct IDirectDrawPalette7 {
    struct IDirectDrawPalette7Vtbl *lpVtbl;
    DDPalette *pal;
};

struct IDirectDrawPalette7Vtbl {
    /* 0: QueryInterface */
    HRESULT (WINAPI *QueryInterface)(struct IDirectDrawPalette7 *, REFIID, PVOID *);
    /* 1: AddRef */
    ULONG   (WINAPI *AddRef)(struct IDirectDrawPalette7 *);
    /* 2: Release */
    ULONG   (WINAPI *Release)(struct IDirectDrawPalette7 *);
    /* 3: GetCaps */
    HRESULT (WINAPI *GetCaps)(struct IDirectDrawPalette7 *, DWORD *);
    /* 4: GetEntries */
    HRESULT (WINAPI *GetEntries)(struct IDirectDrawPalette7 *, DWORD, DWORD, DWORD, PVOID);
    /* 5: Initialize */
    HRESULT (WINAPI *Initialize)(struct IDirectDrawPalette7 *, PVOID, DWORD, PVOID);
    /* 6: SetEntries */
    HRESULT (WINAPI *SetEntries)(struct IDirectDrawPalette7 *, DWORD, DWORD, DWORD, PVOID);
};

static HRESULT WINAPI pal_QueryInterface(struct IDirectDrawPalette7 *self, REFIID iid, PVOID *ppv)
{
    (void)iid;
    if (!ppv) return E_INVALIDARG;
    *ppv = self;
    return S_OK;
}
static ULONG WINAPI pal_AddRef(struct IDirectDrawPalette7 *self) { (void)self; return 2; }
static ULONG WINAPI pal_Release(struct IDirectDrawPalette7 *self) { (void)self; return 1; }
static HRESULT WINAPI pal_GetCaps(struct IDirectDrawPalette7 *self, DWORD *caps)
{
    (void)self;
    if (caps) *caps = 0x04; /* DDPCAPS_8BIT */
    return S_OK;
}

static HRESULT WINAPI pal_GetEntries(struct IDirectDrawPalette7 *self,
                                      DWORD dwFlags, DWORD dwBase,
                                      DWORD dwNumEntries, PVOID lpEntries)
{
    (void)dwFlags;
    if (!self || !self->pal || !lpEntries) return DDERR_INVALIDPARAMS;
    if (dwBase + dwNumEntries > 256) dwNumEntries = 256 - dwBase;

    /* PALETTEENTRY is { BYTE peRed, peGreen, peBlue, peFlags } = 4 bytes */
    BYTE *out = (BYTE *)lpEntries;
    for (DWORD i = 0; i < dwNumEntries; i++) {
        uint32_t c = self->pal->entries[dwBase + i];
        out[i * 4 + 0] = (BYTE)((c >> 16) & 0xFF); /* peRed */
        out[i * 4 + 1] = (BYTE)((c >> 8)  & 0xFF); /* peGreen */
        out[i * 4 + 2] = (BYTE)(c & 0xFF);          /* peBlue */
        out[i * 4 + 3] = 0;                          /* peFlags */
    }
    return S_OK;
}

static HRESULT WINAPI pal_Initialize(struct IDirectDrawPalette7 *self,
                                      PVOID dd, DWORD flags, PVOID entries)
{
    (void)self; (void)dd; (void)flags; (void)entries;
    return S_OK;
}

static HRESULT WINAPI pal_SetEntries(struct IDirectDrawPalette7 *self,
                                      DWORD dwFlags, DWORD dwStartingEntry,
                                      DWORD dwCount, PVOID lpEntries)
{
    (void)dwFlags;
    if (!self || !self->pal || !lpEntries) return DDERR_INVALIDPARAMS;
    if (dwStartingEntry + dwCount > 256) dwCount = 256 - dwStartingEntry;

    /* PALETTEENTRY: { BYTE peRed, peGreen, peBlue, peFlags } */
    const BYTE *in = (const BYTE *)lpEntries;
    for (DWORD i = 0; i < dwCount; i++) {
        BYTE r = in[i * 4 + 0];
        BYTE g = in[i * 4 + 1];
        BYTE b = in[i * 4 + 2];
        self->pal->entries[dwStartingEntry + i] = ((uint32_t)r << 16) |
                                                   ((uint32_t)g << 8) | b;
    }
    serial_puts("[DDRAW] Palette SetEntries: ");
    serial_puthex(dwStartingEntry, 2); serial_puts("+");
    serial_puthex(dwCount, 2); serial_puts("\n");
    return S_OK;
}

static struct IDirectDrawPalette7Vtbl palette_vtbl = {
    pal_QueryInterface, pal_AddRef, pal_Release,
    pal_GetCaps, pal_GetEntries, pal_Initialize, pal_SetEntries
};

static struct IDirectDrawPalette7 g_ddpalette_obj = { &palette_vtbl, &g_ddpalette };

/* ── IDirectDrawClipper ───────────────────────────────────── */

typedef struct DDClipper {
    HANDLE hwnd;
    LONG   clip_left, clip_top, clip_right, clip_bottom;
    int    has_clip;
} DDClipper;

static DDClipper g_ddclipper;

struct IDirectDrawClipper7;
struct IDirectDrawClipper7Vtbl;

struct IDirectDrawClipper7 {
    struct IDirectDrawClipper7Vtbl *lpVtbl;
    DDClipper *clip;
};

struct IDirectDrawClipper7Vtbl {
    /* 0: QueryInterface */
    HRESULT (WINAPI *QueryInterface)(struct IDirectDrawClipper7 *, REFIID, PVOID *);
    /* 1: AddRef */
    ULONG   (WINAPI *AddRef)(struct IDirectDrawClipper7 *);
    /* 2: Release */
    ULONG   (WINAPI *Release)(struct IDirectDrawClipper7 *);
    /* 3: GetClipList */
    HRESULT (WINAPI *GetClipList)(struct IDirectDrawClipper7 *, PVOID, PVOID, DWORD *);
    /* 4: GetHWnd */
    HRESULT (WINAPI *GetHWnd)(struct IDirectDrawClipper7 *, HANDLE *);
    /* 5: Initialize */
    HRESULT (WINAPI *Initialize)(struct IDirectDrawClipper7 *, PVOID, DWORD);
    /* 6: IsClipListChanged */
    HRESULT (WINAPI *IsClipListChanged)(struct IDirectDrawClipper7 *, BOOL *);
    /* 7: SetClipList */
    HRESULT (WINAPI *SetClipList)(struct IDirectDrawClipper7 *, PVOID, DWORD);
    /* 8: SetHWnd */
    HRESULT (WINAPI *SetHWnd)(struct IDirectDrawClipper7 *, DWORD, HANDLE);
};

static HRESULT WINAPI clip_QueryInterface(struct IDirectDrawClipper7 *self, REFIID iid, PVOID *ppv)
{
    (void)iid;
    if (!ppv) return E_INVALIDARG;
    *ppv = self;
    return S_OK;
}
static ULONG WINAPI clip_AddRef(struct IDirectDrawClipper7 *self) { (void)self; return 2; }
static ULONG WINAPI clip_Release(struct IDirectDrawClipper7 *self) { (void)self; return 1; }
static HRESULT WINAPI clip_GetClipList(struct IDirectDrawClipper7 *self, PVOID r, PVOID d, DWORD *s)
{
    (void)self; (void)r; (void)d; (void)s;
    return S_OK;
}
static HRESULT WINAPI clip_GetHWnd(struct IDirectDrawClipper7 *self, HANDLE *hwnd)
{
    if (!self || !hwnd) return DDERR_INVALIDPARAMS;
    *hwnd = self->clip ? self->clip->hwnd : NULL;
    return S_OK;
}
static HRESULT WINAPI clip_Initialize(struct IDirectDrawClipper7 *self, PVOID dd, DWORD flags)
{
    (void)self; (void)dd; (void)flags;
    return S_OK;
}
static HRESULT WINAPI clip_IsClipListChanged(struct IDirectDrawClipper7 *self, BOOL *changed)
{
    (void)self;
    if (changed) *changed = FALSE;
    return S_OK;
}
static HRESULT WINAPI clip_SetClipList(struct IDirectDrawClipper7 *self, PVOID list, DWORD flags)
{
    (void)self; (void)list; (void)flags;
    return S_OK;
}
static HRESULT WINAPI clip_SetHWnd(struct IDirectDrawClipper7 *self, DWORD flags, HANDLE hwnd)
{
    (void)flags;
    if (!self || !self->clip) return DDERR_INVALIDPARAMS;
    self->clip->hwnd = hwnd;
    /* Set clip_rect to current display dimensions */
    self->clip->clip_left   = 0;
    self->clip->clip_top    = 0;
    self->clip->clip_right  = (LONG)display_width;
    self->clip->clip_bottom = (LONG)display_height;
    self->clip->has_clip    = 1;
    serial_puts("[DDRAW] Clipper SetHWnd\n");
    return S_OK;
}

static struct IDirectDrawClipper7Vtbl clipper_vtbl = {
    clip_QueryInterface, clip_AddRef, clip_Release,
    clip_GetClipList, clip_GetHWnd, clip_Initialize,
    clip_IsClipListChanged, clip_SetClipList, clip_SetHWnd
};

static struct IDirectDrawClipper7 g_ddclipper_obj = { &clipper_vtbl, &g_ddclipper };

/* ── DDSurface ────────────────────────────────────────────── */

typedef struct {
    BYTE  *pixels;      /* surface pixel buffer */
    DWORD  width;
    DWORD  height;
    DWORD  bpp;
    LONG   pitch;
    SIZE_T buf_size;
    int    locked;
    int    is_primary;
    struct DDSurface *back_buffer;  /* for primary with flip chain */
    DDPalette        *palette;      /* attached palette (8bpp) */
    DDClipper        *clipper;      /* attached clipper */
} DDSurface;

struct IDirectDrawSurface7Vtbl;

struct IDirectDrawSurface7 {
    struct IDirectDrawSurface7Vtbl *lpVtbl;
    DDSurface surf;
};

/* Vtable function signatures — all WINAPI (ms_abi) */
typedef HRESULT (WINAPI *SURF_QueryInterface)(IDirectDrawSurface7 *, REFIID, PVOID *);
typedef ULONG   (WINAPI *SURF_AddRef)(IDirectDrawSurface7 *);
typedef ULONG   (WINAPI *SURF_Release)(IDirectDrawSurface7 *);

/* We number vtable slots to match the actual IDirectDrawSurface7 vtable layout.
 * The important ones for UT99: Lock(25), Unlock(32), Blt(5), Flip(11),
 * GetSurfaceDesc(22), GetDC(17), ReleaseDC(26) */

struct IDirectDrawSurface7Vtbl {
    /* 0: IUnknown */
    SURF_QueryInterface QueryInterface;
    SURF_AddRef         AddRef;
    SURF_Release        Release;
    /* 3-4: AddAttachedSurface, AddOverlayDirtyRect */
    PVOID _pad3;
    PVOID _pad4;
    /* 5: Blt */
    HRESULT (WINAPI *Blt)(IDirectDrawSurface7 *self, LPRECT destRect,
                           IDirectDrawSurface7 *src, LPRECT srcRect,
                           DWORD dwFlags, PVOID lpDDBltFx);
    /* 6: BltBatch, 7: BltFast, 8-10 */
    PVOID _pad6;
    HRESULT (WINAPI *BltFast)(IDirectDrawSurface7 *self, DWORD dwX, DWORD dwY,
                               IDirectDrawSurface7 *src, LPRECT srcRect, DWORD dwTrans);
    PVOID _pad8; PVOID _pad9; PVOID _pad10;
    /* 11: Flip */
    HRESULT (WINAPI *Flip)(IDirectDrawSurface7 *self,
                            IDirectDrawSurface7 *override, DWORD flags);
    /* 12: GetAttachedSurface, 13: GetBltStatus, 14: GetCaps */
    PVOID _pad12; PVOID _pad13; PVOID _pad14;
    /* 15: GetClipper */
    HRESULT (WINAPI *GetClipper)(IDirectDrawSurface7 *self, PVOID *lplpDDClipper);
    PVOID _pad16;
    /* 17: GetDC */
    HRESULT (WINAPI *GetDC)(IDirectDrawSurface7 *self, HDC *hdc);
    /* 18: GetFlipStatus, 19: GetOverlayPosition */
    PVOID _pad18; PVOID _pad19;
    /* 20: GetPalette */
    HRESULT (WINAPI *GetPalette)(IDirectDrawSurface7 *self, PVOID *lplpDDPalette);
    PVOID _pad21;
    /* 22: GetSurfaceDesc */
    HRESULT (WINAPI *GetSurfaceDesc)(IDirectDrawSurface7 *self, DDSURFACEDESC2 *desc);
    /* 23-24 */
    PVOID _pad23; PVOID _pad24;
    /* 25: Lock */
    HRESULT (WINAPI *Lock)(IDirectDrawSurface7 *self, LPRECT destRect,
                            DDSURFACEDESC2 *desc, DWORD flags, HANDLE hEvent);
    /* 26: ReleaseDC */
    HRESULT (WINAPI *ReleaseDC)(IDirectDrawSurface7 *self, HDC hdc);
    /* 27: Restore */
    PVOID _pad27;
    /* 28: SetClipper */
    HRESULT (WINAPI *SetClipper)(IDirectDrawSurface7 *self, PVOID lpDDClipper);
    /* 29: SetColorKey */
    PVOID _pad29;
    /* 30: SetOverlayPosition */
    PVOID _pad30;
    /* 31: SetPalette */
    HRESULT (WINAPI *SetPalette)(IDirectDrawSurface7 *self, PVOID lpDDPalette);
    /* 32: Unlock */
    HRESULT (WINAPI *Unlock)(IDirectDrawSurface7 *self, LPRECT lpRect);
};

/* Surface implementations */

/* ── COM32 proxy types ────────────────────────────────────── */
typedef struct {
    uint32_t lpVtbl32;
    uint32_t surf_index;
} COM32_Surface;
static COM32_Surface *surf_proxy32;  /* allocated dynamically */

/* Forward declarations — defined later in file */
static IDirectDrawSurface7 *com32_to_surface(uint32_t proxy_addr);

/* All surf_* functions receive a COM32_Surface* as 'self' (via thunk).
 * Convert to real surface with this macro. */
#define REAL_SURF(self) com32_to_surface((uint32_t)(ULONG_PTR)(self))

static HRESULT WINAPI surf_QueryInterface(IDirectDrawSurface7 *self, REFIID iid, PVOID *ppv)
{
    (void)iid;
    if (!ppv) return E_INVALIDARG;
    *ppv = self; /* return the proxy, not the real surface */
    return S_OK;
}

static ULONG WINAPI surf_AddRef(IDirectDrawSurface7 *self)  { (void)self; return 2; }
static ULONG WINAPI surf_Release(IDirectDrawSurface7 *self) { (void)self; return 1; }

static void ddraw_compositor_notify(void);

/* The display-sized surface SoftDrv renders into (it Locks the back buffer
 * once and writes pixels every frame without re-Locking, and never issues the
 * fullscreen Flip in our environment). The per-frame message pump calls
 * ddraw_present_hook() to copy it to the GOP framebuffer so frames are seen. */
static DDSurface *g_present_surface = NULL;

/* Copy a software-rendered DD surface to the GOP framebuffer (RGB565/8bpp/32
 * → XRGB8888). NEAREST-NEIGHBOR UPSCALES the surface to fill the whole GOP
 * (e.g. UT99's 640x480 → 1024x768, both 4:3 so no distortion) instead of the
 * old 1:1 top-left blit that left the game letterboxed. Mouse mapping in
 * win32_post_mouse_abs scales the tablet to the SAME source space so the cursor
 * lines up with the scaled image. Shared by Flip/Blt/Unlock. */
/* Core scaled present: convert (bpp) + nearest-neighbor scale an arbitrary
 * pixel buffer to fill the GOP framebuffer, then flush shadow→VRAM. Exported
 * so the GDI path (BitBlt of a DIB section to the window DC — UT99 SoftDrv's
 * windowed present) shares the exact same pipeline as DirectDraw Flip/Blt.
 * pitch_bytes may be NEGATIVE for bottom-up DIBs (pixels then points at the
 * FIRST scanline in memory order = the bottom row). pal256 is the 256-entry
 * 0x00RRGGBB palette for 8bpp sources (may be NULL → 8bpp skipped). */
/* Size of the SOURCE buffer most recently scaled to the GOP — i.e. the pixel
 * space the user actually sees. The mouse mapping (win32_post_mouse_abs via
 * ddraw_get_display_size) must use THIS space, not ddraw's display mode: the
 * GDI/DIB present path changes resolution without a ddraw SetDisplayMode, and
 * a stale mapping makes every click land offset (e.g. UT99's "Confirm Video
 * Settings Change" Yes button becomes unclickable after a resolution switch →
 * 15s auto-revert). */
static uint32_t g_present_src_w = 0, g_present_src_h = 0;

void ddraw_present_pixels(const void *pixels, uint32_t sw, uint32_t sh,
                          uint32_t bpp, int32_t pitch_bytes, const uint32_t *pal256)
{
    ensure_framebuffer();
    if (!framebuffer || !pixels || !sw || !sh) return;
    g_present_src_w = sw;
    g_present_src_h = sh;
    uint32_t pitch = gop_pitch ? gop_pitch : sw;
    uint32_t fbw = (fb_get_width  && fb_get_width())  ? fb_get_width()  : sw;
    uint32_t fbh = (fb_get_height && fb_get_height()) ? fb_get_height() : sh;
    uint32_t *dst32 = (uint32_t *)framebuffer;

    /* per-column source-x LUT for the horizontal scale (sw -> fbw) */
    static uint32_t xlut[4096];
    uint32_t outw = fbw > 4096 ? 4096 : fbw;
    for (uint32_t dx = 0; dx < outw; dx++) xlut[dx] = (dx * sw) / fbw;

    const uint8_t *base = (const uint8_t *)pixels;
    for (uint32_t dy = 0; dy < fbh; dy++) {
        uint32_t sy = (dy * sh) / fbh;
        const uint8_t *srow8 = base + (int64_t)(int32_t)sy * pitch_bytes;
        uint32_t *drow = &dst32[dy * pitch];
        if (bpp == 16) {
            const uint16_t *srow = (const uint16_t *)srow8;
            for (uint32_t dx = 0; dx < outw; dx++) {
                uint16_t c = srow[xlut[dx]];
                uint32_t r = ((c >> 11) & 0x1F) * 255 / 31;
                uint32_t g = ((c >> 5)  & 0x3F) * 255 / 63;
                uint32_t b = (c & 0x1F) * 255 / 31;
                drow[dx] = (r << 16) | (g << 8) | b;
            }
        } else if (bpp == 8 && pal256) {
            const uint8_t *srow = srow8;
            for (uint32_t dx = 0; dx < outw; dx++)
                drow[dx] = pal256[srow[xlut[dx]]];
        } else if (bpp == 32) {
            const uint32_t *srow = (const uint32_t *)srow8;
            for (uint32_t dx = 0; dx < outw; dx++)
                drow[dx] = srow[xlut[dx]];
        }
    }

    /* Push the cached shadow framebuffer to displayed VRAM, else nothing
     * appears on screen (the console path flushes; we must too). */
    if (fb_flush_all) fb_flush_all();
}

static void present_surface_to_gop(DDSurface *s)
{
    if (!s || !s->pixels || !s->width) return;
    ddraw_present_pixels(s->pixels, s->width, s->height, s->bpp,
                         (int32_t)(s->width * (s->bpp / 8)),
                         s->palette ? s->palette->entries : NULL);
}

/* Report the current SoftDrv render resolution (the surface space that
 * present_surface_to_gop scales to fill the whole screen). win32_post_mouse_abs
 * maps the absolute tablet into THIS space so the cursor lines up with the
 * scaled image. Returns 0/0 until a display mode is set. */
void ddraw_get_display_size(uint32_t *w, uint32_t *h)
{
    /* Prefer the size of what is actually being PRESENTED (DIB or ddraw
     * surface) so the cursor space always matches the visible image; fall back
     * to the ddraw display mode before the first present. */
    if (w) *w = g_present_src_w ? g_present_src_w : display_width;
    if (h) *h = g_present_src_h ? g_present_src_h : display_height;
}

/* Called by the GDI present path (BitBlt of a DIB to the window DC): the game
 * has switched to GDI/DIB presentation, so stop the per-pump re-present of the
 * last DirectDraw surface — it would overwrite the DIB frames with stale
 * content. A later ddraw Lock/Flip re-arms g_present_surface. */
void ddraw_suspend_present_hook(void)
{
    g_present_surface = NULL;
}

/* Full current display mode incl. depth — used by user32's
 * EnumDisplaySettings(ENUM_CURRENT_SETTINGS) so the whole layer tells one
 * consistent "current mode" story (ddraw is the authority). */
void ddraw_get_display_mode(uint32_t *w, uint32_t *h, uint32_t *bpp)
{
    if (w)   *w   = display_width;
    if (h)   *h   = display_height;
    if (bpp) *bpp = display_bpp;
}

/* Per-frame present hook, called by the user32 message pump (PeekMessage).
 * SoftDrv renders into a persistently-locked surface and does not issue the
 * fullscreen Flip in our setup, so we present the current render target each
 * frame here. No-op until SoftDrv has Locked a display-sized surface. */
void ddraw_present_hook(void)
{
    if (!g_present_surface) return;
    present_surface_to_gop(g_present_surface);
    ddraw_compositor_notify();
}

static HRESULT WINAPI surf_Lock(IDirectDrawSurface7 *self, LPRECT destRect,
                                 DDSURFACEDESC2 *desc, DWORD flags, HANDLE hEvent)
{
    (void)destRect; (void)flags; (void)hEvent;
    IDirectDrawSurface7 *real = REAL_SURF(self);
    if (!real) return DDERR_INVALIDPARAMS;
    DDSurface *s = &real->surf;

    if (!desc) return DDERR_INVALIDPARAMS;

    /* Write DDSURFACEDESC2 in 32-bit layout (4-byte pointers, 124 bytes total).
     * Critical: lpSurface at offset 36 is 4 bytes (not 8). */
    uint32_t *d = (uint32_t *)desc;
    dd_memset(d, 0, 124);  /* 32-bit DDSURFACEDESC2 = 124 bytes */
    d[0]  = 124;  /* dwSize */
    d[1]  = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_LPSURFACE | DDSD_PIXELFORMAT;
    d[2]  = s->height;              /* dwHeight at offset 8 */
    d[3]  = s->width;               /* dwWidth at offset 12 */
    d[4]  = (uint32_t)s->pitch;     /* lPitch at offset 16 */
    d[9]  = (uint32_t)(ULONG_PTR)s->pixels; /* lpSurface at offset 36 */

    /* Remember the display-sized surface being rendered into so the per-frame
     * pump can present it (SoftDrv keeps this Locked and never Flips). */
    if (s->pixels && s->width == display_width && s->height == display_height)
        g_present_surface = s;

    /* ddpfPixelFormat at offset 72 (32-bit DDSURFACEDESC2 layout). Field
     * offsets within ddpf: dwSize@72(d18), dwFlags@76(d19), dwFourCC@80(d20),
     * dwRGBBitCount@84(d21), dwRBitMask@88(d22), dwGBitMask@92(d23),
     * dwBBitMask@96(d24), dwRGBAlphaBitMask@100(d25).
     * BUGFIX 2026-06-07: every field from dwRGBBitCount on was written one dword
     * too high (bitcount at d22, R/G/B masks at d23/d24/d25), so SoftDrv read
     * dwRGBBitCount=0, dwRBitMask=bpp, dwGBitMask=0xF800, dwBBitMask=0x07E0 and
     * built a blitter that wrote only the middle 6 bits (green field) — the
     * green-tint. Use the correct ABI offsets. */
    d[18] = 32;       /* dwSize @72 */
    d[21] = s->bpp;   /* dwRGBBitCount @84 */

    if (s->bpp == 8) {
        d[19] = 0x00000020;  /* DDPF_PALETTEINDEXED8 @76 */
        /* No bit masks for palettized mode */
    } else if (s->bpp == 16) {
        d[19] = DDPF_RGB;    /* dwFlags @76 */
        d[22] = 0xF800;      /* dwRBitMask @88 */
        d[23] = 0x07E0;      /* dwGBitMask @92 */
        d[24] = 0x001F;      /* dwBBitMask @96 */
    } else {
        d[19] = DDPF_RGB;    /* dwFlags @76 */
        d[22] = 0x00FF0000;  /* dwRBitMask @88 */
        d[23] = 0x0000FF00;  /* dwGBitMask @92 */
        d[24] = 0x000000FF;  /* dwBBitMask @96 */
    }

    s->locked = 1;
    return DD_OK;
}

static HRESULT WINAPI surf_Unlock(IDirectDrawSurface7 *self, LPRECT lpRect)
{
    (void)lpRect;
    IDirectDrawSurface7 *real = REAL_SURF(self);
    if (!real) return DDERR_INVALIDPARAMS;
    real->surf.locked = 0;

    /* SoftDrv renders the frame into a locked surface (typically the back
     * buffer) and unlocks it; the fullscreen Flip that would present it is
     * not always issued. Present the just-rendered surface to the GOP
     * framebuffer here so the frame becomes visible. */
    DDSurface *s = &real->surf;
    if (s->pixels && s->width == display_width && s->height == display_height) {
        present_surface_to_gop(s);
        ddraw_compositor_notify();
    }
    return DD_OK;
}

/* Notify compositor that the primary surface was updated */
static void ddraw_compositor_notify(void)
{
    if (!ddraw_hwnd || !compositor_signal_dirty || !user32_get_window_compositor_id)
        return;
    uint32_t wid = user32_get_window_compositor_id(ddraw_hwnd);
    if (wid)
        compositor_signal_dirty(wid);
}

/* Blt: copy source surface to this surface (or to framebuffer if primary) */
static HRESULT WINAPI surf_Blt(IDirectDrawSurface7 *self, LPRECT destRect,
                                IDirectDrawSurface7 *src, LPRECT srcRect,
                                DWORD dwFlags, PVOID lpDDBltFx)
{
    (void)srcRect; (void)dwFlags; (void)lpDDBltFx;
    IDirectDrawSurface7 *real_self = REAL_SURF(self);
    if (!real_self) return DDERR_INVALIDPARAMS;
    DDSurface *dst = &real_self->surf;

    /* Clipper intersection: if surface has clipper and destRect, clip destRect */
    LONG clip_x = 0, clip_y = 0, clip_w = (LONG)dst->width, clip_h = (LONG)dst->height;
    if (dst->clipper && dst->clipper->has_clip && destRect) {
        /* Intersect destRect with clip_rect */
        LONG dl = destRect->left, dt = destRect->top;
        LONG dr = destRect->right, db = destRect->bottom;
        LONG cl = dst->clipper->clip_left, ct = dst->clipper->clip_top;
        LONG cr = dst->clipper->clip_right, cb = dst->clipper->clip_bottom;
        clip_x = dl > cl ? dl : cl;
        clip_y = dt > ct ? dt : ct;
        LONG rx = dr < cr ? dr : cr;
        LONG ry = db < cb ? db : cb;
        clip_w = rx - clip_x;
        clip_h = ry - clip_y;
        if (clip_w <= 0 || clip_h <= 0) return DD_OK; /* fully clipped */
    } else if (destRect) {
        clip_x = destRect->left;
        clip_y = destRect->top;
        clip_w = destRect->right - destRect->left;
        clip_h = destRect->bottom - destRect->top;
    }
    (void)clip_x; (void)clip_y; (void)clip_w; (void)clip_h;

    if (src) {
        IDirectDrawSurface7 *real_src = REAL_SURF(src);
        DDSurface *s = real_src ? &real_src->surf : dst;
        /* Copy source buffer to destination */
        SIZE_T copy_size = (SIZE_T)dst->height * dst->pitch;
        SIZE_T src_size  = (SIZE_T)s->height * s->pitch;
        if (copy_size > src_size) copy_size = src_size;
        if (copy_size > dst->buf_size) copy_size = dst->buf_size;
        dd_memcpy(dst->pixels, s->pixels, copy_size);
    }

    /* If primary surface, blit (nearest-neighbor scaled to full GOP) */
    if (dst->is_primary)
        present_surface_to_gop(dst);

    /* Also copy to compositor's shm surface if available */
    if (dst->is_primary)
        ddraw_compositor_notify();

    return DD_OK;
}

/* BltFast: fast rectangular blit without clipping or ROP */
static HRESULT WINAPI surf_BltFast(IDirectDrawSurface7 *self,
                                    DWORD dwX, DWORD dwY,
                                    IDirectDrawSurface7 *src, LPRECT srcRect,
                                    DWORD dwTrans)
{
    IDirectDrawSurface7 *real_self = REAL_SURF(self);
    IDirectDrawSurface7 *real_src  = src ? REAL_SURF(src) : NULL;
    if (!real_self || !real_src) return DDERR_INVALIDPARAMS;

    DDSurface *dst = &real_self->surf;
    DDSurface *s   = &real_src->surf;

    /* Source rect (default: entire source surface) */
    DWORD sx = 0, sy = 0, sw = s->width, sh = s->height;
    if (srcRect) {
        sx = (DWORD)srcRect->left;  sy = (DWORD)srcRect->top;
        sw = (DWORD)(srcRect->right - srcRect->left);
        sh = (DWORD)(srcRect->bottom - srcRect->top);
    }

    /* Clip to destination bounds */
    if (dwX + sw > dst->width)  sw = dst->width - dwX;
    if (dwY + sh > dst->height) sh = dst->height - dwY;
    if (sx + sw > s->width)  sw = s->width - sx;
    if (sy + sh > s->height) sh = s->height - sy;

    DWORD bpp_bytes = dst->bpp / 8;
    int use_src_colorkey = (dwTrans & 0x1); /* DDBLTFAST_SRCCOLORKEY */

    for (DWORD y = 0; y < sh; y++) {
        BYTE *dp = dst->pixels + (dwY + y) * dst->pitch + dwX * bpp_bytes;
        BYTE *sp = s->pixels   + (sy + y)  * s->pitch   + sx * bpp_bytes;

        if (!use_src_colorkey) {
            dd_memcpy(dp, sp, sw * bpp_bytes);
        } else {
            /* Color key transparency: skip pixels matching color key */
            if (bpp_bytes == 2) {
                uint16_t *d16 = (uint16_t *)dp, *s16 = (uint16_t *)sp;
                for (DWORD x = 0; x < sw; x++)
                    if (s16[x] != 0) d16[x] = s16[x]; /* key=0 (black) */
            } else {
                uint32_t *d32 = (uint32_t *)dp, *s32 = (uint32_t *)sp;
                for (DWORD x = 0; x < sw; x++)
                    if (s32[x] & 0xFF000000) d32[x] = s32[x]; /* key=transparent alpha */
            }
        }
    }

    (void)dwTrans;
    return DD_OK;
}

/* Flip: swap primary ↔ back buffer, then blit to framebuffer */
static HRESULT WINAPI surf_Flip(IDirectDrawSurface7 *self,
                                 IDirectDrawSurface7 *override, DWORD flags)
{
    (void)flags;

    IDirectDrawSurface7 *real_self = REAL_SURF(self);
    if (!real_self) return DDERR_INVALIDPARAMS;
    DDSurface *primary = &real_self->surf;
    DDSurface *back = NULL;
    if (override) {
        IDirectDrawSurface7 *real_ov = REAL_SURF(override);
        if (real_ov) back = &real_ov->surf;
    }
    /* If no override, try attached back buffer */
    if (!back && primary->back_buffer)
        back = (DDSurface *)primary->back_buffer;

    if (!back) {
        /* UT99 expects primary.Flip() to swap with the attached back buffer.
         * Since our back buffer is separate, we just blit back→primary→fb. */
        serial_puts("[DDRAW] Flip without back buffer\n");
        return DD_OK;
    }

    /* Swap pixel pointers */
    BYTE *tmp = primary->pixels;
    primary->pixels = back->pixels;
    back->pixels = tmp;

    /* Blit primary to GOP framebuffer (nearest-neighbor scaled to full GOP) */
    present_surface_to_gop(primary);

    ddraw_compositor_notify();
    return DD_OK;
}

static HRESULT WINAPI surf_GetSurfaceDesc(IDirectDrawSurface7 *self, DDSURFACEDESC2 *desc)
{
    if (!desc) return DDERR_INVALIDPARAMS;
    /* Just lock and unlock to fill the descriptor */
    HRESULT hr = surf_Lock(self, NULL, desc, 0, NULL);
    if (hr == DD_OK) surf_Unlock(self, NULL);
    return hr;
}

/* IDirectDrawSurface7::GetPixelFormat (vtbl slot 21). UE1
 * USoftwareRenderDevice::SetRes calls this on the render-target surface to learn
 * the RGB bit layout and build its 16-bit shade/color tables. It was previously
 * UNIMPLEMENTED (the generic S_OK stub left the caller's DDPIXELFORMAT
 * untouched), so SoftDrv read a zeroed format and built a green-biased table —
 * the green tint. Fill a proper DDPIXELFORMAT (32-bit layout, 32 bytes):
 *   dwSize@0 dwFlags@4 dwFourCC@8 dwRGBBitCount@12
 *   dwRBitMask@16 dwGBitMask@20 dwBBitMask@24 dwRGBAlphaBitMask@28 */
static HRESULT WINAPI surf_GetPixelFormat(IDirectDrawSurface7 *self, void *lpDDPF)
{
    IDirectDrawSurface7 *real = REAL_SURF(self);
    if (!real || !lpDDPF) return DDERR_INVALIDPARAMS;
    DDSurface *s = &real->surf;
    uint32_t *pf = (uint32_t *)lpDDPF;
    dd_memset(pf, 0, 32);
    pf[0] = 32;          /* dwSize */
    pf[3] = s->bpp;      /* dwRGBBitCount */
    if (s->bpp == 8) {
        pf[1] = 0x00000020;       /* DDPF_PALETTEINDEXED8 */
    } else if (s->bpp == 16) {
        pf[1] = DDPF_RGB;
        pf[4] = 0xF800;  pf[5] = 0x07E0;  pf[6] = 0x001F;
    } else {
        pf[1] = DDPF_RGB;
        pf[4] = 0x00FF0000; pf[5] = 0x0000FF00; pf[6] = 0x000000FF;
    }
    static int gpf_logged = 0;
    if (!gpf_logged) {
        gpf_logged = 1;
        serial_puts("[DDRAW] GetPixelFormat -> bpp="); serial_putdec(s->bpp);
        serial_puts(" R=0x"); serial_puthex(pf[4], 4);
        serial_puts(" G=0x"); serial_puthex(pf[5], 4);
        serial_puts(" B=0x"); serial_puthex(pf[6], 4); serial_puts("\n");
    }
    return DD_OK;
}

static HRESULT WINAPI surf_GetDC(IDirectDrawSurface7 *self, HDC *hdc)
{
    (void)self;
    if (hdc) *hdc = (HDC)(ULONG_PTR)0xDC00DD01;
    return DD_OK;
}

static HRESULT WINAPI surf_ReleaseDC(IDirectDrawSurface7 *self, HDC hdc)
{
    (void)self; (void)hdc;
    return DD_OK;
}

/* SetClipper: attach clipper to surface (slot 28) */
static HRESULT WINAPI surf_SetClipper(IDirectDrawSurface7 *self, PVOID lpDDClipper)
{
    IDirectDrawSurface7 *real = REAL_SURF(self);
    if (!real) return DDERR_INVALIDPARAMS;
    if (lpDDClipper) {
        real->surf.clipper = &g_ddclipper;
        serial_puts("[DDRAW] SetClipper on surface\n");
    } else {
        real->surf.clipper = NULL;
    }
    return DD_OK;
}

/* SetPalette: attach palette to surface (slot 31) */
static HRESULT WINAPI surf_SetPalette(IDirectDrawSurface7 *self, PVOID lpDDPalette)
{
    IDirectDrawSurface7 *real = REAL_SURF(self);
    if (!real) return DDERR_INVALIDPARAMS;
    if (lpDDPalette) {
        real->surf.palette = &g_ddpalette;
        serial_puts("[DDRAW] SetPalette on surface\n");
    } else {
        real->surf.palette = NULL;
    }
    return DD_OK;
}

/* GetPalette: return attached palette (slot 20) */
static HRESULT WINAPI surf_GetPalette(IDirectDrawSurface7 *self, PVOID *lplpDDPalette)
{
    IDirectDrawSurface7 *real = REAL_SURF(self);
    if (!real || !lplpDDPalette) return DDERR_INVALIDPARAMS;
    if (!real->surf.palette) {
        *(uint32_t *)lplpDDPalette = 0;
        return DDERR_NOPALETTEATTACHED;
    }
    /* Return palette COM32 proxy if available, or the palette object directly */
    extern uint32_t *pal_proxy32_ptr;
    if (pal_proxy32_ptr)
        *(uint32_t *)lplpDDPalette = (uint32_t)(ULONG_PTR)pal_proxy32_ptr;
    else
        *(uint32_t *)lplpDDPalette = (uint32_t)(ULONG_PTR)&g_ddpalette_obj;
    return S_OK;
}

/* GetClipper: return attached clipper (slot 15) */
static HRESULT WINAPI surf_GetClipper(IDirectDrawSurface7 *self, PVOID *lplpDDClipper)
{
    IDirectDrawSurface7 *real = REAL_SURF(self);
    if (!real || !lplpDDClipper) return DDERR_INVALIDPARAMS;
    if (!real->surf.clipper) {
        *(uint32_t *)lplpDDClipper = 0;
        return DDERR_NOCLIPPERATTACHED;
    }
    extern uint32_t *clip_proxy32_ptr;
    if (clip_proxy32_ptr)
        *(uint32_t *)lplpDDClipper = (uint32_t)(ULONG_PTR)clip_proxy32_ptr;
    else
        *(uint32_t *)lplpDDClipper = (uint32_t)(ULONG_PTR)&g_ddclipper_obj;
    return S_OK;
}

/* Shared vtable */
static struct IDirectDrawSurface7Vtbl surface_vtbl = {
    surf_QueryInterface,
    surf_AddRef,
    surf_Release,
    NULL, NULL,                 /* 3-4 */
    surf_Blt,                   /* 5 */
    NULL,                       /* 6: BltBatch */
    surf_BltFast,               /* 7: BltFast */
    NULL, NULL, NULL,           /* 8-10 */
    surf_Flip,                  /* 11 */
    NULL, NULL, NULL,           /* 12-14 */
    surf_GetClipper,            /* 15 */
    NULL,                       /* 16 */
    surf_GetDC,                 /* 17 */
    NULL, NULL,                 /* 18-19 */
    surf_GetPalette,            /* 20 */
    NULL,                       /* 21 */
    surf_GetSurfaceDesc,        /* 22 */
    NULL, NULL,                 /* 23-24 */
    surf_Lock,                  /* 25 */
    surf_ReleaseDC,             /* 26 */
    NULL,                       /* 27: Restore */
    surf_SetClipper,            /* 28 */
    NULL,                       /* 29: SetColorKey */
    NULL,                       /* 30: SetOverlayPosition */
    surf_SetPalette,            /* 31 */
    surf_Unlock                 /* 32 */
};

/* Surface pool */
static IDirectDrawSurface7 surfaces[MAX_SURFACES];
static int surface_count = 0;

static IDirectDrawSurface7 *alloc_surface(DWORD w, DWORD h, DWORD bpp, int is_primary)
{
    if (surface_count >= MAX_SURFACES) return NULL;

    IDirectDrawSurface7 *s = &surfaces[surface_count++];
    s->lpVtbl = &surface_vtbl;

    DDSurface *ds = &s->surf;
    ds->width      = w;
    ds->height     = h;
    ds->bpp        = bpp;
    ds->pitch      = (LONG)(w * (bpp / 8));
    ds->buf_size   = (SIZE_T)h * ds->pitch;
    ds->pixels     = (BYTE *)mem_alloc_pages((ds->buf_size + 4095) / 4096);
    ds->locked     = 0;
    ds->is_primary = is_primary;
    ds->back_buffer = NULL;
    ds->palette     = NULL;
    ds->clipper     = NULL;

    if (ds->pixels) dd_memset(ds->pixels, 0, ds->buf_size);

    serial_puts("[DDRAW] created surface ");
    serial_puthex(w, 4); serial_puts("x"); serial_puthex(h, 4);
    serial_puts("x"); serial_puthex(bpp, 2);
    serial_puts(is_primary ? " (primary)" : " (offscreen)");
    serial_puts("\n");

    return s;
}

/* ── IDirectDraw7 ──────────────────────────────────────────── */

struct IDirectDraw7Vtbl;

struct IDirectDraw7 {
    struct IDirectDraw7Vtbl *lpVtbl;
};

struct IDirectDraw7Vtbl {
    /* 0: IUnknown */
    HRESULT (WINAPI *QueryInterface)(IDirectDraw7 *, REFIID, PVOID *);
    ULONG   (WINAPI *AddRef)(IDirectDraw7 *);
    ULONG   (WINAPI *Release)(IDirectDraw7 *);
    /* 3: Compact */
    PVOID _pad3;
    /* 4: CreateClipper */
    HRESULT (WINAPI *CreateClipper)(IDirectDraw7 *self, DWORD dwFlags,
                                     PVOID *lplpDDClipper, PVOID pUnkOuter);
    /* 5: CreatePalette */
    HRESULT (WINAPI *CreatePalette)(IDirectDraw7 *self, DWORD dwFlags,
                                     PVOID lpDDColorArray,
                                     PVOID *lplpDDPalette, PVOID pUnkOuter);
    /* 6: CreateSurface */
    HRESULT (WINAPI *CreateSurface)(IDirectDraw7 *self, DDSURFACEDESC2 *desc,
                                     IDirectDrawSurface7 **surf, PVOID pUnkOuter);
    /* 7-9 */
    PVOID _pad7; PVOID _pad8; PVOID _pad9;
    /* 10: EnumSurfaces */
    PVOID _pad10;
    /* 11: FlipToGDISurface */
    PVOID _pad11;
    /* 12: GetCaps */
    PVOID _pad12;
    /* 13: GetDisplayMode */
    HRESULT (WINAPI *GetDisplayMode)(IDirectDraw7 *self, DDSURFACEDESC2 *desc);
    /* 14-19 */
    PVOID _pad14; PVOID _pad15; PVOID _pad16; PVOID _pad17;
    PVOID _pad18; PVOID _pad19;
    /* 20: SetCooperativeLevel */
    HRESULT (WINAPI *SetCooperativeLevel)(IDirectDraw7 *self, HANDLE hwnd, DWORD flags);
    /* 21: SetDisplayMode */
    HRESULT (WINAPI *SetDisplayMode)(IDirectDraw7 *self, DWORD w, DWORD h,
                                      DWORD bpp, DWORD refreshRate, DWORD flags);
};

static HRESULT WINAPI dd_QueryInterface(IDirectDraw7 *self, REFIID iid, PVOID *ppv)
{
    (void)iid;
    if (!ppv) return E_INVALIDARG;
    *ppv = self;
    return S_OK;
}

static ULONG WINAPI dd_AddRef(IDirectDraw7 *self) { (void)self; return 2; }
static ULONG WINAPI dd_Release(IDirectDraw7 *self) { (void)self; return 1; }

static HRESULT WINAPI dd_SetCooperativeLevel(IDirectDraw7 *self, HANDLE hwnd, DWORD flags)
{
    (void)self; (void)flags;
    ddraw_hwnd = hwnd;
    serial_puts("[DDRAW] SetCooperativeLevel\n");

    /* Clear GErrorHist + GIsCriticalError. The engine's Client init phase
     * triggers null-pointer faults (handled by our write-through) that set
     * GErrorHist="General protection fault!". SetCooperativeLevel is called
     * AFTER Client init and BEFORE Browse(). If GErrorHist is set, Browse()
     * skips rendering → error exit. Clear both for a fresh start. */
    {
        volatile uint16_t *gerr = (volatile uint16_t *)(uintptr_t)0x101E3474;
        volatile uint32_t *gcrit = (volatile uint32_t *)(uintptr_t)0x101E568C;
        if (*gerr != 0 || *gcrit != 0) {
            *gerr = 0;
            *gcrit = 0;
            serial_puts("[DDRAW] Cleared GErrorHist for Browse()\n");
        }
    }

    return DD_OK;
}

static HRESULT WINAPI dd_SetDisplayMode(IDirectDraw7 *self, DWORD w, DWORD h,
                                         DWORD bpp, DWORD refreshRate, DWORD flags)
{
    (void)self; (void)refreshRate; (void)flags;
    /* A 0-width/height mode is invalid (real DDraw → DDERR_INVALIDMODE). UT99's
     * SoftDrv passes (0,0,16) here — its UWindowsViewport hands SetRes a 0x0
     * extent. Rather than brick every surface with a 0x0 size, keep the current
     * (real) mode: SetDisplayMode(0,...) means "don't change resolution". Only
     * adopt w/h when both are non-zero. */
    if (w > 0 && h > 0) {
        display_width  = w;
        display_height = h;
    }
    if (bpp > 0)
        display_bpp = bpp;

    serial_puts("[DDRAW] SetDisplayMode req ");
    serial_puthex(w, 4); serial_puts("x"); serial_puthex(h, 4);
    serial_puts("x"); serial_puthex(bpp, 2);
    serial_puts(" -> using ");
    serial_puthex(display_width, 4); serial_puts("x"); serial_puthex(display_height, 4);
    serial_puts("\n");

    ensure_framebuffer();
    return DD_OK;
}

/* Map COM32 surface proxy to real surface */
static IDirectDrawSurface7 *com32_to_surface(uint32_t proxy_addr)
{
    /* The proxy_addr points to a COM32_Surface in surf_proxy32[] */
    COM32_Surface *p = (COM32_Surface *)(uintptr_t)proxy_addr;
    if (p >= surf_proxy32 && p < surf_proxy32 + MAX_SURFACES) {
        uint32_t idx = p->surf_index;
        if (idx < (uint32_t)surface_count)
            return &surfaces[idx];
    }
    /* Fallback: try as direct surface pointer (backward compat) */
    for (int i = 0; i < surface_count; i++) {
        if ((uint32_t)(ULONG_PTR)&surfaces[i] == proxy_addr)
            return &surfaces[i];
    }
    return surface_count > 0 ? &surfaces[0] : NULL;
}

/* GetAttachedSurface: returns back buffer attached to primary.
 * Args (stdcall): this, lpDDSCaps (DDSCAPS2*), lplpDDAttachedSurface (ptr*).
 * SoftDrv calls this after CreateSurface(PRIMARY+BACKBUFFERCOUNT=1) to get
 * the back buffer for Lock/render/Flip. */
static HRESULT WINAPI surf_GetAttachedSurface(IDirectDrawSurface7 *self,
                                                PVOID lpDDSCaps,
                                                uint32_t *lplpDDAttachedSurface)
{
    (void)lpDDSCaps;
    IDirectDrawSurface7 *real = REAL_SURF(self);
    if (!real || !lplpDDAttachedSurface)
        return DDERR_INVALIDPARAMS;

    DDSurface *ds = &real->surf;
    if (!ds->back_buffer) {
        serial_puts("[DDRAW] GetAttachedSurface: no back buffer\n");
        return DDERR_NOTFOUND;
    }

    /* Find the back buffer's index in surfaces[] */
    IDirectDrawSurface7 *back = (IDirectDrawSurface7 *)ds->back_buffer;
    int idx = (int)(back - surfaces);
    if (idx < 0 || idx >= surface_count) {
        serial_puts("[DDRAW] GetAttachedSurface: bad back buffer index\n");
        return DDERR_NOTFOUND;
    }

    /* Return COM32 proxy address (32-bit) */
    *lplpDDAttachedSurface = (uint32_t)(ULONG_PTR)&surf_proxy32[idx];
    serial_puts("[DDRAW] GetAttachedSurface: proxy=0x");
    serial_puthex((uint64_t)*lplpDDAttachedSurface, 8);
    serial_puts("\n");
    return DD_OK;
}

/* CreateClipper: IDirectDraw7 slot 4 — creates an IDirectDrawClipper.
 * Args (stdcall): this, dwFlags, lplpDDClipper, pUnkOuter */
static HRESULT WINAPI dd_CreateClipper(IDirectDraw7 *self, DWORD dwFlags,
                                        PVOID *lplpDDClipper, PVOID pUnkOuter)
{
    (void)self; (void)dwFlags; (void)pUnkOuter;
    if (!lplpDDClipper) return DDERR_INVALIDPARAMS;
    dd_memset(&g_ddclipper, 0, sizeof(g_ddclipper));
    /* Return 32-bit proxy if COM32 is initialized */
    extern uint32_t *clip_proxy32_ptr;
    if (clip_proxy32_ptr)
        *(uint32_t *)lplpDDClipper = (uint32_t)(ULONG_PTR)clip_proxy32_ptr;
    else
        *(uint32_t *)lplpDDClipper = (uint32_t)(ULONG_PTR)&g_ddclipper_obj;
    serial_puts("[DDRAW] CreateClipper\n");
    return DD_OK;
}

/* CreatePalette: IDirectDraw7 slot 5 — creates an IDirectDrawPalette.
 * Args (stdcall): this, dwFlags, lpDDColorArray, lplpDDPalette, pUnkOuter */
static HRESULT WINAPI dd_CreatePalette(IDirectDraw7 *self, DWORD dwFlags,
                                        PVOID lpDDColorArray,
                                        PVOID *lplpDDPalette, PVOID pUnkOuter)
{
    (void)self; (void)pUnkOuter;
    if (!lplpDDPalette) return DDERR_INVALIDPARAMS;

    /* Initialize palette entries from caller's color array if provided */
    dd_memset(&g_ddpalette, 0, sizeof(g_ddpalette));
    if (lpDDColorArray && (dwFlags & 0x04)) {
        /* DDPCAPS_8BIT = 0x04 → 256 entries */
        const BYTE *in = (const BYTE *)lpDDColorArray;
        for (int i = 0; i < 256; i++) {
            BYTE r = in[i * 4 + 0];
            BYTE g = in[i * 4 + 1];
            BYTE b = in[i * 4 + 2];
            g_ddpalette.entries[i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }

    /* Return 32-bit proxy if COM32 is initialized */
    extern uint32_t *pal_proxy32_ptr;
    if (pal_proxy32_ptr)
        *(uint32_t *)lplpDDPalette = (uint32_t)(ULONG_PTR)pal_proxy32_ptr;
    else
        *(uint32_t *)lplpDDPalette = (uint32_t)(ULONG_PTR)&g_ddpalette_obj;
    serial_puts("[DDRAW] CreatePalette flags=0x");
    serial_puthex(dwFlags, 4);
    serial_puts("\n");
    return DD_OK;
}

static HRESULT WINAPI dd_CreateSurface(IDirectDraw7 *self, DDSURFACEDESC2 *desc,
                                        IDirectDrawSurface7 **surf, PVOID pUnkOuter)
{
    (void)self; (void)pUnkOuter;
    if (!desc || !surf) return DDERR_INVALIDPARAMS;

    /* Read DDSURFACEDESC2 from 32-bit caller — use raw uint32_t access
     * because the struct layout differs between 32 and 64 bit */
    uint32_t *d32 = (uint32_t *)desc;
    uint32_t flags32 = d32[1];   /* dwFlags at offset 4 */
    uint32_t caps32  = d32[26];  /* ddsCaps.dwCaps at offset 104 */

    DWORD w   = (flags32 & DDSD_WIDTH)  ? d32[3] : display_width;   /* offset 12 */
    DWORD h   = (flags32 & DDSD_HEIGHT) ? d32[2] : display_height;  /* offset 8 */
    /* Honor an explicitly requested pixel format (real DDraw semantics); the
     * global display_bpp is only the default when the desc doesn't specify one. */
    DWORD bpp = display_bpp;
    if (flags32 & DDSD_PIXELFORMAT) {
        uint32_t req = d32[21];      /* ddpfPixelFormat.dwRGBBitCount @84 */
        if (req == 8 || req == 16 || req == 32) bpp = req;
    }
    int is_primary = (caps32 & DDSCAPS_PRIMARYSURFACE) ? 1 : 0;

    IDirectDrawSurface7 *s = alloc_surface(w, h, bpp, is_primary);
    if (!s) return E_OUTOFMEMORY;

    int idx = (int)(s - surfaces);

    /* If primary with back buffer, create back buffer surface */
    uint32_t bb_count = (flags32 & DDSD_BACKBUFFERCOUNT) ? d32[5] : 0; /* offset 20 */
    if (is_primary && bb_count > 0) {
        IDirectDrawSurface7 *back = alloc_surface(w, h, bpp, 0);
        if (back) {
            s->surf.back_buffer = (struct DDSurface *)back;
        }
    }

    /* Return 32-bit proxy address */
    *(uint32_t *)surf = (uint32_t)(ULONG_PTR)&surf_proxy32[idx];
    serial_puts("[DDRAW] CreateSurface: proxy=0x");
    serial_puthex((uint64_t)(ULONG_PTR)&surf_proxy32[idx], 8);
    serial_puts("\n");
    return DD_OK;
}

/* Fill a 32-bit DDPIXELFORMAT (8 dwords at `pf`) for the given bpp. Shared by
 * GetDisplayMode / EnumDisplayModes; mirrors surf_GetPixelFormat (the 565-mask
 * fix that cured the green tint). */
static void fill_pixfmt32(uint32_t *pf, uint32_t bpp)
{
    pf[0] = 32;                       /* dwSize */
    pf[2] = 0;                        /* dwFourCC */
    pf[3] = bpp;                      /* dwRGBBitCount */
    pf[7] = 0;                        /* dwRGBAlphaBitMask */
    if (bpp == 8) {
        pf[1] = 0x00000020;           /* DDPF_PALETTEINDEXED8 */
        pf[4] = pf[5] = pf[6] = 0;
    } else if (bpp == 16) {
        pf[1] = DDPF_RGB;
        pf[4] = 0xF800; pf[5] = 0x07E0; pf[6] = 0x001F;       /* RGB565 */
    } else {
        pf[1] = DDPF_RGB;
        pf[4] = 0x00FF0000; pf[5] = 0x0000FF00; pf[6] = 0x000000FF; /* XRGB8888 */
    }
}

static HRESULT WINAPI dd_GetDisplayMode(IDirectDraw7 *self, DDSURFACEDESC2 *desc)
{
    (void)self;
    if (!desc) return DDERR_INVALIDPARAMS;
    /* The caller is 32-bit PE code: write the 32-bit DDSURFACEDESC2 layout
     * (dwSize 124, ddpfPixelFormat at offset 72) with raw dword stores. The
     * 64-bit struct has an 8-byte lpSurface, so writing through it lands the
     * pixel format at the WRONG offset for the 32-bit reader. Also fill the
     * RGB masks — SoftDrv builds its color tables from them. */
    uint32_t *d = (uint32_t *)desc;
    for (int i = 0; i < 31; i++) d[i] = 0;
    d[0]  = 124;                                  /* dwSize */
    d[1]  = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
    d[2]  = display_height;                       /* dwHeight  @8  */
    d[3]  = display_width;                        /* dwWidth   @12 */
    d[4]  = display_width * (display_bpp / 8);    /* lPitch    @16 */
    fill_pixfmt32(d + 18, display_bpp);           /* ddpf      @72 */
    return DD_OK;
}

/* IDirectDraw::GetCaps — real vtable slot 11 (this + lpDDDriverCaps + lpDDHELCaps).
 * UE1's UWindowsViewport reads these caps to decide whether DirectDraw is usable
 * (fullscreen / BLIT_DirectDraw). A zeroed/garbage DDCAPS makes it bail. Report an
 * honest HEL-style software device: GDI-coexistent, blit + stretch + colorfill,
 * palette, sysmem blits. DDCAPS layout (32-bit): dwSize@0, dwCaps@4, dwCaps2@8,
 * dwPalCaps@24, dwVidMemTotal@60, dwVidMemFree@64. */
static HRESULT WINAPI dd_GetCaps(IDirectDraw7 *self, PVOID lpDriverCaps, PVOID lpHELCaps)
{
    (void)self;
    if (!lpDriverCaps && !lpHELCaps) return DDERR_INVALIDPARAMS;
    PVOID targets[2] = { lpDriverCaps, lpHELCaps };
    for (int t = 0; t < 2; t++) {
        uint32_t *c = (uint32_t *)targets[t];
        if (!c) continue;
        uint32_t size = c[0];                   /* caller sets dwSize */
        if (size < 8 || size > 1024) size = 380; /* DDCAPS_DX7 default */
        for (uint32_t i = 1; i < size / 4; i++) c[i] = 0;
        c[1] = 0x00000040u   /* DDCAPS_BLT          */
             | 0x00000200u   /* DDCAPS_BLTSTRETCH   */
             | 0x00008000u   /* DDCAPS_PALETTE      */
             | 0x00010000u   /* DDCAPS_GDI          */
             | 0x00400000u   /* DDCAPS_COLORKEY     */
             | 0x04000000u   /* DDCAPS_BLTCOLORFILL */
             | 0x80000000u;  /* DDCAPS_CANBLTSYSMEM */
        if (size >= 28)
            c[6] = 0x04 | 0x10 | 0x40;  /* dwPalCaps: 8BIT|PRIMARYSURFACE|ALLOW256 */
        if (size >= 68) {
            c[15] = 16u * 1024 * 1024;  /* dwVidMemTotal @60 */
            c[16] = 12u * 1024 * 1024;  /* dwVidMemFree  @64 */
        }
    }
    serial_puts("[DDRAW] GetCaps -> HEL caps reported\n");
    return DD_OK;
}

/* IDirectDraw7 vtable */
static struct IDirectDraw7Vtbl dd_vtbl = {
    dd_QueryInterface,
    dd_AddRef,
    dd_Release,
    NULL,                       /* 3: Compact */
    dd_CreateClipper,           /* 4 */
    dd_CreatePalette,           /* 5 */
    dd_CreateSurface,           /* 6 */
    NULL, NULL, NULL,           /* 7-9 */
    NULL,                       /* 10: EnumSurfaces */
    NULL,                       /* 11: FlipToGDISurface */
    NULL,                       /* 12: GetCaps */
    dd_GetDisplayMode,          /* 13 */
    NULL, NULL, NULL, NULL, NULL, NULL, /* 14-19 */
    dd_SetCooperativeLevel,     /* 20 */
    dd_SetDisplayMode           /* 21 */
};

static IDirectDraw7 g_ddraw = { &dd_vtbl };

/* ── DirectDraw entry points ───────────────────────────────── */

/* ── 32-bit COM proxy for compat32 mode ───────────────────── */
/*
 * The 64-bit IDirectDraw7 vtable has 8-byte function pointers.
 * 32-bit PE code reads 4-byte slots → reads wrong offsets.
 * Solution: create a 32-bit proxy with a vtable of 4-byte thunk addrs.
 * Each thunk does INT 0x2E → dispatches to the real 64-bit function.
 *
 * IDirectDraw vtable layout (22 methods, 4 bytes each):
 *   0: QueryInterface   6: CreateSurface  12: GetCaps
 *  13: GetDisplayMode  20: SetCooperativeLevel  21: SetDisplayMode
 * IDirectDrawSurface vtable (33 methods):
 *   5: Blt  11: Flip  12: GetAttachedSurface  17: GetDC
 *  22: GetSurfaceDesc  25: Lock  26: ReleaseDC  32: Unlock
 */

/* 32-bit COM proxy objects — allocated from PE32-accessible memory
 * (mem_alloc_pages) so their addresses are in the PE32 address space,
 * NOT in kernel BSS.  Previously these were BSS variables at ~0x025Dxxxx
 * which caused UT99 to confuse the DD proxy with UGameEngine (their
 * addresses overlapped with kernel globals the PE32 code was reading). */
static uint32_t *dd_vtbl32;            /* IDirectDraw vtable (23 slots) */
static uint32_t *dd_proxy32_ptr;       /* → 1 uint32_t: the lpVtbl32 */

static uint32_t *surf_vtbl32;          /* IDirectDrawSurface vtable (33 slots) */

/* Palette COM32 proxy */
static uint32_t *pal_vtbl32;           /* IDirectDrawPalette vtable (7 slots) */
uint32_t *pal_proxy32_ptr;             /* → 1 uint32_t: the lpVtbl32 */

/* Clipper COM32 proxy */
static uint32_t *clip_vtbl32;          /* IDirectDrawClipper vtable (9 slots) */
uint32_t *clip_proxy32_ptr;            /* → 1 uint32_t: the lpVtbl32 */

static int com32_initialized = 0;

/* IDirectDraw7::EnumDisplayModes — report available display modes via callback.
 * Args (stdcall, including this): this, dwFlags, lpDDSurfaceDesc, lpContext, lpCallback
 * Callback: HRESULT CALLBACK(LPDDSURFACEDESC2 desc, LPVOID ctx)
 *   Returns DDENUMRET_OK (1) to continue, DDENUMRET_CANCEL (0) to stop. */
static uint64_t WINAPI dd_EnumDisplayModes(
    uint64_t _this, uint64_t dwFlags, uint64_t lpDesc,
    uint64_t lpContext, uint64_t lpCallback)
{
    (void)_this; (void)dwFlags;

    /* Real DirectDraw semantics: a non-NULL lpDDSurfaceDesc is a FILTER — only
     * modes matching its specified fields are enumerated. UE1's UWindowsViewport
     * enumerates once per color depth with a pixel-format filter, storing each
     * depth's modes in a separate per-ColorBytes array; feeding ALL bpps into
     * every array breaks its mode bookkeeping. Honor bpp/width/height filters. */
    uint32_t f_flags = 0, f_bpp = 0, f_w = 0, f_h = 0;
    if (lpDesc) {
        const uint32_t *fd = (const uint32_t *)(uintptr_t)(uint32_t)lpDesc;
        f_flags = fd[1];
        if (f_flags & DDSD_PIXELFORMAT) f_bpp = fd[21];  /* dwRGBBitCount @84 */
        if (f_flags & DDSD_WIDTH)       f_w   = fd[3];
        if (f_flags & DDSD_HEIGHT)      f_h   = fd[2];
    }

    serial_puts("[DDRAW] EnumDisplayModes cb=0x");
    serial_puthex((uint32_t)lpCallback, 8);
    serial_puts(" filter bpp=");
    serial_putdec(f_bpp);
    serial_puts("\n");

    if (!lpCallback) return 0; /* DD_OK */

    static const struct { uint32_t w, h, bpp; } modes[] = {
        {640, 480, 8}, {800, 600, 8}, {1024, 768, 8},
        {640, 480, 16}, {800, 600, 16}, {1024, 768, 16},
        {640, 480, 32}, {800, 600, 32}, {1024, 768, 32},
    };

    for (int i = 0; i < 9; i++) {
        if (f_bpp && modes[i].bpp != f_bpp) continue;
        if (f_w   && modes[i].w   != f_w)   continue;
        if (f_h   && modes[i].h   != f_h)   continue;

        /* DDSURFACEDESC2 = 124 bytes. Use static buffer so the 32-bit
         * callback can access it (must be in lower 4GB). */
        static uint8_t desc_buf[128];
        for (int j = 0; j < 128; j++) desc_buf[j] = 0;
        uint32_t *d = (uint32_t *)desc_buf;
        d[0]  = 124;                  /* dwSize */
        d[1]  = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT
              | 0x00040000;           /* DDSD_REFRESHRATE */
        d[2]  = modes[i].h;           /* dwHeight */
        d[3]  = modes[i].w;           /* dwWidth */
        d[4]  = modes[i].w * (modes[i].bpp / 8); /* lPitch */
        d[5]  = 60;                   /* dwRefreshRate @20 (union) */
        fill_pixfmt32((uint32_t *)(desc_buf + 72), modes[i].bpp);

        uint32_t args[2] = {
            (uint32_t)(uintptr_t)desc_buf,
            (uint32_t)lpContext
        };
        uint32_t ret = compat32_callback_args((uint32_t)lpCallback, 2, args);
        if (ret == 0) break; /* DDENUMRET_CANCEL */
    }

    return 0; /* DD_OK */
}

/* Generic COM stub — returns S_OK for any unimplemented method */
static HRESULT WINAPI dd_com_stub(PVOID this_ptr)
{
    (void)this_ptr;
    return 0; /* S_OK */
}

static void ddraw_init_com32(void)
{
    if (com32_initialized) return;

    extern uint32_t compat32_make_thunk_ex(uint64_t target, const char *name,
                                            uint8_t num_args, uint8_t callconv);
    extern void *mem_alloc_pages(uint64_t count);
    /* CC_STDCALL comes from compat32.h (== 0). Do NOT redefine it here:
     * a local `#define CC_STDCALL 1` collides with CC_CDECL (==1), so
     * emit_thunk() would emit `ret` (cdecl, no cleanup) instead of
     * `ret N`. The ddraw COM methods are stdcall (callee cleans), and
     * the leaked args drift ESP → corrupt the caller's saved ESI →
     * NULL virtual call in WinDrv ResizeViewport (no frame presents). */

    /* Allocate COM proxy objects from PE32-accessible memory.
     * Layout in 1 page:
     *   dd_vtbl32[23]       @ 0     (92 bytes)
     *   dd_proxy32          @ 96    (4 bytes)
     *   surf_vtbl32[33]     @ 128   (132 bytes)
     *   surf_proxy32[8]     @ 272   (64 bytes)
     *   pal_vtbl32[7]       @ 340   (28 bytes)
     *   pal_proxy32         @ 372   (4 bytes)
     *   clip_vtbl32[9]      @ 380   (36 bytes)
     *   clip_proxy32        @ 420   (4 bytes)
     */
    uint8_t *page = (uint8_t *)mem_alloc_pages(1);
    if (!page) { serial_puts("[DDRAW] COM proxy alloc FAILED\n"); return; }
    for (int i = 0; i < 4096; i++) page[i] = 0;

    dd_vtbl32       = (uint32_t *)(page + 0);           /* 23 * 4 = 92 bytes */
    dd_proxy32_ptr  = (uint32_t *)(page + 96);           /* 4 bytes */
    surf_vtbl32     = (uint32_t *)(page + 128);          /* 33 * 4 = 132 bytes */
    surf_proxy32    = (COM32_Surface *)(page + 272);     /* 8 * 8 = 64 bytes */
    pal_vtbl32      = (uint32_t *)(page + 340);          /* 7 * 4 = 28 bytes */
    pal_proxy32_ptr = (uint32_t *)(page + 372);          /* 4 bytes */
    clip_vtbl32     = (uint32_t *)(page + 380);          /* 9 * 4 = 36 bytes */
    clip_proxy32_ptr = (uint32_t *)(page + 420);         /* 4 bytes */

    serial_puts("[DDRAW] COM proxies at 0x");
    extern void serial_puthex(uint64_t val, int digits);
    serial_puthex((uint64_t)(uintptr_t)page, 8);
    serial_puts("\n");

    /* IDirectDraw vtable thunks (stdcall, include 'this' in arg count) */
    dd_vtbl32[0]  = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_QueryInterface,
                                            "DD_QI", 3, CC_STDCALL);
    dd_vtbl32[1]  = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_AddRef,
                                            "DD_AddRef", 1, CC_STDCALL);
    dd_vtbl32[2]  = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_Release,
                                            "DD_Release", 1, CC_STDCALL);
    dd_vtbl32[4]  = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_CreateClipper,
                                            "DD_CreateClipper", 4, CC_STDCALL);
    dd_vtbl32[5]  = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_CreatePalette,
                                            "DD_CreatePalette", 5, CC_STDCALL);
    dd_vtbl32[6]  = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_CreateSurface,
                                            "DD_CreateSurface", 4, CC_STDCALL);
    dd_vtbl32[8]  = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_EnumDisplayModes,
                                            "DD_EnumDisplayModes", 5, CC_STDCALL);
    dd_vtbl32[11] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_GetCaps,
                                            "DD_GetCaps", 3, CC_STDCALL);
    dd_vtbl32[12] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_GetDisplayMode,
                                            "DD_GetDisplayMode", 2, CC_STDCALL);
    /* slot 13 = GetFourCCCodes (NOT a duplicate GetDisplayMode) — left 0 so the
     * fill loop installs a correct 3-arg stub. The old `[13]=[12]` duplicate
     * shifted the dd_args labels and broke RestoreDisplayMode (slot 19). */
    dd_vtbl32[20] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_SetCooperativeLevel,
                                            "DD_SetCoopLevel", 3, CC_STDCALL);
    dd_vtbl32[21] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_SetDisplayMode,
                                            "DD_SetDisplayMode", 6, CC_STDCALL);

    /* IDirectDrawSurface vtable thunks */
    surf_vtbl32[0]  = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_QueryInterface,
                                              "Surf_QI", 3, CC_STDCALL);
    surf_vtbl32[1]  = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_AddRef,
                                              "Surf_AddRef", 1, CC_STDCALL);
    surf_vtbl32[2]  = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_Release,
                                              "Surf_Release", 1, CC_STDCALL);
    surf_vtbl32[5]  = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_Blt,
                                              "Surf_Blt", 6, CC_STDCALL);  /* this+DestRect,SrcSurf,SrcRect,Flags,BltFx */
    surf_vtbl32[7]  = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_BltFast,
                                              "Surf_BltFast", 6, CC_STDCALL);
    surf_vtbl32[11] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_Flip,
                                              "Surf_Flip", 3, CC_STDCALL);
    surf_vtbl32[12] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_GetAttachedSurface,
                                              "Surf_GetAttached", 3, CC_STDCALL);
    surf_vtbl32[21] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_GetPixelFormat,
                                              "Surf_GetPixelFormat", 2, CC_STDCALL);
    surf_vtbl32[22] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_GetSurfaceDesc,
                                              "Surf_GetDesc", 2, CC_STDCALL);
    surf_vtbl32[15] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_GetClipper,
                                              "Surf_GetClipper", 2, CC_STDCALL);
    surf_vtbl32[20] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_GetPalette,
                                              "Surf_GetPalette", 2, CC_STDCALL);
    surf_vtbl32[25] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_Lock,
                                              "Surf_Lock", 5, CC_STDCALL);
    surf_vtbl32[28] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_SetClipper,
                                              "Surf_SetClipper", 2, CC_STDCALL);
    surf_vtbl32[31] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_SetPalette,
                                              "Surf_SetPalette", 2, CC_STDCALL);
    surf_vtbl32[32] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_Unlock,
                                              "Surf_Unlock", 2, CC_STDCALL);

    /* Fill unimplemented DD slots with stub thunks (return S_OK).
     * CRITICAL: each stub MUST have the correct stdcall arg count,
     * otherwise RET N pops wrong number of bytes → stack corruption
     * → SEH chain destroyed → engine can't catch exceptions. */
    {
        /* IDirectDraw vtable arg counts (including 'this'). MUST match the real
         * IDirectDraw layout exactly — a wrong count makes the stub's RET N
         * over/under-clean the caller's stack. The table was previously
         * mis-labelled from slot 13 on (a stray "GetDisplayMode at 13"
         * duplicate shifted everything by one), which left slot 19
         * (RestoreDisplayMode = 1 arg) registered as 2. UWindowsViewport's
         * fullscreen ResizeViewport calls RestoreDisplayMode (RenDev->vtable[19],
         * 1 pushed arg); the 2-arg stub did RET 8 → over-cleaned 4 bytes →
         * stack imbalance → the WinDrv wrapper's `pop edi/esi/ebx` (which run
         * BEFORE `mov esp,ebp`) read shifted slots → corrupt viewport `this` →
         * #PF on a 0x20 vtable. Correct layout below. */
        static const uint8_t dd_args[23] = {
            3,1,1,  /* 0:QI 1:AddRef 2:Release (implemented) */
            1,      /* 3:Compact */
            4,      /* 4:CreateClipper */
            5,      /* 5:CreatePalette */
            4,      /* 6:CreateSurface (implemented) */
            3,      /* 7:DuplicateSurface */
            5,      /* 8:EnumDisplayModes */
            5,      /* 9:EnumSurfaces (dwFlags,lpDDSD,lpCtx,lpCb) */
            1,      /* 10:FlipToGDISurface */
            3,      /* 11:GetCaps */
            2,      /* 12:GetDisplayMode (implemented) */
            3,      /* 13:GetFourCCCodes (lpNumCodes,lpCodes) */
            2,      /* 14:GetGDISurface */
            2,      /* 15:GetMonitorFrequency */
            2,      /* 16:GetScanLine */
            2,      /* 17:GetVerticalBlankStatus */
            2,      /* 18:Initialize (lpGUID) */
            1,      /* 19:RestoreDisplayMode (this only) */
            3,      /* 20:SetCooperativeLevel (implemented) */
            6,      /* 21:SetDisplayMode (implemented) */
            3,      /* 22:WaitForVerticalBlank */
        };
        for (int i = 0; i < 23; i++) {
            if (dd_vtbl32[i] == 0) {
                dd_vtbl32[i] = compat32_make_thunk_ex(
                    (uint64_t)(ULONG_PTR)dd_com_stub,
                    "DD_stub", dd_args[i], CC_STDCALL);
            }
        }
    }

    /* Fill unimplemented Surface slots with correct arg counts */
    {
        /* IDirectDrawSurface arg counts (including 'this'). Corrected against
         * the real IDirectDrawSurface vtable — several were short, which makes
         * an unimplemented-slot stub RET too few bytes and imbalance the caller
         * (the engine's SetRes Blt-clears the surfaces during fullscreen
         * ResizeViewport). */
        static const uint8_t sf_args[33] = {
            3,1,1,  /* 0:QI 1:AddRef 2:Release */
            2,      /* 3:AddAttachedSurface */
            2,      /* 4:AddOverlayDirtyRect */
            6,      /* 5:Blt (DestRect,SrcSurf,SrcRect,Flags,BltFx) */
            4,      /* 6:BltBatch */
            6,      /* 7:BltFast (x,y,SrcSurf,SrcRect,Trans) */
            3,      /* 8:DeleteAttachedSurface (Flags,Surf) */
            3,      /* 9:EnumAttachedSurfaces */
            4,      /* 10:EnumOverlayZOrders (Flags,Ctx,Cb) */
            3,      /* 11:Flip (implemented) */
            3,      /* 12:GetAttachedSurface */
            2,      /* 13:GetBltStatus */
            2,      /* 14:GetCaps */
            2,      /* 15:GetClipper */
            3,      /* 16:GetColorKey (Flags,ColorKey) */
            2,      /* 17:GetDC */
            2,      /* 18:GetFlipStatus */
            3,      /* 19:GetOverlayPosition (lX,lY) */
            2,      /* 20:GetPalette */
            2,      /* 21:GetPixelFormat */
            2,      /* 22:GetSurfaceDesc (implemented) */
            3,      /* 23:Initialize */
            1,      /* 24:IsLost */
            5,      /* 25:Lock (implemented) */
            2,      /* 26:ReleaseDC */
            1,      /* 27:Restore */
            2,      /* 28:SetClipper */
            3,      /* 29:SetColorKey */
            3,      /* 30:SetOverlayPosition (X,Y) */
            2,      /* 31:SetPalette */
            2,      /* 32:Unlock (implemented) */
        };
        for (int i = 0; i < 33; i++) {
            if (surf_vtbl32[i] == 0) {
                surf_vtbl32[i] = compat32_make_thunk_ex(
                    (uint64_t)(ULONG_PTR)dd_com_stub,
                    "Surf_stub", sf_args[i], CC_STDCALL);
            }
        }
    }

    /* Setup DD proxy object */
    *dd_proxy32_ptr = (uint32_t)(ULONG_PTR)dd_vtbl32;

    /* Setup surface proxies */
    for (int i = 0; i < MAX_SURFACES; i++) {
        surf_proxy32[i].lpVtbl32 = (uint32_t)(ULONG_PTR)surf_vtbl32;
        surf_proxy32[i].surf_index = (uint32_t)i;
    }

    /* IDirectDrawPalette COM32 vtable (7 methods) */
    pal_vtbl32[0] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)pal_QueryInterface,
                                            "Pal_QI", 3, CC_STDCALL);
    pal_vtbl32[1] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)pal_AddRef,
                                            "Pal_AddRef", 1, CC_STDCALL);
    pal_vtbl32[2] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)pal_Release,
                                            "Pal_Release", 1, CC_STDCALL);
    pal_vtbl32[3] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)pal_GetCaps,
                                            "Pal_GetCaps", 2, CC_STDCALL);
    pal_vtbl32[4] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)pal_GetEntries,
                                            "Pal_GetEntries", 5, CC_STDCALL);
    pal_vtbl32[5] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)pal_Initialize,
                                            "Pal_Init", 4, CC_STDCALL);
    pal_vtbl32[6] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)pal_SetEntries,
                                            "Pal_SetEntries", 5, CC_STDCALL);
    *pal_proxy32_ptr = (uint32_t)(ULONG_PTR)pal_vtbl32;
    /* Point the palette object at the proxy vtable for COM32 calls */
    g_ddpalette_obj.pal = &g_ddpalette;

    /* IDirectDrawClipper COM32 vtable (9 methods) */
    clip_vtbl32[0] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)clip_QueryInterface,
                                             "Clip_QI", 3, CC_STDCALL);
    clip_vtbl32[1] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)clip_AddRef,
                                             "Clip_AddRef", 1, CC_STDCALL);
    clip_vtbl32[2] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)clip_Release,
                                             "Clip_Release", 1, CC_STDCALL);
    clip_vtbl32[3] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_com_stub,
                                             "Clip_stub", 4, CC_STDCALL); /* GetClipList */
    clip_vtbl32[4] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)clip_GetHWnd,
                                             "Clip_GetHWnd", 2, CC_STDCALL);
    clip_vtbl32[5] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_com_stub,
                                             "Clip_stub", 3, CC_STDCALL); /* Initialize */
    clip_vtbl32[6] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_com_stub,
                                             "Clip_stub", 2, CC_STDCALL); /* IsClipListChanged */
    clip_vtbl32[7] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_com_stub,
                                             "Clip_stub", 3, CC_STDCALL); /* SetClipList */
    clip_vtbl32[8] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)clip_SetHWnd,
                                             "Clip_SetHWnd", 3, CC_STDCALL);
    *clip_proxy32_ptr = (uint32_t)(ULONG_PTR)clip_vtbl32;
    g_ddclipper_obj.clip = &g_ddclipper;

    com32_initialized = 1;
    serial_puts("[DDRAW] COM32 proxies initialized (surfaces+palette+clipper)\n");

    /* Hardware watchpoint on dd_vtbl32[0] to catch runtime corruption. */
    {
        uint64_t watch0 = (uint64_t)(uintptr_t)dd_vtbl32;
        /* DR1 for GIsCriticalError DISABLED: Core.dll legitimately writes
         * this flag during post-rendering init. The #DB handler can't advance
         * RIP, creating an infinite #DB loop → triple fault. The flag is
         * already cleared on every INT 0x2E in compat32_dispatch. */
        /* DR7: L0=1(bit0) RW0=01(bits16-17) LEN0=11(bits18-19)
         * = 0x000D0001 (DR0 only, DR1 disabled) */
        __asm__ volatile (
            "mov %0, %%dr0\n"
            "mov $0x000D0001, %%rax\n"
            "mov %%rax, %%dr7\n"
            :: "r"(watch0) : "rax"
        );
        serial_puts("[DDRAW] DR0 watchpoint on dd_vtbl32 at 0x");
        extern void serial_puthex(uint64_t val, int digits);
        serial_puthex(watch0, 8);
        serial_puts(" (DR1/GIsCriticalError disabled)\n");
    }
}

HRESULT WINAPI DirectDrawCreate(LPGUID lpGUID, PVOID *lplpDD, PVOID pUnkOuter)
{
    (void)lpGUID; (void)pUnkOuter;
    serial_puts("[DDRAW] DirectDrawCreate\n");
    if (!lplpDD) return DDERR_INVALIDPARAMS;

    ddraw_init_com32();

    /* Return 32-bit proxy address (not the 64-bit g_ddraw) */
    *(uint32_t *)lplpDD = (uint32_t)(ULONG_PTR)dd_proxy32_ptr;
    return DD_OK;
}

HRESULT WINAPI DirectDrawCreateEx(LPGUID lpGUID, PVOID *lplpDD,
                                   REFIID iid, PVOID pUnkOuter)
{
    (void)iid;
    return DirectDrawCreate(lpGUID, lplpDD, pUnkOuter);
}

HRESULT WINAPI DirectDrawEnumerateA(LPDDENUMCALLBACKA lpCallback, PVOID lpContext)
{
    if (lpCallback) {
        /* Call 32-bit callback via compat32 mode switch.
         * lpCallback is a 32-bit function — can't call directly from 64-bit. */
        static const char dd_driver_desc[] = "Primary Display Driver";
        static const char dd_driver_name[] = "display";
        uint32_t args[4] = {
            0,                                          /* lpGUID = NULL */
            (uint32_t)(uintptr_t)dd_driver_desc,        /* lpDriverDescription */
            (uint32_t)(uintptr_t)dd_driver_name,        /* lpDriverName */
            (uint32_t)(uintptr_t)lpContext               /* lpContext */
        };
        compat32_callback_args((uint32_t)(uintptr_t)lpCallback, 4, args);
    }
    return DD_OK;
}

/* ── Export table ──────────────────────────────────────────── */

typedef struct { const char *name; PVOID func; uint8_t argc; uint8_t cc; } SHIM_EXPORT;

static const SHIM_EXPORT ddraw_exports[] = {
    { "DirectDrawCreate",      (PVOID)DirectDrawCreate,     3, CC_STDCALL },
    { "DirectDrawCreateEx",    (PVOID)DirectDrawCreateEx,   4, CC_STDCALL },
    { "DirectDrawEnumerateA",  (PVOID)DirectDrawEnumerateA, 2, CC_STDCALL },
    { NULL, NULL, 0, CC_STDCALL }
};

const WIN32_EXPORT *ddraw_abi_table(int *count) {
    *count = (int)(sizeof(ddraw_exports)/sizeof(ddraw_exports[0]));
    return (const WIN32_EXPORT *)ddraw_exports;
}

static int dd_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

PVOID ddraw_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    if (by_ordinal) return NULL;
    for (int i = 0; ddraw_exports[i].name; i++) {
        if (dd_strcmp(func_name, ddraw_exports[i].name) == 0)
            return ddraw_exports[i].func;
    }
    return NULL;
}

PVOID ddraw_shim_init(void)
{
    /* Full reset for process re-exec (UT99 relaunches itself to apply a video
     * change). CRITICAL: com32_initialized must drop so ddraw_init_com32 builds
     * FRESH COM proxy vtables — compat32_init() re-allocates the thunk pool each
     * run, so the old vtables hold dangling thunk addresses. Old proxy/surface
     * pages leak (bounded by relaunch count). */
    com32_initialized = 0;
    surface_count = 0;
    dd_memset(surfaces, 0, sizeof(surfaces));
    g_present_surface = NULL;
    g_present_src_w = 0;
    g_present_src_h = 0;
    display_width  = 640;
    display_height = 480;
    display_bpp    = 16;
    framebuffer = NULL;
    fb_size = 0;
    gop_pitch = 0;
    ddraw_hwnd = NULL;
    dd_memset(&g_ddpalette, 0, sizeof(g_ddpalette));
    dd_memset(&g_ddclipper, 0, sizeof(g_ddclipper));
    return (PVOID)ddraw_exports;
}
