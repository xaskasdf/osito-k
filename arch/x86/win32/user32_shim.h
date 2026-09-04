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
typedef HANDLE HBITMAP;
typedef HANDLE HBRUSH;
typedef HANDLE HDC;
typedef HANDLE HRGN;
typedef HANDLE HDWP;
typedef HANDLE HHOOK;
typedef ULONG_PTR WPARAM;
typedef LONG_PTR  LPARAM;
typedef LONG_PTR  LRESULT;

typedef struct tagICONINFO {
    BOOL    fIcon;
    DWORD   xHotspot;
    DWORD   yHotspot;
    HBITMAP hbmMask;
    HBITMAP hbmColor;
} ICONINFO, *PICONINFO;

typedef LRESULT (WINAPI *WNDPROC)(HWND, DWORD, WPARAM, LPARAM);
typedef BOOL (WINAPI *WNDENUMPROC)(HWND, LPARAM);
typedef LRESULT (WINAPI *HOOKPROC)(int, WPARAM, LPARAM);

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
#define WM_GETOBJECT        0x003D
#define WM_PARENTNOTIFY     0x0210
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
#define WM_NCMOUSEMOVE      0x00A0
#define WM_NCLBUTTONDOWN    0x00A1
#define WM_NCLBUTTONUP      0x00A2
#define WM_KEYDOWN          0x0100
#define WM_KEYUP            0x0101
#define WM_CHAR             0x0102
#define WM_SYSKEYDOWN       0x0104
#define WM_SYSKEYUP         0x0105
#define WM_SYSCHAR          0x0106
#define WM_INITDIALOG       0x0110
#define WM_COMMAND          0x0111
#define WM_SYSCOMMAND       0x0112
#define WM_TIMER            0x0113
#define WM_INITMENU         0x0116
#define WM_INITMENUPOPUP    0x0117
#define WM_MOUSEMOVE        0x0200
#define WM_LBUTTONDOWN      0x0201
#define WM_LBUTTONUP        0x0202
#define WM_RBUTTONDOWN      0x0204
#define WM_RBUTTONUP        0x0205
#define WM_MBUTTONDOWN      0x0207
#define WM_MBUTTONUP        0x0208
#define WM_MOUSEWHEEL       0x020A
#define WM_XBUTTONDOWN      0x020B
#define WM_XBUTTONUP        0x020C
#define WM_MOUSEHWHEEL      0x020E
#define WM_CAPTURECHANGED   0x0215
#define WM_MOVING           0x0216
#define WM_ENTERSIZEMOVE    0x0231
#define WM_EXITSIZEMOVE     0x0232
#define WM_NCMOUSEHOVER     0x02A0
#define WM_MOUSEHOVER       0x02A1
#define WM_NCMOUSELEAVE     0x02A2
#define WM_MOUSELEAVE       0x02A3
#define WM_USER             0x0400

/* Mouse key flags (wParam for mouse messages) */
#define MK_LBUTTON          0x0001
#define MK_RBUTTON          0x0002
#define MK_SHIFT            0x0004
#define MK_CONTROL          0x0008
#define MK_MBUTTON          0x0010
#define MK_XBUTTON1         0x0020
#define MK_XBUTTON2         0x0040

#define XBUTTON1            0x0001
#define XBUTTON2            0x0002

/* WM_NCHITTEST results. */
#define HTCLIENT            1
#define HTCAPTION           2

#define TME_HOVER           0x00000001
#define TME_LEAVE           0x00000002
#define TME_NONCLIENT       0x00000010
#define TME_QUERY           0x40000000
#define TME_CANCEL          0x80000000
#define HOVER_DEFAULT       0xFFFFFFFF

/* ShowWindow commands */
#define SW_HIDE             0
#define SW_SHOWNORMAL       1
#define SW_SHOWMINIMIZED    2
#define SW_SHOWMAXIMIZED    3
#define SW_SHOWNOACTIVATE   4
#define SW_SHOW             5
#define SW_MINIMIZE         6
#define SW_SHOWMINNOACTIVE  7
#define SW_SHOWNA           8
#define SW_RESTORE          9
#define SW_SHOWDEFAULT      10
#define SW_FORCEMINIMIZE    11

#define SIZE_RESTORED       0
#define SIZE_MINIMIZED      1
#define SIZE_MAXIMIZED      2

/* SetWindowPos flags */
#define SWP_NOSIZE          0x0001
#define SWP_NOMOVE          0x0002
#define SWP_NOZORDER        0x0004
#define SWP_NOREDRAW        0x0008
#define SWP_NOACTIVATE      0x0010
#define SWP_FRAMECHANGED    0x0020
#define SWP_SHOWWINDOW      0x0040
#define SWP_HIDEWINDOW      0x0080
#define SWP_NOCOPYBITS      0x0100
#define SWP_NOOWNERZORDER   0x0200
#define SWP_NOSENDCHANGING  0x0400
#define SWP_DEFERERASE      0x2000
#define SWP_ASYNCWINDOWPOS  0x4000

