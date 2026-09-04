/*
 * OsitoK Windows Compatibility Layer — ddraw.dll Shim Implementation
 *
 * Implements the legacy IDirectDraw interface generations on top of shared
 * software surfaces. Blt/Flip copy surface contents to OsitoK's scanout path.
 */

#include "ddraw_shim.h"
#include "kernel32_shim.h"
#include "paging.h"
#include "winmm_shim.h"
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
extern uint64_t mem_get_total(void) __attribute__((weak));
extern uint64_t mem_get_free(void) __attribute__((weak));
extern DWORD win32_current_process_id(void);
extern NTSTATUS nt_vm_release_allocation_for_process(ULONG owner_pid,
                                                       PVOID allocation_base);

/* Compositor integration — user32 provides the per-window shm info */
extern void    *user32_get_window_shm_pixels(void *hwnd)      __attribute__((weak));
extern uint32_t user32_get_window_compositor_id(void *hwnd)    __attribute__((weak));
extern BOOL user32_get_window_surface(HANDLE hwnd, void **pixels, int *width,
                                      int *height, int *pitch)
    __attribute__((weak));
extern void user32_mark_window_dirty(HANDLE hwnd) __attribute__((weak));
extern BOOL user32_configure_directdraw_window(HANDLE hwnd, BOOL exclusive,
                                                BOOL allow_window_changes,
                                                int width, int height)
    __attribute__((weak));
extern uint32_t user32_get_display_resolution_count(void)
    __attribute__((weak));
extern BOOL user32_get_display_resolution(uint32_t index, uint32_t *width,
                                          uint32_t *height)
    __attribute__((weak));
extern void compositor_signal_dirty(uint32_t window_id)        __attribute__((weak));

/* Display fallback for callers without a compositor-managed window. */
extern uint32_t *display_get_back_buffer(void) __attribute__((weak));
extern uint32_t display_get_width(void)        __attribute__((weak));
extern uint32_t display_get_height(void)       __attribute__((weak));
extern uint32_t display_get_pitch(void)        __attribute__((weak));
extern void display_flip(void)                 __attribute__((weak));

/* ── Memory helpers ────────────────────────────────────────── */

static void dd_memset(void *p, int v, SIZE_T n)
{
    BYTE *b = (BYTE *)p;
    while (n--) *b++ = (BYTE)v;
}

static void dd_memmove(void *dst, const void *src, SIZE_T n)
{
    BYTE *d = (BYTE *)dst;
    const BYTE *s = (const BYTE *)src;
    if ((ULONG_PTR)d <= (ULONG_PTR)s ||
        (ULONG_PTR)d >= (ULONG_PTR)s + n) {
        while (n--) *d++ = *s++;
        return;
    }
    d += n;
    s += n;
    while (n--) *--d = *--s;
}

#define DDRAW_INTERFACE_COUNT 5

enum {
    DDRAW_IFACE_1 = 1,
    DDRAW_IFACE_2 = 2,
    DDRAW_IFACE_3 = 3,
    DDRAW_IFACE_4 = 4,
    DDRAW_IFACE_7 = 7
};

#define DDRAW1_SETDISPLAYMODE_ARG_COUNT 4U

/* stdcall argument counts include the COM interface pointer. */
static const uint8_t ddraw_method_arg_counts[30] = {
    3, 1, 1, 1, 4, 5, 4, 3, 5, 5,
    1, 3, 2, 3, 2, 2, 2, 2, 2, 1,
    3, 6, 3, 4, 3, 1, 1, 3, 4, 3
};

static const uint8_t ddraw_surface_method_arg_counts[49] = {
    3, 1, 1, 2, 2, 6, 4, 6, 3, 3,
    4, 3, 3, 2, 2, 2, 3, 2, 2, 3,
    2, 2, 2, 3, 1, 5, 2, 1, 2, 3,
    3, 2, 2, 6, 2, 3, 2, 2, 2, 3,
    5, 4, 2, 2, 1, 2, 2, 2, 2
};

_Static_assert(sizeof(ddraw_method_arg_counts) == 30,
               "IDirectDraw method count changed");
_Static_assert(sizeof(ddraw_surface_method_arg_counts) == 49,
               "IDirectDrawSurface method count changed");

static uint8_t ddraw_method_arg_count(unsigned slot)
{
    return slot < sizeof(ddraw_method_arg_counts)
        ? ddraw_method_arg_counts[slot] : 0;
}

static uint8_t ddraw_surface_method_arg_count(unsigned slot)
{
    return slot < sizeof(ddraw_surface_method_arg_counts)
        ? ddraw_surface_method_arg_counts[slot] : 0;
}

static BOOL dd_guid_equal(REFIID left, REFIID right)
{
    if (!left || !right) return FALSE;
    if (left->Data1 != right->Data1 || left->Data2 != right->Data2 ||
        left->Data3 != right->Data3)
        return FALSE;
    for (int index = 0; index < 8; index++) {
        if (left->Data4[index] != right->Data4[index]) return FALSE;
    }
    return TRUE;
}

static const GUID dd_iid_iunknown = {
    0x00000000, 0x0000, 0x0000,
    { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 }
};
static const GUID dd_iid_directdraw = {
    0x6C14DB80, 0xA733, 0x11CE,
    { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 }
};
static const GUID dd_iid_directdraw2 = {
    0xB3A6F3E0, 0x2B43, 0x11CF,
    { 0xA2, 0xDE, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56 }
};
static const GUID dd_iid_directdraw3 = {
    0x618F8AD4, 0x8B7A, 0x11D0,
    { 0x8F, 0xCC, 0x00, 0xC0, 0x4F, 0xD9, 0x18, 0x9D }
};
static const GUID dd_iid_directdraw4 = {
    0x9C59509A, 0x39BD, 0x11D1,
    { 0x8C, 0x4A, 0x00, 0xC0, 0x4F, 0xD9, 0x30, 0xC5 }
};
static const GUID dd_iid_directdraw7 = {
    0x15E65EC0, 0x3B9C, 0x11D2,
    { 0xB9, 0x2F, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B }
};
static const GUID dd_iid_surface = {
    0x6C14DB81, 0xA733, 0x11CE,
    { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 }
};
static const GUID dd_iid_surface2 = {
    0x57805885, 0x6EEC, 0x11CF,
    { 0x94, 0x41, 0xA8, 0x23, 0x03, 0xC1, 0x0E, 0x27 }
};
static const GUID dd_iid_surface3 = {
    0xDA044E00, 0x69B2, 0x11D0,
    { 0xA1, 0xD5, 0x00, 0xAA, 0x00, 0xB8, 0xDF, 0xBB }
};
static const GUID dd_iid_surface4 = {
    0x0B2B8630, 0xAD35, 0x11D0,
    { 0x8E, 0xA6, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B }
};
static const GUID dd_iid_surface7 = {
    0x06675A80, 0x3B9B, 0x11D2,
    { 0xB9, 0x2F, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B }
};
static const GUID dd_iid_palette = {
    0x6C14DB84, 0xA733, 0x11CE,
    { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 }
};
static const GUID dd_iid_clipper = {
    0x6C14DB85, 0xA733, 0x11CE,
    { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 }
};

static uint8_t dd_version_from_iid(REFIID iid)
{
    if (dd_guid_equal(iid, &dd_iid_directdraw)) return DDRAW_IFACE_1;
    if (dd_guid_equal(iid, &dd_iid_directdraw2)) return DDRAW_IFACE_2;
    if (dd_guid_equal(iid, &dd_iid_directdraw3)) return DDRAW_IFACE_3;
    if (dd_guid_equal(iid, &dd_iid_directdraw4)) return DDRAW_IFACE_4;
    if (dd_guid_equal(iid, &dd_iid_directdraw7)) return DDRAW_IFACE_7;
    return 0;
}

static uint8_t dd_surface_version_from_iid(REFIID iid)
{
    if (dd_guid_equal(iid, &dd_iid_surface)) return DDRAW_IFACE_1;
    if (dd_guid_equal(iid, &dd_iid_surface2)) return DDRAW_IFACE_2;
    if (dd_guid_equal(iid, &dd_iid_surface3)) return DDRAW_IFACE_3;
    if (dd_guid_equal(iid, &dd_iid_surface4)) return DDRAW_IFACE_4;
    if (dd_guid_equal(iid, &dd_iid_surface7)) return DDRAW_IFACE_7;
    return 0;
}

static uint32_t ddraw_descriptor_size_for_version(uint8_t version)
{
    return version == DDRAW_IFACE_4 || version == DDRAW_IFACE_7
        ? 124U : 108U;
}

static uint8_t ddraw_created_surface_version_for_interface(uint8_t version)
{
    if (version == DDRAW_IFACE_4 || version == DDRAW_IFACE_7)
        return version;
    return version ? DDRAW_IFACE_1 : 0;
}

/* Framebuffer pointer — connect to real GOP LFB on OsitoK bare metal,
 * or allocate a separate buffer in test harness mode. */
static BYTE *framebuffer = NULL;
static SIZE_T fb_size = 0;
static uint32_t gop_pitch = 0;   /* GOP scanline pitch in pixels */
static void *fallback_fb_phys = NULL;
static SIZE_T fallback_fb_size = 0;

/* GOP framebuffer accessors (defined in kernel/framebuffer.c) */
extern uint32_t *fb_get_base(void)   __attribute__((weak));
extern uint32_t  fb_get_width(void)  __attribute__((weak));
extern uint32_t  fb_get_height(void) __attribute__((weak));
extern uint32_t  fb_get_pitch(void)  __attribute__((weak));
/* fb_get_base() returns the cached RAM *shadow*; writes only reach the
 * displayed VRAM after fb_flush_all() copies shadow→vram. */
extern void      fb_flush_all(void)  __attribute__((weak));

static void ensure_framebuffer(uint32_t requested_width,
                               uint32_t requested_height)
{
    /* fb_get_base() is borrowed storage. It may change when the console is
     * redirected to a compositor surface, so refresh it on every present and
     * never release it through the physical page allocator. */
    if (fb_get_base) {
        uint32_t *gop = fb_get_base();
        if (gop) {
            framebuffer = (BYTE *)gop;
            gop_pitch = fb_get_pitch ? fb_get_pitch() : requested_width;
            fb_size = (SIZE_T)gop_pitch *
                (fb_get_height ? fb_get_height() : requested_height) * 4;
            return;
        }
    }

    /* Headless fallback. mem_alloc_pages() returns a physical address; kernel
     * code under an isolated process CR3 must use the shared direct map. */
    if (!requested_width) requested_width = 640;
    if (!requested_height) requested_height = 480;
    SIZE_T needed = (SIZE_T)requested_width * requested_height * 4;
    if (fallback_fb_phys && fallback_fb_size >= needed) {
        framebuffer = (BYTE *)PHYS_TO_VIRT(fallback_fb_phys);
        fb_size = fallback_fb_size;
        gop_pitch = requested_width;
        return;
    }

    void *new_phys = mem_alloc_pages((needed + 4095) / 4096);
    if (!new_phys) {
        framebuffer = NULL;
        fb_size = 0;
        return;
    }
    if (fallback_fb_phys)
        mem_free_pages(fallback_fb_phys, (fallback_fb_size + 4095) / 4096);
    fallback_fb_phys = new_phys;
    fallback_fb_size = needed;
    fb_size = needed;
    gop_pitch = requested_width;
    framebuffer = (BYTE *)PHYS_TO_VIRT(fallback_fb_phys);
    if (framebuffer) dd_memset(framebuffer, 0, fb_size);
}

/* ── Forward declarations ──────────────────────────────────── */

/* COM objects have vtable pointer as first member */
typedef struct IDirectDraw7       IDirectDraw7;
typedef struct IDirectDrawSurface7 IDirectDrawSurface7;
typedef struct DDRAW_PROCESS_STATE DDRAW_PROCESS_STATE;

/* ── IDirectDrawSurface7 ───────────────────────────────────── */

#define MAX_SURFACES 8
#define MAX_PALETTES 4
#define MAX_CLIPPERS 4
#define DDRAW_PROCESS_SLOTS 32

static const uint8_t dd_interface_versions[DDRAW_INTERFACE_COUNT] = {
    DDRAW_IFACE_1, DDRAW_IFACE_2, DDRAW_IFACE_3,
    DDRAW_IFACE_4, DDRAW_IFACE_7
};

/* ── IDirectDrawPalette ────────────────────────────────────── */

typedef struct DDPalette {
    uint32_t entries[256];   /* RGBQUAD: 0x00RRGGBB per entry */
    DDRAW_PROCESS_STATE *owner;
    volatile ULONG refs;
    uint32_t slot;
    volatile uint32_t used;
} DDPalette;

static DDPalette *ddraw_palette_from_object(PVOID object);
static ULONG ddraw_palette_add_ref(DDPalette *palette);
static ULONG ddraw_palette_release(DDPalette *palette);

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
    if (!ppv) return E_INVALIDARG;
    *(uint32_t *)ppv = 0;
    DDPalette *palette = ddraw_palette_from_object(self);
    if (!palette) return DDERR_INVALIDPARAMS;
    if (!dd_guid_equal(iid, &dd_iid_iunknown) &&
        !dd_guid_equal(iid, &dd_iid_palette))
        return E_NOINTERFACE;
    *(uint32_t *)ppv = (uint32_t)(ULONG_PTR)self;
    ddraw_palette_add_ref(palette);
    return S_OK;
}
static ULONG WINAPI pal_AddRef(struct IDirectDrawPalette7 *self)
{
    return ddraw_palette_add_ref(ddraw_palette_from_object(self));
}
static ULONG WINAPI pal_Release(struct IDirectDrawPalette7 *self)
{
    return ddraw_palette_release(ddraw_palette_from_object(self));
}
static HRESULT WINAPI pal_GetCaps(struct IDirectDrawPalette7 *self, DWORD *caps)
{
    if (!ddraw_palette_from_object(self) || !caps)
        return DDERR_INVALIDPARAMS;
    *caps = 0x04; /* DDPCAPS_8BIT */
    return S_OK;
}

static HRESULT WINAPI pal_GetEntries(struct IDirectDrawPalette7 *self,
                                      DWORD dwFlags, DWORD dwBase,
                                      DWORD dwNumEntries, PVOID lpEntries)
{
    (void)dwFlags;
    DDPalette *palette = ddraw_palette_from_object(self);
    if (!palette || !lpEntries || dwBase >= 256)
        return DDERR_INVALIDPARAMS;
    if (dwNumEntries > 256 - dwBase) dwNumEntries = 256 - dwBase;

    /* PALETTEENTRY is { BYTE peRed, peGreen, peBlue, peFlags } = 4 bytes */
    BYTE *out = (BYTE *)lpEntries;
    for (DWORD i = 0; i < dwNumEntries; i++) {
        uint32_t c = palette->entries[dwBase + i];
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
    (void)dd; (void)flags; (void)entries;
    return ddraw_palette_from_object(self) ? S_OK : DDERR_INVALIDPARAMS;
}

static HRESULT WINAPI pal_SetEntries(struct IDirectDrawPalette7 *self,
                                      DWORD dwFlags, DWORD dwStartingEntry,
                                      DWORD dwCount, PVOID lpEntries)
{
    (void)dwFlags;
    DDPalette *palette = ddraw_palette_from_object(self);
    if (!palette || !lpEntries || dwStartingEntry >= 256)
        return DDERR_INVALIDPARAMS;
    if (dwCount > 256 - dwStartingEntry) dwCount = 256 - dwStartingEntry;

    /* PALETTEENTRY: { BYTE peRed, peGreen, peBlue, peFlags } */
    const BYTE *in = (const BYTE *)lpEntries;
    for (DWORD i = 0; i < dwCount; i++) {
        BYTE r = in[i * 4 + 0];
        BYTE g = in[i * 4 + 1];
        BYTE b = in[i * 4 + 2];
        palette->entries[dwStartingEntry + i] = ((uint32_t)r << 16) |
                                                ((uint32_t)g << 8) | b;
    }
    serial_puts("[DDRAW] Palette SetEntries: ");
    serial_puthex(dwStartingEntry, 2); serial_puts("+");
    serial_puthex(dwCount, 2); serial_puts("\n");
    return S_OK;
}

/* ── IDirectDrawClipper ───────────────────────────────────── */

typedef struct DDClipper {
    HANDLE hwnd;
    LONG   clip_left, clip_top, clip_right, clip_bottom;
    int    has_clip;
    volatile uint32_t changed;
    DDRAW_PROCESS_STATE *owner;
    volatile ULONG refs;
    uint32_t slot;
    volatile uint32_t used;
} DDClipper;

static DDClipper *ddraw_clipper_from_object(PVOID object);
static ULONG ddraw_clipper_add_ref(DDClipper *clipper);
static ULONG ddraw_clipper_release(DDClipper *clipper);
static HRESULT ddraw_clipper_set_window(DDClipper *clipper, HANDLE hwnd);

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
    if (!ppv) return E_INVALIDARG;
    *(uint32_t *)ppv = 0;
    DDClipper *clipper = ddraw_clipper_from_object(self);
    if (!clipper) return DDERR_INVALIDPARAMS;
    if (!dd_guid_equal(iid, &dd_iid_iunknown) &&
        !dd_guid_equal(iid, &dd_iid_clipper))
        return E_NOINTERFACE;
    *(uint32_t *)ppv = (uint32_t)(ULONG_PTR)self;
    ddraw_clipper_add_ref(clipper);
    return S_OK;
}
static ULONG WINAPI clip_AddRef(struct IDirectDrawClipper7 *self)
{
    return ddraw_clipper_add_ref(ddraw_clipper_from_object(self));
}
static ULONG WINAPI clip_Release(struct IDirectDrawClipper7 *self)
{
    return ddraw_clipper_release(ddraw_clipper_from_object(self));
}
static HRESULT WINAPI clip_GetClipList(struct IDirectDrawClipper7 *self, PVOID r, PVOID d, DWORD *s)
{
    DDClipper *clipper = ddraw_clipper_from_object(self);
    if (!clipper || !s) return DDERR_INVALIDPARAMS;
    if (!clipper->has_clip) return DDERR_NOTFOUND;

    RECT_DD rect = {
        clipper->clip_left, clipper->clip_top,
        clipper->clip_right, clipper->clip_bottom
    };
    if (r) {
        const RECT_DD *filter = (const RECT_DD *)r;
        if (filter->left > rect.left) rect.left = filter->left;
        if (filter->top > rect.top) rect.top = filter->top;
        if (filter->right < rect.right) rect.right = filter->right;
        if (filter->bottom < rect.bottom) rect.bottom = filter->bottom;
    }

    BOOL nonempty = rect.left < rect.right && rect.top < rect.bottom;
    DWORD required = 32 + (nonempty ? (DWORD)sizeof(RECT_DD) : 0);
    if (!d) {
        *s = required;
        return S_OK;
    }
    if (*s < required) {
        *s = required;
        return DDERR_REGIONTOOSMALL;
    }

    uint32_t *region = (uint32_t *)d;
    region[0] = 32;                  /* RGNDATAHEADER.dwSize */
    region[1] = 1;                   /* RDH_RECTANGLES */
    region[2] = nonempty ? 1 : 0;    /* nCount */
    region[3] = nonempty ? sizeof(RECT_DD) : 0;
    region[4] = (uint32_t)rect.left;
    region[5] = (uint32_t)rect.top;
    region[6] = (uint32_t)rect.right;
    region[7] = (uint32_t)rect.bottom;
    if (nonempty)
        *(RECT_DD *)&region[8] = rect;
    *s = required;
    __atomic_store_n(&clipper->changed, 0, __ATOMIC_RELEASE);
    return S_OK;
}
static HRESULT WINAPI clip_GetHWnd(struct IDirectDrawClipper7 *self, HANDLE *hwnd)
{
    DDClipper *clipper = ddraw_clipper_from_object(self);
    if (!clipper || !hwnd) return DDERR_INVALIDPARAMS;
    *hwnd = clipper->hwnd;
    return S_OK;
}
static HRESULT WINAPI clip_Initialize(struct IDirectDrawClipper7 *self, PVOID dd, DWORD flags)
{
    (void)dd; (void)flags;
    return ddraw_clipper_from_object(self) ? S_OK : DDERR_INVALIDPARAMS;
}
static HRESULT WINAPI clip_IsClipListChanged(struct IDirectDrawClipper7 *self, BOOL *changed)
{
    DDClipper *clipper = ddraw_clipper_from_object(self);
    if (!clipper || !changed) return DDERR_INVALIDPARAMS;
    *changed = __atomic_load_n(&clipper->changed, __ATOMIC_ACQUIRE)
        ? TRUE : FALSE;
    return S_OK;
}
static HRESULT WINAPI clip_SetClipList(struct IDirectDrawClipper7 *self, PVOID list, DWORD flags)
{
    DDClipper *clipper = ddraw_clipper_from_object(self);
    if (!clipper || flags) return DDERR_INVALIDPARAMS;
    if (clipper->hwnd) return DDERR_CLIPPERISUSINGHWND;
    if (!list) {
        clipper->has_clip = 0;
        __atomic_store_n(&clipper->changed, 1, __ATOMIC_RELEASE);
        return S_OK;
    }

    const uint32_t *region = (const uint32_t *)list;
    if (region[0] < 32 || region[1] != 1)
        return DDERR_INVALIDPARAMS;
    if (!region[2]) {
        clipper->has_clip = 0;
    } else {
        const RECT_DD *rect = (const RECT_DD *)&region[8];
        if (rect->left >= rect->right || rect->top >= rect->bottom)
            return DDERR_INVALIDRECT;
        clipper->clip_left = rect->left;
        clipper->clip_top = rect->top;
        clipper->clip_right = rect->right;
        clipper->clip_bottom = rect->bottom;
        clipper->has_clip = 1;
    }
    __atomic_store_n(&clipper->changed, 1, __ATOMIC_RELEASE);
    return S_OK;
}
static HRESULT WINAPI clip_SetHWnd(struct IDirectDrawClipper7 *self, DWORD flags, HANDLE hwnd)
{
    (void)flags;
    return ddraw_clipper_set_window(ddraw_clipper_from_object(self), hwnd);
}

