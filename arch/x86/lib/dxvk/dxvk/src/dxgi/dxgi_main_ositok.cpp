#include "dxgi_factory.h"
#include "dxgi_include.h"

extern "C" int printf(const char*, ...);  /* DIAG */

namespace dxvk {

  HRESULT createDxgiFactory(UINT Flags, REFIID riid, void **ppFactory) {
    /* OsitoK W5.3: -fno-exceptions, so the upstream try/catch around
     * `new DxgiFactory(Flags)` collapses to direct construction. The
     * DxvkError throw sites in dxvk core/d3d11/dxgi were rewritten to
     * abort_ositok in W5.2 + W5.3-t1, so a thrown error is now a
     * kernel-side abort with a logged message. */
    DxgiFactory* rawf = new DxgiFactory(Flags);
    printf("[DBG cdf] new DxgiFactory done raw=%p\n", (void*)rawf);
    Com<DxgiFactory> factory = rawf;
    printf("[DBG cdf] Com assigned; QueryInterface...\n");
    HRESULT hr = factory->QueryInterface(riid, ppFactory);
    printf("[DBG cdf] QueryInterface done hr=0x%x\n", (unsigned)hr);

    if (FAILED(hr))
      return hr;

    return S_OK;
  }
}

extern "C" {
  DLLEXPORT HRESULT __stdcall CreateDXGIFactory2(UINT Flags, REFIID riid, void **ppFactory) {
    dxvk::Logger::warn("CreateDXGIFactory2: Ignoring flags");
    return dxvk::createDxgiFactory(Flags, riid, ppFactory);
  }

  DLLEXPORT HRESULT __stdcall CreateDXGIFactory1(REFIID riid, void **ppFactory) {
    return dxvk::createDxgiFactory(0, riid, ppFactory);
  }
  
  DLLEXPORT HRESULT __stdcall CreateDXGIFactory(REFIID riid, void **ppFactory) {
    return dxvk::createDxgiFactory(0, riid, ppFactory);
  }

  DLLEXPORT HRESULT __stdcall DXGIDeclareAdapterRemovalSupport() {
    static bool enabled = false;

    if (std::exchange(enabled, true))
      return 0x887a0036; // DXGI_ERROR_ALREADY_EXISTS;

    dxvk::Logger::warn("DXGIDeclareAdapterRemovalSupport: Stub");
    return S_OK;
  }

  DLLEXPORT HRESULT __stdcall DXGIGetDebugInterface1(UINT Flags, REFIID riid, void **ppDebug) {
    static bool errorShown = false;

    if (!std::exchange(errorShown, true))
      dxvk::Logger::warn("DXGIGetDebugInterface1: Stub");

    return E_NOINTERFACE;
  }

}
