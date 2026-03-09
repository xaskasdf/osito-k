/*
 * OsitoK Windows Compatibility Layer — ntdll.dll Shim
 *
 * Provides a minimal in-memory ntdll.dll that PE executables can import.
 * Each exported function is a small wrapper that loads args into registers
 * and executes SYSCALL with the appropriate service number.
 *
 * Also provides Rtl* utility functions that run in user mode.
 */

#ifndef NTDLL_SHIM_H
#define NTDLL_SHIM_H

#include "nttypes.h"
#include "pe.h"

/* ── Shim initialization ────────────────────────────────────── */

/*
 * Build the in-memory ntdll shim. Creates an export table that
 * pe_resolve_import() can use to resolve PE imports.
 *
 * Returns the base address of the shim, or NULL on failure.
 */
PVOID ntdll_shim_init(void);

/*
 * Resolve an import from ntdll.dll by name or ordinal.
 * Called by the PE loader's import resolution.
 */
PVOID ntdll_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal);

/* ── Exported function list ─────────────────────────────────── */
/*
 * These are the functions that our ntdll shim exports.
 * PE executables that import from ntdll.dll will get pointers
 * to these implementations.
 *
 * Each Nt* function is a thin wrapper around SYSCALL.
 * Rtl* functions are implemented directly in C.
 */

/* NT API wrappers (will SYSCALL into kernel) */
NTSTATUS NtCreateFile(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
                      PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG,
                      ULONG, ULONG, PVOID, ULONG);
NTSTATUS NtReadFile(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK,
                    PVOID, ULONG, PLARGE_INTEGER, PVOID);
NTSTATUS NtWriteFile(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK,
                     PVOID, ULONG, PLARGE_INTEGER, PVOID);
NTSTATUS NtClose(HANDLE);
NTSTATUS NtAllocateVirtualMemory(HANDLE, PVOID *, ULONG_PTR,
                                  SIZE_T *, ULONG, ULONG);
NTSTATUS NtFreeVirtualMemory(HANDLE, PVOID *, SIZE_T *, ULONG);
NTSTATUS NtTerminateProcess(HANDLE, NTSTATUS);
NTSTATUS NtDelayExecution(BOOL, PLARGE_INTEGER);
NTSTATUS NtQueryPerformanceCounter(PLARGE_INTEGER, PLARGE_INTEGER);
NTSTATUS NtQueryInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG,
                                 FILE_INFORMATION_CLASS);
NTSTATUS NtSetInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG,
                               FILE_INFORMATION_CLASS);
NTSTATUS NtDuplicateObject(HANDLE, HANDLE, HANDLE, PHANDLE,
                            ACCESS_MASK, ULONG, ULONG);
NTSTATUS NtProtectVirtualMemory(HANDLE, PVOID *, SIZE_T *, ULONG, ULONG *);
NTSTATUS NtQueryVirtualMemory(HANDLE, PVOID, ULONG, PVOID, SIZE_T, SIZE_T *);

/* Synchronization */
NTSTATUS NtCreateEvent(PHANDLE EventHandle, ACCESS_MASK DesiredAccess,
                       POBJECT_ATTRIBUTES ObjectAttributes,
                       ULONG EventType, BOOL InitialState);
NTSTATUS NtSetEvent(HANDLE EventHandle, LONG *PreviousState);
NTSTATUS NtResetEvent(HANDLE EventHandle, LONG *PreviousState);
NTSTATUS NtPulseEvent(HANDLE EventHandle, LONG *PreviousState);
NTSTATUS NtWaitForSingleObject(HANDLE Handle, BOOL Alertable,
                                PLARGE_INTEGER Timeout);

/* Rtl* utilities (run in user mode, no SYSCALL needed) */
void     RtlInitUnicodeString(PUNICODE_STRING dest, PCWSTR src);
NTSTATUS RtlUnicodeStringToAnsiString(PSTR dest, PCUNICODE_STRING src,
                                       ULONG dest_size);
void     RtlCopyMemory(PVOID dest, PCVOID src, SIZE_T length);
void     RtlZeroMemory(PVOID dest, SIZE_T length);
void     RtlFillMemory(PVOID dest, SIZE_T length, BYTE fill);
ULONG    RtlNtStatusToDosError(NTSTATUS status);

/* SEH support (Phase 17) */
void     RtlRaiseException(PEXCEPTION_RECORD ExceptionRecord);
void     RtlUnwind(PVOID TargetFrame, PVOID TargetIp,
                    PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue);
void     RtlCaptureContext(PCONTEXT ContextRecord);
NTSTATUS NtRaiseException(PEXCEPTION_RECORD ExceptionRecord,
                           PCONTEXT ContextRecord, BOOL FirstChance);

#endif /* NTDLL_SHIM_H */