/* ── DDSurface ────────────────────────────────────────────── */

typedef struct {
    BYTE  *pixels;      /* surface pixel buffer */
    BYTE  *allocation_base;
    DWORD  width;
    DWORD  height;
    DWORD  bpp;
    LONG   pitch;
    SIZE_T buf_size;
    SIZE_T pages;
    DDRAW_PROCESS_STATE *owner;
    volatile ULONG refs;
    volatile uint32_t used;
    int    locked;
    int    is_primary;
    DWORD  caps;
    IDirectDrawSurface7 *back_buffer; /* owned attachment reference */
    DDPalette        *palette;      /* attached palette (8bpp) */
    DDClipper        *clipper;      /* attached clipper */
    DWORD  src_key_low;
    DWORD  src_key_high;
    DWORD  dst_key_low;
    DWORD  dst_key_high;
    int    has_src_key;
    int    has_dst_key;
    uint32_t present_hash;
    int    present_hash_valid;
    DWORD  present_last_ms;
    uint32_t present_count;
    uint32_t uniqueness;
    DWORD priority;
    DWORD lod;
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
    PVOID _pad12;
    HRESULT (WINAPI *GetBltStatus)(IDirectDrawSurface7 *self, DWORD flags);
    HRESULT (WINAPI *GetCaps)(IDirectDrawSurface7 *self, DDSCAPS2 *caps);
    /* 15: GetClipper */
    HRESULT (WINAPI *GetClipper)(IDirectDrawSurface7 *self, PVOID *lplpDDClipper);
    /* 16: GetColorKey */
    HRESULT (WINAPI *GetColorKey)(IDirectDrawSurface7 *self, DWORD flags,
                                   PVOID lpDDColorKey);
    /* 17: GetDC */
    HRESULT (WINAPI *GetDC)(IDirectDrawSurface7 *self, HDC *hdc);
    /* 18: GetFlipStatus, 19: GetOverlayPosition */
    HRESULT (WINAPI *GetFlipStatus)(IDirectDrawSurface7 *self, DWORD flags);
    PVOID _pad19;
    /* 20: GetPalette */
    HRESULT (WINAPI *GetPalette)(IDirectDrawSurface7 *self, PVOID *lplpDDPalette);
    PVOID _pad21;
    /* 22: GetSurfaceDesc */
    HRESULT (WINAPI *GetSurfaceDesc)(IDirectDrawSurface7 *self, DDSURFACEDESC2 *desc);
    /* 23: Initialize, 24: IsLost */
    PVOID _pad23;
    HRESULT (WINAPI *IsLost)(IDirectDrawSurface7 *self);
    /* 25: Lock */
    HRESULT (WINAPI *Lock)(IDirectDrawSurface7 *self, LPRECT destRect,
                            DDSURFACEDESC2 *desc, DWORD flags, HANDLE hEvent);
    /* 26: ReleaseDC */
    HRESULT (WINAPI *ReleaseDC)(IDirectDrawSurface7 *self, HDC hdc);
    /* 27: Restore */
    HRESULT (WINAPI *Restore)(IDirectDrawSurface7 *self);
    /* 28: SetClipper */
    HRESULT (WINAPI *SetClipper)(IDirectDrawSurface7 *self, PVOID lpDDClipper);
    /* 29: SetColorKey */
    HRESULT (WINAPI *SetColorKey)(IDirectDrawSurface7 *self, DWORD flags,
                                   PVOID lpDDColorKey);
    /* 30: SetOverlayPosition */
    PVOID _pad30;
    /* 31: SetPalette */
    HRESULT (WINAPI *SetPalette)(IDirectDrawSurface7 *self, PVOID lpDDPalette);
    /* 32: Unlock */
    HRESULT (WINAPI *Unlock)(IDirectDrawSurface7 *self, LPRECT lpRect);
    /* 33-48: Surface2/3/4/7 extensions */
    PVOID _pad33; PVOID _pad34; PVOID _pad35; PVOID _pad36;
    PVOID _pad37; PVOID _pad38; PVOID _pad39; PVOID _pad40;
    PVOID _pad41; PVOID _pad42; PVOID _pad43; PVOID _pad44;
    PVOID _pad45; PVOID _pad46; PVOID _pad47; PVOID _pad48;
};

/* Surface implementations */

/* ── COM32 proxy types ────────────────────────────────────── */
typedef struct {
    uint32_t lpVtbl32;
    uint16_t surf_index;
    uint8_t interface_version;
    uint8_t reserved;
} COM32_Surface;

typedef struct {
    uint32_t lpVtbl32;
    uint8_t interface_version;
    uint8_t reserved[3];
} COM32_DirectDraw;

_Static_assert(sizeof(COM32_Surface) == 8,
               "PE32 surface proxy layout changed");
_Static_assert(sizeof(COM32_DirectDraw) == 8,
               "PE32 DirectDraw proxy layout changed");

typedef struct {
    uint32_t lpVtbl32;
    uint32_t palette_index;
} COM32_Palette;

typedef struct {
    uint32_t lpVtbl32;
    uint32_t clipper_index;
} COM32_Clipper;

struct DDRAW_PROCESS_STATE {
    volatile uint32_t used;
    DWORD owner_pid;
    volatile ULONG refs;
    DWORD display_width;
    DWORD display_height;
    DWORD display_bpp;
    DWORD cooperative_flags;
    int mode_set;
    HANDLE hwnd;
    uint8_t *com32_page;
    uint32_t *dd_vtbl1_32;
    uint32_t *dd_vtbl32;
    COM32_DirectDraw *dd_proxies;
    uint32_t *surf_vtbl32;
    COM32_Surface *surface_proxies;
    uint32_t *palette_vtbl32;
    COM32_Palette *palette_proxies;
    uint32_t *clipper_vtbl32;
    COM32_Clipper *clipper_proxies;
    volatile uint32_t proxy_state;
    IDirectDrawSurface7 surfaces[MAX_SURFACES];
    DDPalette palettes[MAX_PALETTES];
    DDClipper clippers[MAX_CLIPPERS];
    DDSurface *present_surface;
};

static DDRAW_PROCESS_STATE ddraw_processes[DDRAW_PROCESS_SLOTS];
static volatile uint32_t ddraw_state_lock;
static volatile uint32_t ddraw_surface_uniqueness;

static void ddraw_lock(void)
{
    while (__sync_lock_test_and_set(&ddraw_state_lock, 1))
        __asm__ volatile ("pause");
}

static void ddraw_unlock(void)
{
    __sync_lock_release(&ddraw_state_lock);
}

static void reset_display_mode(DDRAW_PROCESS_STATE *state)
{
    uint32_t width = display_get_width ? display_get_width() : 0;
    uint32_t height = display_get_height ? display_get_height() : 0;

    if (!width && fb_get_width) width = fb_get_width();
    if (!height && fb_get_height) height = fb_get_height();

    state->display_width = width ? width : 640;
    state->display_height = height ? height : 480;
    state->display_bpp = 32;
    state->mode_set = 0;
}

static DDRAW_PROCESS_STATE *ddraw_state_for_pid(DWORD owner_pid, BOOL create)
{
    DDRAW_PROCESS_STATE *free_state = NULL;
    if (!owner_pid) owner_pid = 1;

    ddraw_lock();
    for (int i = 0; i < DDRAW_PROCESS_SLOTS; i++) {
        DDRAW_PROCESS_STATE *state = &ddraw_processes[i];
        if (__atomic_load_n(&state->used, __ATOMIC_ACQUIRE) == 1 &&
            state->owner_pid == owner_pid) {
            ddraw_unlock();
            return state;
        }
        if (!free_state &&
            __atomic_load_n(&state->used, __ATOMIC_RELAXED) == 0)
            free_state = state;
    }
    if (!create || !free_state) {
        ddraw_unlock();
        return NULL;
    }

    __atomic_store_n(&free_state->used, 2, __ATOMIC_RELAXED);
    dd_memset((BYTE *)free_state + sizeof(free_state->used), 0,
              sizeof(*free_state) - sizeof(free_state->used));
    free_state->owner_pid = owner_pid;
    reset_display_mode(free_state);
    __atomic_store_n(&free_state->used, 1, __ATOMIC_RELEASE);
    ddraw_unlock();
    return free_state;
}

static DDRAW_PROCESS_STATE *ddraw_current_state(BOOL create)
{
    return ddraw_state_for_pid(win32_current_process_id(), create);
}

static int dd_interface_index(uint8_t version)
{
    for (int index = 0; index < DDRAW_INTERFACE_COUNT; index++) {
        if (dd_interface_versions[index] == version) return index;
    }
    return -1;
}

static COM32_DirectDraw *ddraw_proxy_for_version(DDRAW_PROCESS_STATE *state,
                                                 uint8_t version)
{
    int index = dd_interface_index(version);
    if (!state || !state->dd_proxies || index < 0) return NULL;
    return &state->dd_proxies[index];
}

static DDRAW_PROCESS_STATE *ddraw_state_from_object(IDirectDraw7 *self)
{
    DDRAW_PROCESS_STATE *state = ddraw_current_state(FALSE);
    if (!state || !self || !state->dd_proxies) return NULL;
    ULONG_PTR value = (ULONG_PTR)self;
    ULONG_PTR first = (ULONG_PTR)&state->dd_proxies[0];
    ULONG_PTR end = (ULONG_PTR)&state->dd_proxies[DDRAW_INTERFACE_COUNT];
    if (value < first || value >= end ||
        (value - first) % sizeof(COM32_DirectDraw))
        return NULL;
    COM32_DirectDraw *proxy = (COM32_DirectDraw *)self;
    return dd_interface_index(proxy->interface_version) >= 0 ? state : NULL;
}

static uint8_t ddraw_interface_version(IDirectDraw7 *self)
{
    return ddraw_state_from_object(self)
        ? ((COM32_DirectDraw *)self)->interface_version : 0;
}

static uint8_t ddraw_created_surface_version(IDirectDraw7 *self)
{
    return ddraw_created_surface_version_for_interface(
        ddraw_interface_version(self));
}

static COM32_Surface *ddraw_surface_proxy(DDRAW_PROCESS_STATE *state,
                                          uint32_t surface_index,
                                          uint8_t version)
{
    int interface_index = dd_interface_index(version);
    if (!state || !state->surface_proxies ||
        surface_index >= MAX_SURFACES || interface_index < 0)
        return NULL;
    return &state->surface_proxies[
        surface_index * DDRAW_INTERFACE_COUNT + (uint32_t)interface_index];
}

static uint8_t ddraw_surface_interface_version(PVOID object)
{
    DDRAW_PROCESS_STATE *state = ddraw_current_state(FALSE);
    if (!state || !object || !state->surface_proxies) return 0;
    ULONG_PTR value = (ULONG_PTR)object;
    ULONG_PTR first = (ULONG_PTR)&state->surface_proxies[0];
    ULONG_PTR end = (ULONG_PTR)&state->surface_proxies[
        MAX_SURFACES * DDRAW_INTERFACE_COUNT];
    if (value < first || value >= end ||
        (value - first) % sizeof(COM32_Surface))
        return 0;
    COM32_Surface *proxy = (COM32_Surface *)object;
    return dd_interface_index(proxy->interface_version) >= 0
        ? proxy->interface_version : 0;
}

static DDPalette *ddraw_palette_from_object(PVOID object)
{
    DDRAW_PROCESS_STATE *state = ddraw_current_state(FALSE);
    if (!state || !object || !state->palette_proxies) return NULL;
    ULONG_PTR value = (ULONG_PTR)object;
    ULONG_PTR first = (ULONG_PTR)&state->palette_proxies[0];
    ULONG_PTR end = (ULONG_PTR)&state->palette_proxies[MAX_PALETTES];
    if (value < first || value >= end || (value - first) % sizeof(COM32_Palette))
        return NULL;
    uint32_t index = (uint32_t)((value - first) / sizeof(COM32_Palette));
    DDPalette *palette = &state->palettes[index];
    return __atomic_load_n(&palette->used, __ATOMIC_ACQUIRE) == 1
        ? palette : NULL;
}

static ULONG ddraw_palette_add_ref(DDPalette *palette)
{
    if (!palette || __atomic_load_n(&palette->used, __ATOMIC_ACQUIRE) != 1)
        return 0;
    return __sync_add_and_fetch(&palette->refs, 1);
}

static ULONG ddraw_palette_release(DDPalette *palette)
{
    if (!palette || __atomic_load_n(&palette->used, __ATOMIC_ACQUIRE) != 1)
        return 0;
    ULONG refs;
    do {
        refs = __atomic_load_n(&palette->refs, __ATOMIC_ACQUIRE);
        if (!refs) return 0;
    } while (!__atomic_compare_exchange_n(&palette->refs, &refs, refs - 1,
                                          FALSE, __ATOMIC_ACQ_REL,
                                          __ATOMIC_ACQUIRE));
    if (refs == 1) {
        palette->owner = NULL;
        __atomic_store_n(&palette->used, 0, __ATOMIC_RELEASE);
    }
    return refs - 1;
}

static DDClipper *ddraw_clipper_from_object(PVOID object)
{
    DDRAW_PROCESS_STATE *state = ddraw_current_state(FALSE);
    if (!state || !object || !state->clipper_proxies) return NULL;
    ULONG_PTR value = (ULONG_PTR)object;
    ULONG_PTR first = (ULONG_PTR)&state->clipper_proxies[0];
    ULONG_PTR end = (ULONG_PTR)&state->clipper_proxies[MAX_CLIPPERS];
    if (value < first || value >= end || (value - first) % sizeof(COM32_Clipper))
        return NULL;
    uint32_t index = (uint32_t)((value - first) / sizeof(COM32_Clipper));
    DDClipper *clipper = &state->clippers[index];
    return __atomic_load_n(&clipper->used, __ATOMIC_ACQUIRE) == 1
        ? clipper : NULL;
}

static ULONG ddraw_clipper_add_ref(DDClipper *clipper)
{
    if (!clipper || __atomic_load_n(&clipper->used, __ATOMIC_ACQUIRE) != 1)
        return 0;
    return __sync_add_and_fetch(&clipper->refs, 1);
}

static ULONG ddraw_clipper_release(DDClipper *clipper)
{
    if (!clipper || __atomic_load_n(&clipper->used, __ATOMIC_ACQUIRE) != 1)
        return 0;
    ULONG refs;
    do {
        refs = __atomic_load_n(&clipper->refs, __ATOMIC_ACQUIRE);
        if (!refs) return 0;
    } while (!__atomic_compare_exchange_n(&clipper->refs, &refs, refs - 1,
                                          FALSE, __ATOMIC_ACQ_REL,
                                          __ATOMIC_ACQUIRE));
    if (refs == 1) {
        clipper->owner = NULL;
        __atomic_store_n(&clipper->used, 0, __ATOMIC_RELEASE);
    }
    return refs - 1;
}

static HRESULT ddraw_clipper_set_window(DDClipper *clipper, HANDLE hwnd)
{
    if (!clipper || !clipper->owner) return DDERR_INVALIDPARAMS;
    clipper->hwnd = hwnd;
    if (!hwnd) {
        clipper->has_clip = 0;
        __atomic_store_n(&clipper->changed, 1, __ATOMIC_RELEASE);
        return S_OK;
    }
    clipper->clip_left = 0;
    clipper->clip_top = 0;
    clipper->clip_right = (LONG)clipper->owner->display_width;
    clipper->clip_bottom = (LONG)clipper->owner->display_height;
    clipper->has_clip = 1;
    __atomic_store_n(&clipper->changed, 1, __ATOMIC_RELEASE);
    serial_puts("[DDRAW] Clipper SetHWnd\n");
    return S_OK;
}

/* Forward declarations — defined later in file */
static IDirectDrawSurface7 *com32_to_surface(uint32_t proxy_addr);
static ULONG ddraw_surface_release(IDirectDrawSurface7 *surface);

/* All surf_* functions receive a COM32_Surface* as 'self' (via thunk).
 * Convert to real surface with this macro. */
#define REAL_SURF(self) com32_to_surface((uint32_t)(ULONG_PTR)(self))

static BOOL ddraw_surface_is_live(const IDirectDrawSurface7 *surface,
                                  const DDRAW_PROCESS_STATE *state)
{
    return surface && state && surface->surf.owner == state &&
           __atomic_load_n(&surface->surf.used, __ATOMIC_ACQUIRE) == 1;
}

static IDirectDrawSurface7 *ddraw_surface_chain_root(
    DDRAW_PROCESS_STATE *state, IDirectDrawSurface7 *surface)
{
    if (!ddraw_surface_is_live(surface, state)) return NULL;

    IDirectDrawSurface7 *current = surface;
    for (uint32_t depth = 0; depth < MAX_SURFACES; depth++) {
        IDirectDrawSurface7 *parent = NULL;
        for (uint32_t index = 0; index < MAX_SURFACES; index++) {
            IDirectDrawSurface7 *candidate = &state->surfaces[index];
            if (ddraw_surface_is_live(candidate, state) &&
                candidate->surf.back_buffer == current) {
                parent = candidate;
                break;
            }
        }
        if (!parent) break;
        current = parent;
    }
    return current;
}

static uint32_t ddraw_surface_chain_length(IDirectDrawSurface7 *root)
{
    if (!root || !root->surf.owner) return 0;

    DDRAW_PROCESS_STATE *state = root->surf.owner;
    uint32_t count = 0;
    IDirectDrawSurface7 *surface = root;
    while (count < MAX_SURFACES && ddraw_surface_is_live(surface, state)) {
        count++;
        surface = surface->surf.back_buffer;
    }
    return count;
}

static BOOL ddraw_surface_caps_match(DWORD actual, DWORD requested)
{
    return (actual & requested) == requested;
}

