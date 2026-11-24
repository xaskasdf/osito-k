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
PVOID ntdll_shared_gdi_table(void);

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
NTSTATUS NTAPI NtCreateFile(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
                      PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG,
                      ULONG, ULONG, PVOID, ULONG);
NTSTATUS NTAPI NtReadFile(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK,
                    PVOID, ULONG, PLARGE_INTEGER, PVOID);
NTSTATUS NTAPI NtWriteFile(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK,
                     PVOID, ULONG, PLARGE_INTEGER, PVOID);
NTSTATUS NTAPI NtLockFile(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK,
                          PLARGE_INTEGER, PLARGE_INTEGER, ULONG, BOOL, BOOL);
NTSTATUS NTAPI NtUnlockFile(HANDLE, PIO_STATUS_BLOCK, PLARGE_INTEGER,
                            PLARGE_INTEGER, ULONG);
NTSTATUS NTAPI NtClose(HANDLE);
NTSTATUS NTAPI NtQueryObject(HANDLE, ULONG, PVOID, ULONG, ULONG *);
NTSTATUS NTAPI NtAllocateVirtualMemory(HANDLE, PVOID *, ULONG_PTR,
                                  SIZE_T *, ULONG, ULONG);
NTSTATUS NTAPI NtFreeVirtualMemory(HANDLE, PVOID *, SIZE_T *, ULONG);
NTSTATUS NTAPI NtTerminateProcess(HANDLE, NTSTATUS);
NTSTATUS NTAPI NtDelayExecution(BOOL, PLARGE_INTEGER);
NTSTATUS NTAPI NtQueryPerformanceCounter(PLARGE_INTEGER, PLARGE_INTEGER);
NTSTATUS NTAPI NtQueryInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG,
                                 FILE_INFORMATION_CLASS);
NTSTATUS NTAPI NtSetInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG,
                               FILE_INFORMATION_CLASS);
NTSTATUS NTAPI NtDuplicateObject(HANDLE, HANDLE, HANDLE, PHANDLE,
                            ACCESS_MASK, ULONG, ULONG);
NTSTATUS NTAPI NtProtectVirtualMemory(HANDLE, PVOID *, SIZE_T *, ULONG, ULONG *);
NTSTATUS NTAPI NtQueryVirtualMemory(HANDLE, PVOID, ULONG, PVOID, SIZE_T, SIZE_T *);
NTSTATUS NTAPI NtQueryInformationProcess(HANDLE, PROCESSINFOCLASS, PVOID,
                                         ULONG, ULONG *);
NTSTATUS NTAPI NtReadVirtualMemory(HANDLE, PVOID, PVOID, SIZE_T, SIZE_T *);
NTSTATUS NTAPI NtWriteVirtualMemory(HANDLE, PVOID, PCVOID, SIZE_T, SIZE_T *);
NTSTATUS NTAPI NtWow64QueryInformationProcess64(HANDLE, PROCESSINFOCLASS,
                                                PVOID, ULONG, ULONG *);
NTSTATUS NTAPI NtWow64ReadVirtualMemory64(HANDLE, ULONGLONG, PVOID,
                                         ULONGLONG, ULONGLONG *);
NTSTATUS NTAPI NtOpenKeyEx(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG);
NTSTATUS NTAPI NtCreateKey(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG,
                           PUNICODE_STRING, ULONG, ULONG *);
NTSTATUS NTAPI NtQueryValueKey(HANDLE, PUNICODE_STRING, ULONG, PVOID, ULONG,
                               ULONG *);
NTSTATUS NTAPI NtSetValueKey(HANDLE, PUNICODE_STRING, ULONG, ULONG, PVOID,
                             ULONG);

/* Synchronization */
NTSTATUS NTAPI NtCreateEvent(PHANDLE EventHandle, ACCESS_MASK DesiredAccess,
                       POBJECT_ATTRIBUTES ObjectAttributes,
                       ULONG EventType, BOOL InitialState);
NTSTATUS NTAPI NtOpenEvent(PHANDLE EventHandle, ACCESS_MASK DesiredAccess,
                           POBJECT_ATTRIBUTES ObjectAttributes);
