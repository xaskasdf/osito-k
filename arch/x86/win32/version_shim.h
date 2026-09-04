/* version.dll surface backed by PE RT_VERSION resources. */

#ifndef VERSION_SHIM_H
#define VERSION_SHIM_H

#include "nttypes.h"

DWORD WINAPI shim_GetFileVersionInfoSizeA(PCSTR filename, DWORD *handle);
DWORD WINAPI shim_GetFileVersionInfoSizeW(PCWSTR filename, DWORD *handle);
BOOL WINAPI shim_GetFileVersionInfoA(PCSTR filename, DWORD handle,
                                     DWORD length, PVOID data);
BOOL WINAPI shim_GetFileVersionInfoW(PCWSTR filename, DWORD handle,
                                     DWORD length, PVOID data);
BOOL WINAPI shim_VerQueryValueA(PCVOID block, PCSTR sub_block,
                                PVOID *buffer, UINT *length);
BOOL WINAPI shim_VerQueryValueW(PCVOID block, PCWSTR sub_block,
                                PVOID *buffer, UINT *length);

PVOID version_shim_init(void);
PVOID version_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal);

#endif /* VERSION_SHIM_H */