static HRESULT WINAPI surf_QueryInterface(IDirectDrawSurface7 *self, REFIID iid, PVOID *ppv)
{
    if (!ppv) return E_INVALIDARG;
    *(uint32_t *)ppv = 0;
    IDirectDrawSurface7 *real = REAL_SURF(self);
    if (!real) return DDERR_INVALIDPARAMS;
    DDRAW_PROCESS_STATE *state = real->surf.owner;
    uint8_t version = dd_guid_equal(iid, &dd_iid_iunknown)
        ? DDRAW_IFACE_1 : dd_surface_version_from_iid(iid);
    int surface_index = state ? (int)(real - state->surfaces) : -1;
    COM32_Surface *proxy = surface_index >= 0
        ? ddraw_surface_proxy(state, (uint32_t)surface_index, version) : NULL;
    if (!proxy) return E_NOINTERFACE;
    *(uint32_t *)ppv = (uint32_t)(ULONG_PTR)proxy;
    __sync_add_and_fetch(&real->surf.refs, 1);
    return S_OK;
}

static ULONG WINAPI surf_AddRef(IDirectDrawSurface7 *self)
{
    IDirectDrawSurface7 *real = REAL_SURF(self);
    if (!real) return 0;
    return __sync_add_and_fetch(&real->surf.refs, 1);
}

static ULONG WINAPI surf_Release(IDirectDrawSurface7 *self)
{
    return ddraw_surface_release(REAL_SURF(self));
}

static void ddraw_compositor_notify(DDRAW_PROCESS_STATE *state);

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
static DWORD g_present_owner_pid = 0;

enum {
    DDRAW_TARGET_NONE,
    DDRAW_TARGET_WINDOW,
    DDRAW_TARGET_DISPLAY,
    DDRAW_TARGET_FRAMEBUFFER
};

static int ddraw_get_present_target(DDRAW_PROCESS_STATE *state,
                                    uint32_t requested_width,
                                    uint32_t requested_height,
                                    uint32_t **pixels, uint32_t *width,
                                    uint32_t *height, uint32_t *pitch)
{
    void *window_pixels = NULL;
    int window_width = 0, window_height = 0, window_pitch = 0;
    if (state && state->hwnd && user32_get_window_surface &&
        user32_get_window_surface(state->hwnd, &window_pixels, &window_width,
                                  &window_height, &window_pitch) &&
        window_pixels && window_width > 0 && window_height > 0 &&
        window_pitch >= window_width * 4) {
        *pixels = (uint32_t *)window_pixels;
        *width = (uint32_t)window_width;
        *height = (uint32_t)window_height;
        *pitch = (uint32_t)window_pitch / 4;
        return DDRAW_TARGET_WINDOW;
    }

    if (display_get_back_buffer && display_get_width && display_get_height &&
        display_get_pitch) {
        uint32_t *back = display_get_back_buffer();
        uint32_t display_width = display_get_width();
        uint32_t display_height = display_get_height();
        uint32_t display_pitch = display_get_pitch();
        if (back && display_width && display_height &&
            display_pitch >= display_width) {
            *pixels = back;
            *width = display_width;
            *height = display_height;
            *pitch = display_pitch;
            return DDRAW_TARGET_DISPLAY;
        }
    }

    ensure_framebuffer(requested_width, requested_height);
    if (!framebuffer) return DDRAW_TARGET_NONE;
    *pixels = (uint32_t *)framebuffer;
    *width = (fb_get_width && fb_get_width())
        ? fb_get_width() : requested_width;
    *height = (fb_get_height && fb_get_height())
        ? fb_get_height() : requested_height;
    *pitch = gop_pitch ? gop_pitch : *width;
    return DDRAW_TARGET_FRAMEBUFFER;
}

void ddraw_present_pixels(const void *pixels, uint32_t sw, uint32_t sh,
                          uint32_t bpp, int32_t pitch_bytes, const uint32_t *pal256)
{
    if (!pixels || !sw || !sh) return;
    DWORD owner_pid = win32_current_process_id();
    DDRAW_PROCESS_STATE *state = ddraw_current_state(FALSE);
    uint32_t *dst32 = NULL;
    uint32_t fbw = 0, fbh = 0, pitch = 0;
    int target = ddraw_get_present_target(state, sw, sh, &dst32, &fbw, &fbh,
                                          &pitch);
    if (target == DDRAW_TARGET_NONE || !dst32 || !fbw || !fbh || !pitch)
        return;

    static DWORD logged_owner;
    static uint32_t logged_width, logged_height;
    static int logged_target;
    if (owner_pid != logged_owner || target != logged_target ||
        fbw != logged_width || fbh != logged_height) {
        serial_puts("[DDRAW] target=");
        serial_puts(target == DDRAW_TARGET_WINDOW ? "window " :
                    target == DDRAW_TARGET_DISPLAY ? "display " :
                    "framebuffer ");
        serial_putdec(fbw);
        serial_puts("x");
        serial_putdec(fbh);
        serial_puts(" pitch=");
        serial_putdec(pitch);
        serial_puts("\n");
        logged_owner = owner_pid;
        logged_target = target;
        logged_width = fbw;
        logged_height = fbh;
    }
    /* Probe: log every change of the PRESENTED source size. After a SetRes,
     * any present at the OLD size means stale-mode frames are being scaled
     * against the new projection (the fisheye symptom) — this line is the
     * cheap discriminator for the next live run. */
    if (owner_pid != g_present_owner_pid ||
        sw != g_present_src_w || sh != g_present_src_h) {
        serial_puts("[DDRAW] present src ");
        serial_puthex(g_present_src_w, 4); serial_puts("x");
        serial_puthex(g_present_src_h, 4); serial_puts(" -> ");
        serial_puthex(sw, 4); serial_puts("x"); serial_puthex(sh, 4);
        serial_puts("\n");
    }
    g_present_src_w = sw;
    g_present_src_h = sh;
    g_present_owner_pid = owner_pid;

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

    if (target == DDRAW_TARGET_WINDOW) {
        if (state && state->hwnd && user32_mark_window_dirty)
            user32_mark_window_dirty(state->hwnd);
    } else if (target == DDRAW_TARGET_DISPLAY) {
        if (display_flip) display_flip();
    } else if (fb_flush_all) {
        fb_flush_all();
    }
}

static void present_surface_to_gop(DDSurface *s)
{
    if (!s || !s->pixels || !s->width) return;
    ddraw_present_pixels(s->pixels, s->width, s->height, s->bpp,
                         (int32_t)s->pitch,
                         s->palette ? s->palette->entries : NULL);
}

#define DDRAW_FRAME_PERIOD_MS 17U
#define DDRAW_VBLANK_BEGIN_MS 16U

static DWORD ddraw_vblank_phase(DWORD now)
{
    return now % DDRAW_FRAME_PERIOD_MS;
}

static BOOL ddraw_vblank_active_at(DWORD now)
{
    return ddraw_vblank_phase(now) >= DDRAW_VBLANK_BEGIN_MS;
}

static DWORD ddraw_vblank_delay_at(DWORD now, DWORD target_phase)
{
    DWORD phase = ddraw_vblank_phase(now);
    return target_phase >= phase
        ? target_phase - phase
        : DDRAW_FRAME_PERIOD_MS - phase + target_phase;
}

static void ddraw_wait_for_vblank(DWORD flags)
{
    DWORD target = flags == DDWAITVB_BLOCKEND
        ? 0U : DDRAW_VBLANK_BEGIN_MS;
    DWORD delay = ddraw_vblank_delay_at(shim_timeGetTime(), target);
    if (delay)
        Sleep(delay);
}

/* Report the active scaled-present coordinate space. A process that has not
 * presented or selected a DirectDraw mode must return 0/0 so USER32 keeps
 * ordinary NT screen coordinates instead of inheriting the 640x480 legacy
 * default. */
void ddraw_get_display_size(uint32_t *w, uint32_t *h)
{
    DWORD owner_pid = win32_current_process_id();
    uint32_t active_w = g_present_owner_pid == owner_pid
        ? g_present_src_w : 0;
    uint32_t active_h = g_present_owner_pid == owner_pid
        ? g_present_src_h : 0;
    DDRAW_PROCESS_STATE *state = ddraw_current_state(FALSE);
    if ((!active_w || !active_h) && state && state->mode_set) {
        active_w = state->display_width;
        active_h = state->display_height;
    }
    if (w) *w = active_w;
    if (h) *h = active_h;
}

/* Called by the GDI present path (BitBlt of a DIB to the window DC): the game
 * has switched to GDI/DIB presentation, so stop the per-pump re-present of the
 * last DirectDraw surface — it would overwrite the DIB frames with stale
 * content. A later ddraw Lock/Flip re-arms g_present_surface. */
void ddraw_suspend_present_hook(void)
{
    DDRAW_PROCESS_STATE *state = ddraw_current_state(FALSE);
    if (state) state->present_surface = NULL;
}

/* Full current display mode incl. depth — used by user32's
 * EnumDisplaySettings(ENUM_CURRENT_SETTINGS) so the whole layer tells one
 * consistent "current mode" story (ddraw is the authority). */
void ddraw_get_display_mode(uint32_t *w, uint32_t *h, uint32_t *bpp)
{
    DDRAW_PROCESS_STATE *state = ddraw_current_state(FALSE);
    uint32_t width = state ? state->display_width
                           : (fb_get_width && fb_get_width() ? fb_get_width() : 640);
    uint32_t height = state ? state->display_height
                            : (fb_get_height && fb_get_height() ? fb_get_height() : 480);
    if (w)   *w   = width;
    if (h)   *h   = height;
    if (bpp) *bpp = state ? state->display_bpp : 32;
}

/* True once a real SetDisplayMode has happened — see g_mode_set above. Used
 * by user32 GetSystemMetrics and gdi32 GetDeviceCaps to switch from GOP size
 * (startup) to the current exclusive mode (in-game), per NT semantics. */
int ddraw_display_mode_active(void)
{
    DDRAW_PROCESS_STATE *state = ddraw_current_state(FALSE);
    return state ? state->mode_set : 0;
}

/* Per-frame present hook for software renderers that retain a surface lock
 * across frames instead of signaling each update through Flip or Unlock. */
void ddraw_present_hook(void)
{
    DDRAW_PROCESS_STATE *state = ddraw_current_state(FALSE);
    if (!state || !state->present_surface || !state->present_surface->used)
        return;
    DDSurface *surface = state->present_surface;
    if (!surface->pixels || !surface->buf_size)
        return;

    /* Polling a persistently mapped primary surface is the scanout fallback
     * for renderers that do not Flip or Unlock every frame. Gate before
     * sampling so tight message loops neither rescan the surface needlessly
     * nor consume a changed hash before that frame is eligible to present. */
    DWORD now = shim_timeGetTime();
    DWORD elapsed = surface->present_count
        ? (DWORD)(now - surface->present_last_ms) : 0;
    if (surface->present_count && elapsed < 16)
        return;

    uint32_t hash = 2166136261u;
    SIZE_T samples = surface->buf_size < 1024 ? surface->buf_size : 1024;
    for (SIZE_T i = 0; i < samples; i++) {
        SIZE_T offset = samples == 1 ? 0
            : (i * (surface->buf_size - 1)) / (samples - 1);
        hash = (hash ^ surface->pixels[offset]) * 16777619u;
    }

    int changed = !surface->present_hash_valid ||
                  hash != surface->present_hash;
    if (changed) {
        surface->present_hash_valid = 1;
        surface->present_hash = hash;
    }

    /* Persistent-lock renderers may call PeekMessage in a tight loop. Keep
     * message pumping cheap: present changed frames at at most 60 Hz and only
     * refresh an unchanged frame occasionally for compositor recovery. */
    if (surface->present_count) {
        if (!changed && elapsed < 250)
            return;
    }
    surface->present_last_ms = now;
    surface->present_count++;
    present_surface_to_gop(surface);
    ddraw_compositor_notify(state);
}

DWORD ddraw_present_poll_interval(void)
{
    DDRAW_PROCESS_STATE *state = ddraw_current_state(FALSE);
    DDSurface *surface = state ? state->present_surface : NULL;

    if (!surface || !surface->used || !surface->pixels || !surface->buf_size)
        return 0;
    return 16;
}

/* True when a surface is the size the user sees (or last saw): either the
 * current ddraw display mode, or the source size of the most recent present.
 * The second test matters because the GDI/DIB present path can change the
 * visible resolution WITHOUT a ddraw SetDisplayMode — after such a mid-stream
 * mode change SoftDrv's render surface must not be orphaned by a strict
 * display_width/height compare. Shared by Lock/Unlock/Flip/Blt so every path
 * agrees on which surface is eligible for the per-pump present hook. */
static int surf_is_present_sized(const DDSurface *s)
{
    DDRAW_PROCESS_STATE *state = s ? s->owner : NULL;
    if (!state || !s->used || !s->pixels || !s->width || !s->height) return 0;
    if (s->width == state->display_width && s->height == state->display_height)
        return 1;
    if (g_present_owner_pid == state->owner_pid &&
        g_present_src_w && g_present_src_h &&
        s->width == g_present_src_w && s->height == g_present_src_h) return 1;
    return 0;
}

static HRESULT WINAPI surf_Lock(IDirectDrawSurface7 *self, LPRECT destRect,
                                 DDSURFACEDESC2 *desc, DWORD flags, HANDLE hEvent)
{
    (void)flags; (void)hEvent;
    IDirectDrawSurface7 *real = REAL_SURF(self);
    if (!real) return DDERR_INVALIDPARAMS;
    DDSurface *s = &real->surf;

    if (!desc || !s->pixels || !s->pitch) return DDERR_INVALIDPARAMS;

    DWORD lock_width = s->width;
    DWORD lock_height = s->height;
    SIZE_T surface_offset = 0;
    if (destRect) {
        if (destRect->left < 0 || destRect->top < 0 ||
            destRect->right <= destRect->left ||
            destRect->bottom <= destRect->top ||
            (DWORD)destRect->right > s->width ||
            (DWORD)destRect->bottom > s->height)
            return DDERR_INVALIDRECT;
        lock_width = (DWORD)(destRect->right - destRect->left);
        lock_height = (DWORD)(destRect->bottom - destRect->top);
        surface_offset = (SIZE_T)destRect->top * (SIZE_T)s->pitch +
            (SIZE_T)destRect->left * (s->bpp / 8);
        if (surface_offset >= s->buf_size)
            return DDERR_INVALIDRECT;
    }

    /* IDirectDrawSurface 1-3 use DDSURFACEDESC (108 bytes); versions 4 and 7
     * use DDSURFACEDESC2 (124 bytes). Both keep lpSurface at offset 36. */
    uint32_t *d = (uint32_t *)desc;
    uint8_t interface_version = ddraw_surface_interface_version(self);
    uint32_t descriptor_size =
        ddraw_descriptor_size_for_version(interface_version);
    dd_memset(d, 0, descriptor_size);
    d[0]  = descriptor_size;
    d[1]  = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH |
            DDSD_LPSURFACE | DDSD_PIXELFORMAT;
    d[2]  = lock_height;            /* dwHeight at offset 8 */
    d[3]  = lock_width;             /* dwWidth at offset 12 */
    d[4]  = (uint32_t)s->pitch;     /* lPitch at offset 16 */
    d[9]  = (uint32_t)(ULONG_PTR)(s->pixels + surface_offset);
    d[26] = s->caps;                /* ddsCaps.dwCaps at offset 104 */
    if ((s->caps & (DDSCAPS_FRONTBUFFER | DDSCAPS_FLIP)) ==
            (DDSCAPS_FRONTBUFFER | DDSCAPS_FLIP)) {
        uint32_t chain_length = ddraw_surface_chain_length(real);
        if (chain_length > 1) {
            d[1] |= DDSD_BACKBUFFERCOUNT;
            d[5] = chain_length - 1;
        }
    }

    /* Remember the display-sized surface being rendered into so the per-frame
     * pump can present it (SoftDrv keeps this Locked and never Flips). Match
     * by current mode OR last-presented source size (see
     * surf_is_present_sized) so a GDI-side resolution change mid-stream
     * doesn't orphan the surface. */
    if (surf_is_present_sized(s))
        s->owner->present_surface = s;

    /* ddpfPixelFormat is at offset 72 in both descriptor generations. Field
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
        d[19] = DDPF_PALETTEINDEXED8; /* dwFlags @76 */
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
    if (surf_is_present_sized(s)) {
        /* Re-arm the per-pump present hook: an Unlock of a display-sized
         * surface is a positive signal the ddraw path is producing frames
         * again. The GDI/DIB present path (the menu) suspends the hook
         * (g_present_surface = NULL), and on resume SoftDrv may keep its
         * surface persistently Locked without ever re-Locking — so without
         * this re-arm the pump presents nothing after a menu round-trip
         * (in-game black screen until something re-Locks, e.g. death-cam). */
        s->owner->present_surface = s;
        present_surface_to_gop(s);
        ddraw_compositor_notify(s->owner);
    }
    return DD_OK;
}

/* Notify compositor that the primary surface was updated */
static void ddraw_compositor_notify(DDRAW_PROCESS_STATE *state)
{
    if (!state || !state->hwnd || !compositor_signal_dirty ||
        !user32_get_window_compositor_id)
        return;
    uint32_t wid = user32_get_window_compositor_id(state->hwnd);
    if (wid)
        compositor_signal_dirty(wid);
}

static uint32_t surface_read_pixel(const BYTE *pixel, DWORD bpp)
{
    if (bpp == 8) return *pixel;
    if (bpp == 16) return *(const uint16_t *)pixel;
    return *(const uint32_t *)pixel;
}

static void surface_write_pixel(BYTE *pixel, DWORD bpp, uint32_t value)
{
    if (bpp == 8) {
        *pixel = (BYTE)value;
    } else if (bpp == 16) {
        *(uint16_t *)pixel = (uint16_t)value;
    } else {
        *(uint32_t *)pixel = value;
    }
}

static int surface_pixel_matches_key(const DDSurface *surface,
                                     uint32_t pixel)
{
    return surface->has_src_key && pixel >= surface->src_key_low &&
           pixel <= surface->src_key_high;
}

