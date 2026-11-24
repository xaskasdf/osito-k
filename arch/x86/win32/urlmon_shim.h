#ifndef URLMON_SHIM_H
#define URLMON_SHIM_H
#include "nttypes.h"
#include "win32_abi.h"
PVOID urlmon_resolve(const char *name, USHORT ordinal, BOOL by_ordinal);
const WIN32_EXPORT *urlmon_abi_table(int *count);
void urlmon_release_process(DWORD process_id);
/* Kernel adapter for installer engines. URL/path still use guest buffers.
 * Event slots match IBindStatusCallback; context lives through the sync call.
 * This entry is deliberately absent from the guest DLL export table. */
typedef LONG (*URLMON_INTERNAL_CALLBACK)(PVOID context, unsigned slot,
    ULONG_PTR a, ULONG_PTR b, ULONG_PTR c, ULONG_PTR d);
LONG urlmon_download_to_file_internal(PCWSTR url, PCWSTR destination,
    URLMON_INTERNAL_CALLBACK callback, PVOID context);
#endif