#define HWND_TOP            ((HWND)(ULONG_PTR)0)
#define HWND_BOTTOM         ((HWND)(ULONG_PTR)1)
#define HWND_TOPMOST        ((HWND)(LONG_PTR)-1)
#define HWND_NOTOPMOST      ((HWND)(LONG_PTR)-2)
#define HWND_MESSAGE        ((HWND)(LONG_PTR)-3)

#define GW_HWNDFIRST        0
#define GW_HWNDLAST         1
#define GW_HWNDNEXT         2
#define GW_HWNDPREV         3
#define GW_OWNER            4
#define GW_CHILD            5
#define GW_ENABLEDPOPUP     6

#define GA_PARENT           1
#define GA_ROOT             2
#define GA_ROOTOWNER        3

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
#define WS_GROUP            0x00020000
#define WS_TABSTOP          0x00010000
#define WS_OVERLAPPEDWINDOW (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | \
                             WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)

/* Dialog/control constants. */
#define IDOK                1
#define IDCANCEL            2
#define BN_CLICKED          0
#define BM_CLICK            0x00F5

/* Window-menu commands (WM_SYSCOMMAND). */
#define SC_SIZE             0xF000
#define SC_MOVE             0xF010
#define SC_MINIMIZE         0xF020
#define SC_MAXIMIZE         0xF030
#define SC_CLOSE            0xF060
#define SC_RESTORE          0xF120

/* Extended window styles */
#define WS_EX_TOPMOST       0x00000008
#define WS_EX_TOOLWINDOW    0x00000080
#define WS_EX_APPWINDOW     0x00040000
#define WS_EX_NOACTIVATE    0x08000000

/* PeekMessage flags */
#define PM_NOREMOVE         0x0000
#define PM_REMOVE           0x0001
#define PM_NOYIELD          0x0002

/* Windows hook types and callback codes. */
#define WH_MSGFILTER        (-1)
#define WH_JOURNALRECORD    0
#define WH_JOURNALPLAYBACK  1
#define WH_KEYBOARD         2
#define WH_GETMESSAGE       3
#define WH_CALLWNDPROC      4
#define WH_CBT              5
#define WH_SYSMSGFILTER     6
#define WH_MOUSE            7
#define WH_HARDWARE         8
#define WH_DEBUG            9
#define WH_SHELL            10
#define WH_FOREGROUNDIDLE   11
#define WH_CALLWNDPROCRET   12
#define WH_KEYBOARD_LL      13
#define WH_MOUSE_LL         14
#define HC_ACTION           0

/* System metrics */
#define SM_CXSCREEN         0
#define SM_CYSCREEN         1
#define SM_CXICON           11
#define SM_CYICON           12
#define SM_CXCURSOR         13
#define SM_CYCURSOR         14
#define SM_CXFULLSCREEN     16
#define SM_CYFULLSCREEN     17
#define SM_MOUSEPRESENT     19
#define SM_SWAPBUTTON       23
#define SM_CMOUSEBUTTONS    43
#define SM_CXSMICON         49
#define SM_CYSMICON         50
#define SM_MOUSEWHEELPRESENT 75
#define SM_XVIRTUALSCREEN   76
#define SM_YVIRTUALSCREEN   77
#define SM_CXVIRTUALSCREEN  78
#define SM_CYVIRTUALSCREEN  79
#define SM_CMONITORS        80
#define SM_SAMEDISPLAYFORMAT 81
#define SM_MOUSEHORIZONTALWHEELPRESENT 91

/* Cursor constants */
#define IDC_ARROW           ((PCSTR)(ULONG_PTR)32512)

/* Virtual key codes */
#define VK_LBUTTON          0x01
#define VK_RBUTTON          0x02
#define VK_CANCEL           0x03
#define VK_MBUTTON          0x04
#define VK_XBUTTON1         0x05
#define VK_XBUTTON2         0x06
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
#define VK_PACKET           0xE7
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

#define USER32_ACCESSIBILITY_TITLE_CAP 256

typedef struct {
    HWND  handle;
    HWND  parent;
    HWND  owner;
    DWORD style;
    DWORD ex_style;
    DWORD owner_pid;
    DWORD owner_tid;
    RECT  window_rect;
    RECT  client_rect;
    BOOL  visible;
    BOOL  enabled;
    BOOL  message_only;
    WCHAR title[USER32_ACCESSIBILITY_TITLE_CAP];
} USER32_ACCESSIBLE_WINDOW_INFO;

typedef BOOL (WINAPI *MONITORENUMPROC)(HANDLE, HDC, LPRECT, LPARAM);

typedef struct tagPOINT {
    LONG x, y;
} POINT, *LPPOINT;

