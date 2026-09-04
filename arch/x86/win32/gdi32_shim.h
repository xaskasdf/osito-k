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

typedef struct {
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;
} GDI_RECT;

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
int   WINAPI OffsetRgn(HGDIOBJ rgn, int x, int y);
int   WINAPI SelectClipRgn(HDC hdc, HGDIOBJ rgn);
UINT  WINAPI SetTextAlign(HDC hdc, UINT align);
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
BOOL  WINAPI LineTo(HDC hdc, int x, int y);

/* Text */
DWORD WINAPI SetTextColor(HDC hdc, DWORD color);
DWORD WINAPI SetBkColor(HDC hdc, DWORD color);
int   WINAPI SetBkMode(HDC hdc, int mode);
BOOL  WINAPI TextOutW(HDC hdc, int x, int y, PCWSTR lpString, int c);
BOOL  WINAPI ExtTextOutA(HDC hdc, int x, int y, UINT options,
                         PVOID lprect, PCSTR lpString, UINT c, PVOID lpDx);
BOOL  WINAPI GetTextExtentPoint32A(HDC hdc, PCSTR lpString, int c, PVOID lpSize);
BOOL  WINAPI GetTextExtentPoint32W(HDC hdc, PCWSTR lpString, int c, PVOID lpSize);

/* Screen DC helpers (for user32 GetDC/ReleaseDC) */
HDC   gdi32_alloc_screen_dc(void);
HDC   gdi32_alloc_window_dc(HANDLE window);
HANDLE gdi32_window_from_dc(HDC hdc);
void  gdi32_free_screen_dc(HDC hdc);

/* Shim */
PVOID gdi32_shim_init(void);
PVOID gdi32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal);
void  gdi32_release_process(DWORD process_id);
int   gdi32_dwrite_selftest(void);
int   gdi32_dib_selftest(void);
int   gdi32_region_selftest(void);

#endif /* GDI32_SHIM_H */
