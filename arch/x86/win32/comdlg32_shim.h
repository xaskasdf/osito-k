/*
 * OsitoK Windows Compatibility Layer — comdlg32.dll Shim
 * Common Dialogs stubs. Window.dll imports this.
 */

#ifndef COMDLG32_SHIM_H
#define COMDLG32_SHIM_H

#include "nttypes.h"

BOOL  WINAPI shim_GetOpenFileNameA(PVOID ofn);
BOOL  WINAPI shim_GetSaveFileNameA(PVOID ofn);
BOOL  WINAPI shim_ChooseColorA(PVOID cc);
BOOL  WINAPI shim_ChooseFontA(PVOID cf);
DWORD WINAPI shim_CommDlgExtendedError(void);
BOOL  WINAPI shim_PrintDlgA(PVOID pd);

PVOID comdlg32_shim_init(void);
PVOID comdlg32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal);

#endif /* COMDLG32_SHIM_H */
