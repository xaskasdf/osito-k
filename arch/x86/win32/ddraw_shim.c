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

extern uint32_t compat32_callback_args(uint32_t func_addr, int nargs, const uint32_t *args);

/* Types we need from user32/gdi32 without pulling in the full headers */
typedef struct tagRECT_DD { LONG left, top, right, bottom; } RECT_DD;
typedef RECT_DD *LPRECT;
typedef HANDLE  HDC;

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
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

static DWORD display_width  = 800;
static DWORD display_height = 600;
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
    /* 6-10 */
    PVOID _pad6; PVOID _pad7; PVOID _pad8; PVOID _pad9; PVOID _pad10;
    /* 11: Flip */
    HRESULT (WINAPI *Flip)(IDirectDrawSurface7 *self,
                            IDirectDrawSurface7 *override, DWORD flags);
    /* 12-16 */
    PVOID _pad12; PVOID _pad13; PVOID _pad14; PVOID _pad15; PVOID _pad16;
    /* 17: GetDC */
    HRESULT (WINAPI *GetDC)(IDirectDrawSurface7 *self, HDC *hdc);
    /* 18-21 */
    PVOID _pad18; PVOID _pad19; PVOID _pad20; PVOID _pad21;
    /* 22: GetSurfaceDesc */
    HRESULT (WINAPI *GetSurfaceDesc)(IDirectDrawSurface7 *self, DDSURFACEDESC2 *desc);
    /* 23-24 */
    PVOID _pad23; PVOID _pad24;
    /* 25: Lock */
    HRESULT (WINAPI *Lock)(IDirectDrawSurface7 *self, LPRECT destRect,
                            DDSURFACEDESC2 *desc, DWORD flags, HANDLE hEvent);
    /* 26: ReleaseDC */
    HRESULT (WINAPI *ReleaseDC)(IDirectDrawSurface7 *self, HDC hdc);
    /* 27-31 */
    PVOID _pad27; PVOID _pad28; PVOID _pad29; PVOID _pad30; PVOID _pad31;
    /* 32: Unlock */
    HRESULT (WINAPI *Unlock)(IDirectDrawSurface7 *self, LPRECT lpRect);
};

/* Surface implementations */

/* ── COM32 proxy types ────────────────────────────────────── */
typedef struct {
    uint32_t lpVtbl32;
    uint32_t surf_index;
} COM32_Surface;
static COM32_Surface surf_proxy32[MAX_SURFACES];

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

    /* ddpfPixelFormat at offset 72 (32-bit layout) */
    d[18] = 32;  /* ddpfPixelFormat.dwSize at offset 72 */
    d[19] = DDPF_RGB;  /* dwFlags at offset 76 */
    d[22] = s->bpp;  /* dwRGBBitCount at offset 88 */

    if (s->bpp == 16) {
        d[23] = 0xF800;  /* dwRBitMask at offset 92 */
        d[24] = 0x07E0;  /* dwGBitMask at offset 96 */
        d[25] = 0x001F;  /* dwBBitMask at offset 100 */
    } else {
        d[23] = 0x00FF0000;
        d[24] = 0x0000FF00;
        d[25] = 0x000000FF;
    }

    s->locked = 1;
    return DD_OK;
}

