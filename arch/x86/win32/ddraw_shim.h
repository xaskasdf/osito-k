/*
 * OsitoK Windows Compatibility Layer — ddraw.dll Shim
 *
 * DirectDraw COM interfaces backed by OsitoK software surfaces and scanout.
 *
 * The core presentation path is shared by the legacy IDirectDraw interface
 * generations. Each exposed COM interface keeps its documented ABI while all
 * software surfaces use the same renderer and scanout backend.
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
#define DDERR_INVALIDCAPS       ((HRESULT)0x88760064)
#define DDERR_INVALIDPIXELFORMAT ((HRESULT)0x88760091)
#define DDERR_INVALIDMODE       ((HRESULT)0x88760078)
#define DDERR_INVALIDRECT       ((HRESULT)0x88760096)
#define DDERR_NOCOLORKEY        ((HRESULT)0x887600D7)
#define DDERR_NOEXCLUSIVEMODE   ((HRESULT)0x887600E1)
#define DDERR_NOCOOPERATIVELEVELSET ((HRESULT)0x887600D4)
#define DDERR_SURFACEBUSY       ((HRESULT)0x887601AE)
#define DDERR_SURFACELOST       ((HRESULT)0x887601C2)
#define DDERR_WASSTILLDRAWING   ((HRESULT)0x8876021C)
#define DDERR_REGIONTOOSMALL    ((HRESULT)0x88760236)
#define DDERR_CLIPPERISUSINGHWND ((HRESULT)0x88760237)
#define DDERR_NOTLOCKED         ((HRESULT)0x88760248)
#define DDERR_NOTFLIPPABLE      ((HRESULT)0x88760246)
#define DDERR_PRIMARYSURFACEALREADYEXISTS ((HRESULT)0x88760234)
#define DDERR_OUTOFVIDEOMEMORY  ((HRESULT)0x8876017C)
#define DDERR_NOTFOUND              ((HRESULT)0x887601CC)
#define DDERR_NOPALETTEATTACHED     ((HRESULT)0x887601C8)
#define DDERR_NOCLIPPERATTACHED     ((HRESULT)0x887601C0)
#define DDERR_UNSUPPORTED       E_NOTIMPL

/* GUID defined in nttypes.h */

/* ── DirectDraw constants ──────────────────────────────────── */

/* SetCooperativeLevel flags */
#define DDSCL_FULLSCREEN        0x00000001
#define DDSCL_ALLOWREBOOT       0x00000002
#define DDSCL_NOWINDOWCHANGES   0x00000004
#define DDSCL_NORMAL            0x00000008
#define DDSCL_EXCLUSIVE         0x00000010
#define DDSCL_ALLOWMODEX        0x00000040
#define DDSCL_SETFOCUSWINDOW    0x00000080
#define DDSCL_SETDEVICEWINDOW   0x00000100
#define DDSCL_CREATEDEVICEWINDOW 0x00000200
#define DDSCL_MULTITHREADED     0x00000400
#define DDSCL_FPUSETUP          0x00000800
#define DDSCL_FPUPRESERVE       0x00001000

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
#define DDSCAPS_FRONTBUFFER     0x00000020
#define DDSCAPS_FLIP            0x00000010
#define DDSCAPS_OFFSCREENPLAIN  0x00000040
#define DDSCAPS_COMPLEX         0x00000008
#define DDSCAPS_VIDEOMEMORY     0x00004000
#define DDSCAPS_SYSTEMMEMORY    0x00000800

/* Device capabilities */
#define DDCAPS_BLT              0x00000040
#define DDCAPS_BLTSTRETCH       0x00000200
#define DDCAPS_GDI              0x00000400
#define DDCAPS_PALETTE          0x00008000
#define DDCAPS_PALETTEVSYNC     0x00010000
#define DDCAPS_READSCANLINE     0x00020000
#define DDCAPS_VBI              0x00080000
#define DDCAPS_COLORKEY         0x00400000
#define DDCAPS_BLTCOLORFILL     0x04000000
#define DDCAPS_CANBLTSYSMEM     0x80000000

/* Flip and vertical-blank flags */
#define DDFLIP_WAIT             0x00000001
#define DDFLIP_NOVSYNC          0x00000008
#define DDFLIP_DONOTWAIT        0x00000020
#define DDWAITVB_BLOCKBEGIN     0x00000001
#define DDWAITVB_BLOCKBEGINEVENT 0x00000002
#define DDWAITVB_BLOCKEND       0x00000004

/* Lock flags */
#define DDLOCK_WAIT             0x00000001
#define DDLOCK_SURFACEMEMORYPTR 0x00000000
#define DDLOCK_READONLY         0x00000010
#define DDLOCK_WRITEONLY        0x00000020

/* Blt and color-key flags */
#define DDBLT_COLORFILL         0x00000400
#define DDBLT_KEYSRC            0x00008000
#define DDBLTFAST_SRCCOLORKEY   0x00000001
#define DDCKEY_DESTBLT          0x00000002
#define DDCKEY_SRCBLT           0x00000008

/* Pixel format flags */
#define DDPF_PALETTEINDEXED8    0x00000020
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
void ddraw_release_process(DWORD process_id);
void ddraw_present_hook(void);
DWORD ddraw_present_poll_interval(void);
int ddraw_selftest(void);

#endif /* DDRAW_SHIM_H */
