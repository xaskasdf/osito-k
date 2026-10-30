/* Unrelated retained exports in ASan GDI host tests must never execute.
 * This file is test-only; physical KMT has separate native suites. */
#include "../include/types.h"
#include "../win32/d3dkmt.h"
extern void abort(void) __attribute__((noreturn));
bool win32_resolve_file_path(PCSTR p,char out[260]) { (void)p;(void)out;abort(); }
BOOL opengl32_has_render_backend(void) { abort(); }
__attribute__((weak)) int win32_user_range_readable(PCVOID p,SIZE_T n,BOOL mode)
{ (void)p;(void)n;(void)mode;abort(); }
__attribute__((weak)) int win32_user_range_writable(PVOID p,SIZE_T n,BOOL mode)
{ (void)p;(void)n;(void)mode;abort(); }
#define UNREACHED_KMT(name,type) NTSTATUS WINAPI name(type p) { (void)p;abort(); }
UNREACHED_KMT(D3DKMTLock,PVOID)
UNREACHED_KMT(D3DKMTCreateContext,PVOID)
UNREACHED_KMT(D3DKMTRender,PVOID)
UNREACHED_KMT(D3DKMTCreateSynchronizationObject,PVOID)
UNREACHED_KMT(D3DKMTCreateSynchronizationObject2,PVOID)
UNREACHED_KMT(D3DKMTDestroySynchronizationObject,PCVOID)
UNREACHED_KMT(D3DKMTWaitForSynchronizationObject,PCVOID)
UNREACHED_KMT(D3DKMTWaitForSynchronizationObject2,PCVOID)
UNREACHED_KMT(D3DKMTSignalSynchronizationObject,PCVOID)
UNREACHED_KMT(D3DKMTSignalSynchronizationObject2,PCVOID)
UNREACHED_KMT(D3DKMTSignalSynchronizationObjectFromCpu,PCVOID)
UNREACHED_KMT(D3DKMTWaitForSynchronizationObjectFromCpu,PCVOID)
UNREACHED_KMT(D3DKMTWaitForSynchronizationObjectFromGpu,PCVOID)
UNREACHED_KMT(D3DKMTSignalSynchronizationObjectFromGpu,PCVOID)
UNREACHED_KMT(D3DKMTSignalSynchronizationObjectFromGpu2,PCVOID)
UNREACHED_KMT(D3DKMTDestroyContext,PCVOID)
UNREACHED_KMT(D3DKMTUnlock,PCVOID)
UNREACHED_KMT(D3DKMTEnumAdapters,PVOID)
UNREACHED_KMT(D3DKMTEnumAdapters2,PVOID)
UNREACHED_KMT(D3DKMTOpenAdapterFromLuid,PVOID)
UNREACHED_KMT(D3DKMTOpenAdapterFromDeviceName,PVOID)
UNREACHED_KMT(D3DKMTOpenAdapterFromGdiDisplayName,PVOID)
UNREACHED_KMT(D3DKMTOpenAdapterFromHdc,PVOID)
UNREACHED_KMT(D3DKMTCloseAdapter,PCVOID)
UNREACHED_KMT(D3DKMTQueryAdapterInfo,PCVOID)
UNREACHED_KMT(D3DKMTCreateDevice,PVOID)
UNREACHED_KMT(D3DKMTDestroyDevice,PCVOID)
UNREACHED_KMT(D3DKMTCreateAllocation,PVOID)
UNREACHED_KMT(D3DKMTCreateAllocation2,PVOID)
UNREACHED_KMT(D3DKMTDestroyAllocation,PCVOID)
UNREACHED_KMT(D3DKMTDestroyAllocation2,PCVOID)
UNREACHED_KMT(D3DKMTQueryAllocationResidency,PCVOID)
UNREACHED_KMT(D3DKMTSetAllocationPriority,PCVOID)
UNREACHED_KMT(D3DKMTGetContextSchedulingPriority,PVOID)
UNREACHED_KMT(D3DKMTSetContextSchedulingPriority,PCVOID)
UNREACHED_KMT(D3DKMTGetDeviceState,PVOID)
UNREACHED_KMT(D3DKMTMarkDeviceAsError,PCVOID)
#undef UNREACHED_KMT
