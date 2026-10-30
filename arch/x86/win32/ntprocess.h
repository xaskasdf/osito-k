#ifndef OSITO_NTPROCESS_H
#define OSITO_NTPROCESS_H

#include "nttypes.h"
#include "handle.h"

void nt_thread_bind_cpu_accounting(PVOID thread_object, uint32_t kernel_pid);
void nt_process_begin_main_cpu_accounting(uint32_t kernel_pid);
/* -1 queries; 0/1 sets the calling thread's disabled state and returns old. */
BOOL nt_thread_fs_redirection(int disabled);
/* Called by the native shell loader before entering any PE code. */
BOOL nt_process_set_main_admin(void);
void nt_process_end_main_cpu_accounting(void);
NTSTATUS nt_process_query_cycles(HANDLE process, ULONGLONG *cycles);
NTSTATUS nt_thread_query_cycles(HANDLE thread, ULONGLONG *cycles);
NTSTATUS nt_thread_open(ULONG_PTR pid, ULONG_PTR tid, ACCESS_MASK access,
                        BOOL inherit, PHANDLE handle);
NTSTATUS nt_thread_query_identity(HANDLE handle, ULONG *tid, DWORD *exit_code);
ULONG nt_thread_live_owner(ULONG tid);
/* One cancellable synchronous request per issuing thread. The token is a
 * kernel object, never a pointer into the caller's process-private stack.
 * NULL means there is no Win32 thread, or an outer request already owns it. */
PVOID nt_thread_sync_io_begin(void);
BOOL nt_thread_sync_io_cancelled(PVOID token);
void nt_thread_sync_io_end(PVOID token);
/* Win32 error code; success only marks an existing request for cancellation. */
DWORD nt_thread_cancel_sync_io(HANDLE thread);
NTSTATUS nt_process_open_client(ULONG_PTR pid, ULONG_PTR tid, ACCESS_MASK access,
                                BOOL inherit, PHANDLE handle);
NTSTATUS nt_process_query_identity(HANDLE handle, DWORD *pid, DWORD *exit_code);
NTSTATUS nt_process_query_handle_count(HANDLE handle, ULONG *count);
/* Returned token owns a reference released with advapi32_token_release. */
NTSTATUS nt_process_reference_primary_token(HANDLE process, PVOID *token);
NTSTATUS nt_thread_reference_impersonation_token(HANDLE thread, PVOID *token);
NTSTATUS nt_thread_assign_token_handle(HANDLE thread, HANDLE token);
/* primary_token is a pinned source; NULL inherits the caller's primary token. */
NTSTATUS nt_process_create_child(PHANDLE process, PHANDLE thread,
    ULONG *pid, ULONG *tid, PVOID *process_object, PVOID *thread_object,
    BOOL suspended, PCVOID primary_token);
void nt_process_release_process_object(PVOID object);
void nt_process_token_handle_closed(OBJECT_TYPE_ID type, PVOID object);
NTSTATUS nt_process_resolve_job_pid(HANDLE process, ACCESS_MASK access, ULONG *pid);
NTSTATUS nt_process_resolve_vm_pid(HANDLE process, ACCESS_MASK access, ULONG *pid);
void nt_process_begin_vm_teardown(ULONG pid);
struct intel_gpu_memory;
/* Kernel-only admission: registered, pinned manager and captured output.
 * Runs on the BSP, where NT process mutation/teardown is serialized; AP
 * workers must not resolve NT process state. The eventual DDI caller obtains
 * pid from its authenticated calling process, never an unchecked guest PID. */
NTSTATUS nt_process_open_gpu_device(ULONG pid, struct intel_gpu_memory *, ULONG *device);
NTSTATUS nt_process_open_gpu_device_retained(ULONG pid, struct intel_gpu_memory *, ULONG *device);

#endif