/* Blt: copy source surface to this surface (or to framebuffer if primary) */
static HRESULT WINAPI surf_Blt(IDirectDrawSurface7 *self, LPRECT destRect,
                                IDirectDrawSurface7 *src, LPRECT srcRect,
                                DWORD dwFlags, PVOID lpDDBltFx)
{
    IDirectDrawSurface7 *real_self = REAL_SURF(self);
    if (!real_self) return DDERR_INVALIDPARAMS;
    DDSurface *dst = &real_self->surf;
    if (!dst->pixels || !dst->width || !dst->height)
        return DDERR_INVALIDPARAMS;

    LONG dest_left = destRect ? destRect->left : 0;
    LONG dest_top = destRect ? destRect->top : 0;
    LONG dest_right = destRect ? destRect->right : (LONG)dst->width;
    LONG dest_bottom = destRect ? destRect->bottom : (LONG)dst->height;
    int64_t dest_width = (int64_t)dest_right - dest_left;
    int64_t dest_height = (int64_t)dest_bottom - dest_top;
    if (dest_width <= 0 || dest_height <= 0)
        return DDERR_INVALIDRECT;

    LONG clip_left = dest_left < 0 ? 0 : dest_left;
    LONG clip_top = dest_top < 0 ? 0 : dest_top;
    LONG clip_right = dest_right > (LONG)dst->width
        ? (LONG)dst->width : dest_right;
    LONG clip_bottom = dest_bottom > (LONG)dst->height
        ? (LONG)dst->height : dest_bottom;
    if (dst->clipper && dst->clipper->has_clip) {
        if (dst->clipper->clip_left > clip_left)
            clip_left = dst->clipper->clip_left;
        if (dst->clipper->clip_top > clip_top)
            clip_top = dst->clipper->clip_top;
        if (dst->clipper->clip_right < clip_right)
            clip_right = dst->clipper->clip_right;
        if (dst->clipper->clip_bottom < clip_bottom)
            clip_bottom = dst->clipper->clip_bottom;
    }
    if (clip_left >= clip_right || clip_top >= clip_bottom)
        return DD_OK;

    DWORD bytes_per_pixel = dst->bpp / 8;
    if (dwFlags & DDBLT_COLORFILL) {
        const uint32_t *fx = (const uint32_t *)lpDDBltFx;
        if (!fx || fx[0] < 84) return DDERR_INVALIDPARAMS;
        uint32_t fill = fx[20]; /* 32-bit DDBLTFX.dwFillColor */
        for (LONG y = clip_top; y < clip_bottom; y++) {
            BYTE *row = dst->pixels + (SIZE_T)y * dst->pitch +
                (SIZE_T)clip_left * bytes_per_pixel;
            for (LONG x = clip_left; x < clip_right; x++) {
                surface_write_pixel(row, dst->bpp, fill);
                row += bytes_per_pixel;
            }
        }
    } else {
        IDirectDrawSurface7 *real_src = src ? REAL_SURF(src) : NULL;
        if (!real_src) return DDERR_INVALIDPARAMS;
        DDSurface *source = &real_src->surf;
        if (!source->pixels || source->owner != dst->owner ||
            source->bpp != dst->bpp)
            return DDERR_INVALIDPARAMS;

        LONG src_left = srcRect ? srcRect->left : 0;
        LONG src_top = srcRect ? srcRect->top : 0;
        LONG src_right = srcRect ? srcRect->right : (LONG)source->width;
        LONG src_bottom = srcRect ? srcRect->bottom : (LONG)source->height;
        int64_t src_width = (int64_t)src_right - src_left;
        int64_t src_height = (int64_t)src_bottom - src_top;
        if (src_left < 0 || src_top < 0 || src_width <= 0 || src_height <= 0 ||
            src_right > (LONG)source->width ||
            src_bottom > (LONG)source->height)
            return DDERR_INVALIDRECT;

        int use_src_key = (dwFlags & DDBLT_KEYSRC) != 0;
        if (use_src_key && !source->has_src_key)
            return DDERR_NOCOLORKEY;

        BOOL same_scale = src_width == dest_width &&
                          src_height == dest_height;
        if (same_scale && !use_src_key) {
            LONG source_x = src_left + (clip_left - dest_left);
            LONG source_y = src_top + (clip_top - dest_top);
            LONG rows = clip_bottom - clip_top;
            SIZE_T row_bytes = (SIZE_T)(clip_right - clip_left) *
                bytes_per_pixel;
            LONG first = 0, end = rows, step = 1;
            if (source == dst && clip_top > source_y) {
                first = rows - 1;
                end = -1;
                step = -1;
            }
            for (LONG row = first; row != end; row += step) {
                BYTE *dp = dst->pixels +
                    (SIZE_T)(clip_top + row) * dst->pitch +
                    (SIZE_T)clip_left * bytes_per_pixel;
                const BYTE *sp = source->pixels +
                    (SIZE_T)(source_y + row) * source->pitch +
                    (SIZE_T)source_x * bytes_per_pixel;
                dd_memmove(dp, sp, row_bytes);
            }
        } else {
            if (source == dst && !same_scale)
                return E_NOTIMPL;
            for (LONG y = clip_top; y < clip_bottom; y++) {
                LONG sy = src_top + (LONG)(((int64_t)(y - dest_top) *
                    src_height) / dest_height);
                BYTE *dp = dst->pixels + (SIZE_T)y * dst->pitch +
                    (SIZE_T)clip_left * bytes_per_pixel;
                for (LONG x = clip_left; x < clip_right; x++) {
                    LONG sx = src_left + (LONG)(((int64_t)(x - dest_left) *
                        src_width) / dest_width);
                    const BYTE *sp = source->pixels +
                        (SIZE_T)sy * source->pitch +
                        (SIZE_T)sx * bytes_per_pixel;
                    uint32_t pixel = surface_read_pixel(sp, source->bpp);
                    if (!use_src_key ||
                        !surface_pixel_matches_key(source, pixel))
                        surface_write_pixel(dp, dst->bpp, pixel);
                    dp += bytes_per_pixel;
                }
            }
        }
    }

    /* If primary surface, blit (nearest-neighbor scaled to full GOP).
     * Re-arm the per-pump present hook on a primary-sized Blt for the same
     * reason as Unlock/Flip: a ddraw present means the ddraw path owns the
     * screen again after any GDI-present suspension. */
    if (dst->is_primary) {
        if (surf_is_present_sized(dst))
            dst->owner->present_surface = dst;
        present_surface_to_gop(dst);
    }

    /* Also copy to compositor's shm surface if available */
    if (dst->is_primary)
        ddraw_compositor_notify(dst->owner);

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
    if (!dst->pixels || !s->pixels || dst->owner != s->owner ||
        dst->bpp != s->bpp || dwX >= dst->width || dwY >= dst->height)
        return DDERR_INVALIDRECT;

    /* Source rect (default: entire source surface) */
    DWORD sx = 0, sy = 0, sw = s->width, sh = s->height;
    if (srcRect) {
        if (srcRect->left < 0 || srcRect->top < 0 ||
            srcRect->right <= srcRect->left ||
            srcRect->bottom <= srcRect->top ||
            srcRect->right > (LONG)s->width ||
            srcRect->bottom > (LONG)s->height)
            return DDERR_INVALIDRECT;
        sx = (DWORD)srcRect->left;  sy = (DWORD)srcRect->top;
        sw = (DWORD)(srcRect->right - srcRect->left);
        sh = (DWORD)(srcRect->bottom - srcRect->top);
    }

    /* Clip to destination bounds */
    if (sw > dst->width - dwX)  sw = dst->width - dwX;
    if (sh > dst->height - dwY) sh = dst->height - dwY;
    if (sw > s->width - sx)  sw = s->width - sx;
    if (sh > s->height - sy) sh = s->height - sy;
    if (!sw || !sh) return DD_OK;

    DWORD bpp_bytes = dst->bpp / 8;
    int use_src_colorkey = (dwTrans & DDBLTFAST_SRCCOLORKEY) != 0;
    if (use_src_colorkey && !s->has_src_key)
        return DDERR_NOCOLORKEY;

    LONG first = 0, end = (LONG)sh, step = 1;
    if (dst == s && dwY > sy) {
        first = (LONG)sh - 1;
        end = -1;
        step = -1;
    }

    for (LONG y = first; y != end; y += step) {
        BYTE *dp = dst->pixels + (dwY + y) * dst->pitch + dwX * bpp_bytes;
        BYTE *sp = s->pixels   + (sy + y)  * s->pitch   + sx * bpp_bytes;

        if (!use_src_colorkey) {
            dd_memmove(dp, sp, (SIZE_T)sw * bpp_bytes);
        } else {
            for (DWORD x = 0; x < sw; x++) {
                uint32_t pixel = surface_read_pixel(sp, s->bpp);
                if (!surface_pixel_matches_key(s, pixel))
                    surface_write_pixel(dp, dst->bpp, pixel);
                sp += bpp_bytes;
                dp += bpp_bytes;
            }
        }
    }

    if (dst->is_primary) {
        if (surf_is_present_sized(dst))
            dst->owner->present_surface = dst;
        present_surface_to_gop(dst);
        ddraw_compositor_notify(dst->owner);
    }
    return DD_OK;
}

static void ddraw_swap_surface_backing(DDSurface *left, DDSurface *right)
{
    BYTE *byte_ptr = left->pixels;
    left->pixels = right->pixels;
    right->pixels = byte_ptr;

    byte_ptr = left->allocation_base;
    left->allocation_base = right->allocation_base;
    right->allocation_base = byte_ptr;

    SIZE_T size = left->buf_size;
    left->buf_size = right->buf_size;
    right->buf_size = size;

    size = left->pages;
    left->pages = right->pages;
    right->pages = size;

    uint32_t value = left->present_hash;
    left->present_hash = right->present_hash;
    right->present_hash = value;

    int valid = left->present_hash_valid;
    left->present_hash_valid = right->present_hash_valid;
    right->present_hash_valid = valid;

    DWORD time = left->present_last_ms;
    left->present_last_ms = right->present_last_ms;
    right->present_last_ms = time;

    value = left->present_count;
    left->present_count = right->present_count;
    right->present_count = value;
}

static void ddraw_rotate_surface_chain_once(IDirectDrawSurface7 *root)
{
    DDSurface *previous = &root->surf;
    IDirectDrawSurface7 *surface = previous->back_buffer;
    for (uint32_t index = 1; index < MAX_SURFACES && surface; index++) {
        ddraw_swap_surface_backing(previous, &surface->surf);
        previous = &surface->surf;
        surface = surface->surf.back_buffer;
    }
}

/* Flip rotates the memory associations of the complete implicit chain. */
static HRESULT WINAPI surf_Flip(IDirectDrawSurface7 *self,
                                 IDirectDrawSurface7 *override, DWORD flags)
{
    IDirectDrawSurface7 *real_self = REAL_SURF(self);
    if (!real_self) return DDERR_INVALIDPARAMS;
    DDSurface *primary = &real_self->surf;
    if ((primary->caps & (DDSCAPS_FRONTBUFFER | DDSCAPS_FLIP)) !=
            (DDSCAPS_FRONTBUFFER | DDSCAPS_FLIP) ||
        !primary->back_buffer)
        return DDERR_NOTFLIPPABLE;

    const DWORD supported_flags =
        DDFLIP_WAIT | DDFLIP_NOVSYNC | DDFLIP_DONOTWAIT;
    if ((flags & ~supported_flags) ||
        (flags & (DDFLIP_WAIT | DDFLIP_DONOTWAIT)) ==
            (DDFLIP_WAIT | DDFLIP_DONOTWAIT))
        return DDERR_INVALIDPARAMS;

    IDirectDrawSurface7 *target = NULL;
    if (override) {
        target = REAL_SURF(override);
        if (!target) return DDERR_INVALIDPARAMS;
    }

    DDRAW_PROCESS_STATE *state = primary->owner;
    uint32_t rotations = target ? 0 : 1;
    BOOL target_found = target ? FALSE : TRUE;
    uint32_t chain_length = 0;
    IDirectDrawSurface7 *surface = real_self;
    while (chain_length < MAX_SURFACES &&
           ddraw_surface_is_live(surface, state)) {
        DDSurface *data = &surface->surf;
        if (data->locked) return DDERR_SURFACEBUSY;
        if (data->width != primary->width ||
            data->height != primary->height || data->bpp != primary->bpp)
            return DDERR_NOTFLIPPABLE;
        if (target && surface == target) {
            rotations = chain_length;
            target_found = TRUE;
        }
        chain_length++;
        surface = data->back_buffer;
    }
    if (chain_length < 2 || !target_found)
        return DDERR_NOTFLIPPABLE;

    if (!(flags & DDFLIP_NOVSYNC))
        ddraw_wait_for_vblank(DDWAITVB_BLOCKBEGIN);

    for (uint32_t step = 0; step < rotations; step++)
        ddraw_rotate_surface_chain_once(real_self);

    /* Blit primary to GOP framebuffer (nearest-neighbor scaled to full GOP).
     * A Flip is the strongest "ddraw owns the screen" signal: re-arm the
     * per-pump present hook so flip-chain games keep presenting after a GDI
     * BitBlt present suspended it (mirrors the Lock/Unlock re-arm). */
    if (surf_is_present_sized(primary))
        primary->owner->present_surface = primary;
    present_surface_to_gop(primary);

    ddraw_compositor_notify(primary->owner);
    return DD_OK;
}

static HRESULT WINAPI surf_GetSurfaceDesc(IDirectDrawSurface7 *self, DDSURFACEDESC2 *desc)
{
    if (!desc) return DDERR_INVALIDPARAMS;
    /* Fill the descriptor via the Lock path, but a Desc QUERY must not have
     * present side effects: don't let the synthetic Lock/Unlock arm the
     * per-pump present hook (that would override a GDI-present suspension and
     * push a stale ddraw frame over the DIB) or present a frame. Restore the
     * arming state and clear the lock flag directly instead of Unlock. */
    IDirectDrawSurface7 *real = REAL_SURF(self);
    if (!real || !real->surf.owner) return DDERR_INVALIDPARAMS;
    DDSurface *saved = real->surf.owner->present_surface;
    HRESULT hr = surf_Lock(self, NULL, desc, 0, NULL);
    real->surf.owner->present_surface = saved;
    if (hr == DD_OK) {
        real->surf.locked = 0;
    }
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
    IDirectDrawSurface7 *real = REAL_SURF(self);
    DDRAW_PROCESS_STATE *state = real ? real->surf.owner : NULL;
    int index = state ? (int)(real - state->surfaces) : -1;
    if (!real || !hdc || index < 0 || index >= MAX_SURFACES)
        return DDERR_INVALIDPARAMS;
    *hdc = (HDC)(ULONG_PTR)(0xDC00DD01U + (uint32_t)index);
    return DD_OK;
}

static HRESULT WINAPI surf_ReleaseDC(IDirectDrawSurface7 *self, HDC hdc)
{
    IDirectDrawSurface7 *real = REAL_SURF(self);
    DDRAW_PROCESS_STATE *state = real ? real->surf.owner : NULL;
    int index = state ? (int)(real - state->surfaces) : -1;
    if (!real || index < 0 || index >= MAX_SURFACES ||
        hdc != (HDC)(ULONG_PTR)(0xDC00DD01U + (uint32_t)index))
        return DDERR_INVALIDPARAMS;
    return DD_OK;
}

static HRESULT WINAPI surf_GetColorKey(IDirectDrawSurface7 *self, DWORD flags,
                                        PVOID lpDDColorKey)
{
    IDirectDrawSurface7 *real = REAL_SURF(self);
    if (!real || !lpDDColorKey) return DDERR_INVALIDPARAMS;
    uint32_t *key = (uint32_t *)lpDDColorKey;
    if (flags == DDCKEY_SRCBLT) {
        if (!real->surf.has_src_key) return DDERR_NOCOLORKEY;
        key[0] = real->surf.src_key_low;
        key[1] = real->surf.src_key_high;
    } else if (flags == DDCKEY_DESTBLT) {
        if (!real->surf.has_dst_key) return DDERR_NOCOLORKEY;
        key[0] = real->surf.dst_key_low;
        key[1] = real->surf.dst_key_high;
    } else {
        return DDERR_INVALIDPARAMS;
    }
    return DD_OK;
}

static HRESULT WINAPI surf_SetColorKey(IDirectDrawSurface7 *self, DWORD flags,
                                        PVOID lpDDColorKey)
{
    IDirectDrawSurface7 *real = REAL_SURF(self);
    if (!real) return DDERR_INVALIDPARAMS;
    if (flags != DDCKEY_SRCBLT && flags != DDCKEY_DESTBLT)
        return DDERR_INVALIDPARAMS;

    if (!lpDDColorKey) {
        if (flags == DDCKEY_SRCBLT)
            real->surf.has_src_key = 0;
        else
            real->surf.has_dst_key = 0;
        return DD_OK;
    }

    const uint32_t *key = (const uint32_t *)lpDDColorKey;
    if (key[0] > key[1]) return DDERR_INVALIDPARAMS;
    if (flags == DDCKEY_SRCBLT) {
        real->surf.src_key_low = key[0];
        real->surf.src_key_high = key[1];
        real->surf.has_src_key = 1;
    } else {
        real->surf.dst_key_low = key[0];
        real->surf.dst_key_high = key[1];
        real->surf.has_dst_key = 1;
    }
    return DD_OK;
}

/* SetClipper: attach clipper to surface (slot 28) */
static HRESULT WINAPI surf_SetClipper(IDirectDrawSurface7 *self, PVOID lpDDClipper)
{
    IDirectDrawSurface7 *real = REAL_SURF(self);
    if (!real) return DDERR_INVALIDPARAMS;
    DDClipper *next = lpDDClipper
        ? ddraw_clipper_from_object(lpDDClipper) : NULL;
    if (lpDDClipper && (!next || next->owner != real->surf.owner))
        return DDERR_INVALIDPARAMS;
    if (next == real->surf.clipper) return DD_OK;
    if (next) ddraw_clipper_add_ref(next);
    DDClipper *previous = real->surf.clipper;
    real->surf.clipper = next;
    if (previous) ddraw_clipper_release(previous);
    if (next) serial_puts("[DDRAW] SetClipper on surface\n");
    return DD_OK;
}

/* SetPalette: attach palette to surface (slot 31) */
static HRESULT WINAPI surf_SetPalette(IDirectDrawSurface7 *self, PVOID lpDDPalette)
{
    IDirectDrawSurface7 *real = REAL_SURF(self);
    if (!real) return DDERR_INVALIDPARAMS;
    DDPalette *next = lpDDPalette
        ? ddraw_palette_from_object(lpDDPalette) : NULL;
    if (lpDDPalette && (!next || next->owner != real->surf.owner))
        return DDERR_INVALIDPARAMS;
    if (next == real->surf.palette) return DD_OK;
    if (next) ddraw_palette_add_ref(next);
    DDPalette *previous = real->surf.palette;
    real->surf.palette = next;
    if (previous) ddraw_palette_release(previous);
    if (next) serial_puts("[DDRAW] SetPalette on surface\n");
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
    DDPalette *palette = real->surf.palette;
    DDRAW_PROCESS_STATE *state = real->surf.owner;
    if (!state || palette->owner != state || palette->slot >= MAX_PALETTES)
        return DDERR_NOPALETTEATTACHED;
    ddraw_palette_add_ref(palette);
    *(uint32_t *)lplpDDPalette =
        (uint32_t)(ULONG_PTR)&state->palette_proxies[palette->slot];
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
    DDClipper *clipper = real->surf.clipper;
    DDRAW_PROCESS_STATE *state = real->surf.owner;
    if (!state || clipper->owner != state || clipper->slot >= MAX_CLIPPERS)
        return DDERR_NOCLIPPERATTACHED;
    ddraw_clipper_add_ref(clipper);
    *(uint32_t *)lplpDDClipper =
        (uint32_t)(ULONG_PTR)&state->clipper_proxies[clipper->slot];
    return S_OK;
}

static HRESULT WINAPI surf_GetBltStatus(IDirectDrawSurface7 *self, DWORD flags)
{
    (void)flags;
    return REAL_SURF(self) ? DD_OK : DDERR_INVALIDPARAMS;
}

static HRESULT WINAPI surf_GetCaps(IDirectDrawSurface7 *self, DDSCAPS2 *caps)
{
    IDirectDrawSurface7 *real = REAL_SURF(self);
    if (!real || !caps) return DDERR_INVALIDPARAMS;

    uint8_t interface_version = ddraw_surface_interface_version(self);
    dd_memset(caps, 0,
              ddraw_descriptor_size_for_version(interface_version) == 124U
                  ? sizeof(*caps) : sizeof(DWORD));
    caps->dwCaps = real->surf.caps;
    return DD_OK;
}

static HRESULT WINAPI surf_GetFlipStatus(IDirectDrawSurface7 *self,
                                          DWORD flags)
{
    (void)flags;
    return REAL_SURF(self) ? DD_OK : DDERR_INVALIDPARAMS;
}

static HRESULT WINAPI surf_IsLost(IDirectDrawSurface7 *self)
{
    return REAL_SURF(self) ? DD_OK : DDERR_SURFACELOST;
}

static HRESULT WINAPI surf_Restore(IDirectDrawSurface7 *self)
{
    IDirectDrawSurface7 *real = REAL_SURF(self);
    return real && real->surf.pixels ? DD_OK : DDERR_SURFACELOST;
}

static HRESULT WINAPI surf_GetDDInterface(IDirectDrawSurface7 *self,
                                           uint32_t *directdraw_out)
{
    IDirectDrawSurface7 *real = REAL_SURF(self);
    if (!real || !directdraw_out) return DDERR_INVALIDPARAMS;
    *directdraw_out = 0;
    DDRAW_PROCESS_STATE *state = real->surf.owner;
    uint8_t version = ddraw_surface_interface_version(self);
    COM32_DirectDraw *proxy = ddraw_proxy_for_version(state, version);
    if (!proxy) return E_NOINTERFACE;
    __sync_add_and_fetch(&state->refs, 1);
    *directdraw_out = (uint32_t)(ULONG_PTR)proxy;
    return DD_OK;
}

