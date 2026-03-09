/*
 * OsitoK Windows Compatibility Layer — shell32.dll Shim
 *
 * Minimal shell API stubs for UT99 compatibility.
 * ShellExecuteA/W are stubbed — no actual process launching.
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

/* Shim init / resolve */
PVOID shell32_shim_init(void);
PVOID shell32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal);

#endif /* SHELL32_SHIM_H */
