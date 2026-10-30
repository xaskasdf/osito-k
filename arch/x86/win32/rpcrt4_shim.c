/* UUID strings used by component registration. RPC transports are not
 * implemented here; only these explicitly described exports are resolved. */
#include "rpcrt4_shim.h"
#include "msvcrt_shim.h"
#include "kernel32_shim.h"

static PVOID rpc_string_pointer(PCVOID slot)
{
    return g_compat32_mode ? (PVOID)(uintptr_t)*(const DWORD *)slot
                           : *(PVOID const *)slot;
}

static void rpc_store_string_pointer(PVOID slot, PVOID value)
{
    if (g_compat32_mode) *(DWORD *)slot = (DWORD)(uintptr_t)value;
    else *(PVOID *)slot = value;
}

static void rpc_uuid_string(const GUID *uuid, char text[37])
{
    static const char hex[] = "0123456789abcdef";
    const BYTE bytes[] = {
        uuid->Data1 >> 24, uuid->Data1 >> 16, uuid->Data1 >> 8, uuid->Data1,
        uuid->Data2 >> 8, uuid->Data2, uuid->Data3 >> 8, uuid->Data3,
        uuid->Data4[0], uuid->Data4[1], uuid->Data4[2], uuid->Data4[3],
        uuid->Data4[4], uuid->Data4[5], uuid->Data4[6], uuid->Data4[7]
    };
    unsigned at = 0;
    for (unsigned i = 0; i < sizeof(bytes); i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) text[at++] = '-';
        text[at++] = hex[bytes[i] >> 4];
        text[at++] = hex[bytes[i] & 15];
    }
    text[at] = 0;
}

static DWORD WINAPI shim_UuidToStringA(const GUID *uuid, PVOID output)
{
    if (!uuid || !output) return 87; /* Invalid pointers are outside the API contract. */
    DWORD saved_error = GetLastError();
    char *text = crt_malloc(37);
    if (text) rpc_uuid_string(uuid, text);
    rpc_store_string_pointer(output, text);
    SetLastError(saved_error);
    return text ? 0 : 14; /* RPC_S_OK / RPC_S_OUT_OF_MEMORY */
}

static DWORD WINAPI shim_UuidToStringW(const GUID *uuid, PVOID output)
{
    if (!uuid || !output) return 87;
    DWORD saved_error = GetLastError();
    WCHAR *text = crt_malloc(37 * sizeof(WCHAR));
    if (text) {
        char narrow[37];
        rpc_uuid_string(uuid, narrow);
        for (unsigned i = 0; i < 37; i++) text[i] = (BYTE)narrow[i];
    }
    rpc_store_string_pointer(output, text);
    SetLastError(saved_error);
    return text ? 0 : 14;
}

static DWORD WINAPI shim_RpcStringFree(PVOID output)
{
    if (!output) return 87;
    DWORD saved_error = GetLastError();
    crt_free(rpc_string_pointer(output));
    rpc_store_string_pointer(output, NULL);
    SetLastError(saved_error);
    return 0;
}

static const WIN32_EXPORT rpcrt4_exports[] = {
    WX_STD("UuidToStringA", shim_UuidToStringA, 2),
    WX_STD("UuidToStringW", shim_UuidToStringW, 2),
    WX_STD("RpcStringFreeA", shim_RpcStringFree, 1),
    WX_STD("RpcStringFreeW", shim_RpcStringFree, 1),
    {NULL, NULL, 0, CC_STDCALL}
};

const WIN32_EXPORT *rpcrt4_abi_table(int *count)
{
    *count = sizeof(rpcrt4_exports) / sizeof(rpcrt4_exports[0]);
    return rpcrt4_exports;
}

PVOID rpcrt4_resolve(const char *name, USHORT ordinal, BOOL by_ordinal)
{
    (void)ordinal;
    if (!by_ordinal && name)
        for (unsigned i = 0; rpcrt4_exports[i].name; i++)
            if (!strcmp(name, rpcrt4_exports[i].name)) return rpcrt4_exports[i].func;
    return NULL;
}