static HRESULT WINAPI surf_PageLock(IDirectDrawSurface7 *self, DWORD flags)
{
    (void)flags;
    return REAL_SURF(self) ? DD_OK : DDERR_INVALIDPARAMS;
}

static HRESULT WINAPI surf_PageUnlock(IDirectDrawSurface7 *self, DWORD flags)
{
    (void)flags;
    return REAL_SURF(self) ? DD_OK : DDERR_INVALIDPARAMS;
}

static HRESULT WINAPI surf_GetUniquenessValue(IDirectDrawSurface7 *self,
                                               DWORD *value)
{
    IDirectDrawSurface7 *real = REAL_SURF(self);
    if (!real || !value) return DDERR_INVALIDPARAMS;
    *value = real->surf.uniqueness;
    return DD_OK;
}

static HRESULT WINAPI surf_ChangeUniquenessValue(IDirectDrawSurface7 *self)
{
    IDirectDrawSurface7 *real = REAL_SURF(self);
    if (!real) return DDERR_INVALIDPARAMS;
    real->surf.uniqueness =
        __sync_add_and_fetch(&ddraw_surface_uniqueness, 1);
    return DD_OK;
}

static HRESULT WINAPI surf_SetPriority(IDirectDrawSurface7 *self, DWORD value)
{
    IDirectDrawSurface7 *real = REAL_SURF(self);
    if (!real) return DDERR_INVALIDPARAMS;
    real->surf.priority = value;
    return DD_OK;
}

static HRESULT WINAPI surf_GetPriority(IDirectDrawSurface7 *self, DWORD *value)
{
    IDirectDrawSurface7 *real = REAL_SURF(self);
    if (!real || !value) return DDERR_INVALIDPARAMS;
    *value = real->surf.priority;
    return DD_OK;
}

static HRESULT WINAPI surf_SetLOD(IDirectDrawSurface7 *self, DWORD value)
{
    IDirectDrawSurface7 *real = REAL_SURF(self);
    if (!real) return DDERR_INVALIDPARAMS;
    real->surf.lod = value;
    return DD_OK;
}

static HRESULT WINAPI surf_GetLOD(IDirectDrawSurface7 *self, DWORD *value)
{
    IDirectDrawSurface7 *real = REAL_SURF(self);
    if (!real || !value) return DDERR_INVALIDPARAMS;
    *value = real->surf.lod;
    return DD_OK;
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
    NULL, surf_GetBltStatus, surf_GetCaps, /* 12-14 */
    surf_GetClipper,            /* 15 */
    surf_GetColorKey,           /* 16 */
    surf_GetDC,                 /* 17 */
    surf_GetFlipStatus, NULL,   /* 18-19 */
    surf_GetPalette,            /* 20 */
    NULL,                       /* 21 */
    surf_GetSurfaceDesc,        /* 22 */
    NULL, surf_IsLost,          /* 23-24 */
    surf_Lock,                  /* 25 */
    surf_ReleaseDC,             /* 26 */
    surf_Restore,               /* 27: Restore */
    surf_SetClipper,            /* 28 */
    surf_SetColorKey,           /* 29 */
    NULL,                       /* 30: SetOverlayPosition */
    surf_SetPalette,            /* 31 */
    surf_Unlock,                /* 32 */
    NULL, NULL, NULL,           /* 33-35: overlay operations */
    (PVOID)surf_GetDDInterface, /* 36 */
    (PVOID)surf_PageLock,       /* 37 */
    (PVOID)surf_PageUnlock,     /* 38 */
    NULL, NULL, NULL, NULL,     /* 39-42 */
    (PVOID)surf_GetUniquenessValue,    /* 43 */
    (PVOID)surf_ChangeUniquenessValue, /* 44 */
    (PVOID)surf_SetPriority,    /* 45 */
    (PVOID)surf_GetPriority,    /* 46 */
    (PVOID)surf_SetLOD,         /* 47 */
    (PVOID)surf_GetLOD          /* 48 */
};

static ULONG ddraw_surface_release(IDirectDrawSurface7 *surface)
{
    if (!surface ||
        __atomic_load_n(&surface->surf.used, __ATOMIC_ACQUIRE) != 1)
        return 0;

    DDSurface *data = &surface->surf;
    ULONG refs;
    do {
        refs = __atomic_load_n(&data->refs, __ATOMIC_ACQUIRE);
        if (!refs) return 0;
    } while (!__atomic_compare_exchange_n(&data->refs, &refs, refs - 1,
                                          FALSE, __ATOMIC_ACQ_REL,
                                          __ATOMIC_ACQUIRE));
    if (refs != 1) return refs - 1;

    __atomic_store_n(&data->used, 2, __ATOMIC_RELEASE);
    DDRAW_PROCESS_STATE *owner = data->owner;
    IDirectDrawSurface7 *back_buffer = data->back_buffer;
    DDPalette *palette = data->palette;
    DDClipper *clipper = data->clipper;
    BYTE *allocation = data->allocation_base;

    if (owner && owner->present_surface == data)
        owner->present_surface = NULL;
    data->pixels = NULL;
    data->allocation_base = NULL;
    data->back_buffer = NULL;
    data->palette = NULL;
    data->clipper = NULL;
    data->locked = 0;

    if (back_buffer) ddraw_surface_release(back_buffer);
    if (palette) ddraw_palette_release(palette);
    if (clipper) ddraw_clipper_release(clipper);
    if (allocation) VirtualFree(allocation, 0, MEM_RELEASE);

    data->owner = NULL;
    data->refs = 0;
    __atomic_store_n(&data->used, 0, __ATOMIC_RELEASE);
    return 0;
}

static IDirectDrawSurface7 *alloc_surface(DDRAW_PROCESS_STATE *state,
                                           DWORD w, DWORD h, DWORD bpp,
                                           DWORD caps)
{
    if (!state || !w || !h || (bpp != 8 && bpp != 16 && bpp != 32))
        return NULL;

    SIZE_T bytes_per_pixel = bpp / 8;
    if ((SIZE_T)w > (SIZE_T)0x7FFFFFFF / bytes_per_pixel)
        return NULL;
    SIZE_T pitch = (SIZE_T)w * bytes_per_pixel;
    if ((SIZE_T)h > (SIZE_T)-1 / pitch)
        return NULL;
    SIZE_T buffer_size = (SIZE_T)h * pitch;

    IDirectDrawSurface7 *surface = NULL;
    ddraw_lock();
    for (int i = 0; i < MAX_SURFACES; i++) {
        if (__atomic_load_n(&state->surfaces[i].surf.used,
                            __ATOMIC_RELAXED) == 0) {
            surface = &state->surfaces[i];
            __atomic_store_n(&surface->surf.used, 2, __ATOMIC_RELAXED);
            break;
        }
    }
    ddraw_unlock();
    if (!surface) return NULL;

    BYTE *allocation = (BYTE *)VirtualAlloc(NULL, buffer_size,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!allocation || (ULONG_PTR)allocation > UINT32_MAX) {
        if (allocation) VirtualFree(allocation, 0, MEM_RELEASE);
        __atomic_store_n(&surface->surf.used, 0, __ATOMIC_RELEASE);
        return NULL;
    }

    ddraw_lock();
    surface->lpVtbl = &surface_vtbl;
    DDSurface *data = &surface->surf;
    dd_memset(data, 0, sizeof(*data));
    data->used = 2;
    data->pixels = allocation;
    data->allocation_base = allocation;
    data->width = w;
    data->height = h;
    data->bpp = bpp;
    data->pitch = (LONG)pitch;
    data->buf_size = buffer_size;
    data->pages = (buffer_size + 4095) / 4096;
    data->owner = state;
    data->refs = 1;
    data->is_primary = (caps & DDSCAPS_PRIMARYSURFACE) != 0;
    data->caps = caps;
    data->uniqueness = __sync_add_and_fetch(&ddraw_surface_uniqueness, 1);
    __atomic_store_n(&data->used, 1, __ATOMIC_RELEASE);
    ddraw_unlock();

    dd_memset(data->pixels, 0, data->buf_size);

    serial_puts("[DDRAW] created surface ");
    serial_puthex(w, 4); serial_puts("x"); serial_puthex(h, 4);
    serial_puts("x"); serial_puthex(bpp, 2);
    serial_puts(data->is_primary ? " (primary)" : " (offscreen)");
    serial_puts(" pixels=0x");
    serial_puthex((ULONG_PTR)data->pixels, 8);
    serial_puts("\n");

    return surface;
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
    /* 23: IDirectDraw2::GetAvailableVidMem */
    HRESULT (WINAPI *GetAvailableVidMem)(IDirectDraw7 *self, DDSCAPS2 *caps,
                                          DWORD *total, DWORD *free);
};

