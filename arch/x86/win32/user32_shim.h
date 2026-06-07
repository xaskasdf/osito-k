/*
 * OsitoK Windows Compatibility Layer — user32.dll Shim
 *
 * Window management, message queue, and input processing.
 * In OsitoK, all "windows" map to a single framebuffer.
 * UT99 creates one main window and processes messages through it.
 */

#ifndef USER32_SHIM_H
#define USER32_SHIM_H

#include "nttypes.h"

/* ── Window handle ─────────────────────────────────────────── */

typedef HANDLE HWND;
typedef HANDLE HMENU;
typedef HANDLE HINSTANCE;
typedef HANDLE HICON;
typedef HANDLE HCURSOR;
typedef HANDLE HBRUSH;
typedef HANDLE HDC;
typedef HANDLE HRGN;
typedef ULONG_PTR WPARAM;
typedef LONG_PTR  LPARAM;
typedef LONG_PTR  LRESULT;

typedef LRESULT (WINAPI *WNDPROC)(HWND, DWORD, WPARAM, LPARAM);

/* ── Window Messages ───────────────────────────────────────── */

#define WM_NULL             0x0000
#define WM_CREATE           0x0001
#define WM_DESTROY          0x0002
#define WM_MOVE             0x0003
#define WM_SIZE             0x0005
#define WM_ACTIVATE         0x0006
#define WM_SETFOCUS         0x0007
#define WM_KILLFOCUS        0x0008
#define WM_ENABLE           0x000A
#define WM_PAINT            0x000F
#define WM_CLOSE            0x0010
#define WM_QUIT             0x0012
#define WM_ERASEBKGND       0x0014
#define WM_SHOWWINDOW       0x0018
#define WM_ACTIVATEAPP      0x001C
#define WM_SETCURSOR        0x0020
#define WM_GETMINMAXINFO    0x0024
#define WM_WINDOWPOSCHANGING 0x0046
#define WM_WINDOWPOSCHANGED  0x0047
#define WM_NCCREATE         0x0081
#define WM_NCDESTROY        0x0082
#define WM_NCCALCSIZE       0x0083
#define WM_NCHITTEST        0x0084
#define WM_NCPAINT          0x0085
#define WM_NCACTIVATE       0x0086
#define WM_KEYDOWN          0x0100
#define WM_KEYUP            0x0101
#define WM_CHAR             0x0102
#define WM_SYSKEYDOWN       0x0104
#define WM_SYSKEYUP         0x0105
#define WM_SYSCOMMAND       0x0112
#define WM_TIMER            0x0113
#define WM_MOUSEMOVE        0x0200
#define WM_LBUTTONDOWN      0x0201
#define WM_LBUTTONUP        0x0202
#define WM_RBUTTONDOWN      0x0204
#define WM_RBUTTONUP        0x0205
#define WM_MBUTTONDOWN      0x0207
#define WM_MBUTTONUP        0x0208
#define WM_MOUSEWHEEL       0x020A
#define WM_USER             0x0400

/* Mouse key flags (wParam for mouse messages) */
#define MK_LBUTTON          0x0001
#define MK_RBUTTON          0x0002
#define MK_SHIFT            0x0004
#define MK_CONTROL          0x0008
#define MK_MBUTTON          0x0010

/* ShowWindow commands */
#define SW_HIDE             0
#define SW_SHOWNORMAL       1
#define SW_SHOW             5
#define SW_SHOWMAXIMIZED    3
#define SW_SHOWMINIMIZED    2

/* Window styles */
#define WS_OVERLAPPED       0x00000000
#define WS_POPUP            0x80000000
#define WS_CHILD            0x40000000
#define WS_MINIMIZE         0x20000000
#define WS_VISIBLE          0x10000000
#define WS_DISABLED         0x08000000
#define WS_CLIPSIBLINGS     0x04000000
#define WS_CLIPCHILDREN     0x02000000
#define WS_MAXIMIZE         0x01000000
#define WS_CAPTION          0x00C00000
#define WS_BORDER           0x00800000
#define WS_DLGFRAME         0x00400000
#define WS_VSCROLL          0x00200000
#define WS_HSCROLL          0x00100000
#define WS_SYSMENU          0x00080000
#define WS_THICKFRAME       0x00040000
#define WS_MINIMIZEBOX      0x00020000
#define WS_MAXIMIZEBOX      0x00010000
#define WS_OVERLAPPEDWINDOW (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | \
                             WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)

