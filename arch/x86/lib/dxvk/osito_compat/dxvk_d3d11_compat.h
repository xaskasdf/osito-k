/* dxvk_d3d11_compat.h -- DXVK -> OsitoK shim for the d3d11/dxgi
 * frontend (W5.3).
 *
 * The util/spirv/dxbc/core libraries use osito_compat/dxvk_compat.h as
 * a force-include, paired with com_stub/IUnknown.h for the COM types.
 * The d3d11/dxgi frontend cannot use that because the WIDL-generated
 * MinGW d3d11/dxgi headers depend on the DXVK native windows shim
 * (dxvk/include/native/windows/) which has a different GUID layout
 * macro and __uuidof helper. Two GUID typedefs in one TU = link errors.
 *
 * So d3d11/dxgi TUs include the native windows shim instead, and this
 * tiny header fills in the few non-typedef bits dxvk_compat.h provided
 * that are still needed:
 *   - DXVK_NO_EXCEPTIONS (already on cmdline)
 *   - VK_*_EXTENSION_NAME macros for Win32 platforms (referenced by
 *     dxvk_extensions.h)
 *   - Sleep / aligned_malloc helpers
 *   - calling-convention macros (windows_base.h covers most)
 */

#ifndef DXVK_OSITOK_D3D11_COMPAT_H
#define DXVK_OSITOK_D3D11_COMPAT_H

#ifndef __OSITO_K__
#define __OSITO_K__ 1
#endif

#define DXVK_NO_EXCEPTIONS 1

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#ifdef __cplusplus
#include <atomic>
#include <limits>
#include <string_view>
#include <cstring>
#include <type_traits>
#include <algorithm>
#include <tuple>
#include <iterator>
#endif

#ifndef __forceinline
#define __forceinline inline __attribute__((always_inline))
#endif

#ifndef __cdecl
#define __cdecl
#endif
#ifndef __fastcall
#define __fastcall
#endif
#ifndef CALLBACK
#define CALLBACK
#endif
#ifndef APIENTRY
#define APIENTRY
#endif

#ifndef MAX_PATH
#define MAX_PATH 260
#endif

/* Vulkan Win32 / FSE extension name strings. dxvk_extensions.h
 * unconditionally instantiates a DxvkExt for each. The Khronos
 * canonical strings are correct on every platform. */
#ifndef VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME
#define VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME "VK_EXT_full_screen_exclusive"
#endif
#ifndef VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME
#define VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME "VK_KHR_external_memory_win32"
#endif
#ifndef VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME
#define VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME "VK_KHR_external_semaphore_win32"
#endif
#ifndef VK_KHR_WIN32_KEYED_MUTEX_EXTENSION_NAME
#define VK_KHR_WIN32_KEYED_MUTEX_EXTENSION_NAME "VK_KHR_win32_keyed_mutex"
#endif
#ifndef VK_KHR_WIN32_SURFACE_EXTENSION_NAME
#define VK_KHR_WIN32_SURFACE_EXTENSION_NAME "VK_KHR_win32_surface"
#endif

/* aligned alloc shim (same as dxvk_compat.h) */
#ifdef __cplusplus
extern "C" {
#endif

static inline void *_aligned_malloc(size_t size, size_t align) {
  void *p = NULL;
  if (align < sizeof(void *)) align = sizeof(void *);
  if (posix_memalign(&p, align, size) != 0) return NULL;
  return p;
}
static inline void _aligned_free(void *p) { free(p); }

long syscall(long number, ...);

struct dxvk_d3d11_compat_timespec { long tv_sec; long tv_nsec; };

static inline void Sleep(unsigned int ms) {
  struct dxvk_d3d11_compat_timespec ts;
  ts.tv_sec  = (long)(ms / 1000);
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  syscall(35, &ts, 0L);
}

static inline void OutputDebugStringA(const char *s) { (void)s; }

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* DXVK_OSITOK_D3D11_COMPAT_H */
