#ifndef OSITO_RPCRT4_SHIM_H
#define OSITO_RPCRT4_SHIM_H
#include "nttypes.h"
#include "win32_abi.h"
PVOID rpcrt4_resolve(const char *name, USHORT ordinal, BOOL by_ordinal);
const WIN32_EXPORT *rpcrt4_abi_table(int *count);
#endif
