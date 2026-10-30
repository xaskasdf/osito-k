/*
 * OsitoK Windows Compatibility Layer — shell32.dll Shim
 *
 * Shell services backed by the generic Win32 process and filesystem layers.
 */

#ifndef SHELL32_SHIM_H
#define SHELL32_SHIM_H

#include "nttypes.h"

typedef HANDLE HWND;
typedef HANDLE HINSTANCE;

/* ShellExecute */
HINSTANCE WINAPI ShellExecuteA(HWND hwnd, PCSTR lpOperation, PCSTR lpFile,
                                PCSTR lpParameters, PCSTR lpDirectory, int nShowCmd);
HINSTANCE WINAPI ShellExecuteW(HWND hwnd, PCWSTR lpOperation, PCWSTR lpFile,
                                PCWSTR lpParameters, PCWSTR lpDirectory, int nShowCmd);
BOOL WINAPI ShellExecuteExA(PVOID information);
BOOL WINAPI ShellExecuteExW(PVOID information);
PVOID WINAPI CommandLineToArgvW(PCWSTR lpCmdLine, int *pNumArgs);
BOOL WINAPI Shell_NotifyIconA(DWORD message, PVOID data);
BOOL WINAPI Shell_NotifyIconW(DWORD message, PVOID data);

/* Shared filesystem PIDLs for the native folder picker. Buffers have 260
 * WCHARs; the creator takes trusted internal text, the parser validates PIDLs. */
PVOID shell32_make_path_pidl(PCWSTR path);
BOOL shell32_pidl_path(PCVOID pidl, PWSTR path);

/* Shim init / resolve */
PVOID shell32_shim_init(void);
void shell32_release_process(DWORD process_id);
PVOID shell32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal);
PVOID shlwapi_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal);

#endif /* SHELL32_SHIM_H */