NTSTATUS NTAPI NtSetEvent(HANDLE EventHandle, LONG *PreviousState);
NTSTATUS NTAPI NtResetEvent(HANDLE EventHandle, LONG *PreviousState);
NTSTATUS NTAPI NtPulseEvent(HANDLE EventHandle, LONG *PreviousState);
NTSTATUS NTAPI NtWaitForSingleObject(HANDLE Handle, BOOL Alertable,
                                PLARGE_INTEGER Timeout);

int ntdll_object_selftest(void);

/* Section (memory-mapped files) */
NTSTATUS NTAPI NtCreateSection(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
                         PLARGE_INTEGER, ULONG, ULONG, HANDLE);
NTSTATUS NTAPI NtMapViewOfSection(HANDLE, HANDLE, PVOID *, ULONG_PTR,
                            SIZE_T, PLARGE_INTEGER, SIZE_T *,
                            ULONG, ULONG, ULONG);
NTSTATUS NTAPI NtUnmapViewOfSection(HANDLE, PVOID);

/* Rtl* utilities (run in user mode, no SYSCALL needed) */
void     NTAPI RtlInitUnicodeString(PUNICODE_STRING dest, PCWSTR src);
NTSTATUS NTAPI RtlFormatCurrentUserKeyPath(PUNICODE_STRING path);
void     NTAPI RtlFreeUnicodeString(PUNICODE_STRING value);
NTSTATUS NTAPI RtlInitializeCriticalSection(PRTL_CRITICAL_SECTION section);
NTSTATUS NTAPI RtlInitializeCriticalSectionAndSpinCount(
    PRTL_CRITICAL_SECTION section, ULONG spin_count);
NTSTATUS NTAPI RtlInitializeCriticalSectionEx(PRTL_CRITICAL_SECTION section,
                                               ULONG spin_count,
                                               ULONG flags);
NTSTATUS NTAPI RtlEnterCriticalSection(PRTL_CRITICAL_SECTION section);
BOOL     NTAPI RtlTryEnterCriticalSection(PRTL_CRITICAL_SECTION section);
NTSTATUS NTAPI RtlLeaveCriticalSection(PRTL_CRITICAL_SECTION section);
NTSTATUS NTAPI RtlDeleteCriticalSection(PRTL_CRITICAL_SECTION section);
void     NTAPI RtlAcquirePebLock(void);
BOOL     NTAPI RtlTryAcquirePebLock(void);
void     NTAPI RtlReleasePebLock(void);
NTSTATUS NTAPI RtlUnicodeStringToAnsiString(PSTR dest, PCUNICODE_STRING src,
                                       ULONG dest_size);
void     NTAPI RtlCopyMemory(PVOID dest, PCVOID src, SIZE_T length);
void     NTAPI RtlZeroMemory(PVOID dest, SIZE_T length);
void     NTAPI RtlFillMemory(PVOID dest, SIZE_T length, BYTE fill);
ULONG    NTAPI RtlNtStatusToDosError(NTSTATUS status);

/* SEH support (Phase 17) */
void     NTAPI RtlRaiseException(PEXCEPTION_RECORD ExceptionRecord);
void     NTAPI RtlRaiseStatus(NTSTATUS status);
void     NTAPI RtlUnwind(PVOID TargetFrame, PVOID TargetIp,
                    PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue);
void     NTAPI RtlUnwindEx(PVOID TargetFrame, PVOID TargetIp,
                      PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue,
                      PCONTEXT ContextRecord, PVOID HistoryTable);
void     NTAPI RtlCaptureContext(PCONTEXT ContextRecord);
void     NTAPI RtlRestoreContext(PCONTEXT ContextRecord,
                                 PEXCEPTION_RECORD ExceptionRecord);
void     NTAPI win32_rtl_raise_exception_impl(
    PEXCEPTION_RECORD ExceptionRecord, PCONTEXT ContextRecord);
NTSTATUS NTAPI NtRaiseException(PEXCEPTION_RECORD ExceptionRecord,
                           PCONTEXT ContextRecord, BOOL FirstChance);

#endif /* NTDLL_SHIM_H */