static BOOL ddraw_is_exclusive(const DDRAW_PROCESS_STATE *state)
{
    return state &&
        (state->cooperative_flags &
         (DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN)) ==
        (DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
}

static BOOL ddraw_resolution_supported(DWORD width, DWORD height)
{
    if (!user32_get_display_resolution_count ||
        !user32_get_display_resolution)
        return TRUE;

    uint32_t count = user32_get_display_resolution_count();
    if (!count) return TRUE;
    for (uint32_t index = 0; index < count; index++) {
        uint32_t candidate_width = 0;
        uint32_t candidate_height = 0;
        if (user32_get_display_resolution(index, &candidate_width,
                                          &candidate_height) &&
            candidate_width == width && candidate_height == height)
            return TRUE;
    }
    return FALSE;
}

static void ddraw_configure_window(DDRAW_PROCESS_STATE *state, BOOL exclusive)
{
    if (!state || !state->hwnd || !user32_configure_directdraw_window)
        return;
    BOOL allow_changes =
        (state->cooperative_flags & DDSCL_NOWINDOWCHANGES) == 0;
    (void)user32_configure_directdraw_window(
        state->hwnd, exclusive, allow_changes,
        (int)state->display_width, (int)state->display_height);
}

static HRESULT WINAPI dd_QueryInterface(IDirectDraw7 *self, REFIID iid, PVOID *ppv)
{
    if (!ppv) return E_INVALIDARG;
    *(uint32_t *)ppv = 0;
    DDRAW_PROCESS_STATE *state = ddraw_state_from_object(self);
    if (!state) return DDERR_INVALIDPARAMS;
    uint8_t version = dd_guid_equal(iid, &dd_iid_iunknown)
        ? DDRAW_IFACE_1 : dd_version_from_iid(iid);
    COM32_DirectDraw *proxy = ddraw_proxy_for_version(state, version);
    if (!proxy) return E_NOINTERFACE;
    *(uint32_t *)ppv = (uint32_t)(ULONG_PTR)proxy;
    __sync_add_and_fetch(&state->refs, 1);
    serial_puts("[DDRAW] QueryInterface -> v");
    serial_putdec(version);
    serial_puts("\n");
    return S_OK;
}

static ULONG WINAPI dd_AddRef(IDirectDraw7 *self)
{
    DDRAW_PROCESS_STATE *state = ddraw_state_from_object(self);
    return state ? __sync_add_and_fetch(&state->refs, 1) : 0;
}

static ULONG WINAPI dd_Release(IDirectDraw7 *self)
{
    DDRAW_PROCESS_STATE *state = ddraw_state_from_object(self);
    if (!state) return 0;
    ULONG refs;
    do {
        refs = __atomic_load_n(&state->refs, __ATOMIC_ACQUIRE);
        if (!refs) return 0;
    } while (!__atomic_compare_exchange_n(&state->refs, &refs, refs - 1,
                                          FALSE, __ATOMIC_ACQ_REL,
                                          __ATOMIC_ACQUIRE));
    return refs - 1;
}

static HRESULT WINAPI dd_SetCooperativeLevel(IDirectDraw7 *self, HANDLE hwnd, DWORD flags)
{
    DDRAW_PROCESS_STATE *state = ddraw_state_from_object(self);
    if (!state) return DDERR_INVALIDPARAMS;

    BOOL normal = (flags & DDSCL_NORMAL) != 0;
    BOOL exclusive = (flags & DDSCL_EXCLUSIVE) != 0;
    BOOL fullscreen = (flags & DDSCL_FULLSCREEN) != 0;
    if (normal == exclusive || exclusive != fullscreen ||
        (exclusive && !hwnd))
        return DDERR_INVALIDPARAMS;

    HANDLE previous_hwnd = state->hwnd;
    BOOL was_exclusive = ddraw_is_exclusive(state);
    state->hwnd = hwnd;
    state->cooperative_flags = flags;

    if (user32_configure_directdraw_window) {
        if (was_exclusive && previous_hwnd && previous_hwnd != hwnd)
            (void)user32_configure_directdraw_window(
                previous_hwnd, FALSE, FALSE, 0, 0);
        ddraw_configure_window(state, exclusive && fullscreen);
    }

    serial_puts("[DDRAW] SetCooperativeLevel hwnd=0x");
    serial_puthex((uint64_t)(ULONG_PTR)hwnd, 8);
    serial_puts(" flags=0x");
    serial_puthex(flags, 8);
    serial_puts(exclusive && fullscreen ? " exclusive\n" : " normal\n");

    return DD_OK;
}

static HRESULT WINAPI dd_SetDisplayMode(IDirectDraw7 *self, DWORD w, DWORD h,
                                         DWORD bpp, DWORD refreshRate, DWORD flags)
{
    (void)refreshRate; (void)flags;
    DDRAW_PROCESS_STATE *state = ddraw_state_from_object(self);
    if (!state) return DDERR_INVALIDPARAMS;
    if (!ddraw_is_exclusive(state)) return DDERR_NOEXCLUSIVEMODE;
    if (!w || !h || (bpp && bpp != 8 && bpp != 16 && bpp != 32)) {
        serial_puts("[DDRAW] SetDisplayMode rejected invalid geometry\n");
        return DDERR_INVALIDMODE;
    }
    if (!ddraw_resolution_supported(w, h)) {
        serial_puts("[DDRAW] SetDisplayMode rejected unsupported resolution\n");
        return DDERR_INVALIDMODE;
    }

    {
        int changed = (state->display_width != w) ||
                      (state->display_height != h);
        state->display_width = w;
        state->display_height = h;
        state->mode_set = 1;

        if (changed) {
            /* A mode transition invalidates frames and surfaces sized for the
             * previous logical display mode. */
            if (g_present_owner_pid == state->owner_pid) {
                g_present_src_w = 0;
                g_present_src_h = 0;
                g_present_owner_pid = 0;
            }
            if (state->present_surface &&
                (state->present_surface->width != state->display_width ||
                 state->present_surface->height != state->display_height))
                state->present_surface = NULL;
            ddraw_configure_window(state, TRUE);
        }
    }
    if (bpp > 0)
        state->display_bpp = bpp;

    serial_puts("[DDRAW] SetDisplayMode req ");
    serial_puthex(w, 4); serial_puts("x"); serial_puthex(h, 4);
    serial_puts("x"); serial_puthex(bpp, 2);
    serial_puts(" -> using ");
    serial_puthex(state->display_width, 4); serial_puts("x");
    serial_puthex(state->display_height, 4);
    serial_puts("\n");

    ensure_framebuffer(state->display_width, state->display_height);
    return DD_OK;
}

static HRESULT WINAPI dd_SetDisplayMode_v1(IDirectDraw7 *self, DWORD w,
                                            DWORD h, DWORD bpp)
{
    return dd_SetDisplayMode(self, w, h, bpp, 0, 0);
}

static HRESULT WINAPI dd_Compact(IDirectDraw7 *self)
{
    return ddraw_state_from_object(self) ? DD_OK : DDERR_INVALIDPARAMS;
}

static HRESULT WINAPI dd_FlipToGDISurface(IDirectDraw7 *self)
{
    DDRAW_PROCESS_STATE *state = ddraw_state_from_object(self);
    if (!state) return DDERR_INVALIDPARAMS;
    state->present_surface = NULL;
    return DD_OK;
}

static HRESULT WINAPI dd_GetFourCCCodes(IDirectDraw7 *self, DWORD *count,
                                         DWORD *codes)
{
    (void)codes;
    if (!ddraw_state_from_object(self) || !count)
        return DDERR_INVALIDPARAMS;
    *count = 0;
    return DD_OK;
}

static HRESULT WINAPI dd_GetGDISurface(IDirectDraw7 *self,
                                        uint32_t *surface_out)
{
    DDRAW_PROCESS_STATE *state = ddraw_state_from_object(self);
    if (!state || !surface_out) return DDERR_INVALIDPARAMS;
    *surface_out = 0;
    for (int index = 0; index < MAX_SURFACES; index++) {
        IDirectDrawSurface7 *surface = &state->surfaces[index];
        if (surface->surf.used != 1 || !surface->surf.is_primary) continue;
        COM32_Surface *proxy = ddraw_surface_proxy(
            state, (uint32_t)index, ddraw_created_surface_version(self));
        if (!proxy) return DDERR_INVALIDPARAMS;
        __sync_add_and_fetch(&surface->surf.refs, 1);
        *surface_out = (uint32_t)(ULONG_PTR)proxy;
        return DD_OK;
    }
    return DDERR_NOTFOUND;
}

static HRESULT WINAPI dd_GetMonitorFrequency(IDirectDraw7 *self,
                                              DWORD *frequency)
{
    if (!ddraw_state_from_object(self) || !frequency)
        return DDERR_INVALIDPARAMS;
    *frequency = 60;
    return DD_OK;
}

static HRESULT WINAPI dd_GetScanLine(IDirectDraw7 *self, DWORD *scanline)
{
    DDRAW_PROCESS_STATE *state = ddraw_state_from_object(self);
    if (!state || !scanline) return DDERR_INVALIDPARAMS;
    DWORD frame_ms = ddraw_vblank_phase(shim_timeGetTime());
    if (frame_ms >= DDRAW_VBLANK_BEGIN_MS)
        frame_ms = DDRAW_VBLANK_BEGIN_MS - 1;
    *scanline = state->display_height
        ? (frame_ms * state->display_height) / DDRAW_VBLANK_BEGIN_MS : 0;
    return DD_OK;
}

static HRESULT WINAPI dd_GetVerticalBlankStatus(IDirectDraw7 *self,
                                                 BOOL *in_vertical_blank)
{
    if (!ddraw_state_from_object(self) || !in_vertical_blank)
        return DDERR_INVALIDPARAMS;
    *in_vertical_blank = ddraw_vblank_active_at(shim_timeGetTime());
    return DD_OK;
}

static HRESULT WINAPI dd_Initialize(IDirectDraw7 *self, LPGUID guid)
{
    (void)guid;
    return ddraw_state_from_object(self) ? E_FAIL : DDERR_INVALIDPARAMS;
}

static HRESULT WINAPI dd_RestoreDisplayMode(IDirectDraw7 *self)
{
    DDRAW_PROCESS_STATE *state = ddraw_state_from_object(self);
    if (!state) return DDERR_INVALIDPARAMS;

    state->present_surface = NULL;
    if (g_present_owner_pid == state->owner_pid) {
        g_present_src_w = 0;
        g_present_src_h = 0;
        g_present_owner_pid = 0;
    }
    reset_display_mode(state);
    if (ddraw_is_exclusive(state))
        ddraw_configure_window(state, TRUE);
    ensure_framebuffer(state->display_width, state->display_height);
    return DD_OK;
}

static HRESULT WINAPI dd_WaitForVerticalBlank(IDirectDraw7 *self,
                                               DWORD flags, HANDLE event)
{
    (void)event;
    if (!ddraw_state_from_object(self)) return DDERR_INVALIDPARAMS;
    if (flags == DDWAITVB_BLOCKBEGINEVENT) return DDERR_UNSUPPORTED;
    if (flags != DDWAITVB_BLOCKBEGIN && flags != DDWAITVB_BLOCKEND)
        return DDERR_INVALIDPARAMS;
    ddraw_wait_for_vblank(flags);
    return DD_OK;
}

/* Map COM32 surface proxy to real surface */
static IDirectDrawSurface7 *com32_to_surface(uint32_t proxy_addr)
{
    DDRAW_PROCESS_STATE *state = ddraw_current_state(FALSE);
    if (!state || !proxy_addr || !state->surface_proxies) return NULL;
    ULONG_PTR value = proxy_addr;
    ULONG_PTR first = (ULONG_PTR)&state->surface_proxies[0];
    ULONG_PTR end = (ULONG_PTR)&state->surface_proxies[
        MAX_SURFACES * DDRAW_INTERFACE_COUNT];
    if (value < first || value >= end || (value - first) % sizeof(COM32_Surface))
        return NULL;
    COM32_Surface *proxy = (COM32_Surface *)value;
    uint32_t index = proxy->surf_index;
    if (index >= MAX_SURFACES ||
        dd_interface_index(proxy->interface_version) < 0)
        return NULL;
    IDirectDrawSurface7 *surface = &state->surfaces[index];
    return surface->surf.owner == state &&
           __atomic_load_n(&surface->surf.used, __ATOMIC_ACQUIRE) == 1
        ? surface : NULL;
}

/* Return the unique member of the implicit chain matching the requested caps. */
static HRESULT WINAPI surf_GetAttachedSurface(IDirectDrawSurface7 *self,
                                                PVOID lpDDSCaps,
                                                uint32_t *lplpDDAttachedSurface)
{
    IDirectDrawSurface7 *real = REAL_SURF(self);
    if (!real || !lpDDSCaps || !lplpDDAttachedSurface)
        return DDERR_INVALIDPARAMS;
    *lplpDDAttachedSurface = 0;

    const uint32_t *requested_caps = (const uint32_t *)lpDDSCaps;
    uint8_t interface_version = ddraw_surface_interface_version(self);
    if (ddraw_descriptor_size_for_version(interface_version) == 124U &&
        (requested_caps[1] || requested_caps[2] || requested_caps[3]))
        return DDERR_NOTFOUND;

    DDSurface *ds = &real->surf;
    DDRAW_PROCESS_STATE *state = ds->owner;
    IDirectDrawSurface7 *root =
        ddraw_surface_chain_root(state, real);
    if (!root || ddraw_surface_chain_length(root) < 2)
        return DDERR_NOTFOUND;

    IDirectDrawSurface7 *match = NULL;
    IDirectDrawSurface7 *candidate = root;
    for (uint32_t index = 0; index < MAX_SURFACES && candidate; index++) {
        if (candidate != real &&
            ddraw_surface_is_live(candidate, state) &&
            ddraw_surface_caps_match(candidate->surf.caps,
                                     requested_caps[0])) {
            if (match) return DDERR_NOTFOUND;
            match = candidate;
        }
        candidate = candidate->surf.back_buffer;
    }
    if (!match) return DDERR_NOTFOUND;

    int idx = state ? (int)(match - state->surfaces) : -1;
    if (!state || idx < 0 || idx >= MAX_SURFACES ||
        !ddraw_surface_is_live(match, state)) {
        return DDERR_NOTFOUND;
    }

    COM32_Surface *proxy = ddraw_surface_proxy(
        state, (uint32_t)idx, ddraw_surface_interface_version(self));
    if (!proxy) return DDERR_INVALIDPARAMS;
    __sync_add_and_fetch(&match->surf.refs, 1);
    *lplpDDAttachedSurface = (uint32_t)(ULONG_PTR)proxy;
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
    (void)dwFlags;
    if (!lplpDDClipper || pUnkOuter) return DDERR_INVALIDPARAMS;
    *(uint32_t *)lplpDDClipper = 0;
    DDRAW_PROCESS_STATE *state = ddraw_state_from_object(self);
    if (!state || !state->clipper_proxies) return DDERR_INVALIDPARAMS;

    DDClipper *clipper = NULL;
    ddraw_lock();
    for (uint32_t i = 0; i < MAX_CLIPPERS; i++) {
        if (__atomic_load_n(&state->clippers[i].used,
                            __ATOMIC_RELAXED) != 0)
            continue;
        clipper = &state->clippers[i];
        dd_memset(clipper, 0, sizeof(*clipper));
        clipper->owner = state;
        clipper->slot = i;
        clipper->refs = 1;
        __atomic_store_n(&clipper->used, 1, __ATOMIC_RELEASE);
        break;
    }
    ddraw_unlock();
    if (!clipper) return E_OUTOFMEMORY;

    *(uint32_t *)lplpDDClipper =
        (uint32_t)(ULONG_PTR)&state->clipper_proxies[clipper->slot];
    serial_puts("[DDRAW] CreateClipper\n");
    return DD_OK;
}

/* CreatePalette: IDirectDraw7 slot 5 — creates an IDirectDrawPalette.
 * Args (stdcall): this, dwFlags, lpDDColorArray, lplpDDPalette, pUnkOuter */
static HRESULT WINAPI dd_CreatePalette(IDirectDraw7 *self, DWORD dwFlags,
                                        PVOID lpDDColorArray,
                                        PVOID *lplpDDPalette, PVOID pUnkOuter)
{
    if (!lplpDDPalette || pUnkOuter) return DDERR_INVALIDPARAMS;
    *(uint32_t *)lplpDDPalette = 0;
    DDRAW_PROCESS_STATE *state = ddraw_state_from_object(self);
    if (!state || !state->palette_proxies) return DDERR_INVALIDPARAMS;

    DDPalette *palette = NULL;
    ddraw_lock();
    for (uint32_t i = 0; i < MAX_PALETTES; i++) {
        if (__atomic_load_n(&state->palettes[i].used,
                            __ATOMIC_RELAXED) != 0)
            continue;
        palette = &state->palettes[i];
        dd_memset(palette, 0, sizeof(*palette));
        palette->owner = state;
        palette->slot = i;
        palette->refs = 1;
        __atomic_store_n(&palette->used, 1, __ATOMIC_RELEASE);
        break;
    }
    ddraw_unlock();
    if (!palette) return E_OUTOFMEMORY;

    /* Initialize palette entries from caller's color array if provided */
    if (lpDDColorArray && (dwFlags & 0x04)) {
        /* DDPCAPS_8BIT = 0x04 → 256 entries */
        const BYTE *in = (const BYTE *)lpDDColorArray;
        for (int i = 0; i < 256; i++) {
            BYTE r = in[i * 4 + 0];
            BYTE g = in[i * 4 + 1];
            BYTE b = in[i * 4 + 2];
            palette->entries[i] = ((uint32_t)r << 16) |
                                  ((uint32_t)g << 8) | b;
        }
    }

    *(uint32_t *)lplpDDPalette =
        (uint32_t)(ULONG_PTR)&state->palette_proxies[palette->slot];
    serial_puts("[DDRAW] CreatePalette flags=0x");
    serial_puthex(dwFlags, 4);
    serial_puts("\n");
    return DD_OK;
}

typedef struct {
    DWORD width;
    DWORD height;
    DWORD bpp;
    DWORD caps;
    DWORD back_buffer_count;
} DDRAW_SURFACE_REQUEST;

static HRESULT ddraw_parse_pixel_format(const uint32_t *descriptor,
                                         DWORD default_bpp, DWORD *bpp)
{
    if (!(descriptor[1] & DDSD_PIXELFORMAT)) {
        *bpp = default_bpp;
        return DD_OK;
    }

    DWORD format_size = descriptor[18];
    DWORD format_flags = descriptor[19];
    DWORD fourcc = descriptor[20];
    DWORD bit_count = descriptor[21];
    if (format_size != sizeof(DDPIXELFORMAT) || fourcc)
        return DDERR_INVALIDPIXELFORMAT;

    if (bit_count == 8) {
        if (!(format_flags & DDPF_PALETTEINDEXED8))
            return DDERR_INVALIDPIXELFORMAT;
    } else if (bit_count == 16) {
        if (!(format_flags & DDPF_RGB))
            return DDERR_INVALIDPIXELFORMAT;
        if ((descriptor[22] || descriptor[23] || descriptor[24]) &&
            (descriptor[22] != 0xF800 || descriptor[23] != 0x07E0 ||
             descriptor[24] != 0x001F))
            return DDERR_INVALIDPIXELFORMAT;
    } else if (bit_count == 32) {
        if (!(format_flags & DDPF_RGB))
            return DDERR_INVALIDPIXELFORMAT;
        if ((descriptor[22] || descriptor[23] || descriptor[24]) &&
            (descriptor[22] != 0x00FF0000 ||
             descriptor[23] != 0x0000FF00 ||
             descriptor[24] != 0x000000FF))
            return DDERR_INVALIDPIXELFORMAT;
    } else {
        return DDERR_INVALIDPIXELFORMAT;
    }

    *bpp = bit_count;
    return DD_OK;
}

static HRESULT ddraw_parse_surface_request(DWORD display_width,
                                            DWORD display_height,
                                            DWORD display_bpp,
                                            uint8_t interface_version,
                                            const uint32_t *descriptor,
                                            DDRAW_SURFACE_REQUEST *request)
{
    if (!descriptor || !request)
        return DDERR_INVALIDPARAMS;
    if (descriptor[0] != ddraw_descriptor_size_for_version(interface_version) ||
        !(descriptor[1] & DDSD_CAPS))
        return DDERR_INVALIDPARAMS;

    DWORD flags = descriptor[1];
    DWORD caps = descriptor[26];
    const DWORD supported_caps =
        DDSCAPS_PRIMARYSURFACE | DDSCAPS_BACKBUFFER |
        DDSCAPS_FRONTBUFFER | DDSCAPS_FLIP | DDSCAPS_OFFSCREENPLAIN |
        DDSCAPS_COMPLEX | DDSCAPS_VIDEOMEMORY | DDSCAPS_SYSTEMMEMORY;
    if (caps & ~supported_caps)
        return DDERR_INVALIDCAPS;
    if ((caps & (DDSCAPS_VIDEOMEMORY | DDSCAPS_SYSTEMMEMORY)) ==
        (DDSCAPS_VIDEOMEMORY | DDSCAPS_SYSTEMMEMORY))
        return DDERR_INVALIDCAPS;
    if (flags & DDSD_LPSURFACE)
        return DDERR_UNSUPPORTED;
    if ((flags & DDSD_PITCH) && !(flags & DDSD_LPSURFACE))
        return DDERR_INVALIDPARAMS;

    BOOL primary = (caps & DDSCAPS_PRIMARYSURFACE) != 0;
    BOOL flip = (caps & DDSCAPS_FLIP) != 0;
    DWORD back_buffer_count =
        (flags & DDSD_BACKBUFFERCOUNT) ? descriptor[5] : 0;

    if (flip) {
        if (!primary || !(caps & DDSCAPS_COMPLEX) ||
            !(flags & DDSD_BACKBUFFERCOUNT) || !back_buffer_count ||
            back_buffer_count >= MAX_SURFACES ||
            (caps & (DDSCAPS_FRONTBUFFER | DDSCAPS_BACKBUFFER |
                     DDSCAPS_OFFSCREENPLAIN)))
            return DDERR_INVALIDCAPS;
        caps |= DDSCAPS_FRONTBUFFER;
    } else if ((caps & DDSCAPS_COMPLEX) || back_buffer_count ||
               (flags & DDSD_BACKBUFFERCOUNT)) {
        return DDERR_INVALIDCAPS;
    }

    if (primary && (caps & (DDSCAPS_BACKBUFFER | DDSCAPS_OFFSCREENPLAIN)))
        return DDERR_INVALIDCAPS;
    if ((caps & DDSCAPS_BACKBUFFER) && (caps & DDSCAPS_FRONTBUFFER))
        return DDERR_INVALIDCAPS;
    if ((caps & DDSCAPS_OFFSCREENPLAIN) &&
        (caps & (DDSCAPS_BACKBUFFER | DDSCAPS_FRONTBUFFER)))
        return DDERR_INVALIDCAPS;

    if (!primary &&
        !(caps & (DDSCAPS_BACKBUFFER | DDSCAPS_FRONTBUFFER |
                  DDSCAPS_OFFSCREENPLAIN)))
        caps |= DDSCAPS_OFFSCREENPLAIN;
    if (!(caps & (DDSCAPS_VIDEOMEMORY | DDSCAPS_SYSTEMMEMORY)))
        caps |= primary ? DDSCAPS_VIDEOMEMORY : DDSCAPS_SYSTEMMEMORY;

    if (!primary &&
        (flags & (DDSD_WIDTH | DDSD_HEIGHT)) !=
            (DDSD_WIDTH | DDSD_HEIGHT))
        return DDERR_INVALIDPARAMS;

    request->width = (flags & DDSD_WIDTH)
        ? descriptor[3] : display_width;
    request->height = (flags & DDSD_HEIGHT)
        ? descriptor[2] : display_height;
    if (!request->width || !request->height)
        return DDERR_INVALIDPARAMS;

    HRESULT status = ddraw_parse_pixel_format(
        descriptor, display_bpp, &request->bpp);
    if (status != DD_OK) return status;

    request->caps = caps;
    request->back_buffer_count = back_buffer_count;
    return DD_OK;
}

static BOOL ddraw_primary_surface_exists(const DDRAW_PROCESS_STATE *state)
{
    if (!state) return FALSE;
    for (uint32_t index = 0; index < MAX_SURFACES; index++) {
        const DDSurface *surface = &state->surfaces[index].surf;
        if (__atomic_load_n(&surface->used, __ATOMIC_ACQUIRE) == 1 &&
            (surface->caps & DDSCAPS_PRIMARYSURFACE))
            return TRUE;
    }
    return FALSE;
}

static HRESULT WINAPI dd_CreateSurface(IDirectDraw7 *self, DDSURFACEDESC2 *desc,
                                        IDirectDrawSurface7 **surf, PVOID pUnkOuter)
{
    if (!desc || !surf || pUnkOuter) return DDERR_INVALIDPARAMS;
    *(uint32_t *)surf = 0;
    DDRAW_PROCESS_STATE *state = ddraw_state_from_object(self);
    if (!state || !state->surface_proxies) return DDERR_INVALIDPARAMS;
    if (!state->cooperative_flags) return DDERR_NOCOOPERATIVELEVELSET;

    /* DirectDraw descriptors originate in the PE32 address space. */
    uint32_t *d32 = (uint32_t *)desc;
    DDRAW_SURFACE_REQUEST request;
    HRESULT status = ddraw_parse_surface_request(
        state->display_width, state->display_height, state->display_bpp,
        ddraw_interface_version(self), d32, &request);
    if (status != DD_OK) return status;
    if ((request.caps & DDSCAPS_PRIMARYSURFACE) &&
        ddraw_primary_surface_exists(state))
        return DDERR_PRIMARYSURFACEALREADYEXISTS;

    IDirectDrawSurface7 *s = alloc_surface(
        state, request.width, request.height, request.bpp, request.caps);
    if (!s)
        return (request.caps & DDSCAPS_VIDEOMEMORY)
            ? DDERR_OUTOFVIDEOMEMORY : E_OUTOFMEMORY;

    IDirectDrawSurface7 *tail = s;
    DWORD memory_caps = request.caps &
        (DDSCAPS_VIDEOMEMORY | DDSCAPS_SYSTEMMEMORY);
    for (DWORD index = 0; index < request.back_buffer_count; index++) {
        DWORD child_caps = memory_caps | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
        if (index == 0) child_caps |= DDSCAPS_BACKBUFFER;
        IDirectDrawSurface7 *child = alloc_surface(
            state, request.width, request.height, request.bpp, child_caps);
        if (!child) {
            ddraw_surface_release(s);
            return (memory_caps & DDSCAPS_VIDEOMEMORY)
                ? DDERR_OUTOFVIDEOMEMORY : E_OUTOFMEMORY;
        }
        tail->surf.back_buffer = child;
        tail = child;
    }

    int idx = (int)(s - state->surfaces);
    COM32_Surface *proxy = ddraw_surface_proxy(
        state, (uint32_t)idx, ddraw_created_surface_version(self));
    if (!proxy) {
        ddraw_surface_release(s);
        return DDERR_INVALIDPARAMS;
    }

    *(uint32_t *)surf = (uint32_t)(ULONG_PTR)proxy;
    serial_puts("[DDRAW] CreateSurface: proxy=0x");
    serial_puthex((uint64_t)(ULONG_PTR)proxy, 8);
    serial_puts(" caps=0x");
    serial_puthex(request.caps, 8);
    serial_puts(" buffers=");
    serial_putdec((uint64_t)request.back_buffer_count + 1);
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
    DDRAW_PROCESS_STATE *state = ddraw_state_from_object(self);
    if (!state || !desc) return DDERR_INVALIDPARAMS;
    /* The caller is 32-bit PE code: write the 32-bit DDSURFACEDESC2 layout
     * (dwSize 124, ddpfPixelFormat at offset 72) with raw dword stores. The
     * 64-bit struct has an 8-byte lpSurface, so writing through it lands the
     * pixel format at the WRONG offset for the 32-bit reader. Also fill the
     * RGB masks — SoftDrv builds its color tables from them. */
    uint32_t *d = (uint32_t *)desc;
    uint8_t version = ddraw_interface_version(self);
    uint32_t descriptor_size = ddraw_descriptor_size_for_version(version);
    dd_memset(d, 0, descriptor_size);
    d[0]  = descriptor_size;                       /* dwSize */
    d[1]  = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
    d[2]  = state->display_height;                 /* dwHeight  @8  */
    d[3]  = state->display_width;                  /* dwWidth   @12 */
    d[4]  = state->display_width *
            (state->display_bpp / 8);              /* lPitch    @16 */
    fill_pixfmt32(d + 18, state->display_bpp);     /* ddpf      @72 */
    return DD_OK;
}

/* Software DirectDraw surfaces consume normal committed pages. Reserve three
 * quarters of physical memory for the kernel and other processes, and expose
 * the remaining quarter as this adapter's dynamic surface budget. The API is
 * DWORD-sized, so large hosts are clamped at the representable limit. */
static void ddraw_get_memory_budget(DDRAW_PROCESS_STATE *state,
                                    DWORD *total_out, DWORD *free_out)
{
    uint64_t total = mem_get_total ? mem_get_total() : 0;
    uint64_t free = mem_get_free ? mem_get_free() : 0;

    if (!total && state) {
        uint64_t frame = (uint64_t)state->display_width *
                         state->display_height * 4;
        total = frame * 4;
        free = total;
    }
    if (!free || free > total) free = total;

    total /= 4;
    free /= 4;
    if (free > total) free = total;
    if (total > 0xFFFFF000ULL) total = 0xFFFFF000ULL;
    if (free > total) free = total;

    total &= ~0xFFFULL;
    free &= ~0xFFFULL;
    if (total_out) *total_out = (DWORD)total;
    if (free_out) *free_out = (DWORD)free;
}

static DWORD ddraw_software_device_caps(void)
{
    return DDCAPS_BLT | DDCAPS_BLTSTRETCH | DDCAPS_GDI |
           DDCAPS_PALETTE | DDCAPS_READSCANLINE | DDCAPS_VBI |
           DDCAPS_COLORKEY | DDCAPS_BLTCOLORFILL |
           DDCAPS_CANBLTSYSMEM;
}

/* IDirectDraw::GetCaps — real vtable slot 11 (this + lpDDDriverCaps + lpDDHELCaps).
 * UE1's UWindowsViewport reads these caps to decide whether DirectDraw is usable
 * (fullscreen / BLIT_DirectDraw). A zeroed/garbage DDCAPS makes it bail. Report an
 * honest HEL-style software device: GDI-coexistent, blit + stretch + colorfill,
 * palette, sysmem blits. DDCAPS layout (32-bit): dwSize@0, dwCaps@4, dwCaps2@8,
 * dwPalCaps@24, dwVidMemTotal@60, dwVidMemFree@64. */
static HRESULT WINAPI dd_GetCaps(IDirectDraw7 *self, PVOID lpDriverCaps, PVOID lpHELCaps)
{
    DDRAW_PROCESS_STATE *state = ddraw_state_from_object(self);
    if (!state) return DDERR_INVALIDPARAMS;
    if (!lpDriverCaps && !lpHELCaps) return DDERR_INVALIDPARAMS;
    DWORD video_total = 0, video_free = 0;
    ddraw_get_memory_budget(state, &video_total, &video_free);
    PVOID targets[2] = { lpDriverCaps, lpHELCaps };
    for (int t = 0; t < 2; t++) {
        uint32_t *c = (uint32_t *)targets[t];
        if (!c) continue;
        uint32_t size = c[0];                   /* caller sets dwSize */
        if (size < 8 || size > 1024) size = 380; /* DDCAPS_DX7 default */
        for (uint32_t i = 1; i < size / 4; i++) c[i] = 0;
        c[1] = ddraw_software_device_caps();
        if (size >= 28)
            c[6] = 0x04 | 0x10 | 0x40;  /* dwPalCaps: 8BIT|PRIMARYSURFACE|ALLOW256 */
        if (size >= 68) {
            c[15] = video_total;         /* dwVidMemTotal @60 */
            c[16] = video_free;          /* dwVidMemFree  @64 */
        }
    }
    serial_puts("[DDRAW] GetCaps -> HEL caps reported\n");
    return DD_OK;
}

static HRESULT WINAPI dd_GetAvailableVidMem(IDirectDraw7 *self,
                                             DDSCAPS2 *caps,
                                             DWORD *total, DWORD *free)
{
    (void)caps;
    DDRAW_PROCESS_STATE *state = ddraw_state_from_object(self);
    if (!state || (!total && !free)) return DDERR_INVALIDPARAMS;
    ddraw_get_memory_budget(state, total, free);
    return DD_OK;
}

static HRESULT WINAPI dd_GetSurfaceFromDC(IDirectDraw7 *self, HDC hdc,
                                           uint32_t *surface_out)
{
    DDRAW_PROCESS_STATE *state = ddraw_state_from_object(self);
    if (!state || !surface_out) return DDERR_INVALIDPARAMS;
    *surface_out = 0;
    for (uint32_t index = 0; index < MAX_SURFACES; index++) {
        IDirectDrawSurface7 *surface = &state->surfaces[index];
        if (__atomic_load_n(&surface->surf.used, __ATOMIC_ACQUIRE) != 1 ||
            hdc != (HDC)(ULONG_PTR)(0xDC00DD01U + index))
            continue;
        COM32_Surface *proxy = ddraw_surface_proxy(
            state, index, ddraw_created_surface_version(self));
        if (!proxy) return DDERR_INVALIDPARAMS;
        __sync_add_and_fetch(&surface->surf.refs, 1);
        *surface_out = (uint32_t)(ULONG_PTR)proxy;
        return DD_OK;
    }
    return DDERR_NOTFOUND;
}

static HRESULT WINAPI dd_RestoreAllSurfaces(IDirectDraw7 *self)
{
    DDRAW_PROCESS_STATE *state = ddraw_state_from_object(self);
    if (!state) return DDERR_INVALIDPARAMS;
    for (int index = 0; index < MAX_SURFACES; index++) {
        IDirectDrawSurface7 *surface = &state->surfaces[index];
        if (__atomic_load_n(&surface->surf.used, __ATOMIC_ACQUIRE) == 1 &&
            !surface->surf.pixels)
            return DDERR_SURFACELOST;
    }
    return DD_OK;
}

static HRESULT WINAPI dd_TestCooperativeLevel(IDirectDraw7 *self)
{
    DDRAW_PROCESS_STATE *state = ddraw_state_from_object(self);
    if (!state) return DDERR_INVALIDPARAMS;
    return state->cooperative_flags ? DD_OK : DDERR_NOCOOPERATIVELEVELSET;
}

static HRESULT WINAPI dd_GetDeviceIdentifier(IDirectDraw7 *self,
                                              PVOID identifier, DWORD flags)
{
    (void)flags;
    if (!ddraw_state_from_object(self) || !identifier)
        return DDERR_INVALIDPARAMS;

    uint8_t version = ddraw_interface_version(self);
    SIZE_T size = version == DDRAW_IFACE_7 ? 1068U : 1064U;
    BYTE *bytes = (BYTE *)identifier;
    dd_memset(bytes, 0, size);
    static const char driver[] = "osito-ddraw";
    static const char description[] = "OsitoK Software DirectDraw";
    for (SIZE_T index = 0; index < sizeof(driver); index++)
        bytes[index] = (BYTE)driver[index];
    for (SIZE_T index = 0; index < sizeof(description); index++)
        bytes[512 + index] = (BYTE)description[index];
    ((DWORD *)(bytes + 1024))[0] = 1; /* low driver version */
    GUID *device_guid = (GUID *)(bytes + 1048);
    *device_guid = dd_iid_directdraw7;
    return DD_OK;
}

static HRESULT WINAPI dd_StartModeTest(IDirectDraw7 *self, PVOID modes,
                                        DWORD mode_count, DWORD flags)
{
    (void)flags;
    if (!ddraw_state_from_object(self) || (!modes && mode_count))
        return DDERR_INVALIDPARAMS;
    const LONG *sizes = (const LONG *)modes;
    for (DWORD index = 0; index < mode_count; index++) {
        LONG width = sizes[index * 2];
        LONG height = sizes[index * 2 + 1];
        if (width <= 0 || height <= 0 ||
            !ddraw_resolution_supported((DWORD)width, (DWORD)height))
            return DDERR_INVALIDMODE;
    }
    return DD_OK;
}

static HRESULT WINAPI dd_EvaluateMode(IDirectDraw7 *self, DWORD flags,
                                       DWORD *timeout)
{
    (void)flags;
    if (!ddraw_state_from_object(self)) return DDERR_INVALIDPARAMS;
    if (timeout) *timeout = 0;
    return DD_OK;
}

/* ── DirectDraw entry points ───────────────────────────────── */

/* ── 32-bit COM proxy for compat32 mode ───────────────────── */
/*
 * The 64-bit IDirectDraw7 vtable has 8-byte function pointers.
 * 32-bit PE code reads 4-byte slots → reads wrong offsets.
 * Solution: create a 32-bit proxy with a vtable of 4-byte thunk addrs.
 * Each thunk does INT 0x2E → dispatches to the real 64-bit function.
 *
 * IDirectDraw v1 has 23 methods; versions 2-7 expose 30 methods:
 *   0: QueryInterface   6: CreateSurface  12: GetCaps
 *  13: GetDisplayMode  20: SetCooperativeLevel  21: SetDisplayMode
 * IDirectDrawSurface7 vtable (49 methods):
 *   5: Blt  11: Flip  12: GetAttachedSurface  17: GetDC
 *  22: GetSurfaceDesc  25: Lock  26: ReleaseDC  32: Unlock
 */

/* 32-bit COM proxy objects — allocated from PE32-accessible memory
 * (mem_alloc_pages) so their addresses are in the PE32 address space,
 * NOT in kernel BSS.  Previously these were BSS variables at ~0x025Dxxxx
 * which caused UT99 to confuse the DD proxy with UGameEngine (their
 * addresses overlapped with kernel globals the PE32 code was reading). */
/* Proxy pointers live in DDRAW_PROCESS_STATE because their thunk addresses
 * are valid only in the process that created them. */

/* IDirectDraw7::EnumDisplayModes — report available display modes via callback.
 * Args (stdcall, including this): this, dwFlags, lpDDSurfaceDesc, lpContext, lpCallback
 * Callback: HRESULT CALLBACK(LPDDSURFACEDESC2 desc, LPVOID ctx)
 *   Returns DDENUMRET_OK (1) to continue, DDENUMRET_CANCEL (0) to stop. */
static uint64_t WINAPI dd_EnumDisplayModes(
    uint64_t _this, uint64_t dwFlags, uint64_t lpDesc,
    uint64_t lpContext, uint64_t lpCallback)
{
    (void)dwFlags;
    IDirectDraw7 *self = (IDirectDraw7 *)(ULONG_PTR)(uint32_t)_this;
    DDRAW_PROCESS_STATE *state = ddraw_state_from_object(self);
    if (!state) return DDERR_INVALIDPARAMS;
    uint8_t version = ddraw_interface_version(self);
    uint32_t descriptor_size = ddraw_descriptor_size_for_version(version);

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

    uint32_t resolution_count = user32_get_display_resolution_count
        ? user32_get_display_resolution_count() : 0;
    if (!resolution_count && state && state->display_width &&
        state->display_height)
        resolution_count = 1;

    /* The descriptor is dereferenced by a PE32 callback. Allocate it in the
     * process address space instead of truncating a higher-half kernel static. */
    uint8_t *desc_buf = (uint8_t *)VirtualAlloc(NULL, 128,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!desc_buf || (ULONG_PTR)desc_buf > UINT32_MAX) {
        if (desc_buf) VirtualFree(desc_buf, 0, MEM_RELEASE);
        return E_OUTOFMEMORY;
    }

    static const uint8_t depths[] = { 8, 16, 32 };
    BOOL keep_enumerating = TRUE;
    for (uint32_t ri = 0; ri < resolution_count && keep_enumerating; ri++) {
        uint32_t width = 0, height = 0;
        BOOL have_resolution = user32_get_display_resolution
            ? user32_get_display_resolution(ri, &width, &height) : FALSE;
        if (!have_resolution && ri == 0 && state) {
            width = state->display_width;
            height = state->display_height;
            have_resolution = width && height;
        }
        if (!have_resolution) continue;
        if (f_w && width != f_w) continue;
        if (f_h && height != f_h) continue;

        for (uint32_t bi = 0; bi < sizeof(depths); bi++) {
            uint32_t bpp = depths[bi];
            if (f_bpp && bpp != f_bpp) continue;

            for (int j = 0; j < 128; j++) desc_buf[j] = 0;
            uint32_t *d = (uint32_t *)desc_buf;
            d[0]  = descriptor_size;      /* dwSize */
            d[1]  = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH |
                    DDSD_PIXELFORMAT | 0x00040000; /* DDSD_REFRESHRATE */
            d[2]  = height;
            d[3]  = width;
            d[4]  = width * (bpp / 8);
            d[5]  = 60;
            fill_pixfmt32((uint32_t *)(desc_buf + 72), bpp);

            uint32_t args[2] = {
                (uint32_t)(uintptr_t)desc_buf,
                (uint32_t)lpContext
            };
            if (compat32_callback_args((uint32_t)lpCallback, 2, args) == 0) {
                keep_enumerating = FALSE;
                break;
            }
        }
    }

    VirtualFree(desc_buf, 0, MEM_RELEASE);

    return 0; /* DD_OK */
}

/* Unsupported methods must fail explicitly. Returning success without filling
 * output parameters makes callers consume uninitialized state. */
static HRESULT WINAPI dd_com_stub(PVOID this_ptr)
{
    (void)this_ptr;
    return E_NOTIMPL;
}

static BOOL ddraw_init_com32(DDRAW_PROCESS_STATE *state)
{
    if (!state) return FALSE;
    uint32_t proxy_state = __atomic_load_n(&state->proxy_state,
                                           __ATOMIC_ACQUIRE);
    if (proxy_state == 1) return TRUE;
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&state->proxy_state, &expected, 2,
                                     FALSE, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        while ((proxy_state = __atomic_load_n(&state->proxy_state,
                                               __ATOMIC_ACQUIRE)) == 2)
            __asm__ volatile ("pause");
        return proxy_state == 1;
    }

    extern uint32_t compat32_make_thunk_ex(uint64_t target, const char *name,
                                            uint8_t num_args, uint8_t callconv);
    /* CC_STDCALL comes from compat32.h (== 0). Do NOT redefine it here:
     * a local `#define CC_STDCALL 1` collides with CC_CDECL (==1), so
     * emit_thunk() would emit `ret` (cdecl, no cleanup) instead of
     * `ret N`. The ddraw COM methods are stdcall (callee cleans), and
     * the leaked args drift ESP → corrupt the caller's saved ESI →
     * NULL virtual call in WinDrv ResizeViewport (no frame presents). */

    /* One PE32 page holds complete vtables plus versioned interface objects.
     * IDirectDraw v1 needs its own vtable because SetDisplayMode has three
     * parameters there and five parameters in every later generation. */
    enum {
        DD_VTBL1_OFFSET = 0,
        DD_VTBL_OFFSET = 96,
        DD_PROXY_OFFSET = 224,
        SURF_VTBL_OFFSET = 272,
        SURF_PROXY_OFFSET = 480,
        PAL_VTBL_OFFSET = 800,
        PAL_PROXY_OFFSET = 832,
        CLIP_VTBL_OFFSET = 864,
        CLIP_PROXY_OFFSET = 904
    };
    uint8_t *page = (uint8_t *)VirtualAlloc(NULL, 4096,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!page || (ULONG_PTR)page > UINT32_MAX) {
        if (page) VirtualFree(page, 0, MEM_RELEASE);
        __atomic_store_n(&state->proxy_state, 0, __ATOMIC_RELEASE);
        serial_puts("[DDRAW] COM proxy alloc FAILED\n");
        return FALSE;
    }
    for (int i = 0; i < 4096; i++) page[i] = 0;

    uint32_t *dd_vtbl1_32 = (uint32_t *)(page + DD_VTBL1_OFFSET);
    uint32_t *dd_vtbl32 = (uint32_t *)(page + DD_VTBL_OFFSET);
    COM32_DirectDraw *dd_proxy32 =
        (COM32_DirectDraw *)(page + DD_PROXY_OFFSET);
    uint32_t *surf_vtbl32 = (uint32_t *)(page + SURF_VTBL_OFFSET);
    COM32_Surface *surf_proxy32 =
        (COM32_Surface *)(page + SURF_PROXY_OFFSET);
    uint32_t *pal_vtbl32 = (uint32_t *)(page + PAL_VTBL_OFFSET);
    COM32_Palette *pal_proxy32 =
        (COM32_Palette *)(page + PAL_PROXY_OFFSET);
    uint32_t *clip_vtbl32 = (uint32_t *)(page + CLIP_VTBL_OFFSET);
    COM32_Clipper *clip_proxy32 =
        (COM32_Clipper *)(page + CLIP_PROXY_OFFSET);

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
    dd_vtbl32[3]  = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_Compact,
                                            "DD_Compact", 1, CC_STDCALL);
    dd_vtbl32[4]  = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_CreateClipper,
                                            "DD_CreateClipper", 4, CC_STDCALL);
    dd_vtbl32[5]  = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_CreatePalette,
                                            "DD_CreatePalette", 5, CC_STDCALL);
    dd_vtbl32[6]  = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_CreateSurface,
                                            "DD_CreateSurface", 4, CC_STDCALL);
    dd_vtbl32[8]  = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_EnumDisplayModes,
                                            "DD_EnumDisplayModes", 5, CC_STDCALL);
    dd_vtbl32[10] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_FlipToGDISurface,
                                            "DD_FlipToGDI", 1, CC_STDCALL);
    dd_vtbl32[11] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_GetCaps,
                                            "DD_GetCaps", 3, CC_STDCALL);
    dd_vtbl32[12] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_GetDisplayMode,
                                            "DD_GetDisplayMode", 2, CC_STDCALL);
    dd_vtbl32[13] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_GetFourCCCodes,
                                            "DD_GetFourCCCodes", 3, CC_STDCALL);
    dd_vtbl32[14] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_GetGDISurface,
                                            "DD_GetGDISurface", 2, CC_STDCALL);
    dd_vtbl32[15] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_GetMonitorFrequency,
                                            "DD_GetMonitorFrequency", 2, CC_STDCALL);
    dd_vtbl32[16] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_GetScanLine,
                                            "DD_GetScanLine", 2, CC_STDCALL);
    dd_vtbl32[17] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_GetVerticalBlankStatus,
                                            "DD_GetVBlankStatus", 2, CC_STDCALL);
    dd_vtbl32[18] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_Initialize,
                                            "DD_Initialize", 2, CC_STDCALL);
    dd_vtbl32[19] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_RestoreDisplayMode,
                                            "DD_RestoreDisplayMode", 1, CC_STDCALL);
    dd_vtbl32[20] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_SetCooperativeLevel,
                                            "DD_SetCoopLevel", 3, CC_STDCALL);
    dd_vtbl32[21] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_SetDisplayMode,
                                            "DD_SetDisplayMode", 6, CC_STDCALL);
    dd_vtbl32[22] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_WaitForVerticalBlank,
                                            "DD_WaitForVBlank", 3, CC_STDCALL);
    dd_vtbl32[23] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_GetAvailableVidMem,
                                            "DD_GetAvailableVidMem", 4, CC_STDCALL);
    dd_vtbl32[24] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_GetSurfaceFromDC,
                                            "DD_GetSurfaceFromDC", 3, CC_STDCALL);
    dd_vtbl32[25] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_RestoreAllSurfaces,
                                            "DD_RestoreAll", 1, CC_STDCALL);
    dd_vtbl32[26] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_TestCooperativeLevel,
                                            "DD_TestCoop", 1, CC_STDCALL);
    dd_vtbl32[27] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_GetDeviceIdentifier,
                                            "DD_GetDeviceId", 3, CC_STDCALL);
    dd_vtbl32[28] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_StartModeTest,
                                            "DD_StartModeTest", 4, CC_STDCALL);
    dd_vtbl32[29] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)dd_EvaluateMode,
                                            "DD_EvaluateMode", 3, CC_STDCALL);

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
    surf_vtbl32[13] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_GetBltStatus,
                                              "Surf_GetBltStatus", 2, CC_STDCALL);
    surf_vtbl32[14] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_GetCaps,
                                              "Surf_GetCaps", 2, CC_STDCALL);
    surf_vtbl32[21] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_GetPixelFormat,
                                              "Surf_GetPixelFormat", 2, CC_STDCALL);
    surf_vtbl32[22] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_GetSurfaceDesc,
                                              "Surf_GetDesc", 2, CC_STDCALL);
    surf_vtbl32[24] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_IsLost,
                                              "Surf_IsLost", 1, CC_STDCALL);
    surf_vtbl32[15] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_GetClipper,
                                              "Surf_GetClipper", 2, CC_STDCALL);
    surf_vtbl32[16] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_GetColorKey,
                                              "Surf_GetColorKey", 3, CC_STDCALL);
    surf_vtbl32[17] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_GetDC,
                                              "Surf_GetDC", 2, CC_STDCALL);
    surf_vtbl32[18] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_GetFlipStatus,
                                              "Surf_GetFlipStatus", 2, CC_STDCALL);
    surf_vtbl32[20] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_GetPalette,
                                              "Surf_GetPalette", 2, CC_STDCALL);
    surf_vtbl32[25] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_Lock,
                                              "Surf_Lock", 5, CC_STDCALL);
    surf_vtbl32[26] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_ReleaseDC,
                                              "Surf_ReleaseDC", 2, CC_STDCALL);
    surf_vtbl32[27] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_Restore,
                                              "Surf_Restore", 1, CC_STDCALL);
    surf_vtbl32[28] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_SetClipper,
                                              "Surf_SetClipper", 2, CC_STDCALL);
    surf_vtbl32[29] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_SetColorKey,
                                              "Surf_SetColorKey", 3, CC_STDCALL);
    surf_vtbl32[31] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_SetPalette,
                                              "Surf_SetPalette", 2, CC_STDCALL);
    surf_vtbl32[32] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_Unlock,
                                              "Surf_Unlock", 2, CC_STDCALL);
    surf_vtbl32[36] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_GetDDInterface,
                                              "Surf_GetDDInterface", 2, CC_STDCALL);
    surf_vtbl32[37] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_PageLock,
                                              "Surf_PageLock", 2, CC_STDCALL);
    surf_vtbl32[38] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_PageUnlock,
                                              "Surf_PageUnlock", 2, CC_STDCALL);
    surf_vtbl32[43] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_GetUniquenessValue,
                                              "Surf_GetUnique", 2, CC_STDCALL);
    surf_vtbl32[44] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_ChangeUniquenessValue,
                                              "Surf_ChangeUnique", 1, CC_STDCALL);
    surf_vtbl32[45] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_SetPriority,
                                              "Surf_SetPriority", 2, CC_STDCALL);
    surf_vtbl32[46] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_GetPriority,
                                              "Surf_GetPriority", 2, CC_STDCALL);
    surf_vtbl32[47] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_SetLOD,
                                              "Surf_SetLOD", 2, CC_STDCALL);
    surf_vtbl32[48] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)surf_GetLOD,
                                              "Surf_GetLOD", 2, CC_STDCALL);

    /* Fill unimplemented DD slots with failing stub thunks.
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
        for (int i = 0; i < 30; i++) {
            if (dd_vtbl32[i] == 0) {
                dd_vtbl32[i] = compat32_make_thunk_ex(
                    (uint64_t)(ULONG_PTR)dd_com_stub,
                    "DD_stub", ddraw_method_arg_count((unsigned)i),
                    CC_STDCALL);
            }
        }
    }

    for (int index = 0; index < 23; index++)
        dd_vtbl1_32[index] = dd_vtbl32[index];
    dd_vtbl1_32[21] = compat32_make_thunk_ex(
        (uint64_t)(ULONG_PTR)dd_SetDisplayMode_v1,
        "DD1_SetDisplayMode", DDRAW1_SETDISPLAYMODE_ARG_COUNT, CC_STDCALL);

    /* Fill unimplemented Surface slots with correct arg counts */
    {
        /* IDirectDrawSurface arg counts (including 'this'). Corrected against
         * the real IDirectDrawSurface vtable — several were short, which makes
         * an unimplemented-slot stub RET too few bytes and imbalance the caller
         * (the engine's SetRes Blt-clears the surfaces during fullscreen
         * ResizeViewport). */
        for (int i = 0; i < 49; i++) {
            if (surf_vtbl32[i] == 0) {
                surf_vtbl32[i] = compat32_make_thunk_ex(
                    (uint64_t)(ULONG_PTR)dd_com_stub,
                    "Surf_stub",
                    ddraw_surface_method_arg_count((unsigned)i),
                    CC_STDCALL);
            }
        }
    }

    /* Each interface pointer records its generation while sharing state. */
    for (int interface_index = 0;
         interface_index < DDRAW_INTERFACE_COUNT; interface_index++) {
        uint8_t version = dd_interface_versions[interface_index];
        dd_proxy32[interface_index].lpVtbl32 = (uint32_t)(ULONG_PTR)
            (version == DDRAW_IFACE_1 ? dd_vtbl1_32 : dd_vtbl32);
        dd_proxy32[interface_index].interface_version = version;
    }

    /* Setup surface proxies */
    for (int surface_index = 0; surface_index < MAX_SURFACES; surface_index++) {
        for (int interface_index = 0;
             interface_index < DDRAW_INTERFACE_COUNT; interface_index++) {
            COM32_Surface *proxy = &surf_proxy32[
                surface_index * DDRAW_INTERFACE_COUNT + interface_index];
            proxy->lpVtbl32 = (uint32_t)(ULONG_PTR)surf_vtbl32;
            proxy->surf_index = (uint16_t)surface_index;
            proxy->interface_version =
                dd_interface_versions[interface_index];
        }
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
    for (uint32_t i = 0; i < MAX_PALETTES; i++) {
        pal_proxy32[i].lpVtbl32 = (uint32_t)(ULONG_PTR)pal_vtbl32;
        pal_proxy32[i].palette_index = i;
    }

    /* IDirectDrawClipper COM32 vtable (9 methods) */
    clip_vtbl32[0] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)clip_QueryInterface,
                                             "Clip_QI", 3, CC_STDCALL);
    clip_vtbl32[1] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)clip_AddRef,
                                             "Clip_AddRef", 1, CC_STDCALL);
    clip_vtbl32[2] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)clip_Release,
                                             "Clip_Release", 1, CC_STDCALL);
    clip_vtbl32[3] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)clip_GetClipList,
                                             "Clip_GetClipList", 4, CC_STDCALL);
    clip_vtbl32[4] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)clip_GetHWnd,
                                             "Clip_GetHWnd", 2, CC_STDCALL);
    clip_vtbl32[5] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)clip_Initialize,
                                             "Clip_Initialize", 3, CC_STDCALL);
    clip_vtbl32[6] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)clip_IsClipListChanged,
                                             "Clip_IsChanged", 2, CC_STDCALL);
    clip_vtbl32[7] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)clip_SetClipList,
                                             "Clip_SetClipList", 3, CC_STDCALL);
    clip_vtbl32[8] = compat32_make_thunk_ex((uint64_t)(ULONG_PTR)clip_SetHWnd,
                                             "Clip_SetHWnd", 3, CC_STDCALL);
    for (uint32_t i = 0; i < MAX_CLIPPERS; i++) {
        clip_proxy32[i].lpVtbl32 = (uint32_t)(ULONG_PTR)clip_vtbl32;
        clip_proxy32[i].clipper_index = i;
    }

    for (int i = 0; i < 23; i++) {
        if (!dd_vtbl1_32[i]) goto proxy_failure;
    }
    for (int i = 0; i < 30; i++) {
        if (!dd_vtbl32[i]) goto proxy_failure;
    }
    for (int i = 0; i < 49; i++) {
        if (!surf_vtbl32[i]) goto proxy_failure;
    }
    for (int i = 0; i < 7; i++) {
        if (!pal_vtbl32[i]) goto proxy_failure;
    }
    for (int i = 0; i < 9; i++) {
        if (!clip_vtbl32[i]) goto proxy_failure;
    }

    state->com32_page = page;
    state->dd_vtbl1_32 = dd_vtbl1_32;
    state->dd_vtbl32 = dd_vtbl32;
    state->dd_proxies = dd_proxy32;
    state->surf_vtbl32 = surf_vtbl32;
    state->surface_proxies = surf_proxy32;
    state->palette_vtbl32 = pal_vtbl32;
    state->palette_proxies = pal_proxy32;
    state->clipper_vtbl32 = clip_vtbl32;
    state->clipper_proxies = clip_proxy32;
    __atomic_store_n(&state->proxy_state, 1, __ATOMIC_RELEASE);
    serial_puts("[DDRAW] COM32 proxies initialized (surfaces+palette+clipper)\n");
    return TRUE;

