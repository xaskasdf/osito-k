#ifndef ADVPACK_SHIM_H
#define ADVPACK_SHIM_H
#include "nttypes.h"
#include "win32_abi.h"
PVOID advpack_resolve(const char *name, USHORT ordinal, BOOL by_ordinal);
const WIN32_EXPORT *advpack_abi_table(int *count);
void advpack_release_process(DWORD process_id);
#endif
