/* Minimal dxgi.dll compatibility surface for software-rendering fallbacks. */

#ifndef DXGI_SHIM_H
#define DXGI_SHIM_H

#include "nttypes.h"

typedef LONG HRESULT;

HRESULT WINAPI shim_CreateDXGIFactory(PCVOID riid, PVOID *factory);
HRESULT WINAPI shim_CreateDXGIFactory1(PCVOID riid, PVOID *factory);
HRESULT WINAPI shim_CreateDXGIFactory2(UINT flags, PCVOID riid, PVOID *factory);
HRESULT WINAPI shim_DXGIGetDebugInterface1(UINT flags, PCVOID riid, PVOID *debug);
HRESULT WINAPI shim_DXGIDeclareAdapterRemovalSupport(void);

PVOID dxgi_shim_init(void);
PVOID dxgi_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal);
int dxgi_selftest(void);

#endif /* DXGI_SHIM_H */
