/*
 * OsitoK Windows Compatibility Layer — NT Syscall Dispatch
 *
 * SSDT (System Service Descriptor Table) for x86-64.
 * Service numbers match Windows Server 2003 SP1 amd64.
 *
 * ABI: EAX = service number
 *      R10 = arg1 (RCX saved before SYSCALL clobbers it)
 *      RDX = arg2, R8 = arg3, R9 = arg4
 *      Stack: arg5, arg6, arg7... (at RSP+0x28 after shadow space)
 */

#ifndef NTSYSCALL_H
#define NTSYSCALL_H

#include "nttypes.h"

/* ── NT syscall numbers (amd64, WS2003 SP1) ─────────────────── */

/* File I/O */
#define NTSYS_ReadFile                      3
#define NTSYS_WriteFile                     5
#define NTSYS_Close                         12
#define NTSYS_CreateFile                    166   /* NtCreateFile (was 0x55 in NT4) */
#define NTSYS_QueryInformationFile          226
#define NTSYS_SetInformationFile            260

/* Memory */
#define NTSYS_AllocateVirtualMemory         105
#define NTSYS_FreeVirtualMemory             111
#define NTSYS_ProtectVirtualMemory          215
#define NTSYS_QueryVirtualMemory            224

/* Process */
#define NTSYS_CreateProcess                 214
#define NTSYS_CreateProcessEx               158
#define NTSYS_TerminateProcess              257
#define NTSYS_QueryInformationProcess       207

/* Thread */
#define NTSYS_CreateThread                  159
#define NTSYS_TerminateThread               258
#define NTSYS_ResumeThread                  243

/* Synchronization */
#define NTSYS_WaitForSingleObject           275
#define NTSYS_WaitForMultipleObjects        274
#define NTSYS_CreateEvent                   137
#define NTSYS_SetEvent                      253
#define NTSYS_ResetEvent                    242
#define NTSYS_PulseEvent                    234

/* Object */
#define NTSYS_DuplicateObject               168

/* Section (memory-mapped files / PE image mapping) */
#define NTSYS_CreateSection                 154
#define NTSYS_MapViewOfSection              188
#define NTSYS_UnmapViewOfSection            268

/* Registry (future) */
#define NTSYS_CreateKey                     139
#define NTSYS_OpenKey                       199
#define NTSYS_QueryValueKey                 231
#define NTSYS_SetValueKey                   261
#define NTSYS_CloseKey                      12    /* same as NtClose */

/* Misc */
#define NTSYS_QuerySystemInformation        198
#define NTSYS_DelayExecution                93    /* NtDelayExecution (Sleep) */
#define NTSYS_QueryPerformanceCounter       206
#define NTSYS_YieldExecution                287

/* ── Maximum syscall number we handle ───────────────────────── */
#define NTSYS_MAX                           296

/* ── Syscall handler type ───────────────────────────────────── */

/*
 * All NT syscalls receive up to 16 args as ULONG_PTR.
 * The dispatcher unpacks register args + stack args into this array.
 * Each handler casts args to their proper types.
 */
typedef NTSTATUS (*NT_SYSCALL_HANDLER)(ULONG_PTR *args);

/* ── Dispatch table ─────────────────────────────────────────── */

typedef struct _NT_SERVICE_TABLE {
    NT_SYSCALL_HANDLER  handlers[NTSYS_MAX];
    UCHAR               arg_counts[NTSYS_MAX];  /* number of args per syscall */
    ULONG               limit;                   /* highest valid syscall + 1 */
} NT_SERVICE_TABLE;

/* ── API ────────────────────────────────────────────────────── */

/* Initialize SSDT with our handlers */
void nt_syscall_init(NT_SERVICE_TABLE *table);

/* Release VirtualAlloc regions that belong to a terminating Win32 process. */
void nt_vm_release_process(ULONG owner_pid);
/* Dump VMA ownership/protection metadata for a fatal guest address. */
void nt_vm_debug_address(uint64_t address);

/* Allocate and release a process-private Win32 stack. AllocationBase includes
 * a protected bottom page; StackLimit..StackBase is committed PAGE_READWRITE. */
NTSTATUS nt_vm_allocate_stack(SIZE_T reserve_size, PVOID *allocation_base,
                              PVOID *stack_limit, PVOID *stack_base);
NTSTATUS nt_vm_free_stack(PVOID allocation_base);
NTSTATUS nt_vm_free_stack_for_process(ULONG owner_pid,
                                      PVOID allocation_base);
int nt_vm_selftest(void);

/* Thread impersonation state is owned by the NT thread object. ADVAPI32 uses
 * these helpers so pseudo handles and duplicated thread handles observe the
 * same per-thread security context. */
PVOID nt_thread_get_impersonation_token(PVOID thread_object);
BOOL nt_thread_set_impersonation_token(PVOID thread_object,
                                       PVOID token_object);

/* Close a handle on behalf of a specific Win32 process. Kernel-owned
 * references use this instead of depending on the caller's current TEB. */
NTSTATUS nt_close_handle_for_process(HANDLE handle, ULONG owner_pid);

/* Apply the section object's DACL when DuplicateHandle requests new rights. */
BOOL nt_section_allows_access_escalation(PVOID section,
                                         ACCESS_MASK desired_access);

/* Named mappings use the identity to reject stale pointers when a section
 * pool slot is recycled. */
BOOL nt_section_is_alive(PVOID section);
uint64_t nt_section_identity(PVOID section);
BOOL nt_section_reopen_handle(PVOID section);

/* Return the underlying byte-stream identity for a pipe endpoint.  Pending
 * overlapped operations use this to preserve FIFO ordering across duplicated
 * handles that refer to the same stream. */
PVOID nt_pipe_stream_identity(HANDLE handle, ULONG owner_pid, BOOL write);

/* Inspect or evict inactive immutable file-backed section control areas. */
void nt_section_cache_dump(void);
int nt_section_cache_flush_unused(void);

/* Dispatch a syscall. Called from assembly entry point.
 * nr = EAX, args[] populated from R10/RDX/R8/R9/stack. */
NTSTATUS nt_syscall_dispatch(NT_SERVICE_TABLE *table,
                             ULONG nr, ULONG_PTR *args);

#endif /* NTSYSCALL_H */
