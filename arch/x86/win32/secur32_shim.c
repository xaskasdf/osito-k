/* SSPI entry points for an OS with no installed security support providers.
 * This is capability/failure handling, not NTLM, Kerberos or Schannel support.
 * No credentials or security contexts are issued by this backend. */
#include "secur32_shim.h"

#define SEC_E_INVALID_HANDLE ((LONG)0x80090301U)
#define SEC_E_SECPKG_NOT_FOUND ((LONG)0x80090305U)

LONG WINAPI secur32_AcquireCredentialsHandleA(PCSTR principal, PCSTR package,
    ULONG use, PVOID logon, PVOID authentication, PVOID get_key,
    PVOID key_argument, PVOID credential, PVOID expiry)
{
    (void)principal;(void)package;(void)use;(void)logon;(void)authentication;
    (void)get_key;(void)key_argument;(void)credential;(void)expiry;
    /* Package discovery precedes creating/mutating a credential or expiry. */
    return SEC_E_SECPKG_NOT_FOUND;
}

LONG WINAPI secur32_InitializeSecurityContextA(PVOID credential, PVOID context,
    PCSTR target, ULONG request, ULONG reserved1, ULONG representation,
    PVOID input, ULONG reserved2, PVOID new_context, PVOID output,
    ULONG *attributes, PVOID expiry)
{
    (void)credential;(void)context;(void)target;(void)request;(void)reserved1;
    (void)representation;(void)input;(void)reserved2;(void)new_context;
    (void)output;(void)attributes;(void)expiry;
    return SEC_E_INVALID_HANDLE;
}

LONG WINAPI secur32_CompleteAuthToken(PVOID context, PVOID token)
{
    (void)context;(void)token;
    return SEC_E_INVALID_HANDLE;
}

LONG WINAPI secur32_DeleteSecurityContext(PVOID context)
{
    /* After package discovery, native SSPI accepts empty context tags 0/1
     * even with a nonzero dwUpper. Read the guest's pointer-sized field only. */
    unsigned bytes = g_compat32_mode ? 4 : 8;
    if (context && win32_user_range_readable(context, bytes, g_compat32_mode)) {
        const BYTE *data = context;
        unsigned nonzero = data[0] & ~1U;
        for (unsigned i = 1; i < bytes; i++) nonzero |= data[i];
        if (!nonzero) return 0;
    }
    return SEC_E_INVALID_HANDLE;
}

LONG WINAPI secur32_FreeCredentialsHandle(PVOID credential)
{
    (void)credential;
    return SEC_E_INVALID_HANDLE;
}

static const WIN32_EXPORT secur32_exports[] = {
    {"AcquireCredentialsHandleA",(PVOID)secur32_AcquireCredentialsHandleA,9,CC_STDCALL},
    {"InitializeSecurityContextA",(PVOID)secur32_InitializeSecurityContextA,12,CC_STDCALL},
    {"CompleteAuthToken",(PVOID)secur32_CompleteAuthToken,2,CC_STDCALL},
    {"DeleteSecurityContext",(PVOID)secur32_DeleteSecurityContext,1,CC_STDCALL},
    {"FreeCredentialsHandle",(PVOID)secur32_FreeCredentialsHandle,1,CC_STDCALL},
};

const WIN32_EXPORT *secur32_abi_table(int *count)
{
    *count = sizeof(secur32_exports) / sizeof(secur32_exports[0]);
    return secur32_exports;
}

PVOID secur32_resolve(const char *name, USHORT ordinal, BOOL by_ordinal)
{
    (void)ordinal;
    if (!name || by_ordinal) return NULL;
    for (unsigned i = 0; i < sizeof(secur32_exports) / sizeof(secur32_exports[0]); i++) {
        const char *a = name, *b = secur32_exports[i].name;
        while (*a && *a == *b) { a++;b++; }
        if (!*a && !*b) return secur32_exports[i].func;
    }
    return NULL;
}
