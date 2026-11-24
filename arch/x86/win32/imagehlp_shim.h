#ifndef OSITOK_WIN32_IMAGEHLP_SHIM_H
#define OSITOK_WIN32_IMAGEHLP_SHIM_H
#include "nttypes.h"
#include "win32_abi.h"
BOOL WINAPI imagehlp_ImageAddCertificate(HANDLE file, PVOID certificate, DWORD *index);
BOOL WINAPI imagehlp_ImageEnumerateCertificates(HANDLE file, WORD filter, DWORD *count,
                                               DWORD *indices, DWORD capacity);
BOOL WINAPI imagehlp_ImageRemoveCertificate(HANDLE file, DWORD index);
PVOID imagehlp_resolve(const char *name, USHORT ordinal, BOOL by_ordinal);
const WIN32_EXPORT *imagehlp_abi_table(int *count);
#endif
