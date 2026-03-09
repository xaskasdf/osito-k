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

/* Dispatch a syscall. Called from assembly entry point.
 * nr = EAX, args[] populated from R10/RDX/R8/R9/stack. */
NTSTATUS nt_syscall_dispatch(NT_SERVICE_TABLE *table,
                             ULONG nr, ULONG_PTR *args);

#endif /* NTSYSCALL_H */