/* Extended window styles */
#define WS_EX_TOPMOST       0x00000008
#define WS_EX_APPWINDOW     0x00040000

/* PeekMessage flags */
#define PM_NOREMOVE         0x0000
#define PM_REMOVE           0x0001
#define PM_NOYIELD          0x0002

/* System metrics */
#define SM_CXSCREEN         0
#define SM_CYSCREEN         1
#define SM_CXFULLSCREEN     16
#define SM_CYFULLSCREEN     17

/* Cursor constants */
#define IDC_ARROW           ((PCSTR)(ULONG_PTR)32512)

/* Virtual key codes */
#define VK_LBUTTON          0x01
#define VK_RBUTTON          0x02
#define VK_CANCEL           0x03
#define VK_MBUTTON          0x04
#define VK_BACK             0x08
#define VK_TAB              0x09
#define VK_RETURN           0x0D
#define VK_SHIFT            0x10
#define VK_CONTROL          0x11
#define VK_MENU             0x12  /* Alt */
#define VK_PAUSE            0x13
#define VK_CAPITAL          0x14  /* Caps Lock */
#define VK_ESCAPE           0x1B
#define VK_SPACE            0x20
#define VK_PRIOR            0x21  /* Page Up */
#define VK_NEXT             0x22  /* Page Down */
#define VK_END              0x23
#define VK_HOME             0x24
#define VK_LEFT             0x25
#define VK_UP               0x26
#define VK_RIGHT            0x27
#define VK_DOWN             0x28
#define VK_INSERT           0x2D
#define VK_DELETE           0x2E
/* 0x30-0x39 = '0'-'9', 0x41-0x5A = 'A'-'Z' (same as ASCII) */
#define VK_LWIN             0x5B
#define VK_RWIN             0x5C
#define VK_NUMPAD0          0x60
#define VK_NUMPAD1          0x61
#define VK_NUMPAD2          0x62
#define VK_NUMPAD3          0x63
#define VK_NUMPAD4          0x64
#define VK_NUMPAD5          0x65
#define VK_NUMPAD6          0x66
#define VK_NUMPAD7          0x67
#define VK_NUMPAD8          0x68
#define VK_NUMPAD9          0x69
#define VK_MULTIPLY         0x6A
#define VK_ADD              0x6B
#define VK_SUBTRACT         0x6D
#define VK_DECIMAL          0x6E
#define VK_DIVIDE           0x6F
#define VK_F1               0x70
#define VK_F2               0x71
#define VK_F3               0x72
#define VK_F4               0x73
#define VK_F5               0x74
#define VK_F6               0x75
#define VK_F7               0x76
#define VK_F8               0x77
#define VK_F9               0x78
#define VK_F10              0x79
#define VK_F11              0x7A
#define VK_F12              0x7B
#define VK_NUMLOCK          0x90
#define VK_SCROLL           0x91
#define VK_LSHIFT           0xA0
#define VK_RSHIFT           0xA1
#define VK_LCONTROL         0xA2
#define VK_RCONTROL         0xA3
#define VK_LMENU            0xA4
#define VK_RMENU            0xA5
#define VK_OEM_1            0xBA  /* ;: */
#define VK_OEM_PLUS         0xBB
#define VK_OEM_COMMA        0xBC
#define VK_OEM_MINUS        0xBD
#define VK_OEM_PERIOD       0xBE
#define VK_OEM_2            0xBF  /* /? */
#define VK_OEM_3            0xC0  /* `~ */
#define VK_OEM_4            0xDB  /* [{ */
#define VK_OEM_5            0xDC  /* \| */
#define VK_OEM_6            0xDD  /* ]} */
#define VK_OEM_7            0xDE  /* '" */

/* ── Structures ────────────────────────────────────────────── */

typedef struct tagRECT {
    LONG left, top, right, bottom;
} RECT, *LPRECT;

typedef struct tagPOINT {
    LONG x, y;
} POINT, *LPPOINT;

typedef struct tagMSG {
    HWND   hwnd;
    DWORD  message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD  time;
    POINT  pt;
} MSG, *LPMSG;

#define CS_HREDRAW          0x0002
#define CS_VREDRAW          0x0001
#define CS_OWNDC            0x0020
#define CS_DBLCLKS          0x0008

