/*
 * OsitoK Windows Compatibility Layer — gdi32.dll Shim Implementation
 *
 * Reports framebuffer capabilities. UT99 uses DirectDraw, not GDI.
 */

#include "gdi32_shim.h"

extern void serial_puts(const char *s);

#define SCREEN_WIDTH  800
#define SCREEN_HEIGHT 600
#define SCREEN_BPP    32

int WINAPI GetDeviceCaps(HDC hdc, int index)
{
    (void)hdc;
    switch (index) {
    case HORZRES:    return SCREEN_WIDTH;
    case VERTRES:    return SCREEN_HEIGHT;
    case BITSPIXEL:  return SCREEN_BPP;
    case PLANES:     return 1;
    case RASTERCAPS: return 0;
    case TECHNOLOGY: return 1; /* DT_RASDISPLAY */
    default:         return 0;
    }
}

HDC WINAPI CreateDCA(PCSTR lpszDriver, PCSTR lpszDevice,
                     PCSTR lpszOutput, PCVOID lpInitData)
{
    (void)lpszDriver; (void)lpszDevice;
    (void)lpszOutput; (void)lpInitData;
    serial_puts("[GDI32] CreateDCA\n");
    return (HDC)(ULONG_PTR)0xDC000002;
}

BOOL WINAPI DeleteDC(HDC hdc)
{
    (void)hdc;
    return TRUE;
}

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
    /* In real OsitoK, this would flip the framebuffer */
    return TRUE;
}

HGDIOBJ WINAPI SelectObject(HDC hdc, HGDIOBJ h)
{
    (void)hdc;
    return h;
}

BOOL WINAPI DeleteObject(HGDIOBJ ho)
{
    (void)ho;
    return TRUE;
}

int WINAPI GetObjectA(HGDIOBJ h, int c, PVOID pv)
{
    (void)h; (void)c; (void)pv;
    return 0;
}

/* ── GDI object creation ─────────────────────────────────── */

HDC WINAPI CreateCompatibleDC(HDC hdc)
{
    (void)hdc;
    serial_puts("[GDI32] CreateCompatibleDC\n");
    return (HDC)(ULONG_PTR)0xDC000002;
}

HBRUSH_GDI WINAPI CreateSolidBrush(DWORD color)
{
    (void)color;
    return (HBRUSH_GDI)(ULONG_PTR)0xBB000001;
}

HBRUSH_GDI WINAPI CreatePatternBrush(HBITMAP hBitmap)
{
    (void)hBitmap;
    return (HBRUSH_GDI)(ULONG_PTR)0xBB000002;
}

HPEN WINAPI CreatePen(int iStyle, int cWidth, DWORD color)
{
    (void)iStyle; (void)cWidth; (void)color;
    return (HPEN)(ULONG_PTR)0xEE000001;
}

HBITMAP WINAPI CreateBitmap(int nWidth, int nHeight, UINT nPlanes,
                            UINT nBitCount, PVOID lpBits)
{
    (void)nWidth; (void)nHeight; (void)nPlanes;
    (void)nBitCount; (void)lpBits;
    return (HBITMAP)(ULONG_PTR)0xBB000003;
}

HBITMAP WINAPI CreateDIBitmap(HDC hdc, PVOID pbmih, DWORD flInit,
                              PVOID pjBits, PVOID pbmi, UINT iUsage)
{
    (void)hdc; (void)pbmih; (void)flInit;
    (void)pjBits; (void)pbmi; (void)iUsage;
    return (HBITMAP)(ULONG_PTR)0xBB000004;
}

HGDIOBJ WINAPI GetStockObject(int i)
{
    return (HGDIOBJ)(ULONG_PTR)(0xAA000000 + i);
}

/* ── Drawing ─────────────────────────────────────────────── */

BOOL WINAPI BitBlt(HDC hdcDest, int x, int y, int cx, int cy,
                   HDC hdcSrc, int x1, int y1, DWORD rop)
{
    (void)hdcDest; (void)x; (void)y; (void)cx; (void)cy;
    (void)hdcSrc; (void)x1; (void)y1; (void)rop;
    return TRUE;
}

BOOL WINAPI PatBlt(HDC hdc, int x, int y, int w, int h, DWORD rop)
{
    (void)hdc; (void)x; (void)y; (void)w; (void)h; (void)rop;
    return TRUE;
}

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

/* ── Text ────────────────────────────────────────────────── */

DWORD WINAPI SetTextColor(HDC hdc, DWORD color)
{
    (void)hdc; (void)color;
    return 0;
}

DWORD WINAPI SetBkColor(HDC hdc, DWORD color)
{
    (void)hdc; (void)color;
    return 0;
}

int WINAPI SetBkMode(HDC hdc, int mode)
{
    (void)hdc; (void)mode;
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

/* ── Export table ──────────────────────────────────────────── */

typedef struct { const char *name; PVOID func; } SHIM_EXPORT;

static const SHIM_EXPORT gdi32_exports[] = {
    { "GetDeviceCaps",       (PVOID)GetDeviceCaps },
    { "CreateDCA",           (PVOID)CreateDCA },
    { "DeleteDC",            (PVOID)DeleteDC },
    { "ChoosePixelFormat",   (PVOID)ChoosePixelFormat },
    { "SetPixelFormat",      (PVOID)SetPixelFormat },
    { "GetPixelFormat",      (PVOID)GetPixelFormat },
    { "DescribePixelFormat", (PVOID)DescribePixelFormat },
    { "SwapBuffers",         (PVOID)SwapBuffers },
    { "SelectObject",        (PVOID)SelectObject },
    { "DeleteObject",        (PVOID)DeleteObject },
    { "GetObjectA",          (PVOID)GetObjectA },
    /* GDI object creation */
    { "CreateCompatibleDC",       (PVOID)CreateCompatibleDC },
    { "CreateSolidBrush",         (PVOID)CreateSolidBrush },
    { "CreatePatternBrush",       (PVOID)CreatePatternBrush },
    { "CreatePen",                (PVOID)CreatePen },
    { "CreateBitmap",             (PVOID)CreateBitmap },
    { "CreateDIBitmap",           (PVOID)CreateDIBitmap },
    { "GetStockObject",           (PVOID)GetStockObject },
    /* Drawing */
    { "BitBlt",                   (PVOID)BitBlt },
    { "PatBlt",                   (PVOID)PatBlt },
    { "MoveToEx",                 (PVOID)MoveToEx },
    { "LineTo",                   (PVOID)LineTo },
    /* Text */
    { "SetTextColor",             (PVOID)SetTextColor },
    { "SetBkColor",               (PVOID)SetBkColor },
    { "SetBkMode",                (PVOID)SetBkMode },
    { "TextOutW",                 (PVOID)TextOutW },
    { "ExtTextOutA",              (PVOID)ExtTextOutA },
    { "GetTextExtentPoint32A",    (PVOID)GetTextExtentPoint32A },
    { "GetTextExtentPoint32W",    (PVOID)GetTextExtentPoint32W },
    { NULL, NULL }
};

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
