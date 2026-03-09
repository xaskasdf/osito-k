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

/* Device caps */
#define HORZRES         8
#define VERTRES         10
#define BITSPIXEL       12
#define PLANES          14
#define RASTERCAPS      38
#define TECHNOLOGY      2

/* GDI API */
int   WINAPI GetDeviceCaps(HDC hdc, int index);
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
BOOL  WINAPI DeleteObject(HGDIOBJ ho);
int   WINAPI GetObjectA(HGDIOBJ h, int c, PVOID pv);

/* GDI object creation */
HDC        WINAPI CreateCompatibleDC(HDC hdc);
HBRUSH_GDI WINAPI CreateSolidBrush(DWORD color);
HBRUSH_GDI WINAPI CreatePatternBrush(HBITMAP hBitmap);
HPEN       WINAPI CreatePen(int iStyle, int cWidth, DWORD color);
HBITMAP    WINAPI CreateBitmap(int nWidth, int nHeight, UINT nPlanes,
                               UINT nBitCount, PVOID lpBits);
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

/* Shim */
PVOID gdi32_shim_init(void);
PVOID gdi32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal);

#endif /* GDI32_SHIM_H */