typedef struct tagWNDCLASSEXA {
    DWORD       cbSize;
    DWORD       style;
    WNDPROC     lpfnWndProc;
    int         cbClsExtra;
    int         cbWndExtra;
    HINSTANCE   hInstance;
    HICON       hIcon;
    HCURSOR     hCursor;
    HBRUSH      hbrBackground;
    PCSTR       lpszMenuName;
    PCSTR       lpszClassName;
    HICON       hIconSm;
} WNDCLASSEXA, *LPWNDCLASSEXA;

typedef struct tagWNDCLASSA {
    DWORD       style;
    WNDPROC     lpfnWndProc;
    int         cbClsExtra;
    int         cbWndExtra;
    HINSTANCE   hInstance;
    HICON       hIcon;
    HCURSOR     hCursor;
    HBRUSH      hbrBackground;
    PCSTR       lpszMenuName;
    PCSTR       lpszClassName;
} WNDCLASSA, *LPWNDCLASSA;

typedef struct tagCREATESTRUCTA {
    PVOID       lpCreateParams;
    HINSTANCE   hInstance;
    HMENU       hMenu;
    HWND        hwndParent;
    int         cy, cx, y, x;
    LONG        style;
    PCSTR       lpszName;
    PCSTR       lpszClass;
    DWORD       dwExStyle;
} CREATESTRUCTA, *LPCREATESTRUCTA;

/* ── Window API ────────────────────────────────────────────── */

WORD    WINAPI RegisterClassExA(const WNDCLASSEXA *lpwcx);
WORD    WINAPI RegisterClassA(const WNDCLASSA *lpwcx);
BOOL    WINAPI UnregisterClassA(PCSTR lpClassName, HINSTANCE hInstance);
HWND    WINAPI CreateWindowExA(DWORD dwExStyle, PCSTR lpClassName,
                               PCSTR lpWindowName, DWORD dwStyle,
                               int X, int Y, int nWidth, int nHeight,
                               HWND hWndParent, HMENU hMenu,
                               HINSTANCE hInstance, PVOID lpParam);
BOOL    WINAPI DestroyWindow(HWND hWnd);
BOOL    WINAPI ShowWindow(HWND hWnd, int nCmdShow);
BOOL    WINAPI UpdateWindow(HWND hWnd);
BOOL    WINAPI SetWindowTextA(HWND hWnd, PCSTR lpString);
BOOL    WINAPI SetWindowPos(HWND hWnd, HWND hWndInsertAfter,
                            int X, int Y, int cx, int cy, DWORD uFlags);
BOOL    WINAPI MoveWindow(HWND hWnd, int X, int Y, int nWidth, int nHeight, BOOL bRepaint);

/* Message loop */
BOOL    WINAPI PeekMessageA(LPMSG lpMsg, HWND hWnd, DWORD wMsgFilterMin,
                             DWORD wMsgFilterMax, DWORD wRemoveMsg);
BOOL    WINAPI GetMessageA(LPMSG lpMsg, HWND hWnd, DWORD wMsgFilterMin,
                            DWORD wMsgFilterMax);
BOOL    WINAPI TranslateMessage(const MSG *lpMsg);
LRESULT WINAPI DispatchMessageA(const MSG *lpMsg);
void    WINAPI PostQuitMessage(int nExitCode);
BOOL    WINAPI PostMessageA(HWND hWnd, DWORD Msg, WPARAM wParam, LPARAM lParam);
LRESULT WINAPI SendMessageA(HWND hWnd, DWORD Msg, WPARAM wParam, LPARAM lParam);
LRESULT WINAPI DefWindowProcA(HWND hWnd, DWORD Msg, WPARAM wParam, LPARAM lParam);

/* Window info */
BOOL    WINAPI GetClientRect(HWND hWnd, LPRECT lpRect);
BOOL    WINAPI GetWindowRect(HWND hWnd, LPRECT lpRect);
BOOL    WINAPI AdjustWindowRect(LPRECT lpRect, DWORD dwStyle, BOOL bMenu);
BOOL    WINAPI AdjustWindowRectEx(LPRECT lpRect, DWORD dwStyle, BOOL bMenu, DWORD dwExStyle);
int     WINAPI GetSystemMetrics(int nIndex);
LONG    WINAPI GetWindowLongA(HWND hWnd, int nIndex);
LONG    WINAPI SetWindowLongA(HWND hWnd, int nIndex, LONG dwNewLong);
HWND    WINAPI GetForegroundWindow(void);
HWND    WINAPI SetFocus(HWND hWnd);
HWND    WINAPI GetDesktopWindow(void);
HWND    WINAPI GetActiveWindow(void);