typedef struct tagWINDOWPLACEMENT {
    UINT  length;
    UINT  flags;
    UINT  showCmd;
    POINT ptMinPosition;
    POINT ptMaxPosition;
    RECT  rcNormalPosition;
} WINDOWPLACEMENT, *LPWINDOWPLACEMENT;

typedef struct tagWINDOWPOS {
    HWND hwnd;
    HWND hwndInsertAfter;
    int  x;
    int  y;
    int  cx;
    int  cy;
    UINT flags;
} WINDOWPOS, *LPWINDOWPOS;

typedef struct tagTRACKMOUSEEVENT {
    DWORD cbSize;
    DWORD dwFlags;
    HWND  hwndTrack;
    DWORD dwHoverTime;
} TRACKMOUSEEVENT, *LPTRACKMOUSEEVENT;

typedef struct tagMSG {
    HWND   hwnd;
    DWORD  message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD  time;
    POINT  pt;
} MSG, *LPMSG;

typedef struct tagCWPSTRUCT {
    LPARAM lParam;
    WPARAM wParam;
    UINT message;
    HWND hwnd;
} CWPSTRUCT, *LPCWPSTRUCT;

typedef struct tagCWPRETSTRUCT {
    LRESULT lResult;
    LPARAM lParam;
    WPARAM wParam;
    UINT message;
    HWND hwnd;
} CWPRETSTRUCT, *LPCWPRETSTRUCT;

#define INPUT_MOUSE         0
#define INPUT_KEYBOARD      1
#define INPUT_HARDWARE      2

#define MOUSEEVENTF_MOVE        0x0001
#define MOUSEEVENTF_LEFTDOWN    0x0002
#define MOUSEEVENTF_LEFTUP      0x0004
#define MOUSEEVENTF_RIGHTDOWN   0x0008
#define MOUSEEVENTF_RIGHTUP     0x0010
#define MOUSEEVENTF_MIDDLEDOWN  0x0020
#define MOUSEEVENTF_MIDDLEUP    0x0040
#define MOUSEEVENTF_XDOWN       0x0080
#define MOUSEEVENTF_XUP         0x0100
#define MOUSEEVENTF_WHEEL       0x0800
#define MOUSEEVENTF_HWHEEL      0x1000
#define MOUSEEVENTF_MOVE_NOCOALESCE 0x2000
#define MOUSEEVENTF_VIRTUALDESK 0x4000
#define MOUSEEVENTF_ABSOLUTE    0x8000

#define KEYEVENTF_EXTENDEDKEY   0x0001
#define KEYEVENTF_KEYUP         0x0002
#define KEYEVENTF_UNICODE       0x0004
#define KEYEVENTF_SCANCODE      0x0008

typedef struct tagMOUSEINPUT {
    LONG dx;
    LONG dy;
    DWORD mouseData;
    DWORD dwFlags;
    DWORD time;
    ULONG_PTR dwExtraInfo;
} MOUSEINPUT;

typedef struct tagKEYBDINPUT {
    WORD wVk;
    WORD wScan;
    DWORD dwFlags;
    DWORD time;
    ULONG_PTR dwExtraInfo;
} KEYBDINPUT;

typedef struct tagHARDWAREINPUT {
    DWORD uMsg;
    WORD wParamL;
    WORD wParamH;
} HARDWAREINPUT;

typedef struct tagINPUT {
    DWORD type;
    union {
        MOUSEINPUT mi;
        KEYBDINPUT ki;
        HARDWAREINPUT hi;
    } data;
} INPUT, *PINPUT;

#define CS_HREDRAW          0x0002
#define CS_VREDRAW          0x0001
#define CS_OWNDC            0x0020
#define CS_DBLCLKS          0x0008
#define CS_NOCLOSE          0x0200
#define CS_GLOBALCLASS      0x4000

/* Menu flags, types, states, and information masks. */
#define MF_BYCOMMAND        0x00000000
#define MF_BYPOSITION       0x00000400
#define MF_SEPARATOR        0x00000800
#define MF_ENABLED          0x00000000
#define MF_GRAYED           0x00000001
#define MF_DISABLED         0x00000002
#define MF_UNCHECKED        0x00000000
#define MF_CHECKED          0x00000008
#define MF_POPUP            0x00000010

#define MFT_STRING          0x00000000
#define MFT_BITMAP          0x00000004
#define MFT_MENUBARBREAK    0x00000020
#define MFT_MENUBREAK       0x00000040
#define MFT_OWNERDRAW       0x00000100
#define MFT_RADIOCHECK      0x00000200
#define MFT_SEPARATOR       0x00000800
#define MFT_RIGHTORDER      0x00002000
#define MFT_RIGHTJUSTIFY    0x00004000