static HRESULT WINAPI surf_Unlock(IDirectDrawSurface7 *self, LPRECT lpRect)
{
    (void)lpRect;
    IDirectDrawSurface7 *real = REAL_SURF(self);
    if (real) real->surf.locked = 0;
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
    (void)destRect; (void)srcRect; (void)dwFlags; (void)lpDDBltFx;
    IDirectDrawSurface7 *real_self = REAL_SURF(self);
    if (!real_self) return DDERR_INVALIDPARAMS;
    DDSurface *dst = &real_self->surf;

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

    /* If primary surface, blit to GOP framebuffer */
    if (dst->is_primary) {
        ensure_framebuffer();
        if (framebuffer && dst->pixels) {
            /* Convert RGB565 → XRGB8888, respecting GOP pitch */
            if (dst->bpp == 16) {
                uint16_t *src16 = (uint16_t *)dst->pixels;
                uint32_t *dst32 = (uint32_t *)framebuffer;
                DWORD pitch = gop_pitch ? gop_pitch : dst->width;
                for (DWORD y = 0; y < dst->height; y++) {
                    for (DWORD x = 0; x < dst->width; x++) {
                        uint16_t c = src16[y * dst->width + x];
                        uint32_t r = ((c >> 11) & 0x1F) * 255 / 31;
                        uint32_t g = ((c >> 5)  & 0x3F) * 255 / 63;
                        uint32_t b = (c & 0x1F) * 255 / 31;
                        dst32[y * pitch + x] = (r << 16) | (g << 8) | b;
                    }
                }
            } else if (dst->bpp == 32) {
                uint32_t *src32 = (uint32_t *)dst->pixels;
                uint32_t *dst32 = (uint32_t *)framebuffer;
                DWORD pitch = gop_pitch ? gop_pitch : dst->width;
                for (DWORD y = 0; y < dst->height; y++)
                    dd_memcpy(&dst32[y * pitch], &src32[y * dst->width], dst->width * 4);
            }
        }
    }

    /* Also copy to compositor's shm surface if available */
    if (dst->is_primary)
        ddraw_compositor_notify();

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

    /* Blit primary to GOP framebuffer */
    ensure_framebuffer();
    if (framebuffer && primary->pixels) {
        if (primary->bpp == 16) {
            uint16_t *src16 = (uint16_t *)primary->pixels;
            uint32_t *dst32 = (uint32_t *)framebuffer;
            DWORD pitch = gop_pitch ? gop_pitch : primary->width;
            for (DWORD y = 0; y < primary->height; y++) {
                for (DWORD x = 0; x < primary->width; x++) {
                    uint16_t c = src16[y * primary->width + x];
                    uint32_t r = ((c >> 11) & 0x1F) * 255 / 31;
                    uint32_t g = ((c >> 5)  & 0x3F) * 255 / 63;
                    uint32_t b = (c & 0x1F) * 255 / 31;
                    dst32[y * pitch + x] = (r << 16) | (g << 8) | b;
                }
            }
        }
    }

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

/* Shared vtable */
static struct IDirectDrawSurface7Vtbl surface_vtbl = {
    surf_QueryInterface,
    surf_AddRef,
    surf_Release,
    NULL, NULL,                 /* 3-4 */
    surf_Blt,                   /* 5 */
    NULL, NULL, NULL, NULL, NULL, /* 6-10 */
    surf_Flip,                  /* 11 */
    NULL, NULL, NULL, NULL, NULL, /* 12-16 */
    surf_GetDC,                 /* 17 */
    NULL, NULL, NULL, NULL,     /* 18-21 */
    surf_GetSurfaceDesc,        /* 22 */
    NULL, NULL,                 /* 23-24 */
    surf_Lock,                  /* 25 */
    surf_ReleaseDC,             /* 26 */
    NULL, NULL, NULL, NULL, NULL, /* 27-31 */
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
    PVOID _pad4;
    /* 5: CreatePalette */
    PVOID _pad5;
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
    return DD_OK;
}

static HRESULT WINAPI dd_SetDisplayMode(IDirectDraw7 *self, DWORD w, DWORD h,
                                         DWORD bpp, DWORD refreshRate, DWORD flags)
{
    (void)self; (void)refreshRate; (void)flags;
    display_width  = w;
    display_height = h;
    display_bpp    = bpp;

    serial_puts("[DDRAW] SetDisplayMode ");
    serial_puthex(w, 4); serial_puts("x"); serial_puthex(h, 4);
    serial_puts("x"); serial_puthex(bpp, 2);
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
    DWORD bpp = display_bpp;
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

static HRESULT WINAPI dd_GetDisplayMode(IDirectDraw7 *self, DDSURFACEDESC2 *desc)
{
    (void)self;
    if (!desc) return DDERR_INVALIDPARAMS;
    dd_memset(desc, 0, sizeof(DDSURFACEDESC2));
    desc->dwSize    = sizeof(DDSURFACEDESC2);
    desc->dwFlags   = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    desc->dwWidth   = display_width;
    desc->dwHeight  = display_height;
    desc->ddpfPixelFormat.dwSize        = sizeof(DDPIXELFORMAT);
    desc->ddpfPixelFormat.dwFlags       = DDPF_RGB;
    desc->ddpfPixelFormat.dwRGBBitCount = display_bpp;
    return DD_OK;
}

/* IDirectDraw7 vtable */
static struct IDirectDraw7Vtbl dd_vtbl = {
    dd_QueryInterface,
    dd_AddRef,
    dd_Release,
    NULL,                       /* 3: Compact */
    NULL,                       /* 4: CreateClipper */
    NULL,                       /* 5: CreatePalette */
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

/* 32-bit proxy objects — BSS, guaranteed < 4GB in our link layout */
static uint32_t dd_vtbl32[23];  /* IDirectDraw vtable (23 slots) */
static uint32_t dd_proxy32;     /* IDirectDraw COM object: just lpVtbl32 */

static uint32_t surf_vtbl32[33]; /* IDirectDrawSurface vtable */
static int com32_initialized = 0;

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
    #define CC_STDCALL 1

    /* Zero all vtables */
    for (int i = 0; i < 23; i++) dd_vtbl32[i] = 0;
    for (int i = 0; i < 33; i++) surf_vtbl32[i] = 0;

    /* IDirectDraw vtable thunks (stdcall, include 'this' in arg count) */
    dd_vtbl32[0]  = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_QueryInterface,
                                            "DD_QI", 3, CC_STDCALL);
    dd_vtbl32[1]  = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_AddRef,
                                            "DD_AddRef", 1, CC_STDCALL);
    dd_vtbl32[2]  = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_Release,
                                            "DD_Release", 1, CC_STDCALL);
    dd_vtbl32[6]  = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_CreateSurface,
                                            "DD_CreateSurface", 4, CC_STDCALL);
    dd_vtbl32[13] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_GetDisplayMode,
                                            "DD_GetDisplayMode", 2, CC_STDCALL);
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
                                              "Surf_Blt", 7, CC_STDCALL);
    surf_vtbl32[11] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_Flip,
                                              "Surf_Flip", 3, CC_STDCALL);
    surf_vtbl32[22] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_GetSurfaceDesc,
                                              "Surf_GetDesc", 2, CC_STDCALL);
    surf_vtbl32[25] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_Lock,
                                              "Surf_Lock", 5, CC_STDCALL);
    surf_vtbl32[32] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_Unlock,
                                              "Surf_Unlock", 2, CC_STDCALL);

    /* Fill unimplemented DD slots with stub thunks (return S_OK).
     * Prevents NULL-CALL when engine calls GetCaps, CreateClipper, etc. */
    {
        uint32_t stub = compat32_make_thunk_ex(
            (uint64_t)(ULONG_PTR)dd_com_stub, "DD_stub", 12, CC_STDCALL);
        for (int i = 0; i < 23; i++)
            if (dd_vtbl32[i] == 0) dd_vtbl32[i] = stub;
    }

    /* Fill unimplemented Surface slots with stub thunks */
    {
        uint32_t stub = compat32_make_thunk_ex(
            (uint64_t)(ULONG_PTR)dd_com_stub, "Surf_stub", 12, CC_STDCALL);
        for (int i = 0; i < 33; i++)
            if (surf_vtbl32[i] == 0) surf_vtbl32[i] = stub;
    }

    /* Setup DD proxy object */
    dd_proxy32 = (uint32_t)(ULONG_PTR)dd_vtbl32;

    /* Setup surface proxies */
    for (int i = 0; i < MAX_SURFACES; i++) {
        surf_proxy32[i].lpVtbl32 = (uint32_t)(ULONG_PTR)surf_vtbl32;
        surf_proxy32[i].surf_index = (uint32_t)i;
    }

    com32_initialized = 1;
    serial_puts("[DDRAW] COM32 proxies initialized\n");
}

HRESULT WINAPI DirectDrawCreate(LPGUID lpGUID, PVOID *lplpDD, PVOID pUnkOuter)
{
    (void)lpGUID; (void)pUnkOuter;
    serial_puts("[DDRAW] DirectDrawCreate\n");
    if (!lplpDD) return DDERR_INVALIDPARAMS;

    ddraw_init_com32();

    /* Return 32-bit proxy address (not the 64-bit g_ddraw) */
    *(uint32_t *)lplpDD = (uint32_t)(ULONG_PTR)&dd_proxy32;
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

typedef struct { const char *name; PVOID func; } SHIM_EXPORT;

static const SHIM_EXPORT ddraw_exports[] = {
    { "DirectDrawCreate",      (PVOID)DirectDrawCreate },
    { "DirectDrawCreateEx",    (PVOID)DirectDrawCreateEx },
    { "DirectDrawEnumerateA",  (PVOID)DirectDrawEnumerateA },
    { NULL, NULL }
};

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
    surface_count = 0;
    framebuffer = NULL;
    fb_size = 0;
    return (PVOID)ddraw_exports;
}
