#ifndef OSITO_WIN32_PROPERTYSHEET_H
#define OSITO_WIN32_PROPERTYSHEET_H
#include "user32_shim.h"
HANDLE WINAPI shim_CreatePropertySheetPageA(PCVOID descriptor);
BOOL WINAPI shim_DestroyPropertySheetPage(HANDLE page);
LONG_PTR WINAPI shim_PropertySheetA(PCVOID descriptor);
void propertysheet_release_window(DWORD owner_pid, HWND window);
void propertysheet_release_process(DWORD owner_pid);
#endif