#define MFS_ENABLED         0x00000000
#define MFS_GRAYED          0x00000003
#define MFS_DISABLED        MFS_GRAYED
#define MFS_CHECKED         0x00000008
#define MFS_HILITE          0x00000080
#define MFS_DEFAULT         0x00001000

#define MIIM_STATE          0x00000001
#define MIIM_ID             0x00000002
#define MIIM_SUBMENU        0x00000004
#define MIIM_CHECKMARKS     0x00000008
#define MIIM_TYPE           0x00000010
#define MIIM_DATA           0x00000020
#define MIIM_STRING         0x00000040
#define MIIM_BITMAP         0x00000080
#define MIIM_FTYPE          0x00000100

#define MIM_MAXHEIGHT       0x00000001
#define MIM_BACKGROUND      0x00000002
#define MIM_HELPID          0x00000004
#define MIM_MENUDATA        0x00000008
#define MIM_STYLE           0x00000010
#define MIM_APPLYTOSUBMENUS 0x80000000

#define GMDI_USEDISABLED    0x0001
#define GMDI_GOINTOPOPUPS   0x0002

typedef struct tagMENUINFO {
    DWORD     cbSize;
    DWORD     fMask;
    DWORD     dwStyle;
    UINT      cyMax;
    HBRUSH    hbrBack;
    DWORD     dwContextHelpID;
    ULONG_PTR dwMenuData;
} MENUINFO, *LPMENUINFO;
typedef const MENUINFO *LPCMENUINFO;

typedef struct tagMENUITEMINFOA {
    UINT      cbSize;
    UINT      fMask;
    UINT      fType;
    UINT      fState;
    UINT      wID;
    HMENU     hSubMenu;
    HBITMAP   hbmpChecked;
    HBITMAP   hbmpUnchecked;
    ULONG_PTR dwItemData;
    PSTR      dwTypeData;
    UINT      cch;
    HBITMAP   hbmpItem;
} MENUITEMINFOA, *LPMENUITEMINFOA;
typedef const MENUITEMINFOA *LPCMENUITEMINFOA;

typedef struct tagMENUITEMINFOW {
    UINT      cbSize;
    UINT      fMask;
    UINT      fType;
    UINT      fState;
    UINT      wID;
    HMENU     hSubMenu;
    HBITMAP   hbmpChecked;
    HBITMAP   hbmpUnchecked;
    ULONG_PTR dwItemData;
    PWSTR     dwTypeData;
    UINT      cch;
    HBITMAP   hbmpItem;
} MENUITEMINFOW, *LPMENUITEMINFOW;
typedef const MENUITEMINFOW *LPCMENUITEMINFOW;

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
int     WINAPI GetWindowTextLengthA(HWND hWnd);
int     WINAPI GetWindowTextLengthW(HWND hWnd);
int     WINAPI GetWindowTextW(HWND hWnd, PWSTR text, int max_count);
BOOL    WINAPI SetWindowPos(HWND hWnd, HWND hWndInsertAfter,
                             int X, int Y, int cx, int cy, DWORD uFlags);
BOOL    WINAPI MoveWindow(HWND hWnd, int X, int Y, int nWidth, int nHeight, BOOL bRepaint);
HDWP    WINAPI BeginDeferWindowPos(int nNumWindows);
HDWP    WINAPI DeferWindowPos(HDWP hWinPosInfo, HWND hWnd,
                              HWND hWndInsertAfter, int x, int y,
                              int cx, int cy, UINT uFlags);
BOOL    WINAPI EndDeferWindowPos(HDWP hWinPosInfo);
BOOL    user32_get_window_surface(HWND hwnd, void **pixels, int *width,
                                  int *height, int *pitch);
uint32_t user32_get_window_compositor_id(HWND hwnd);
void    user32_mark_window_dirty(HWND hwnd);

/* Message loop */
BOOL    WINAPI PeekMessageA(LPMSG lpMsg, HWND hWnd, DWORD wMsgFilterMin,
                             DWORD wMsgFilterMax, DWORD wRemoveMsg);
BOOL    WINAPI GetMessageA(LPMSG lpMsg, HWND hWnd, DWORD wMsgFilterMin,
                            DWORD wMsgFilterMax);
BOOL    WINAPI TranslateMessage(const MSG *lpMsg);
LRESULT WINAPI DispatchMessageA(const MSG *lpMsg);
HHOOK   WINAPI SetWindowsHookExA(int idHook, HOOKPROC lpfn,
                                  HINSTANCE hMod, DWORD dwThreadId);
HHOOK   WINAPI SetWindowsHookExW(int idHook, HOOKPROC lpfn,
                                  HINSTANCE hMod, DWORD dwThreadId);
BOOL    WINAPI UnhookWindowsHookEx(HHOOK hhk);
LRESULT WINAPI CallNextHookEx(HHOOK hhk, int nCode,
                               WPARAM wParam, LPARAM lParam);
