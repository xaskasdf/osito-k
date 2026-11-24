/*
 * OsitoK Windows Compatibility Layer — comctl32.dll Shim
 * Common Controls stubs. Window.dll imports this.
 */

#ifndef COMCTL32_SHIM_H
#define COMCTL32_SHIM_H

#include "user32_shim.h"

typedef struct tagINITCOMMONCONTROLSEX {
    DWORD dwSize;
    DWORD dwICC;
} INITCOMMONCONTROLSEX, *LPINITCOMMONCONTROLSEX;

#define ICC_LISTVIEW_CLASSES     0x00000001
#define ICC_TREEVIEW_CLASSES     0x00000002
#define ICC_BAR_CLASSES          0x00000004
#define ICC_TAB_CLASSES          0x00000008
#define ICC_UPDOWN_CLASS         0x00000010
#define ICC_PROGRESS_CLASS       0x00000020
#define ICC_HOTKEY_CLASS         0x00000040
#define ICC_ANIMATE_CLASS        0x00000080
#define ICC_WIN95_CLASSES        0x000000FF
#define ICC_DATE_CLASSES         0x00000100
#define ICC_USEREX_CLASSES       0x00000200
#define ICC_COOL_CLASSES         0x00000400
#define ICC_INTERNET_CLASSES     0x00000800
#define ICC_PAGESCROLLER_CLASS   0x00001000
#define ICC_NATIVEFNTCTL_CLASS   0x00002000
#define ICC_STANDARD_CLASSES     0x00004000
#define ICC_LINK_CLASS           0x00008000

void  WINAPI shim_InitCommonControls(void);
BOOL  WINAPI shim_InitCommonControlsEx(const INITCOMMONCONTROLSEX *icc);
PVOID WINAPI shim_CreateStatusWindowA(LONG style, const char *text,
                                       PVOID hwnd, UINT id);
PVOID WINAPI shim_ImageList_Create(int cx, int cy, UINT flags,
                                    int initial, int grow);
BOOL WINAPI shim_SetWindowSubclass(HWND window, PVOID subclass_proc,
                                   ULONG_PTR subclass_id,
                                   ULONG_PTR reference_data);
BOOL WINAPI shim_GetWindowSubclass(HWND window, PVOID subclass_proc,
                                   ULONG_PTR subclass_id,
                                   ULONG_PTR *reference_data);
BOOL WINAPI shim_RemoveWindowSubclass(HWND window, PVOID subclass_proc,
                                      ULONG_PTR subclass_id);
LRESULT WINAPI shim_DefSubclassProc(HWND window, UINT message,
                                    WPARAM wparam, LPARAM lparam);
void comctl32_release_window(DWORD owner_pid, HWND window);
void comctl32_release_process(DWORD owner_pid);

PVOID comctl32_shim_init(void);
PVOID comctl32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal);

#endif /* COMCTL32_SHIM_H */
