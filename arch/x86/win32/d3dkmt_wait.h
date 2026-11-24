#ifndef OSITOK_D3DKMT_WAIT_H
#define OSITOK_D3DKMT_WAIT_H
#include "d3dkmt_registry.h"
NTSTATUS d3dkmt_wait_cpu(DWORD,OKMT_HANDLE,DWORD,const OKMT_HANDLE *,const uint64_t *,
    BOOL,uint64_t,const D3DKMT_SYNC_EVENT_PROVIDER *);
BOOL d3dkmt_wait_idle(void);
#endif