void    WINAPI PostQuitMessage(int nExitCode);
BOOL    WINAPI PostMessageA(HWND hWnd, DWORD Msg, WPARAM wParam, LPARAM lParam);
LRESULT WINAPI SendMessageA(HWND hWnd, DWORD Msg, WPARAM wParam, LPARAM lParam);
LRESULT WINAPI DefWindowProcA(HWND hWnd, DWORD Msg, WPARAM wParam, LPARAM lParam);

/* Window info */
BOOL    WINAPI GetClientRect(HWND hWnd, LPRECT lpRect);
BOOL    WINAPI GetWindowRect(HWND hWnd, LPRECT lpRect);
BOOL    WINAPI GetWindowPlacement(HWND hWnd, LPWINDOWPLACEMENT placement);
BOOL    WINAPI SetWindowPlacement(HWND hWnd, const WINDOWPLACEMENT *placement);
BOOL    WINAPI IsRectEmpty(const RECT *lpRect);
BOOL    WINAPI EqualRect(const RECT *a, const RECT *b);
BOOL    WINAPI SetRect(LPRECT rect, int left, int top, int right, int bottom);
BOOL    WINAPI SetRectEmpty(LPRECT rect);
BOOL    WINAPI OffsetRect(LPRECT rect, int dx, int dy);
BOOL    WINAPI InflateRect(LPRECT rect, int dx, int dy);
BOOL    WINAPI IntersectRect(LPRECT result, const RECT *a, const RECT *b);
BOOL    WINAPI PtInRect(const RECT *rect, POINT point);
int     WINAPI GetWindowRgn(HWND hWnd, HRGN hRgn);
int     WINAPI SetWindowRgn(HWND hWnd, HRGN hRgn, BOOL bRedraw);
BOOL    WINAPI AdjustWindowRect(LPRECT lpRect, DWORD dwStyle, BOOL bMenu);
BOOL    WINAPI AdjustWindowRectEx(LPRECT lpRect, DWORD dwStyle, BOOL bMenu, DWORD dwExStyle);
int     WINAPI GetSystemMetrics(int nIndex);
int     WINAPI GetSystemMetricsForDpi(int nIndex, UINT dpi);
HANDLE  WINAPI MonitorFromWindow(HWND hWnd, DWORD dwFlags);
HANDLE  WINAPI MonitorFromPoint(POINT pt, DWORD dwFlags);
HANDLE  WINAPI MonitorFromRect(const RECT *rect, DWORD dwFlags);
BOOL    WINAPI GetMonitorInfoW(HANDLE hMonitor, PVOID lpmi);
BOOL    WINAPI EnumDisplayMonitors(HDC hdc, const RECT *clip,
                                   MONITORENUMPROC callback, LPARAM data);
LONG    WINAPI GetWindowLongA(HWND hWnd, int nIndex);
LONG    WINAPI SetWindowLongA(HWND hWnd, int nIndex, LONG dwNewLong);
LONG_PTR WINAPI GetWindowLongPtrA(HWND hWnd, int nIndex);
LONG_PTR WINAPI SetWindowLongPtrA(HWND hWnd, int nIndex, LONG_PTR dwNewLong);
BOOL    WINAPI SetLayeredWindowAttributes(HWND hWnd, DWORD crKey,
                                           BYTE bAlpha, DWORD dwFlags);
BOOL    WINAPI GetLayeredWindowAttributes(HWND hWnd, DWORD *crKey,
                                          BYTE *alpha, DWORD *flags);
HWND    WINAPI GetForegroundWindow(void);
HWND    WINAPI WindowFromPoint(POINT Point);
HWND    WINAPI GetAncestor(HWND hWnd, UINT gaFlags);
HWND    WINAPI GetWindow(HWND hWnd, UINT uCmd);
HWND    WINAPI GetTopWindow(HWND hWnd);
BOOL    WINAPI BringWindowToTop(HWND hWnd);
HWND    WINAPI SetFocus(HWND hWnd);
HWND    WINAPI GetDesktopWindow(void);
HWND    WINAPI GetShellWindow(void);
HWND    WINAPI GetActiveWindow(void);

