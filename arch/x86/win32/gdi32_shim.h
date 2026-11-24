/*
 * OsitoK Windows Compatibility Layer — gdi32.dll Shim
 *
 * Minimal GDI — enough for DirectDraw pixel format setup.
 * UT99 uses DirectDraw, not GDI, for rendering.
 */

#ifndef GDI32_SHIM_H
#define GDI32_SHIM_H

#include "nttypes.h"

typedef HANDLE HDC;
typedef HANDLE HBITMAP;
typedef HANDLE HFONT;
typedef HANDLE HPEN;
typedef HANDLE HBRUSH_GDI;
typedef HANDLE HPALETTE;
typedef HANDLE HGDIOBJ;
/* Bounded metadata lookup, no allocation/surface refresh. Caller serializes
 * against BSP DC destruction. Does not assert a KMT display mapping exists. */
BOOL gdi32_dc_display_name(HDC,WCHAR name[32]);

typedef struct {
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;
} GDI_RECT;

/* Small regions remain inline. Larger immutable snapshots share their
 * backing until a writer detaches it; no HRGN table entry is needed. */
#define GDI_MAX_REGION_RECTS 64
typedef struct GDI_REGION_STORAGE {
    UINT references, capacity;
    GDI_RECT rects[];
} GDI_REGION_STORAGE;
typedef struct GDI_REGION {
    int in_use;
    UINT count;
    GDI_REGION_STORAGE *storage;
    /* Native polygon extents may retain empty trailing scanlines. */
    int polygon_type;
    GDI_RECT polygon_bounds;
    GDI_RECT rects[GDI_MAX_REGION_RECTS];
} GDI_REGION;

/* All region values must be zero-initialized before assignment and cleared
 * at the end of their lifetime. Borrowed array pointers expire on mutation. */
#define GDI_REGION_RECTS(r) ((r)->storage ? (r)->storage->rects : (r)->rects)
#define GDI_REGION_SCOPE(name) \
    GDI_REGION name __attribute__((cleanup(gdi32_region_data_clear))) = {0}
void gdi32_region_data_clear(GDI_REGION *data);
void gdi32_region_data_retain(GDI_REGION *data);
void gdi32_region_data_assign(GDI_REGION *destination, const GDI_REGION *source);
void gdi32_region_data_move(GDI_REGION *destination, GDI_REGION *source);
BOOL gdi32_region_data_read(HGDIOBJ handle, GDI_REGION *data);
int  gdi32_region_data_write(HGDIOBJ handle, const GDI_REGION *data);
int  gdi32_region_data_combine(GDI_REGION *destination,
                                const GDI_REGION *first,
                                const GDI_REGION *second, int mode);
int  gdi32_region_data_bounds(const GDI_REGION *data, GDI_RECT *bounds);

typedef struct {
    LONG height, ascent, descent, internal_leading, external_leading;
    LONG average_width, maximum_width, weight, overhang, aspect_x, aspect_y;
    WCHAR first_char, last_char, default_char, break_char;
    BYTE italic, underlined, struck_out, pitch_and_family, charset;
} GDI_TEXTMETRICW;

/* Pixel format descriptor */
typedef struct tagPIXELFORMATDESCRIPTOR {
    WORD  nSize;
    WORD  nVersion;
    DWORD dwFlags;
    BYTE  iPixelType;
    BYTE  cColorBits;
    BYTE  cRedBits, cRedShift;
    BYTE  cGreenBits, cGreenShift;
    BYTE  cBlueBits, cBlueShift;
    BYTE  cAlphaBits, cAlphaShift;
    BYTE  cAccumBits;
    BYTE  cAccumRedBits, cAccumGreenBits, cAccumBlueBits, cAccumAlphaBits;
    BYTE  cDepthBits;
    BYTE  cStencilBits;
    BYTE  cAuxBuffers;
    BYTE  iLayerType;
    BYTE  bReserved;
    DWORD dwLayerMask;
    DWORD dwVisibleMask;
    DWORD dwDamageMask;
} PIXELFORMATDESCRIPTOR, *LPPIXELFORMATDESCRIPTOR;

#define PFD_DOUBLEBUFFER   0x00000001U
#define PFD_DRAW_TO_WINDOW 0x00000004U
#define PFD_SUPPORT_GDI    0x00000010U
#define PFD_SUPPORT_OPENGL 0x00000020U
#define PFD_GENERIC_FORMAT  0x00000040U
#define PFD_GENERIC_ACCELERATED 0x00001000U
#define PFD_TYPE_RGBA      0
#define PFD_MAIN_PLANE     0

