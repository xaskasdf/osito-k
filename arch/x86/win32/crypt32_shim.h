#ifndef CRYPT32_SHIM_H
#define CRYPT32_SHIM_H

#include "nttypes.h"
#include "win32_abi.h"

PVOID crypt32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal);
const WIN32_EXPORT *crypt32_abi_table(int *count);
PVOID ncrypt_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal);
const WIN32_EXPORT *ncrypt_abi_table(int *count);
PVOID wintrust_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal);
const WIN32_EXPORT *wintrust_abi_table(int *count);

#endif