/* Cursor/Input */
BOOL    WINAPI SetCursorPos(int X, int Y);
BOOL    WINAPI GetCursorPos(LPPOINT lpPoint);
BOOL    WINAPI GetCursorInfo(PVOID cursor_info);
BOOL    WINAPI GetLastInputInfo(PVOID last_input_info);
BOOL    WINAPI CreateCaret(HWND hWnd, HBITMAP hBitmap, int nWidth, int nHeight);
BOOL    WINAPI DestroyCaret(void);
BOOL    WINAPI SetCaretPos(int X, int Y);
int     WINAPI ShowCursor(BOOL bShow);
BOOL    WINAPI ClipCursor(const RECT *lpRect);
BOOL    WINAPI GetClipCursor(RECT *lpRect);
BOOL    WINAPI TrackMouseEvent(PVOID lpEventTrack);
HWND    WINAPI SetCapture(HWND hWnd);
HWND    WINAPI GetCapture(void);
BOOL    WINAPI ReleaseCapture(void);
UINT    WINAPI SendInput(UINT cInputs, const INPUT *pInputs, int cbSize);
short   WINAPI GetAsyncKeyState(int vKey);
short   WINAPI GetKeyState(int nVirtKey);
BOOL    WINAPI GetKeyboardState(BYTE *lpKeyState);
DWORD   WINAPI MapVirtualKeyA(DWORD uCode, DWORD uMapType);
DWORD   WINAPI MapVirtualKeyW(DWORD uCode, DWORD uMapType);
DWORD   WINAPI MapVirtualKeyExA(DWORD uCode, DWORD uMapType, HANDLE dwhkl);
DWORD   WINAPI MapVirtualKeyExW(DWORD uCode, DWORD uMapType, HANDLE dwhkl);
int     WINAPI GetKeyNameTextA(LONG lParam, PSTR lpString, int cchSize);
int     WINAPI ToAscii(DWORD uVirtKey, DWORD uScanCode, const BYTE *lpKeyState,
                       WORD *lpChar, DWORD uFlags);
int     WINAPI ToAsciiEx(DWORD uVirtKey, DWORD uScanCode,
                         const BYTE *lpKeyState, WORD *lpChar,
                         DWORD uFlags, HANDLE dwhkl);
int     WINAPI ToUnicode(DWORD uVirtKey, DWORD uScanCode,
                         const BYTE *lpKeyState, WCHAR *pwszBuff,
                         int cchBuff, DWORD uFlags);
int     WINAPI ToUnicodeEx(DWORD uVirtKey, DWORD uScanCode,
                           const BYTE *lpKeyState, WCHAR *pwszBuff,
                           int cchBuff, DWORD uFlags, HANDLE dwhkl);

/*
 * OsitoK input injection API — called by kernel IRQ handlers.
 * These are NOT Win32 API; they feed events into the message queue.
 */
void win32_post_keyboard_event(BYTE scancode, BOOL key_up);
void win32_post_mouse_event(int dx, int dy, DWORD buttons, short wheel_delta);
void win32_post_mouse_screen(int screen_x, int screen_y,
                             int raw_dx, int raw_dy,
                             DWORD buttons, short wheel_delta);
void win32_post_mouse_abs(int ax, int ay, int lmin, int lmax, DWORD buttons);
BOOL user32_get_current_display_mode(uint32_t *width, uint32_t *height,
                                     uint32_t *bpp, uint32_t *frequency);
uint32_t user32_get_display_resolution_count(void);
BOOL user32_get_display_resolution(uint32_t index, uint32_t *width,
                                   uint32_t *height);

/* Misc */
int     WINAPI MessageBoxA(HWND hWnd, PCSTR lpText, PCSTR lpCaption, DWORD uType);
int     WINAPI MessageBoxW(HWND hWnd, PCWSTR lpText, PCWSTR lpCaption, DWORD uType);
HCURSOR WINAPI LoadCursorA(HINSTANCE hInstance, PCSTR lpCursorName);
HICON   WINAPI LoadIconA(HINSTANCE hInstance, PCSTR lpIconName);
HICON   WINAPI LoadIconW(HINSTANCE hInstance, PCWSTR lpIconName);
HICON   WINAPI CreateIconIndirect(const ICONINFO *icon_info);
HICON   WINAPI CopyIcon(HICON icon);
BOOL    WINAPI GetIconInfo(HICON icon, ICONINFO *icon_info);
BOOL    WINAPI DrawIconEx(HDC hdc, int x, int y, HICON icon, int width,
                          int height, UINT step, HBRUSH brush, UINT flags);
BOOL    WINAPI DestroyIcon(HICON icon);
HDC     WINAPI GetDC(HWND hWnd);
HDC     WINAPI GetWindowDC(HWND hWnd);
HWND    WINAPI WindowFromDC(HDC hDC);
int     WINAPI ReleaseDC(HWND hWnd, HDC hDC);
BOOL    WINAPI InvalidateRect(HWND hWnd, const RECT *lpRect, BOOL bErase);
BOOL    user32_configure_directdraw_window(HWND hWnd, BOOL exclusive,
                                            BOOL allow_window_changes,
                                            int width, int height);
BOOL    WINAPI RedrawWindow(HWND hWnd, const RECT *lprcUpdate,
                            HANDLE hrgnUpdate, UINT flags);
BOOL    WINAPI SetForegroundWindow(HWND hWnd);
BOOL    WINAPI AllowSetForegroundWindow(DWORD dwProcessId);
BOOL    user32_activate_compositor_window(uint32_t compositor_id);
void    user32_deactivate_compositor_windows(void);

