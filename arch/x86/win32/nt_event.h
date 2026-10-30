#ifndef OSITO_NT_EVENT_H
#define OSITO_NT_EVENT_H
#include "nttypes.h"
/* Kernel-owned references, independent of numeric handles and process exit.
 * retain requires an existing reference or the handle-table lock. A newly
 * created event starts with the creator's reference. The final release frees
 * the pool slot only after subsystem cleanup; no weak pointer may revive it.
 * Signal/release run on the BSP, outside graphics/handle metadata locks. */
BOOL ntsync_event_retain(PVOID object);
void ntsync_event_release(PVOID object);
NTSTATUS ntsync_reference_event_for_process(HANDLE,DWORD,PVOID *object);
void ntsync_signal_event_reference(PVOID object,DWORD actor_pid);
#endif
