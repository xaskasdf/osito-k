#ifndef OSITO_CABINET_SHIM_H
#define OSITO_CABINET_SHIM_H
#include "nttypes.h"
#include "win32_abi.h"
PVOID cabinet_resolve(const char *name, USHORT ordinal, BOOL by_ordinal);
const WIN32_EXPORT *cabinet_abi_table(int *count);
void cabinet_release_process(DWORD owner);
#endif