/* Dialog */
typedef ULONG_PTR (WINAPI *DLGPROC)(HWND, DWORD, WPARAM, LPARAM);
HWND    WINAPI CreateDialogParamA(HINSTANCE hInstance, PCSTR lpTemplateName,
                                   HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam);
HWND    WINAPI CreateDialogParamW(HINSTANCE hInstance, PCWSTR lpTemplateName,
                                   HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam);
HWND    WINAPI CreateDialogIndirectParamA(HINSTANCE hInstance,
                                           PCVOID lpTemplate,
                                           HWND hWndParent,
                                           DLGPROC lpDialogFunc,
                                           LPARAM dwInitParam);
HWND    WINAPI CreateDialogIndirectParamW(HINSTANCE hInstance,
                                           PCVOID lpTemplate,
                                           HWND hWndParent,
                                           DLGPROC lpDialogFunc,
                                           LPARAM dwInitParam);
BOOL    WINAPI EndDialog(HWND hDlg, LONG_PTR nResult);
LRESULT WINAPI DefDlgProcA(HWND hDlg, DWORD msg, WPARAM wParam,
                            LPARAM lParam);
LRESULT WINAPI DefDlgProcW(HWND hDlg, DWORD msg, WPARAM wParam,
                            LPARAM lParam);
BOOL    WINAPI IsDialogMessageA(HWND hDlg, LPMSG lpMsg);
BOOL    WINAPI IsDialogMessageW(HWND hDlg, LPMSG lpMsg);
HWND    WINAPI GetDlgItem(HWND hDlg, int nIDDlgItem);
UINT    WINAPI GetDlgItemInt(HWND hDlg, int nIDDlgItem, BOOL *translated,
                             BOOL is_signed);
BOOL    WINAPI SetDlgItemInt(HWND hDlg, int nIDDlgItem, UINT value,
                             BOOL is_signed);
BOOL    WINAPI SetDlgItemTextA(HWND hDlg, int nIDDlgItem, PCSTR lpString);
int     WINAPI GetDlgCtrlID(HWND hWnd);

/* Window search */
HWND    WINAPI FindWindowA(PCSTR lpszClass, PCSTR lpszWindow);
HWND    WINAPI FindWindowW(PCWSTR lpszClass, PCWSTR lpszWindow);
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
BOOL    WINAPI EnumThreadWindows(DWORD dwThreadId, WNDENUMPROC lpfn,
                                   LPARAM lParam);
BOOL    WINAPI EnumWindows(WNDENUMPROC lpfn, LPARAM lParam);

/* Clipboard */
BOOL    WINAPI OpenClipboard(HANDLE hWndNewOwner);
BOOL    WINAPI CloseClipboard(void);
BOOL    WINAPI EmptyClipboard(void);
HANDLE  WINAPI SetClipboardData(UINT uFormat, HANDLE hMem);
HANDLE  WINAPI GetClipboardData(UINT uFormat);
UINT    WINAPI RegisterClipboardFormatA(PCSTR lpszFormat);
UINT    WINAPI RegisterClipboardFormatW(PCWSTR lpszFormat);
int     WINAPI GetClipboardFormatNameA(UINT format, PSTR buffer, int max_count);
int     WINAPI GetClipboardFormatNameW(UINT format, PWSTR buffer, int max_count);
UINT    WINAPI EnumClipboardFormats(UINT format);
DWORD   WINAPI GetClipboardSequenceNumber(void);
BOOL    WINAPI IsClipboardFormatAvailable(UINT format);

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
LONG_PTR WINAPI GetWindowLongPtrW(HWND hWnd, int nIndex);
LONG_PTR WINAPI SetWindowLongPtrW(HWND hWnd, int nIndex, LONG_PTR dwNewLong);
LONG_PTR WINAPI GetClassLongPtrW(HWND hWnd, int nIndex);
LONG_PTR WINAPI SetClassLongPtrW(HWND hWnd, int nIndex, LONG_PTR dwNewLong);
LONG    WINAPI SetClassLongW(HWND hWnd, int nIndex, LONG dwNewLong);
int     WINAPI GetClassNameW(HWND hWnd, PWSTR class_name, int max_count);
BOOL    WINAPI IsWindow(HWND hWnd);
BOOL    WINAPI IsIconic(HWND hWnd);
BOOL    WINAPI IsZoomed(HWND hWnd);
BOOL    WINAPI IsWindowEnabled(HWND hWnd);
BOOL    WINAPI IsChild(HWND hWndParent, HWND hWnd);
HWND    WINAPI GetParent(HWND hWnd);
BOOL    WINAPI EnumChildWindows(HWND hWndParent, WNDENUMPROC lpEnumFunc,
                                LPARAM lParam);
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
                                 HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam);
LONG_PTR WINAPI DialogBoxParamW(HINSTANCE hInstance, PCWSTR lpTemplateName,
                                 HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam);
