#ifndef OSITOK_MSI_SHIM_H
#define OSITOK_MSI_SHIM_H
#include "nttypes.h"
#include "win32_abi.h"
PVOID msi_resolve(const char *name, USHORT ordinal, BOOL by_ordinal);
const WIN32_EXPORT *msi_abi_table(int *count);
void msi_release_process(DWORD owner);
#endif