/* Cursor/Input */
BOOL    WINAPI SetCursorPos(int X, int Y);
BOOL    WINAPI GetCursorPos(LPPOINT lpPoint);
int     WINAPI ShowCursor(BOOL bShow);
BOOL    WINAPI ClipCursor(const RECT *lpRect);
HWND    WINAPI SetCapture(HWND hWnd);
BOOL    WINAPI ReleaseCapture(void);
short   WINAPI GetAsyncKeyState(int vKey);
short   WINAPI GetKeyState(int nVirtKey);
BOOL    WINAPI GetKeyboardState(BYTE *lpKeyState);
DWORD   WINAPI MapVirtualKeyA(DWORD uCode, DWORD uMapType);
int     WINAPI GetKeyNameTextA(LONG lParam, PSTR lpString, int cchSize);
int     WINAPI ToAscii(DWORD uVirtKey, DWORD uScanCode, const BYTE *lpKeyState,
                       WORD *lpChar, DWORD uFlags);

/*
 * OsitoK input injection API — called by kernel IRQ handlers.
 * These are NOT Win32 API; they feed events into the message queue.
 */
void win32_post_keyboard_event(BYTE scancode, BOOL key_up);
void win32_post_mouse_event(int dx, int dy, DWORD buttons, short wheel_delta);
void win32_post_mouse_abs(int ax, int ay, int lmin, int lmax, DWORD buttons);

/* Misc */
int     WINAPI MessageBoxA(HWND hWnd, PCSTR lpText, PCSTR lpCaption, DWORD uType);
int     WINAPI MessageBoxW(HWND hWnd, PCWSTR lpText, PCWSTR lpCaption, DWORD uType);
HCURSOR WINAPI LoadCursorA(HINSTANCE hInstance, PCSTR lpCursorName);
HICON   WINAPI LoadIconA(HINSTANCE hInstance, PCSTR lpIconName);
HICON   WINAPI LoadIconW(HINSTANCE hInstance, PCWSTR lpIconName);
HDC     WINAPI GetDC(HWND hWnd);
int     WINAPI ReleaseDC(HWND hWnd, HDC hDC);
BOOL    WINAPI InvalidateRect(HWND hWnd, const RECT *lpRect, BOOL bErase);
BOOL    WINAPI SetForegroundWindow(HWND hWnd);

/* Dialog */
typedef ULONG_PTR (WINAPI *DLGPROC)(HWND, DWORD, WPARAM, LPARAM);
HWND    WINAPI CreateDialogParamA(HINSTANCE hInstance, PCSTR lpTemplateName,
                                   HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam);
HWND    WINAPI CreateDialogParamW(HINSTANCE hInstance, PCWSTR lpTemplateName,
                                   HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam);
BOOL    WINAPI EndDialog(HWND hDlg, LONG_PTR nResult);
HWND    WINAPI GetDlgItem(HWND hDlg, int nIDDlgItem);

/* Window search */
HWND    WINAPI FindWindowExA(HWND hWndParent, HWND hWndChildAfter,
                              PCSTR lpszClass, PCSTR lpszWindow);
HWND    WINAPI FindWindowExW(HWND hWndParent, HWND hWndChildAfter,
                              PCWSTR lpszClass, PCWSTR lpszWindow);

/* Wide message loop */
BOOL    WINAPI PeekMessageW(LPMSG lpMsg, HWND hWnd, DWORD wMsgFilterMin,
                             DWORD wMsgFilterMax, DWORD wRemoveMsg);
BOOL    WINAPI GetMessageW(LPMSG lpMsg, HWND hWnd, DWORD wMsgFilterMin,
                            DWORD wMsgFilterMax);
LRESULT WINAPI DispatchMessageW(const MSG *lpMsg);
LRESULT WINAPI SendMessageW(HWND hWnd, DWORD Msg, WPARAM wParam, LPARAM lParam);
LRESULT WINAPI SendMessageTimeoutW(HWND hWnd, DWORD Msg, WPARAM wParam, LPARAM lParam,
                                    DWORD fuFlags, DWORD uTimeout, ULONG_PTR *lpdwResult);