proxy_failure:
    VirtualFree(page, 0, MEM_RELEASE);
    __atomic_store_n(&state->proxy_state, 0, __ATOMIC_RELEASE);
    serial_puts("[DDRAW] COM32 thunk allocation FAILED\n");
    return FALSE;
}

static void ddraw_selftest_expect(int condition, const char *name,
                                  int *failures)
{
    if (condition) return;
    serial_puts("[DDRAW-TEST] FAIL: ");
    serial_puts(name);
    serial_puts("\n");
    (*failures)++;
}

int ddraw_selftest(void)
{
    static const GUID unknown_iid = {
        0xDEADBEEF, 0x1357, 0x2468,
        { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80 }
    };
    int failures = 0;

    ddraw_selftest_expect(
        dd_version_from_iid(&dd_iid_directdraw) == DDRAW_IFACE_1,
        "IDirectDraw IID", &failures);
    ddraw_selftest_expect(
        dd_version_from_iid(&dd_iid_directdraw2) == DDRAW_IFACE_2,
        "IDirectDraw2 IID", &failures);
    ddraw_selftest_expect(
        dd_version_from_iid(&dd_iid_directdraw3) == DDRAW_IFACE_3,
        "IDirectDraw3 IID", &failures);
    ddraw_selftest_expect(
        dd_version_from_iid(&dd_iid_directdraw4) == DDRAW_IFACE_4,
        "IDirectDraw4 IID", &failures);
    ddraw_selftest_expect(
        dd_version_from_iid(&dd_iid_directdraw7) == DDRAW_IFACE_7,
        "IDirectDraw7 IID", &failures);

    ddraw_selftest_expect(
        dd_surface_version_from_iid(&dd_iid_surface) == DDRAW_IFACE_1,
        "IDirectDrawSurface IID", &failures);
    ddraw_selftest_expect(
        dd_surface_version_from_iid(&dd_iid_surface2) == DDRAW_IFACE_2,
        "IDirectDrawSurface2 IID", &failures);
    ddraw_selftest_expect(
        dd_surface_version_from_iid(&dd_iid_surface3) == DDRAW_IFACE_3,
        "IDirectDrawSurface3 IID", &failures);
    ddraw_selftest_expect(
        dd_surface_version_from_iid(&dd_iid_surface4) == DDRAW_IFACE_4,
        "IDirectDrawSurface4 IID", &failures);
    ddraw_selftest_expect(
        dd_surface_version_from_iid(&dd_iid_surface7) == DDRAW_IFACE_7,
        "IDirectDrawSurface7 IID", &failures);
    ddraw_selftest_expect(dd_version_from_iid(&unknown_iid) == 0,
                          "unknown DirectDraw IID", &failures);
    ddraw_selftest_expect(dd_surface_version_from_iid(&unknown_iid) == 0,
                          "unknown surface IID", &failures);

    ddraw_selftest_expect(
        ddraw_descriptor_size_for_version(DDRAW_IFACE_1) == 108U &&
        ddraw_descriptor_size_for_version(DDRAW_IFACE_2) == 108U &&
        ddraw_descriptor_size_for_version(DDRAW_IFACE_3) == 108U,
        "DDSURFACEDESC size", &failures);
    ddraw_selftest_expect(
        ddraw_descriptor_size_for_version(DDRAW_IFACE_4) == 124U &&
        ddraw_descriptor_size_for_version(DDRAW_IFACE_7) == 124U,
        "DDSURFACEDESC2 size", &failures);
    ddraw_selftest_expect(
        ddraw_created_surface_version_for_interface(DDRAW_IFACE_1) ==
            DDRAW_IFACE_1 &&
        ddraw_created_surface_version_for_interface(DDRAW_IFACE_2) ==
            DDRAW_IFACE_1 &&
        ddraw_created_surface_version_for_interface(DDRAW_IFACE_3) ==
            DDRAW_IFACE_1 &&
        ddraw_created_surface_version_for_interface(DDRAW_IFACE_4) ==
            DDRAW_IFACE_4 &&
        ddraw_created_surface_version_for_interface(DDRAW_IFACE_7) ==
            DDRAW_IFACE_7,
        "created surface generation", &failures);

    ddraw_selftest_expect(
        ddraw_method_arg_count(19) == 1 &&
        ddraw_method_arg_count(21) == 6 &&
        DDRAW1_SETDISPLAYMODE_ARG_COUNT == 4,
        "IDirectDraw stdcall ABI", &failures);
    ddraw_selftest_expect(
        ddraw_surface_method_arg_count(5) == 6 &&
        ddraw_surface_method_arg_count(12) == 3 &&
        ddraw_surface_method_arg_count(25) == 5 &&
        ddraw_surface_method_arg_count(32) == 2 &&
        ddraw_surface_method_arg_count(48) == 2,
        "IDirectDrawSurface stdcall ABI", &failures);

    ddraw_selftest_expect(
        ddraw_vblank_delay_at(0, DDRAW_VBLANK_BEGIN_MS) == 16 &&
        ddraw_vblank_delay_at(16, DDRAW_VBLANK_BEGIN_MS) == 0 &&
        ddraw_vblank_delay_at(16, 0) == 1 &&
        ddraw_vblank_delay_at(5, 0) == 12 &&
        !ddraw_vblank_active_at(15) && ddraw_vblank_active_at(16),
        "vertical blank phase model", &failures);

    DWORD caps = ddraw_software_device_caps();
    ddraw_selftest_expect(
        (caps & (DDCAPS_GDI | DDCAPS_READSCANLINE | DDCAPS_VBI)) ==
            (DDCAPS_GDI | DDCAPS_READSCANLINE | DDCAPS_VBI) &&
        !(caps & DDCAPS_PALETTEVSYNC),
        "software device capabilities", &failures);

    uint32_t descriptor[31];
    DDRAW_SURFACE_REQUEST request;
    dd_memset(descriptor, 0, sizeof(descriptor));
    descriptor[0] = 124;
    descriptor[1] = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    descriptor[5] = 2;
    descriptor[26] = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP |
                     DDSCAPS_COMPLEX;
    HRESULT parse_status = ddraw_parse_surface_request(
        640, 480, 32, DDRAW_IFACE_7, descriptor, &request);
    ddraw_selftest_expect(
        parse_status == DD_OK && request.width == 640 &&
        request.height == 480 && request.bpp == 32 &&
        request.back_buffer_count == 2 &&
        (request.caps & (DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER |
                         DDSCAPS_FLIP | DDSCAPS_COMPLEX |
                         DDSCAPS_VIDEOMEMORY)) ==
            (DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER |
             DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_VIDEOMEMORY),
        "flip-chain surface request", &failures);

    descriptor[26] &= ~DDSCAPS_COMPLEX;
    ddraw_selftest_expect(
        ddraw_parse_surface_request(
            640, 480, 32, DDRAW_IFACE_7, descriptor, &request) ==
            DDERR_INVALIDCAPS,
        "flip chain requires complex caps", &failures);

    dd_memset(descriptor, 0, sizeof(descriptor));
    descriptor[0] = 124;
    descriptor[1] = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT |
                    DDSD_PIXELFORMAT;
    descriptor[2] = 200;
    descriptor[3] = 320;
    descriptor[18] = sizeof(DDPIXELFORMAT);
    descriptor[19] = DDPF_RGB;
    descriptor[21] = 16;
    descriptor[22] = 0xF800;
    descriptor[23] = 0x07E0;
    descriptor[24] = 0x001F;
    descriptor[26] = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    parse_status = ddraw_parse_surface_request(
        640, 480, 32, DDRAW_IFACE_7, descriptor, &request);
    ddraw_selftest_expect(
        parse_status == DD_OK && request.width == 320 &&
        request.height == 200 && request.bpp == 16 &&
        request.back_buffer_count == 0,
        "offscreen RGB565 surface request", &failures);

    descriptor[26] |= DDSCAPS_VIDEOMEMORY;
    ddraw_selftest_expect(
        ddraw_parse_surface_request(
            640, 480, 32, DDRAW_IFACE_7, descriptor, &request) ==
            DDERR_INVALIDCAPS,
        "exclusive surface memory class", &failures);
    ddraw_selftest_expect(
        ddraw_surface_caps_match(
            DDSCAPS_BACKBUFFER | DDSCAPS_FLIP | DDSCAPS_COMPLEX,
            DDSCAPS_BACKBUFFER | DDSCAPS_FLIP) &&
        !ddraw_surface_caps_match(DDSCAPS_FLIP, DDSCAPS_BACKBUFFER),
        "attached surface capability matching", &failures);

    IDirectDrawSurface7 chain[3];
    dd_memset(chain, 0, sizeof(chain));
    chain[0].surf.back_buffer = &chain[1];
    chain[1].surf.back_buffer = &chain[2];
    chain[0].surf.pixels = (BYTE *)(ULONG_PTR)1;
    chain[1].surf.pixels = (BYTE *)(ULONG_PTR)2;
    chain[2].surf.pixels = (BYTE *)(ULONG_PTR)3;
    ddraw_rotate_surface_chain_once(&chain[0]);
    ddraw_selftest_expect(
        chain[0].surf.pixels == (BYTE *)(ULONG_PTR)2 &&
        chain[1].surf.pixels == (BYTE *)(ULONG_PTR)3 &&
        chain[2].surf.pixels == (BYTE *)(ULONG_PTR)1,
        "three-buffer flip rotation", &failures);

    serial_puts("[DDRAW-TEST] ");
    serial_puts(failures ? "FAIL count=" : "PASS count=");
    serial_putdec((uint64_t)failures);
    serial_puts("\n");
    return failures;
}