/* Device caps */
#define HORZRES         8
#define VERTRES         10
#define BITSPIXEL       12
#define PLANES          14
#define RASTERCAPS      38
#define TECHNOLOGY      2
#define LOGPIXELSX      88
#define LOGPIXELSY      90

/* GDI API */
#define GDI_RGN_ERROR         0
#define GDI_NULLREGION        1
#define GDI_SIMPLEREGION      2
#define GDI_COMPLEXREGION     3
#define GDI_RGN_AND           1
#define GDI_RGN_OR            2
#define GDI_RGN_XOR           3
#define GDI_RGN_DIFF          4
#define GDI_RGN_COPY          5
int   WINAPI GetDeviceCaps(HDC hdc, int index);
BOOL  WINAPI GetDeviceGammaRamp(HDC hdc, PVOID ramp);
BOOL  WINAPI SetDeviceGammaRamp(HDC hdc, PCVOID ramp);
HDC   WINAPI CreateDCA(PCSTR lpszDriver, PCSTR lpszDevice,
                       PCSTR lpszOutput, PCVOID lpInitData);
BOOL  WINAPI DeleteDC(HDC hdc);
int   WINAPI ChoosePixelFormat(HDC hdc, const PIXELFORMATDESCRIPTOR *ppfd);
BOOL  WINAPI SetPixelFormat(HDC hdc, int format, const PIXELFORMATDESCRIPTOR *ppfd);
int   WINAPI GetPixelFormat(HDC hdc);
int   WINAPI DescribePixelFormat(HDC hdc, int iPixelFormat, DWORD nBytes,
                                  LPPIXELFORMATDESCRIPTOR ppfd);
BOOL  WINAPI SwapBuffers(HDC hdc);
HGDIOBJ WINAPI SelectObject(HDC hdc, HGDIOBJ h);
HGDIOBJ WINAPI GetCurrentObject(HDC hdc, UINT type);
HGDIOBJ WINAPI CreateRectRgn(int left, int top, int right, int bottom);
HGDIOBJ WINAPI CreateRectRgnIndirect(const GDI_RECT *rect);
BOOL  WINAPI SetRectRgn(HGDIOBJ rgn, int left, int top, int right, int bottom);
int   WINAPI CombineRgn(HGDIOBJ dest, HGDIOBJ src1, HGDIOBJ src2, int mode);
BOOL  WINAPI EqualRgn(HGDIOBJ first, HGDIOBJ second);
BOOL  WINAPI PtInRegion(HGDIOBJ rgn, int x, int y);
BOOL  WINAPI RectInRegion(HGDIOBJ rgn, const GDI_RECT *rect);
int   WINAPI GetRgnBox(HGDIOBJ rgn, GDI_RECT *rect);
DWORD WINAPI GetRegionData(HGDIOBJ rgn, DWORD bytes, PVOID data);
int   WINAPI GetRandomRgn(HDC hdc, HGDIOBJ rgn, int kind);
int   WINAPI OffsetRgn(HGDIOBJ rgn, int x, int y);
int   WINAPI SelectClipRgn(HDC hdc, HGDIOBJ rgn);
UINT  WINAPI SetTextAlign(HDC hdc, UINT align);
UINT  WINAPI GetTextAlign(HDC hdc);
BOOL  WINAPI DeleteObject(HGDIOBJ ho);
int   WINAPI GetObjectA(HGDIOBJ h, int c, PVOID pv);

/* GDI object creation */
HDC        WINAPI CreateCompatibleDC(HDC hdc);
HBRUSH_GDI WINAPI CreateSolidBrush(DWORD color);
HBRUSH_GDI WINAPI CreatePatternBrush(HBITMAP hBitmap);
HPEN       WINAPI CreatePen(int iStyle, int cWidth, DWORD color);
HBITMAP    WINAPI CreateCompatibleBitmap(HDC hdc, int cx, int cy);
HBITMAP    WINAPI CreateBitmap(int nWidth, int nHeight, UINT nPlanes,
                               UINT nBitCount, PVOID lpBits);
HBITMAP    gdi32_clone_bitmap(HBITMAP bitmap);
typedef struct {
    HBITMAP mask, color;
    UINT width, height, source_width, source_height;
} GDI_ICON_IMAGE;
BOOL gdi32_decode_icon_dib(const BYTE *data, DWORD bytes, int width, int height,
                            BOOL monochrome, GDI_ICON_IMAGE *image);
BOOL       gdi32_draw_icon_bitmap(HDC hdc, HBITMAP color, HBITMAP mask,
                                  int x, int y, int width, int height);