/* Thread messages */
BOOL    WINAPI PostThreadMessageA(DWORD idThread, DWORD Msg, WPARAM wParam, LPARAM lParam);
BOOL    WINAPI PostThreadMessageW(DWORD idThread, DWORD Msg, WPARAM wParam, LPARAM lParam);

/* Window properties */
HANDLE  WINAPI GetPropA(HWND hWnd, PCSTR lpString);
HANDLE  WINAPI GetPropW(HWND hWnd, PCWSTR lpString);
BOOL    WINAPI SetPropA(HWND hWnd, PCSTR lpString, HANDLE hData);
BOOL    WINAPI SetPropW(HWND hWnd, PCWSTR lpString, HANDLE hData);
HANDLE  WINAPI RemovePropA(HWND hWnd, PCSTR lpString);
HANDLE  WINAPI RemovePropW(HWND hWnd, PCWSTR lpString);

/* Window thread */
DWORD   WINAPI GetWindowThreadProcessId(HWND hWnd, DWORD *lpdwProcessId);

/* Clipboard */
BOOL    WINAPI OpenClipboard(HANDLE hWndNewOwner);
BOOL    WINAPI CloseClipboard(void);
BOOL    WINAPI EmptyClipboard(void);
HANDLE  WINAPI SetClipboardData(UINT uFormat, HANDLE hMem);
HANDLE  WINAPI GetClipboardData(UINT uFormat);

/* Paint / drawing (UT99 Window.dll / Core.dll) */
HDC     WINAPI BeginPaint(HWND hWnd, PVOID lpPaint);
BOOL    WINAPI EndPaint(HWND hWnd, PVOID lpPaint);
LRESULT WINAPI CallWindowProcA(PVOID lpPrevWndFunc, HWND hWnd, DWORD Msg,
                                WPARAM wParam, LPARAM lParam);
LRESULT WINAPI CallWindowProcW(PVOID lpPrevWndFunc, HWND hWnd, DWORD Msg,
                                WPARAM wParam, LPARAM lParam);
LRESULT WINAPI DefWindowProcW(HWND hWnd, DWORD Msg, WPARAM wParam, LPARAM lParam);
LRESULT WINAPI DefMDIChildProcA(HWND hWnd, DWORD Msg, WPARAM wParam, LPARAM lParam);
LRESULT WINAPI DefMDIChildProcW(HWND hWnd, DWORD Msg, WPARAM wParam, LPARAM lParam);
HWND    WINAPI CreateWindowExW(DWORD dwExStyle, PCWSTR lpClassName,
                                PCWSTR lpWindowName, DWORD dwStyle,
                                int X, int Y, int nWidth, int nHeight,
                                HWND hWndParent, HMENU hMenu,
                                HINSTANCE hInstance, PVOID lpParam);
WORD    WINAPI RegisterClassExW(PVOID lpwcx);
BOOL    WINAPI GetClassInfoExA(HINSTANCE hInstance, PCSTR lpszClass, PVOID lpwcx);
BOOL    WINAPI GetClassInfoExW(HINSTANCE hInstance, PCWSTR lpszClass, PVOID lpwcx);
LONG    WINAPI GetWindowLongW(HWND hWnd, int nIndex);
LONG    WINAPI SetWindowLongW(HWND hWnd, int nIndex, LONG dwNewLong);
BOOL    WINAPI IsWindow(HWND hWnd);
BOOL    WINAPI IsIconic(HWND hWnd);
BOOL    WINAPI IsZoomed(HWND hWnd);
BOOL    WINAPI IsWindowEnabled(HWND hWnd);
HWND    WINAPI GetParent(HWND hWnd);
BOOL    WINAPI EnumChildWindows(HWND hWndParent, PVOID lpEnumFunc, LPARAM lParam);
BOOL    WINAPI ClientToScreen(HWND hWnd, PVOID lpPoint);
BOOL    WINAPI ScreenToClient(HWND hWnd, PVOID lpPoint);
BOOL    WINAPI GetUpdateRect(HWND hWnd, PVOID lpRect, BOOL bErase);
int     WINAPI FillRect(HDC hDC, PVOID lprc, HBRUSH hbr);
BOOL    WINAPI DrawFocusRect(HDC hDC, PVOID lprc);
int     WINAPI DrawTextA(HDC hdc, PCSTR lpchText, int cchText, PVOID lprc, UINT format);
int     WINAPI DrawTextExA(HDC hdc, PSTR lpchText, int cchText, PVOID lprc,
                            UINT format, PVOID lpdtp);
