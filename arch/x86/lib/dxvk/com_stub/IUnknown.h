/* IUnknown.h -- minimal COM emulation for OsitoK DXVK build.
 *
 * Provides the GUID/IUnknown contract DXVK uses for in-process COM-style
 * refcounted interfaces. We have no system COM here; everything compiles
 * down to plain vtables. This header is intentionally tiny — no Windows
 * COM marshaling, no apartment threads, no aggregation.
 *
 * Force-included by both windows.h shim and unknwn.h shim, both of which
 * live alongside this file in com_stub/.
 */

#ifndef OSITOK_DXVK_IUNKNOWN_H
#define OSITOK_DXVK_IUNKNOWN_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GUID layout matches Windows. */
typedef struct _GUID {
  uint32_t Data1;
  uint16_t Data2;
  uint16_t Data3;
  uint8_t  Data4[8];
} GUID;

typedef GUID IID;
typedef GUID CLSID;
typedef GUID UUID;

#ifdef __cplusplus
}  /* extern "C" */
#endif

/* GUID equality.  Wine and the Windows SDK provide IsEqualGUID + the
 * `operator==` overload; we provide both.  Defined inline in the header
 * because each TU is its own namespace as far as the linker cares (this
 * is a static inline). */
#ifdef __cplusplus
static inline bool operator==(const GUID& a, const GUID& b) {
  return a.Data1 == b.Data1 && a.Data2 == b.Data2 && a.Data3 == b.Data3
      && a.Data4[0] == b.Data4[0] && a.Data4[1] == b.Data4[1]
      && a.Data4[2] == b.Data4[2] && a.Data4[3] == b.Data4[3]
      && a.Data4[4] == b.Data4[4] && a.Data4[5] == b.Data4[5]
      && a.Data4[6] == b.Data4[6] && a.Data4[7] == b.Data4[7];
}
static inline bool operator!=(const GUID& a, const GUID& b) {
  return !(a == b);
}
static inline bool IsEqualGUID(const GUID& a, const GUID& b) {
  return a == b;
}
#endif

/* In Windows headers, REFIID/REFGUID are `const IID&` in C++ and a
 * pointer in C. DXVK is C++ throughout. */
#ifdef __cplusplus
typedef const GUID& REFGUID;
typedef const GUID& REFIID;
typedef const GUID& REFCLSID;
#else
typedef const GUID* REFGUID;
typedef const GUID* REFIID;
typedef const GUID* REFCLSID;
#endif

#ifndef ULONG
typedef unsigned int ULONG;  /* Windows ABI: 32-bit even on LP64 */
#endif
#ifndef HRESULT
typedef int HRESULT;
#endif

/* Standard HRESULT facility codes used by DXVK. */
#define S_OK            ((HRESULT)0)
#define S_FALSE         ((HRESULT)1)
#define E_FAIL          ((HRESULT)0x80004005)
#define E_INVALIDARG    ((HRESULT)0x80070057)
#define E_NOINTERFACE   ((HRESULT)0x80004002)
#define E_OUTOFMEMORY   ((HRESULT)0x8007000E)
#define E_NOTIMPL       ((HRESULT)0x80004001)
#define E_POINTER       ((HRESULT)0x80004003)
#define DXGI_ERROR_NOT_FOUND ((HRESULT)0x887A0002)
#define DXGI_ERROR_MORE_DATA ((HRESULT)0x887A0003)

#define SUCCEEDED(hr)   (((HRESULT)(hr)) >= 0)
#define FAILED(hr)      (((HRESULT)(hr)) < 0)

/* DEFINE_GUID — the canonical C-flavor macro. */
#define DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    extern "C" const GUID name = { (l), (w1), (w2), { (b1), (b2), (b3), (b4), (b5), (b6), (b7), (b8) } }

/* MIDL_INTERFACE -- DXVK uses this on interface decls; with __uuidof
 * disabled we emit it as a plain `struct`. */
#define MIDL_INTERFACE(x) struct

/* DECLARE_INTERFACE -- ditto. */
#define DECLSPEC_UUID(x)
#define DECLSPEC_NOVTABLE
#define DECLSPEC_ALIGN(x) __attribute__((aligned(x)))

/* IUnknown — the root of every COM interface.
 *
 * DXVK derives ID3D11Device, ID3D11DeviceContext etc. from this and
 * uses Com<T> smart pointers in src/util/com/com_pointer.h that just
 * call AddRef / Release. */
#ifdef __cplusplus
struct IUnknown {
  virtual HRESULT QueryInterface(REFIID riid, void** ppvObject) = 0;
  virtual ULONG   AddRef() = 0;
  virtual ULONG   Release() = 0;
  virtual ~IUnknown() = default;
};
#else
typedef struct IUnknown IUnknown;
#endif

/* __uuidof support.
 *
 * MSVC has a builtin; we emulate via per-class trait. DXVK calls
 * __uuidof(IUnknown) in com_private_data.cpp. We provide a default
 * specialisation returning the well-known IID_IUnknown. Other
 * specialisations can be added in the d3d11/dxgi headers later. */
#ifdef __cplusplus
namespace osito_uuid {
  template<typename T> struct uuid_of_t;

  template<typename T>
  static inline const GUID& uuid_of() {
    return uuid_of_t<T>::value;
  }
}

#define __uuidof(T) (::osito_uuid::uuid_of<T>())

/* IID_IUnknown = 00000000-0000-0000-C000-000000000046 */
namespace osito_uuid {
  template<>
  struct uuid_of_t<IUnknown> {
    static constexpr GUID value = {
      0x00000000, 0x0000, 0x0000,
      { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 }
    };
  };
}
#endif /* __cplusplus */

/* Standard Windows-isms DXVK touches but does not depend on. */
#define INTERFACE struct
#define STDMETHOD(x) virtual HRESULT x
#define STDMETHOD_(t,x) virtual t x
#define PURE = 0
#define THIS_
#define THIS

#endif /* OSITOK_DXVK_IUNKNOWN_H */