LONG_PTR WINAPI DialogBoxIndirectParamA(HINSTANCE hInstance,
                                         PCVOID lpTemplate,
                                         HWND hWndParent,
                                         DLGPROC lpDialogFunc,
                                         LPARAM dwInitParam);
LONG_PTR WINAPI DialogBoxIndirectParamW(HINSTANCE hInstance,
                                         PCVOID lpTemplate,
                                         HWND hWndParent,
                                         DLGPROC lpDialogFunc,
                                         LPARAM dwInitParam);

/* Menu */
HMENU   WINAPI CreateMenu(void);
HMENU   WINAPI CreatePopupMenu(void);
BOOL    WINAPI DestroyMenu(HMENU hMenu);
BOOL    WINAPI IsMenu(HMENU hMenu);
HMENU   WINAPI GetSystemMenu(HWND hWnd, BOOL bRevert);
BOOL    WINAPI EnableMenuItem(HMENU hMenu, UINT uIDEnableItem, UINT uEnable);
BOOL    WINAPI InsertMenuItemA(HMENU hMenu, UINT item, BOOL by_position,
                               LPCMENUITEMINFOA info);
BOOL    WINAPI InsertMenuItemW(HMENU hMenu, UINT item, BOOL by_position,
                               LPCMENUITEMINFOW info);
BOOL    WINAPI GetMenuInfo(HMENU hMenu, LPMENUINFO info);
BOOL    WINAPI SetMenuInfo(HMENU hMenu, LPCMENUINFO info);
BOOL    WINAPI SetMenuDefaultItem(HMENU hMenu, UINT item, UINT by_position);
UINT    WINAPI GetMenuDefaultItem(HMENU hMenu, UINT by_position, UINT flags);
HMENU   WINAPI LoadMenuA(HINSTANCE hInstance, PCSTR lpMenuName);
HMENU   WINAPI LoadMenuW(HINSTANCE hInstance, PCWSTR lpMenuName);
HMENU   WINAPI GetSubMenu(HMENU hMenu, int nPos);
int     WINAPI GetMenuItemCount(HMENU hMenu);
UINT    WINAPI GetMenuState(HMENU hMenu, UINT uId, UINT uFlags);
BOOL    WINAPI GetMenuItemInfoA(HMENU hmenu, UINT item, BOOL fByPosition,
                                LPMENUITEMINFOA lpmii);
BOOL    WINAPI GetMenuItemInfoW(HMENU hmenu, UINT item, BOOL fByPosition,
                                LPMENUITEMINFOW lpmii);
BOOL    WINAPI SetMenuItemInfoA(HMENU hmenu, UINT item, BOOL fByPosition,
                                LPCMENUITEMINFOA lpmii);
BOOL    WINAPI SetMenuItemInfoW(HMENU hmenu, UINT item, BOOL fByPosition,
                                LPCMENUITEMINFOW lpmii);
DWORD   WINAPI CheckMenuItem(HMENU hMenu, UINT uIDCheckItem, UINT uCheck);
BOOL    WINAPI TrackPopupMenu(HMENU hMenu, UINT uFlags, int x, int y,
                               int nReserved, HWND hWnd, PVOID prcRect);

/* Cursor / misc (additional) */
HCURSOR WINAPI GetCursor(void);
HCURSOR WINAPI SetCursor(HCURSOR hCursor);
HCURSOR WINAPI LoadCursorW(HINSTANCE hInstance, PCWSTR lpCursorName);
BOOL    WINAPI DestroyCursor(HCURSOR hCursor);
HANDLE  WINAPI LoadImageA(HINSTANCE hInst, PCSTR name, UINT type,
                           int cx, int cy, UINT fuLoad);
HANDLE  WINAPI LoadImageW(HINSTANCE hInst, PCWSTR name, UINT type,
                           int cx, int cy, UINT fuLoad);
HANDLE  WINAPI CopyImage(HANDLE image, UINT type, int cx, int cy, UINT flags);
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

void  user32_release_thread(DWORD pid, DWORD tid);
void  user32_release_process(DWORD pid);
WORD  user32_register_library_class(PCSTR class_name, DWORD style,
                                    int cb_cls_extra, int cb_wnd_extra,
                                    HBRUSH background, WNDPROC wndproc);
BOOL  user32_unregister_library_class(PCSTR class_name, WNDPROC wndproc);
BOOL  user32_release_icon(HICON icon);
int   user32_window_model_selftest(void);
int   user32_input_selftest(void);
int   user32_dialog_selftest(void);
BOOL  user32_accessibility_snapshot(HWND window,
                                    USER32_ACCESSIBLE_WINDOW_INFO *info);
UINT  user32_accessibility_children(HWND parent, HWND *children,
                                    UINT capacity);
PVOID user32_shim_init(void);
PVOID user32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal);

#endif /* USER32_SHIM_H */