int     WINAPI DrawTextExW(HDC hdc, PWSTR lpchText, int cchText, PVOID lprc,
                            UINT format, PVOID lpdtp);
DWORD   WINAPI GetSysColor(int nIndex);

/* Dialog box */
LONG_PTR WINAPI DialogBoxParamA(HINSTANCE hInstance, PCSTR lpTemplateName,
                                 HWND hWndParent, PVOID lpDialogFunc, LPARAM dwInitParam);
LONG_PTR WINAPI DialogBoxParamW(HINSTANCE hInstance, PCWSTR lpTemplateName,
                                 HWND hWndParent, PVOID lpDialogFunc, LPARAM dwInitParam);

/* Menu */
HMENU   WINAPI LoadMenuA(HINSTANCE hInstance, PCSTR lpMenuName);
HMENU   WINAPI LoadMenuW(HINSTANCE hInstance, PCWSTR lpMenuName);
HMENU   WINAPI GetSubMenu(HMENU hMenu, int nPos);
int     WINAPI GetMenuItemCount(HMENU hMenu);
UINT    WINAPI GetMenuState(HMENU hMenu, UINT uId, UINT uFlags);
BOOL    WINAPI GetMenuItemInfoA(HMENU hmenu, UINT item, BOOL fByPosition, PVOID lpmii);
BOOL    WINAPI GetMenuItemInfoW(HMENU hmenu, UINT item, BOOL fByPosition, PVOID lpmii);
BOOL    WINAPI SetMenuItemInfoA(HMENU hmenu, UINT item, BOOL fByPosition, PVOID lpmii);
BOOL    WINAPI SetMenuItemInfoW(HMENU hmenu, UINT item, BOOL fByPosition, PVOID lpmii);
DWORD   WINAPI CheckMenuItem(HMENU hMenu, UINT uIDCheckItem, UINT uCheck);
BOOL    WINAPI TrackPopupMenu(HMENU hMenu, UINT uFlags, int x, int y,
                               int nReserved, HWND hWnd, PVOID prcRect);

/* Cursor / misc (additional) */
HCURSOR WINAPI SetCursor(HCURSOR hCursor);
HCURSOR WINAPI LoadCursorW(HINSTANCE hInstance, PCWSTR lpCursorName);
HANDLE  WINAPI LoadImageA(HINSTANCE hInst, PCSTR name, UINT type,
                           int cx, int cy, UINT fuLoad);
UINT    WINAPI RegisterWindowMessageA(PCSTR lpString);
UINT    WINAPI RegisterWindowMessageW(PCWSTR lpString);
BOOL    WINAPI PostMessageW(HWND hWnd, DWORD Msg, WPARAM wParam, LPARAM lParam);

/* Additional stubs */
BOOL  WINAPI EnableWindow(HWND hWnd, BOOL bEnable);
HMENU WINAPI GetMenu(HWND hWnd);
DWORD WINAPI GetMessageTime(void);
HWND  WINAPI GetFocus(void);
BOOL  WINAPI IsWindowVisible(HWND hWnd);
int   WINAPI MapWindowPoints(HWND hWndFrom, HWND hWndTo, LPPOINT lpPoints, UINT cPoints);
BOOL  WINAPI RegisterHotKey(HWND hWnd, int id, UINT fsModifiers, UINT vk);
BOOL  WINAPI SetMenu(HWND hWnd, HMENU hMenu);
HWND  WINAPI SetParent(HWND hWndChild, HWND hWndNewParent);
HWND  WINAPI SetActiveWindow(HWND hWnd);
BOOL  WINAPI SystemParametersInfoW(UINT uiAction, UINT uiParam, PVOID pvParam, UINT fWinIni);
BOOL  WINAPI UnregisterHotKey(HWND hWnd, int id);
BOOL  WINAPI ValidateRect(HWND hWnd, const RECT *lpRect);

/* ── Shim init / resolve ───────────────────────────────────── */

PVOID user32_shim_init(void);
PVOID user32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal);

#endif /* USER32_SHIM_H */