HRESULT WINAPI DirectDrawCreate(LPGUID lpGUID, PVOID *lplpDD, PVOID pUnkOuter)
{
    (void)lpGUID;
    serial_puts("[DDRAW] DirectDrawCreate\n");
    if (!lplpDD || pUnkOuter) return DDERR_INVALIDPARAMS;
    *(uint32_t *)lplpDD = 0;

    DDRAW_PROCESS_STATE *state = ddraw_current_state(TRUE);
    if (!state || !ddraw_init_com32(state)) return E_OUTOFMEMORY;

    COM32_DirectDraw *proxy =
        ddraw_proxy_for_version(state, DDRAW_IFACE_1);
    if (!proxy) return E_OUTOFMEMORY;
    __sync_add_and_fetch(&state->refs, 1);
    *(uint32_t *)lplpDD = (uint32_t)(ULONG_PTR)proxy;
    return DD_OK;
}

HRESULT WINAPI DirectDrawCreateEx(LPGUID lpGUID, PVOID *lplpDD,
                                   REFIID iid, PVOID pUnkOuter)
{
    (void)lpGUID;
    if (!lplpDD || pUnkOuter) return DDERR_INVALIDPARAMS;
    *(uint32_t *)lplpDD = 0;

    uint8_t version = dd_version_from_iid(iid);
    if (!version) return E_NOINTERFACE;
    DDRAW_PROCESS_STATE *state = ddraw_current_state(TRUE);
    if (!state || !ddraw_init_com32(state)) return E_OUTOFMEMORY;
    COM32_DirectDraw *proxy = ddraw_proxy_for_version(state, version);
    if (!proxy) return E_NOINTERFACE;
    __sync_add_and_fetch(&state->refs, 1);
    *(uint32_t *)lplpDD = (uint32_t)(ULONG_PTR)proxy;
    serial_puts("[DDRAW] DirectDrawCreateEx v");
    serial_putdec(version);
    serial_puts("\n");
    return DD_OK;
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
    (void)ordinal;
    if (by_ordinal) return NULL;
    for (int i = 0; ddraw_exports[i].name; i++) {
        if (dd_strcmp(func_name, ddraw_exports[i].name) == 0)
            return ddraw_exports[i].func;
    }
    return NULL;
}

void ddraw_release_process(DWORD process_id)
{
    if (!process_id) return;

    DDRAW_PROCESS_STATE *state = NULL;
    ddraw_lock();
    for (int i = 0; i < DDRAW_PROCESS_SLOTS; i++) {
        if (__atomic_load_n(&ddraw_processes[i].used,
                            __ATOMIC_RELAXED) == 1 &&
            ddraw_processes[i].owner_pid == process_id) {
            state = &ddraw_processes[i];
            __atomic_store_n(&state->used, 2, __ATOMIC_RELEASE);
            break;
        }
    }
    ddraw_unlock();
    if (!state) return;

    PVOID allocations[MAX_SURFACES + 1];
    uint32_t allocation_count = 0;
    for (int i = 0; i < MAX_SURFACES; i++) {
        DDSurface *surface = &state->surfaces[i].surf;
        if (surface->allocation_base)
            allocations[allocation_count++] = surface->allocation_base;
    }
    if (state->com32_page)
        allocations[allocation_count++] = state->com32_page;
    state->present_surface = NULL;
    if (g_present_owner_pid == process_id) {
        g_present_src_w = 0;
        g_present_src_h = 0;
        g_present_owner_pid = 0;
    }

    for (uint32_t i = 0; i < allocation_count; i++)
        (void)nt_vm_release_allocation_for_process(process_id,
                                                   allocations[i]);

    serial_puts("[DDRAW] released pid=");
    serial_putdec(process_id);
    serial_puts(" mappings=");
    serial_putdec(allocation_count);
    serial_puts("\n");

    ddraw_lock();
    dd_memset((BYTE *)state + sizeof(state->used), 0,
              sizeof(*state) - sizeof(state->used));
    __atomic_store_n(&state->used, 0, __ATOMIC_RELEASE);
    ddraw_unlock();
}

PVOID ddraw_shim_init(void)
{
    DWORD stale_pids[DDRAW_PROCESS_SLOTS];
    uint32_t stale_count = 0;
    for (int i = 0; i < DDRAW_PROCESS_SLOTS; i++) {
        if (__atomic_load_n(&ddraw_processes[i].used,
                            __ATOMIC_ACQUIRE) == 1)
            stale_pids[stale_count++] = ddraw_processes[i].owner_pid;
    }
    for (uint32_t i = 0; i < stale_count; i++)
        ddraw_release_process(stale_pids[i]);

    g_present_src_w = 0;
    g_present_src_h = 0;
    g_present_owner_pid = 0;
    framebuffer = NULL;
    fb_size = 0;
    gop_pitch = 0;
    return (PVOID)ddraw_exports;
}