int        WINAPI GetDIBits(HDC hdc, HBITMAP bitmap, UINT start_scan,
                            UINT scan_lines, PVOID bits, PVOID bitmap_info,
                            UINT usage);
HBITMAP    WINAPI CreateDIBitmap(HDC hdc, PVOID pbmih, DWORD flInit,
                                 PVOID pjBits, PVOID pbmi, UINT iUsage);
HGDIOBJ    WINAPI GetStockObject(int i);

/* Drawing */
BOOL  WINAPI BitBlt(HDC hdcDest, int x, int y, int cx, int cy,
                    HDC hdcSrc, int x1, int y1, DWORD rop);
BOOL  WINAPI PatBlt(HDC hdc, int x, int y, int w, int h, DWORD rop);
BOOL  WINAPI MoveToEx(HDC hdc, int x, int y, PVOID lppt);
BOOL  WINAPI GetCurrentPositionEx(HDC hdc, PVOID point);
BOOL  WINAPI LineTo(HDC hdc, int x, int y);

/* Text */
DWORD WINAPI SetTextColor(HDC hdc, DWORD color);
DWORD WINAPI GetTextColor(HDC hdc);
DWORD WINAPI SetBkColor(HDC hdc, DWORD color);
DWORD WINAPI GetBkColor(HDC hdc);
int   WINAPI SetBkMode(HDC hdc, int mode);
BOOL  WINAPI TextOutW(HDC hdc, int x, int y, PCWSTR lpString, int c);
BOOL  WINAPI ExtTextOutA(HDC hdc, int x, int y, UINT options,
                         PVOID lprect, PCSTR lpString, UINT c, PVOID lpDx);
BOOL  WINAPI ExtTextOutW(HDC hdc, int x, int y, UINT options,
                         const GDI_RECT *rect, PCWSTR text, UINT count,
                         const int *spacing);
BOOL  WINAPI GetTextMetricsW(HDC hdc, PVOID metrics);
BOOL  WINAPI GetTextExtentPoint32A(HDC hdc, PCSTR lpString, int c, PVOID lpSize);
BOOL  WINAPI GetTextExtentPoint32W(HDC hdc, PCWSTR lpString, int c, PVOID lpSize);

/* The current GDI font mapper renders 8x16 bitmap cells. This internal bridge
 * uses that same rasterizer, with DWORD-aligned, bottom-up monochrome rows. */
typedef struct {
    UINT width, height;
    int origin_x, origin_y, advance_x, advance_y;
    BYTE bits[64];
} GDI_FONT_BITMAP;
BOOL gdi32_rasterize_glyph(HDC hdc, WCHAR character, GDI_FONT_BITMAP *bitmap);

/* Screen DC helpers (for user32 GetDC/ReleaseDC) */
HDC   gdi32_alloc_screen_dc(void);
HDC   gdi32_alloc_window_dc(HANDLE window);
HDC   gdi32_alloc_nonclient_dc(HANDLE window);
HDC   gdi32_alloc_extended_dc(HANDLE window, BOOL whole,
                               HGDIOBJ visible, HGDIOBJ owned_clip,
                               HGDIOBJ device_clip, DWORD flags);
void  gdi32_release_thread_dc(DWORD pid, DWORD tid);
HANDLE gdi32_window_from_dc(HDC hdc);
int   gdi32_free_screen_dc(HDC hdc);
BOOL  gdi32_push_paint_clip(HDC hdc, const GDI_RECT *rect, uint64_t *token);
BOOL  gdi32_push_paint_region(HDC hdc, const GDI_REGION *region, uint64_t *token);
void  gdi32_pop_paint_clip(HDC hdc, uint64_t token, BOOL restore_previous);
void  gdi32_clear_window_coverage(HDC hdc);
BOOL  gdi32_copy_layered_pixels(HDC hdc, int x, int y, int width, int height,
                                 uint32_t *pixels, BOOL per_pixel);
void  gdi32_release_window_dc(HANDLE window);
BOOL  gdi32_fill_rect(HDC hdc, const GDI_RECT *rect, HGDIOBJ brush);
BOOL  gdi32_draw_focus_rect(HDC hdc, const GDI_RECT *rect);

/* Shim */
PVOID gdi32_shim_init(void);
PVOID gdi32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal);
void  gdi32_release_process(DWORD process_id);
int   gdi32_dwrite_selftest(void);
int   gdi32_dib_selftest(void);
int   gdi32_region_selftest(void);

#endif /* GDI32_SHIM_H */
