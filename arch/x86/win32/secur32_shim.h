#ifndef OSITOK_WIN32_SECUR32_SHIM_H
#define OSITOK_WIN32_SECUR32_SHIM_H
#include "nttypes.h"
#include "win32_abi.h"
LONG WINAPI secur32_AcquireCredentialsHandleA(PCSTR principal, PCSTR package,
    ULONG use, PVOID logon, PVOID authentication, PVOID get_key,
    PVOID key_argument, PVOID credential, PVOID expiry);
LONG WINAPI secur32_InitializeSecurityContextA(PVOID credential, PVOID context,
    PCSTR target, ULONG request, ULONG reserved1, ULONG representation,
    PVOID input, ULONG reserved2, PVOID new_context, PVOID output,
    ULONG *attributes, PVOID expiry);
LONG WINAPI secur32_CompleteAuthToken(PVOID context, PVOID token);
LONG WINAPI secur32_DeleteSecurityContext(PVOID context);
LONG WINAPI secur32_FreeCredentialsHandle(PVOID credential);
PVOID secur32_resolve(const char *name, USHORT ordinal, BOOL by_ordinal);
const WIN32_EXPORT *secur32_abi_table(int *count);
#endif
