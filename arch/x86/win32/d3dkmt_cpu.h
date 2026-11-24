/* Native CPU mapping service. Internal, not a user ABI or a render adapter. */
#ifndef OSITO_D3DKMT_CPU_H
#define OSITO_D3DKMT_CPU_H
#include "d3dkmt_registry.h"
/* Callers hold a KMT cancellation scope through acquire/publication/rollback.
 * Stable kernel storage and its detached BSP collector live until shutdown. */
NTSTATUS d3dkmt_cpu_acquire(DWORD pid,OKMT_HANDLE device,OKMT_HANDLE allocation,
    BOOL compat,BOOL writable,uint64_t *ticket,uint64_t *address);
NTSTATUS d3dkmt_cpu_release(DWORD pid,uint64_t ticket);
NTSTATUS d3dkmt_cpu_unlock(DWORD pid,OKMT_HANDLE device,DWORD count,
    const OKMT_HANDLE *allocations);
BOOL d3dkmt_cpu_idle(void);
#endif
