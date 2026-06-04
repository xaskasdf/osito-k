/*
 * OsitoK Windows Compatibility Layer — ole32.dll Shim
 * OLE/COM stubs. CoCreateInstance returns CLASS_E_CLASSNOTAVAILABLE.
 */

#include "ole32_shim.h"
#include "win32_abi.h"

extern void serial_puts(const char *s);

/* Use crt_malloc from msvcrt shim for CoTaskMemAlloc */
extern PVOID WINAPI crt_malloc(SIZE_T size);
extern void  WINAPI crt_free(PVOID ptr);

/* ── COM initialization ────────────────────────────────────── */

HRESULT WINAPI shim_CoInitialize(PVOID reserved)
{
    (void)reserved;
    serial_puts("[OLE32] CoInitialize (stub)\n");
    return S_OK;
}

HRESULT WINAPI shim_CoInitializeEx(PVOID reserved, DWORD coinit)
{
    (void)reserved; (void)coinit;
    serial_puts("[OLE32] CoInitializeEx (stub)\n");
    return S_OK;
}

void WINAPI shim_CoUninitialize(void)
{
    /* no-op */
}

/* ── CoCreateInstance ──────────────────────────────────────── */

HRESULT WINAPI shim_CoCreateInstance(PVOID rclsid, PVOID pUnkOuter,
                                     DWORD dwClsContext, PVOID riid,
                                     PVOID *ppv)
{
    (void)rclsid; (void)pUnkOuter; (void)dwClsContext; (void)riid;
    serial_puts("[OLE32] CoCreateInstance (stub — CLASS_E_CLASSNOTAVAILABLE)\n");
    if (ppv) *ppv = NULL;
    return CLASS_E_CLASSNOTAVAILABLE;
}

/* ── Task memory ───────────────────────────────────────────── */

PVOID WINAPI shim_CoTaskMemAlloc(SIZE_T size)
{
    return crt_malloc(size);
}

void WINAPI shim_CoTaskMemFree(PVOID ptr)
{
    crt_free(ptr);
}

/* ── OLE initialization ───────────────────────────────────── */

HRESULT WINAPI shim_OleInitialize(PVOID reserved)
{
    (void)reserved;
    serial_puts("[OLE32] OleInitialize (stub)\n");
    return S_OK;
}

void WINAPI shim_OleUninitialize(void)
{
    /* no-op */
}

/* ── GUID utilities ────────────────────────────────────────── */

HRESULT WINAPI shim_CoCreateGuid(PVOID guid)
{
    if (guid) {
        BYTE *p = (BYTE *)guid;
        for (int i = 0; i < 16; i++) p[i] = 0;
    }
    return S_OK;
}

int WINAPI shim_StringFromGUID2(PVOID guid, PVOID str, int max)
{
    (void)guid; (void)str; (void)max;
    return 0; /* return 0 = failure (not enough space / not implemented) */
}

/* ── Export table ──────────────────────────────────────────── */

typedef struct { const char *name; PVOID func; uint8_t argc; uint8_t cc; } SHIM_EXPORT;

static const SHIM_EXPORT ole32_exports[] = {
    { "CoInitialize",      (PVOID)shim_CoInitialize,     1, CC_STDCALL },
    { "CoInitializeEx",    (PVOID)shim_CoInitializeEx,   2, CC_STDCALL },
    { "CoUninitialize",    (PVOID)shim_CoUninitialize,   0, CC_STDCALL },
    { "CoCreateInstance",  (PVOID)shim_CoCreateInstance, 5, CC_STDCALL },
    { "CoTaskMemAlloc",    (PVOID)shim_CoTaskMemAlloc,   1, CC_STDCALL },
    { "CoTaskMemFree",     (PVOID)shim_CoTaskMemFree,    1, CC_STDCALL },
    { "OleInitialize",     (PVOID)shim_OleInitialize,    1, CC_STDCALL },
    { "OleUninitialize",   (PVOID)shim_OleUninitialize,  0, CC_STDCALL },
    { "CoCreateGuid",      (PVOID)shim_CoCreateGuid,     1, CC_STDCALL },
    { "StringFromGUID2",   (PVOID)shim_StringFromGUID2,  3, CC_STDCALL },
    { NULL, NULL, 0, CC_STDCALL }
};

const WIN32_EXPORT *ole32_abi_table(int *count) {
    *count = (int)(sizeof(ole32_exports)/sizeof(ole32_exports[0]));
    return (const WIN32_EXPORT *)ole32_exports;
}

static int ole_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

PVOID ole32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    (void)ordinal;
    if (by_ordinal) return NULL;
    for (int i = 0; ole32_exports[i].name; i++) {
        if (ole_strcmp(func_name, ole32_exports[i].name) == 0)
            return ole32_exports[i].func;
    }
    return NULL;
}

PVOID ole32_shim_init(void)
{
    serial_puts("[OLE32] ole32.dll shim initialized\n");
    return (PVOID)ole32_exports;
}
