/*
 * OsitoK Windows Compatibility Layer — comctl32.dll Shim
 * Common Controls stubs. Window.dll imports this.
 */

#ifndef COMCTL32_SHIM_H
#define COMCTL32_SHIM_H

#include "user32_shim.h"

void  WINAPI shim_InitCommonControls(void);
BOOL  WINAPI shim_InitCommonControlsEx(PVOID icc);
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
void comctl32_release_process(DWORD owner_pid);

PVOID comctl32_shim_init(void);
PVOID comctl32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal);

#endif /* COMCTL32_SHIM_H */
