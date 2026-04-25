/* dxvk_compat.h -- DXVK -> OsitoK compatibility shim (W5.0).
 *
 * Force-included via -include into every DXVK translation unit so the
 * upstream sources compile unmodified on bare-metal x86-64 with a hosted
 * libstdc++ but no Win32, no POSIX file I/O, and no exceptions.
 *
 * Strategy
 *   * Define DXVK_NO_EXCEPTIONS so DxvkError throw-sites become abort().
 *   * Provide Win32 ABI typedefs (HRESULT, DWORD, etc.) DXVK uses outside
 *     of d3d11 frontend code.
 *   * Replace _aligned_malloc / _aligned_free with posix_memalign / free.
 *   * Provide Sleep / QueryPerformanceCounter wrappers around our libc.
 *   * Stub OutputDebugStringA so log/log_debug.cpp can compile.
 *   * Force __stdcall / STDMETHODCALLTYPE / WINAPI to empty (we are SysV
 *     x86_64; DXVK's interface ABIs collapse to plain C calls).
 *
 * This header MUST stay C++-only safe — every DXVK TU is a .cpp.
 */

#ifndef DXVK_OSITOK_COMPAT_H
#define DXVK_OSITOK_COMPAT_H

#ifndef __OSITO_K__
#define __OSITO_K__ 1
#endif

#define DXVK_NO_EXCEPTIONS 1

/* C runtime essentials. Pulled before anything else so DXVK headers see
 * the standard fixed-width types. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
#include <atomic>
#include <cstdlib>
#endif

/* -- Calling-convention macros ---------------------------------------- */
/* We are SysV x86-64 freestanding; all the Win32 calling-convention
 * decorations DXVK sprinkles around its interface methods collapse to
 * empty. */
#ifndef __stdcall
#define __stdcall
#endif
#ifndef __cdecl
#define __cdecl
#endif
#ifndef __fastcall
#define __fastcall
#endif
#ifndef WINAPI
#define WINAPI
#endif
#ifndef CALLBACK
#define CALLBACK
#endif
#ifndef STDMETHODCALLTYPE
#define STDMETHODCALLTYPE
#endif
#ifndef APIENTRY
#define APIENTRY
#endif

#ifndef __forceinline
#define __forceinline inline __attribute__((always_inline))
#endif

/* -- Win32 ABI typedefs ----------------------------------------------- */
typedef int             BOOL;
typedef unsigned char   BYTE;
typedef unsigned short  WORD;
typedef unsigned int    DWORD;
typedef unsigned long   ULONG;
typedef long            LONG;
typedef long long       LONGLONG;
typedef unsigned long long  ULONGLONG;
typedef int             INT;
typedef unsigned int    UINT;
typedef unsigned int    UINT32;
typedef unsigned long long UINT64;
typedef int             HRESULT;

typedef void *          HANDLE;
typedef void *          HMODULE;
typedef void *          HINSTANCE;
typedef void *          HWND;
typedef void *          HMONITOR;
typedef void *          HDC;
typedef void *          HGLRC;
typedef void *          PVOID;
typedef void *          LPVOID;
typedef const void *    LPCVOID;
typedef char *          LPSTR;
typedef const char *    LPCSTR;
typedef wchar_t *       LPWSTR;
typedef const wchar_t * LPCWSTR;

typedef union _LARGE_INTEGER {
  struct { uint32_t LowPart; int32_t HighPart; };
  int64_t QuadPart;
} LARGE_INTEGER;

typedef union _ULARGE_INTEGER {
  struct { uint32_t LowPart; uint32_t HighPart; };
  uint64_t QuadPart;
} ULARGE_INTEGER;

#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif

/* -- Aligned alloc ---------------------------------------------------- */
#ifdef __cplusplus
extern "C" {
#endif

static inline void *_aligned_malloc(size_t size, size_t align) {
  void *p = NULL;
  /* posix_memalign requires alignment >= sizeof(void*) and a power of 2.
   * Fall back to plain malloc for the degenerate case. */
  if (align < sizeof(void *)) align = sizeof(void *);
  if (posix_memalign(&p, align, size) != 0) return NULL;
  return p;
}
static inline void _aligned_free(void *p) { free(p); }

/* -- Time + sleep wrappers ------------------------------------------- */
/* Forward decls — real impls live in OsitoK libc. */
long syscall(long number, ...);

/* nanosleep timespec */
struct dxvk_compat_timespec { long tv_sec; long tv_nsec; };

static inline void Sleep(unsigned int ms) {
  struct dxvk_compat_timespec ts;
  ts.tv_sec  = (long)(ms / 1000);
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  /* SYS_nanosleep == 35 on Linux/x86_64; OsitoK wires the same number. */
  syscall(35, &ts, 0L);
}

static inline BOOL QueryPerformanceFrequency(LARGE_INTEGER *freq) {
  if (freq) freq->QuadPart = 1000000ll; /* microsecond ticks */
  return TRUE;
}

static inline BOOL QueryPerformanceCounter(LARGE_INTEGER *count) {
  /* OsitoK exposes gettimeofday via syscall 96 (CLAUDE.md note). */
  struct { long tv_sec; long tv_usec; } tv = {0, 0};
  syscall(96, &tv, 0L);
  if (count) count->QuadPart = (int64_t)tv.tv_sec * 1000000ll + (int64_t)tv.tv_usec;
  return TRUE;
}

/* -- Debug string ----------------------------------------------------- */
static inline void OutputDebugStringA(const char *s) {
  (void)s;  /* no-op on OsitoK; serial logging routed elsewhere */
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

/* -- Misc Windows-isms DXVK uses ------------------------------------- */
#define MAX_PATH 260

/* DXVK refers to UUID/IID/GUID types via util_misc.h and com_include.h.
 * We define the minimal aliases here so transitively-included headers
 * stop yelling before com_stub/IUnknown.h is included.
 *
 * Forward decl of GUID — full definition lives in com_stub/IUnknown.h
 * which DXVK pulls in via com/com_include.h (we redirect via -I). */
#ifdef __cplusplus
struct GUID;
#else
struct GUID;
#endif

#endif /* DXVK_OSITOK_COMPAT_H */
