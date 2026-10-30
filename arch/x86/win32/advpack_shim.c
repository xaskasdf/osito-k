/* Legacy setup helpers backed by the same token model as advapi32/shell32. */
#include "advpack_shim.h"
#include "advapi32_shim.h"
#include "kernel32_shim.h"

/* Native advpack caches a positive result for the DLL/process lifetime. This
 * legacy advisory result must not be used by the actual access-check engine. */
static DWORD advpack_admin_owners[64];

static BOOL WINAPI shim_IsNTAdmin(DWORD reserved, DWORD *reserved_pointer)
{
    (void)reserved;
    (void)reserved_pointer; /* Reserved, neither required nor an output. */
    DWORD owner = GetCurrentProcessId();
    for (unsigned i = 0; i < 64; i++)
        if (__atomic_load_n(&advpack_admin_owners[i], __ATOMIC_ACQUIRE) == owner)
            return TRUE;
    DWORD saved_error = GetLastError();
    BOOL result = advapi32_effective_is_admin();
    if (result) for (unsigned i = 0; i < 64; i++) {
        DWORD expected = 0;
        if (__atomic_compare_exchange_n(&advpack_admin_owners[i], &expected, owner,
                FALSE, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) || expected == owner) break;
    }
    SetLastError(saved_error);
    return result;
}

void advpack_release_process(DWORD process_id)
{
    for (unsigned i = 0; i < 64; i++) {
        DWORD expected = process_id;
        __atomic_compare_exchange_n(&advpack_admin_owners[i], &expected, 0,
                                     FALSE, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
}

static const WIN32_EXPORT advpack_exports[] = {
    WX_STD("IsNTAdmin", shim_IsNTAdmin, 2),
    {NULL, NULL, 0, CC_STDCALL}
};
const WIN32_EXPORT *advpack_abi_table(int *count)
{
    *count = sizeof(advpack_exports) / sizeof(advpack_exports[0]);
    return advpack_exports;
}
PVOID advpack_resolve(const char *name, USHORT ordinal, BOOL by_ordinal)
{
    (void)ordinal;
    if (!by_ordinal && name)
        for (unsigned i = 0; advpack_exports[i].name; i++)
            if (!strcmp(name, advpack_exports[i].name)) return advpack_exports[i].func;
    return NULL;
}
