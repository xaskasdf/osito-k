#ifndef OSITO_MMIO_SHIM_H
#define OSITO_MMIO_SHIM_H
#include "nttypes.h"
typedef struct {DWORD id, size, type, offset, flags;} OSITO_MMCKINFO;
HANDLE WINAPI shim_mmioOpenA(PSTR name, PVOID info, DWORD flags);
HANDLE WINAPI shim_mmioOpenW(PWSTR name, PVOID info, DWORD flags);
UINT WINAPI shim_mmioClose(HANDLE handle, UINT flags);
LONG WINAPI shim_mmioRead(HANDLE handle, PSTR buffer, LONG count);
LONG WINAPI shim_mmioSeek(HANDLE handle, LONG offset, int origin);
UINT WINAPI shim_mmioDescend(HANDLE handle, OSITO_MMCKINFO *chunk,
                             const OSITO_MMCKINFO *parent, UINT flags);
UINT WINAPI shim_mmioAscend(HANDLE handle, OSITO_MMCKINFO *chunk, UINT flags);
/* Runs after all owner threads stop. NT handle cleanup closes underlying files. */
void mmio_process_cleanup(DWORD owner);
#endif
