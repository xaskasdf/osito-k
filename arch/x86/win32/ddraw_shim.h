/*
 * OsitoK Windows Compatibility Layer — ddraw.dll Shim
 *
 * DirectDraw 7 COM interface → OsitoK framebuffer.
 * UT99's SoftDrv.dll creates a primary surface, locks it,
 * writes RGB565 pixels, then blits to the framebuffer.
 *
 * We implement the minimum COM vtable surface:
 *   DirectDrawCreate → IDirectDraw7 → CreateSurface → IDirectDrawSurface7
 *   Lock/Unlock/Blt/Flip
 */

#ifndef DDRAW_SHIM_H
#define DDRAW_SHIM_H

#include "nttypes.h"

/* ── COM base types ────────────────────────────────────────── */

typedef LONG HRESULT;

#define S_OK            ((HRESULT)0)
#define S_FALSE         ((HRESULT)1)
#define E_NOINTERFACE   ((HRESULT)0x80004002)
#define E_INVALIDARG    ((HRESULT)0x80070057)
#define E_OUTOFMEMORY   ((HRESULT)0x8007000E)
#define E_FAIL          ((HRESULT)0x80004005)
#define E_NOTIMPL       ((HRESULT)0x80004001)

#define DD_OK           S_OK
#define DDERR_GENERIC           ((HRESULT)0x80004005)
#define DDERR_INVALIDPARAMS     ((HRESULT)0x80070057)
#define DDERR_SURFACELOST       ((HRESULT)0x887601C2)
#define DDERR_WASSTILLDRAWING   ((HRESULT)0x8876021C)
#define DDERR_NOTFOUND          ((HRESULT)0x887601CC)

/* GUID defined in nttypes.h */

/* ── DirectDraw constants ──────────────────────────────────── */

/* SetCooperativeLevel flags */
#define DDSCL_FULLSCREEN        0x00000001
#define DDSCL_EXCLUSIVE         0x00000010
#define DDSCL_NORMAL            0x00000008

/* Surface description flags */
#define DDSD_CAPS               0x00000001
#define DDSD_HEIGHT             0x00000002
#define DDSD_WIDTH              0x00000004
#define DDSD_PITCH              0x00000010
#define DDSD_PIXELFORMAT        0x00001000
#define DDSD_BACKBUFFERCOUNT    0x00000020
#define DDSD_LPSURFACE          0x00000800

/* Surface caps */
#define DDSCAPS_PRIMARYSURFACE  0x00000200
#define DDSCAPS_BACKBUFFER      0x00000004
#define DDSCAPS_FLIP            0x00000010
#define DDSCAPS_OFFSCREENPLAIN  0x00000040
#define DDSCAPS_COMPLEX         0x00000008
#define DDSCAPS_VIDEOMEMORY     0x00004000
#define DDSCAPS_SYSTEMMEMORY    0x00000800

/* Lock flags */
#define DDLOCK_WAIT             0x00000001
#define DDLOCK_SURFACEMEMORYPTR 0x00000000
#define DDLOCK_READONLY         0x00000010
#define DDLOCK_WRITEONLY        0x00000020

/* Pixel format flags */
#define DDPF_RGB                0x00000040

/* ── Structures ────────────────────────────────────────────── */

typedef struct _DDSCAPS2 {
    DWORD dwCaps;
    DWORD dwCaps2;
    DWORD dwCaps3;
    DWORD dwCaps4;
} DDSCAPS2;

typedef struct _DDPIXELFORMAT {
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwFourCC;
    DWORD dwRGBBitCount;
    DWORD dwRBitMask;
    DWORD dwGBitMask;
    DWORD dwBBitMask;
    DWORD dwRGBAlphaBitMask;
} DDPIXELFORMAT;

typedef struct _DDSURFACEDESC2 {
    DWORD           dwSize;
    DWORD           dwFlags;
    DWORD           dwHeight;
    DWORD           dwWidth;
    union {
        LONG        lPitch;
        DWORD       dwLinearSize;
    };
    DWORD           dwBackBufferCount;
    DWORD           dwMipMapCount;
    DWORD           dwAlphaBitDepth;
    DWORD           dwReserved;
    PVOID           lpSurface;
    DWORD           _unused1[2]; /* ddckCKDestOverlay */
    DWORD           _unused2[2]; /* ddckCKDestBlt */
    DWORD           _unused3[2]; /* ddckCKSrcOverlay */
    DWORD           _unused4[2]; /* ddckCKSrcBlt */
    DDPIXELFORMAT   ddpfPixelFormat;
    DDSCAPS2        ddsCaps;
    DWORD           dwTextureStage;
} DDSURFACEDESC2;

/* ── DirectDraw entry point ────────────────────────────────── */

/*
 * DirectDrawCreate — creates IDirectDraw object.
 * We return a pointer to our static vtable-based object.
 */
typedef HRESULT (WINAPI *LPDDENUMCALLBACKA)(GUID *, char *, char *, PVOID);

HRESULT WINAPI DirectDrawCreate(LPGUID lpGUID, PVOID *lplpDD, PVOID pUnkOuter);
HRESULT WINAPI DirectDrawCreateEx(LPGUID lpGUID, PVOID *lplpDD,
                                   REFIID iid, PVOID pUnkOuter);
HRESULT WINAPI DirectDrawEnumerateA(LPDDENUMCALLBACKA lpCallback, PVOID lpContext);

/* ── Shim ──────────────────────────────────────────────────── */

PVOID ddraw_shim_init(void);
PVOID ddraw_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal);

#endif /* DDRAW_SHIM_H */
