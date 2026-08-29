/*
 * OsitoK Windows Compatibility Layer — ole32.dll Shim
 * OLE/COM infrastructure stubs. Window.dll imports this.
 */

#ifndef OLE32_SHIM_H
#define OLE32_SHIM_H

#include "nttypes.h"

typedef LONG HRESULT;

#ifndef S_OK
#define S_OK                        ((HRESULT)0)
#endif
#define CLASS_E_CLASSNOTAVAILABLE   ((HRESULT)0x80040111)

/* GUID defined in nttypes.h */

HRESULT WINAPI shim_CoInitialize(PVOID reserved);
HRESULT WINAPI shim_CoInitializeEx(PVOID reserved, DWORD coinit);
void    WINAPI shim_CoUninitialize(void);
HRESULT WINAPI shim_CoCreateInstance(PVOID rclsid, PVOID pUnkOuter,
                                     DWORD dwClsContext, PVOID riid,
                                     PVOID *ppv);
HRESULT WINAPI shim_CoGetMalloc(DWORD context, PVOID *allocator);
PVOID   WINAPI shim_CoTaskMemAlloc(SIZE_T size);
void    WINAPI shim_CoTaskMemFree(PVOID ptr);
HRESULT WINAPI shim_OleInitialize(PVOID reserved);
void    WINAPI shim_OleUninitialize(void);
HRESULT WINAPI shim_RegisterDragDrop(HANDLE hwnd, PVOID drop_target);
HRESULT WINAPI shim_RevokeDragDrop(HANDLE hwnd);
HRESULT WINAPI shim_CoCreateGuid(PVOID guid);
int     WINAPI shim_StringFromGUID2(PVOID guid, PVOID str, int max);
PWSTR   WINAPI shim_SysAllocString(PCWSTR source);
PWSTR   WINAPI shim_SysAllocStringLen(PCWSTR source, UINT length);
void    WINAPI shim_SysFreeString(PWSTR string);
UINT    WINAPI shim_SysStringLen(PCWSTR string);
UINT    WINAPI shim_SysStringByteLen(PCWSTR string);
PWSTR   WINAPI shim_SysAllocStringByteLen(PCSTR source, UINT length);
void    WINAPI shim_VariantInit(PVOID variant);
HRESULT WINAPI shim_VariantClear(PVOID variant);
HRESULT WINAPI shim_VariantCopy(PVOID destination, PCVOID source);

PVOID ole32_shim_init(void);
PVOID ole32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal);
PVOID oleaut32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal);

#endif /* OLE32_SHIM_H */
