/*
 * OsitoK Windows Compatibility Layer — ntdll.dll Shim Implementation
 *
 * Provides Rtl* utility functions and an export lookup table
 * so PE imports from "ntdll.dll" resolve to our implementations.
 *
 * The Nt* functions here are kernel-side handlers called directly
 * (in OsitoK's identity-mapped model, user/kernel share address space).
 * When we add proper ring separation, these become SYSCALL wrappers.
 */

#include "ntdll_shim.h"
#include "ntsyscall.h"
#include "win32_abi.h"

/* ── Rtl* Utilities ─────────────────────────────────────────── */

void RtlInitUnicodeString(PUNICODE_STRING dest, PCWSTR src)
{
    if (!dest) return;

    if (!src) {
        dest->Length        = 0;
        dest->MaximumLength = 0;
        dest->Buffer        = NULL;
        return;
    }

    USHORT len = 0;
    while (src[len]) len++;

    dest->Length        = len * sizeof(WCHAR);
    dest->MaximumLength = (len + 1) * sizeof(WCHAR);
    dest->Buffer        = (PWSTR)src;
}

NTSTATUS RtlUnicodeStringToAnsiString(PSTR dest, PCUNICODE_STRING src,
                                       ULONG dest_size)
{
    if (!dest || !src)
        return STATUS_INVALID_PARAMETER;

    ULONG chars = src->Length / sizeof(WCHAR);
    if (chars >= dest_size)
        chars = dest_size - 1;

    for (ULONG i = 0; i < chars; i++)
        dest[i] = (char)(src->Buffer[i] & 0xFF);

    dest[chars] = '\0';
    return STATUS_SUCCESS;
}

void RtlCopyMemory(PVOID dest, PCVOID src, SIZE_T length)
{
    BYTE *d = (BYTE *)dest;
    const BYTE *s = (const BYTE *)src;
    while (length--) *d++ = *s++;
}

void RtlZeroMemory(PVOID dest, SIZE_T length)
{
    BYTE *d = (BYTE *)dest;
    while (length--) *d++ = 0;
}

void RtlFillMemory(PVOID dest, SIZE_T length, BYTE fill)
{
    BYTE *d = (BYTE *)dest;
    while (length--) *d++ = fill;
}

/* NTSTATUS → Win32 error code (simplified mapping) */
ULONG RtlNtStatusToDosError(NTSTATUS status)
{
    switch (status) {
    case STATUS_SUCCESS:                return 0;      /* ERROR_SUCCESS */
    case STATUS_INVALID_PARAMETER:      return 87;     /* ERROR_INVALID_PARAMETER */
    case STATUS_NO_MEMORY:              return 8;      /* ERROR_NOT_ENOUGH_MEMORY */
    case STATUS_INVALID_HANDLE:         return 6;      /* ERROR_INVALID_HANDLE */
    case STATUS_OBJECT_NAME_NOT_FOUND:  return 2;      /* ERROR_FILE_NOT_FOUND */
    case STATUS_OBJECT_PATH_NOT_FOUND:  return 3;      /* ERROR_PATH_NOT_FOUND */
    case STATUS_ACCESS_VIOLATION:       return 998;    /* ERROR_NOACCESS */
    case STATUS_NOT_IMPLEMENTED:        return 120;    /* ERROR_CALL_NOT_IMPLEMENTED */
    case STATUS_INSUFFICIENT_RESOURCES: return 8;      /* ERROR_NOT_ENOUGH_MEMORY */
    case STATUS_END_OF_FILE:            return 38;     /* ERROR_HANDLE_EOF */
    default:                            return 317;    /* ERROR_MR_MID_NOT_FOUND */
    }
}

/* ── Export table ────────────────────────────────────────────── */

typedef struct _SHIM_EXPORT {
    const char *name;
    PVOID       func;
    uint8_t     argc;
    uint8_t     cc;
} SHIM_EXPORT;

/* Forward declare NT handlers — these are the kernel-side implementations
 * from ntsyscall.c. In OsitoK's flat address space, PE code calls them
 * directly. With ring separation, replace with SYSCALL stubs. */
extern NTSTATUS sys_NtCreateFile(ULONG_PTR *args);
extern NTSTATUS sys_NtReadFile(ULONG_PTR *args);
extern NTSTATUS sys_NtWriteFile(ULONG_PTR *args);
extern NTSTATUS sys_NtClose(ULONG_PTR *args);
extern NTSTATUS sys_NtAllocateVirtualMemory(ULONG_PTR *args);
extern NTSTATUS sys_NtFreeVirtualMemory(ULONG_PTR *args);
extern NTSTATUS sys_NtTerminateProcess(ULONG_PTR *args);
extern NTSTATUS sys_NtDelayExecution(ULONG_PTR *args);
extern NTSTATUS sys_NtQueryPerformanceCounter(ULONG_PTR *args);

/*
 * NOTE: The Nt* functions as exported to user code take normal C args,
 * not ULONG_PTR arrays. We need thin wrapper functions that match the
 * actual NT API signatures. In the identity-mapped model, these wrappers
 * pack args into an array and call the kernel handler. With proper SYSCALL,
 * they'd do: mov eax, SYS_NUM; syscall; ret.
 */

/* Wrapper: NtCreateFile with proper signature → array-based handler */
NTSTATUS NtCreateFile(PHANDLE fh, ACCESS_MASK access, POBJECT_ATTRIBUTES oa,
                      PIO_STATUS_BLOCK iosb, PLARGE_INTEGER alloc_size,
                      ULONG attrs, ULONG share, ULONG disp, ULONG opts,
                      PVOID ea, ULONG ea_len)
{
    ULONG_PTR args[11] = {
        (ULONG_PTR)fh, (ULONG_PTR)access, (ULONG_PTR)oa,
        (ULONG_PTR)iosb, (ULONG_PTR)alloc_size, (ULONG_PTR)attrs,
        (ULONG_PTR)share, (ULONG_PTR)disp, (ULONG_PTR)opts,
        (ULONG_PTR)ea, (ULONG_PTR)ea_len
    };
    return sys_NtCreateFile(args);
}

NTSTATUS NtReadFile(HANDLE fh, HANDLE event, PVOID apc_routine,
                    PVOID apc_ctx, PIO_STATUS_BLOCK iosb,
                    PVOID buf, ULONG len, PLARGE_INTEGER offset, PVOID key)
{
    ULONG_PTR args[9] = {
        (ULONG_PTR)fh, (ULONG_PTR)event, (ULONG_PTR)apc_routine,
        (ULONG_PTR)apc_ctx, (ULONG_PTR)iosb, (ULONG_PTR)buf,
        (ULONG_PTR)len, (ULONG_PTR)offset, (ULONG_PTR)key
    };
    return sys_NtReadFile(args);
}

NTSTATUS NtWriteFile(HANDLE fh, HANDLE event, PVOID apc_routine,
                     PVOID apc_ctx, PIO_STATUS_BLOCK iosb,
                     PVOID buf, ULONG len, PLARGE_INTEGER offset, PVOID key)
{
    ULONG_PTR args[9] = {
        (ULONG_PTR)fh, (ULONG_PTR)event, (ULONG_PTR)apc_routine,
        (ULONG_PTR)apc_ctx, (ULONG_PTR)iosb, (ULONG_PTR)buf,
        (ULONG_PTR)len, (ULONG_PTR)offset, (ULONG_PTR)key
    };
    return sys_NtWriteFile(args);
}

NTSTATUS NtClose(HANDLE h)
{
    ULONG_PTR args[1] = { (ULONG_PTR)h };
    return sys_NtClose(args);
}

/*
 * WoW64-style pointer-width thunking for NtAllocateVirtualMemory.
 *
 * When called from 32-bit PE32 code via INT 0x2E thunk, the `base` and
 * `size` parameters are pointers to 4-byte DWORDs (PVOID32/SIZE_T32).
 * But sys_NtAllocateVirtualMemory reads/writes them as 8-byte values.
 * Without thunking, it reads 4 bytes of garbage above each DWORD and
 * writes 4 bytes of corruption after each DWORD.
 *
 * Fix: detect compat32 mode, read DWORD→uint64_t, call syscall with
 * 64-bit temporaries, then truncate results back to DWORD.
 */
NTSTATUS NtAllocateVirtualMemory(HANDLE proc, PVOID *base, ULONG_PTR zbits,
                                  SIZE_T *size, ULONG type, ULONG prot)
{
    extern int g_compat32_mode;

    if (g_compat32_mode) {
        /* base and size point to 4-byte DWORDs in 32-bit memory */
        uint32_t *base32 = (uint32_t *)base;
        uint32_t *size32 = (uint32_t *)size;

        PVOID  base64 = (PVOID)(ULONG_PTR)*base32;
        SIZE_T size64 = (SIZE_T)*size32;

        ULONG_PTR args[6] = {
            (ULONG_PTR)proc, (ULONG_PTR)&base64, zbits,
            (ULONG_PTR)&size64, type, prot
        };
        NTSTATUS st = sys_NtAllocateVirtualMemory(args);

        if (st == 0) {  /* STATUS_SUCCESS */
            *base32 = (uint32_t)(ULONG_PTR)base64;
            *size32 = (uint32_t)size64;
        }
        return st;
    }

    ULONG_PTR args[6] = {
        (ULONG_PTR)proc, (ULONG_PTR)base, zbits,
        (ULONG_PTR)size, type, prot
    };
    return sys_NtAllocateVirtualMemory(args);
}

NTSTATUS NtFreeVirtualMemory(HANDLE proc, PVOID *base, SIZE_T *size, ULONG type)
{
    extern int g_compat32_mode;

    if (g_compat32_mode) {
        uint32_t *base32 = (uint32_t *)base;
        uint32_t *size32 = (uint32_t *)size;

        PVOID  base64 = (PVOID)(ULONG_PTR)*base32;
        SIZE_T size64 = size32 ? (SIZE_T)*size32 : 0;

        ULONG_PTR args[4] = {
            (ULONG_PTR)proc, (ULONG_PTR)&base64,
            (ULONG_PTR)(size32 ? &size64 : NULL), type
        };
        NTSTATUS st = sys_NtFreeVirtualMemory(args);

        if (st == 0) {
            *base32 = (uint32_t)(ULONG_PTR)base64;
            if (size32) *size32 = (uint32_t)size64;
        }
        return st;
    }

    ULONG_PTR args[4] = {
        (ULONG_PTR)proc, (ULONG_PTR)base,
        (ULONG_PTR)size, type
    };
    return sys_NtFreeVirtualMemory(args);
}

NTSTATUS NtTerminateProcess(HANDLE proc, NTSTATUS exit_status)
{
    ULONG_PTR args[2] = { (ULONG_PTR)proc, (ULONG_PTR)exit_status };
    return sys_NtTerminateProcess(args);
}

NTSTATUS NtDelayExecution(BOOL alertable, PLARGE_INTEGER delay)
{
    ULONG_PTR args[2] = { (ULONG_PTR)alertable, (ULONG_PTR)delay };
    return sys_NtDelayExecution(args);
}

NTSTATUS NtQueryPerformanceCounter(PLARGE_INTEGER counter, PLARGE_INTEGER freq)
{
    ULONG_PTR args[2] = { (ULONG_PTR)counter, (ULONG_PTR)freq };
    return sys_NtQueryPerformanceCounter(args);
}

/* New handlers */
extern NTSTATUS sys_NtQueryInformationFile(ULONG_PTR *args);
extern NTSTATUS sys_NtSetInformationFile(ULONG_PTR *args);
extern NTSTATUS sys_NtDuplicateObject(ULONG_PTR *args);
extern NTSTATUS sys_NtProtectVirtualMemory(ULONG_PTR *args);
extern NTSTATUS sys_NtQueryVirtualMemory(ULONG_PTR *args);

NTSTATUS NtQueryInformationFile(HANDLE fh, PIO_STATUS_BLOCK iosb,
                                 PVOID info, ULONG len,
                                 FILE_INFORMATION_CLASS cls)
{
    ULONG_PTR args[5] = {
        (ULONG_PTR)fh, (ULONG_PTR)iosb, (ULONG_PTR)info,
        (ULONG_PTR)len, (ULONG_PTR)cls
    };
    return sys_NtQueryInformationFile(args);
}

NTSTATUS NtSetInformationFile(HANDLE fh, PIO_STATUS_BLOCK iosb,
                               PVOID info, ULONG len,
                               FILE_INFORMATION_CLASS cls)
{
    ULONG_PTR args[5] = {
        (ULONG_PTR)fh, (ULONG_PTR)iosb, (ULONG_PTR)info,
        (ULONG_PTR)len, (ULONG_PTR)cls
    };
    return sys_NtSetInformationFile(args);
}

NTSTATUS NtDuplicateObject(HANDLE src_proc, HANDLE src_handle,
                            HANDLE tgt_proc, PHANDLE tgt_handle,
                            ACCESS_MASK access, ULONG attrs, ULONG options)
{
    ULONG_PTR args[7] = {
        (ULONG_PTR)src_proc, (ULONG_PTR)src_handle,
        (ULONG_PTR)tgt_proc, (ULONG_PTR)tgt_handle,
        (ULONG_PTR)access, (ULONG_PTR)attrs, (ULONG_PTR)options
    };
    return sys_NtDuplicateObject(args);
}

NTSTATUS NtProtectVirtualMemory(HANDLE proc, PVOID *base, SIZE_T *size,
                                 ULONG new_prot, ULONG *old_prot)
{
    extern int g_compat32_mode;

    if (g_compat32_mode) {
        uint32_t *base32 = (uint32_t *)base;
        uint32_t *size32 = (uint32_t *)size;

        PVOID  base64 = (PVOID)(ULONG_PTR)*base32;
        SIZE_T size64 = (SIZE_T)*size32;

        ULONG_PTR args[5] = {
            (ULONG_PTR)proc, (ULONG_PTR)&base64, (ULONG_PTR)&size64,
            (ULONG_PTR)new_prot, (ULONG_PTR)old_prot
        };
        NTSTATUS st = sys_NtProtectVirtualMemory(args);

        if (st == 0) {
            *base32 = (uint32_t)(ULONG_PTR)base64;
            *size32 = (uint32_t)size64;
        }
        return st;
    }

    ULONG_PTR args[5] = {
        (ULONG_PTR)proc, (ULONG_PTR)base, (ULONG_PTR)size,
        (ULONG_PTR)new_prot, (ULONG_PTR)old_prot
    };
    return sys_NtProtectVirtualMemory(args);
}

NTSTATUS NtQueryVirtualMemory(HANDLE proc, PVOID base, ULONG info_class,
                               PVOID info, SIZE_T info_len, SIZE_T *ret_len)
{
    ULONG_PTR args[6] = {
        (ULONG_PTR)proc, (ULONG_PTR)base, (ULONG_PTR)info_class,
        (ULONG_PTR)info, (ULONG_PTR)info_len, (ULONG_PTR)ret_len
    };
    return sys_NtQueryVirtualMemory(args);
}

/* ── Synchronization wrappers (Phase 21) ────────────────────── */

extern NTSTATUS sys_NtCreateEvent(ULONG_PTR *args);
extern NTSTATUS sys_NtSetEvent(ULONG_PTR *args);
extern NTSTATUS sys_NtResetEvent(ULONG_PTR *args);
extern NTSTATUS sys_NtPulseEvent(ULONG_PTR *args);
extern NTSTATUS sys_NtWaitForMultipleObjects(ULONG_PTR *args);

NTSTATUS NtCreateEvent(PHANDLE EventHandle, ACCESS_MASK DesiredAccess,
                       POBJECT_ATTRIBUTES ObjectAttributes,
                       ULONG EventType, BOOL InitialState)
{
    ULONG_PTR args[5] = {
        (ULONG_PTR)EventHandle, (ULONG_PTR)DesiredAccess,
        (ULONG_PTR)ObjectAttributes, (ULONG_PTR)EventType,
        (ULONG_PTR)InitialState
    };
    return sys_NtCreateEvent(args);
}

NTSTATUS NtSetEvent(HANDLE EventHandle, LONG *PreviousState)
{
    ULONG_PTR args[2] = {
        (ULONG_PTR)EventHandle, (ULONG_PTR)PreviousState
    };
    return sys_NtSetEvent(args);
}

NTSTATUS NtResetEvent(HANDLE EventHandle, LONG *PreviousState)
{
    ULONG_PTR args[2] = {
        (ULONG_PTR)EventHandle, (ULONG_PTR)PreviousState
    };
    return sys_NtResetEvent(args);
}

NTSTATUS NtPulseEvent(HANDLE EventHandle, LONG *PreviousState)
{
    ULONG_PTR args[2] = {
        (ULONG_PTR)EventHandle, (ULONG_PTR)PreviousState
    };
    return sys_NtPulseEvent(args);
}

NTSTATUS NtWaitForSingleObject(HANDLE Handle, BOOL Alertable,
                                PLARGE_INTEGER Timeout)
{
    /* Wrap as single-object wait via NtWaitForMultipleObjects */
    ULONG_PTR args[5] = {
        (ULONG_PTR)1,              /* Count = 1 */
        (ULONG_PTR)&Handle,        /* array of 1 handle */
        (ULONG_PTR)1,              /* WaitAny */
        (ULONG_PTR)Alertable,
        (ULONG_PTR)Timeout
    };
    return sys_NtWaitForMultipleObjects(args);
}

/* ── Section wrappers (memory-mapped files) ────────────────── */

extern NTSTATUS sys_NtCreateSection(ULONG_PTR *args);
extern NTSTATUS sys_NtMapViewOfSection(ULONG_PTR *args);
extern NTSTATUS sys_NtUnmapViewOfSection(ULONG_PTR *args);

NTSTATUS NtCreateSection(PHANDLE SectionHandle, ACCESS_MASK DesiredAccess,
                         POBJECT_ATTRIBUTES ObjectAttributes,
                         PLARGE_INTEGER MaximumSize,
                         ULONG SectionPageProtection,
                         ULONG AllocationAttributes,
                         HANDLE FileHandle)
{
    ULONG_PTR args[7] = {
        (ULONG_PTR)SectionHandle, (ULONG_PTR)DesiredAccess,
        (ULONG_PTR)ObjectAttributes, (ULONG_PTR)MaximumSize,
        (ULONG_PTR)SectionPageProtection, (ULONG_PTR)AllocationAttributes,
        (ULONG_PTR)FileHandle
    };
    return sys_NtCreateSection(args);
}

NTSTATUS NtMapViewOfSection(HANDLE SectionHandle, HANDLE ProcessHandle,
                            PVOID *BaseAddress, ULONG_PTR ZeroBits,
                            SIZE_T CommitSize, PLARGE_INTEGER SectionOffset,
                            SIZE_T *ViewSize, ULONG InheritDisposition,
                            ULONG AllocationType, ULONG Win32Protect)
{
    ULONG_PTR args[10] = {
        (ULONG_PTR)SectionHandle, (ULONG_PTR)ProcessHandle,
        (ULONG_PTR)BaseAddress, (ULONG_PTR)ZeroBits,
        (ULONG_PTR)CommitSize, (ULONG_PTR)SectionOffset,
        (ULONG_PTR)ViewSize, (ULONG_PTR)InheritDisposition,
        (ULONG_PTR)AllocationType, (ULONG_PTR)Win32Protect
    };
    return sys_NtMapViewOfSection(args);
}

NTSTATUS NtUnmapViewOfSection(HANDLE ProcessHandle, PVOID BaseAddress)
{
    ULONG_PTR args[2] = {
        (ULONG_PTR)ProcessHandle, (ULONG_PTR)BaseAddress
    };
    return sys_NtUnmapViewOfSection(args);
}

/* ── SEH Support (Phase 17) ─────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern TEB g_teb;

/*
 * RtlCaptureContext — snapshot current register state.
 * In our flat model, we capture what we can. The caller typically uses
 * this for exception dispatch or stack walking.
 */
void RtlCaptureContext(PCONTEXT ctx)
{
    if (!ctx) return;

    /* Zero the context, then fill what we know */
    BYTE *p = (BYTE *)ctx;
    for (SIZE_T i = 0; i < sizeof(CONTEXT); i++) p[i] = 0;

    ctx->ContextFlags = CONTEXT_FULL;

    /* We can't easily capture registers from C, but we set up
     * a reasonable context. The important fields for SEH are RSP/RBP/RIP. */
    /* Use inline asm to grab RSP and RBP */
#ifndef TEST_HARNESS
    __asm__ volatile ("movq %%rsp, %0" : "=r"(ctx->Rsp));
    __asm__ volatile ("movq %%rbp, %0" : "=r"(ctx->Rbp));
#endif
}

/*
 * RtlRaiseException — dispatch an exception through the SEH chain.
 *
 * Walks TEB.ExceptionList, calling each handler. If a handler returns
 * ExceptionContinueExecution, we return (caller continues). If all
 * handlers return ExceptionContinueSearch, calls the unhandled exception
 * filter if set, then terminates.
 */

/* Forward declare the unhandled filter from kernel32 */
extern PVOID g_unhandled_filter;

void RtlRaiseException(PEXCEPTION_RECORD ExceptionRecord)
{
    serial_puts("[SEH] RtlRaiseException: code=0x");
    serial_puthex(ExceptionRecord->ExceptionCode, 8);
    serial_puts("\n");

    /* Delegate to compat32 SEH dispatch which correctly reads 32-bit
     * SEH frames (4-byte Next + 4-byte Handler). The previous code
     * used 64-bit EXCEPTION_REGISTRATION_RECORD (8+8 bytes) which
     * misread the 32-bit frame, concatenating Handler with stack garbage
     * → non-canonical RIP → #GP. */
    extern int compat32_seh_dispatch(PEXCEPTION_RECORD ExceptionRecord);
    int dispatch_rc = compat32_seh_dispatch(ExceptionRecord);
    if (dispatch_rc > 0) {
        serial_puts("[SEH] exception handled by compat32\n");
        return;
    }

    /* compat32_seh_dispatch already walked the 32-bit SEH chain with
     * the correct 32-bit semantics (4-byte Next + 4-byte Handler). If
     * it returned 0, the chain was traversed and no handler caught the
     * exception. Don't run a second buggy 64-bit walker that:
     *   (a) re-reads the same chain with wrong 8+8 byte layout,
     *   (b) casts a 32-bit handler address as a 64-bit function pointer
     *       and calls it from kernel-mode 64-bit context (→ crash).
     *
     * Skip directly to the UnhandledExceptionFilter / terminate path. */

    /* No handler caught the exception — try unhandled filter */
    if (g_unhandled_filter) {
        serial_puts("[SEH] calling UnhandledExceptionFilter\n");
        EXCEPTION_POINTERS ep;
        ep.ExceptionRecord = ExceptionRecord;
        ep.ContextRecord   = NULL;

        typedef LONG (WINAPI *uef_fn)(PEXCEPTION_POINTERS);
        uef_fn filter = (uef_fn)g_unhandled_filter;
        LONG result = filter(&ep);

        if (result == EXCEPTION_CONTINUE_EXECUTION)
            return;
    }

    /* Unhandled C++ throws (0xE06D7363) suppress and continue — UT99's
     * engine FCriticalError throw cycle would otherwise terminate before
     * init completes. Combined with IST1 stack at 256KB (vs prior 64KB)
     * the nested catch chain now fits without overflowing into garbage. */
    if (ExceptionRecord->ExceptionCode == 0xE06D7363) {
        serial_puts("[SEH] suppressing unhandled C++ throw (continuing)\n");
        return;
    }

    serial_puts("[SEH] UNHANDLED EXCEPTION 0x");
    serial_puthex(ExceptionRecord->ExceptionCode, 8);
    serial_puts(" — terminating\n");

    extern void proc_exit(int32_t code);
    proc_exit((int32_t)ExceptionRecord->ExceptionCode);
}

/*
 * RtlUnwind — unwind the exception handler chain to a target frame.
 *
 * Calls each handler with EXCEPTION_UNWINDING flag set, then removes
 * frames up to (but not including) TargetFrame.
 */
void RtlUnwind(PVOID TargetFrame, PVOID TargetIp,
               PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue)
{
    (void)TargetIp;
    (void)ReturnValue;

    serial_puts("[SEH] RtlUnwind to frame ");
    serial_puthex((uint64_t)(ULONG_PTR)TargetFrame, 16);
    serial_puts("\n");

    /* Build unwind exception record if not provided */
    EXCEPTION_RECORD local_rec;
    if (!ExceptionRecord) {
        BYTE *p = (BYTE *)&local_rec;
        for (SIZE_T i = 0; i < sizeof(EXCEPTION_RECORD); i++) p[i] = 0;
        local_rec.ExceptionCode  = STATUS_SUCCESS;
        local_rec.ExceptionFlags = EXCEPTION_UNWINDING;
        ExceptionRecord = &local_rec;
    } else {
        ExceptionRecord->ExceptionFlags |= EXCEPTION_UNWINDING;
    }

    /* Walk frames, call handlers with UNWIND flag */
    PEXCEPTION_REGISTRATION_RECORD frame =
        (PEXCEPTION_REGISTRATION_RECORD)g_teb.ExceptionList;

    while (frame && frame != EXCEPTION_CHAIN_END) {
        if ((PVOID)frame == TargetFrame) {
            /* Reached target — set as new chain head */
            g_teb.ExceptionList = (PVOID)frame;
            return;
        }

        PEXCEPTION_REGISTRATION_RECORD next = frame->Next;

        if (frame->Handler) {
            typedef EXCEPTION_DISPOSITION (WINAPI *seh_handler_fn)(
                PEXCEPTION_RECORD, PVOID, PCONTEXT, PVOID);
            seh_handler_fn handler = (seh_handler_fn)frame->Handler;
            handler(ExceptionRecord, frame, NULL, NULL);
        }

        /* Remove this frame from chain */
        g_teb.ExceptionList = (PVOID)next;
        frame = next;
    }
}

/*
 * NtRaiseException — NT syscall wrapper for exception dispatch.
 * Delegates to RtlRaiseException.
 */
NTSTATUS NtRaiseException(PEXCEPTION_RECORD ExceptionRecord,
                           PCONTEXT ContextRecord, BOOL FirstChance)
{
    (void)ContextRecord;
    (void)FirstChance;

    if (!ExceptionRecord) return STATUS_INVALID_PARAMETER;

    RtlRaiseException(ExceptionRecord);
    return STATUS_SUCCESS;
}

/* ── LdrLoadDll — NT loader API ─────────────────────────────── */

/*
 * LdrLoadDll — loads a DLL by name. Used internally by ntdll.
 * We delegate to kernel32 LoadLibraryA.
 */
extern HANDLE WINAPI LoadLibraryA(PCSTR lpLibFileName);

static NTSTATUS NTAPI LdrLoadDll(PVOID SearchPath, ULONG *DllCharacteristics,
                                  PUNICODE_STRING DllName, PVOID *BaseAddress)
{
    (void)SearchPath;
    (void)DllCharacteristics;

    if (!DllName || !BaseAddress) return STATUS_INVALID_PARAMETER;

    /* Convert UNICODE_STRING to ANSI */
    char name[256];
    ULONG chars = DllName->Length / sizeof(WCHAR);
    if (chars >= sizeof(name)) chars = sizeof(name) - 1;
    for (ULONG i = 0; i < chars; i++)
        name[i] = (char)(DllName->Buffer[i] & 0xFF);
    name[chars] = '\0';

    serial_puts("[NTDLL] LdrLoadDll: ");
    serial_puts(name);
    serial_puts("\n");

    HANDLE h = LoadLibraryA(name);
    if (h) {
        *BaseAddress = h;
        return STATUS_SUCCESS;
    }

    return STATUS_OBJECT_NAME_NOT_FOUND;
}

static NTSTATUS NTAPI LdrGetProcedureAddress(PVOID BaseAddress,
                                               PVOID FunctionName,
                                               ULONG Ordinal,
                                               PVOID *FunctionAddress)
{
    (void)BaseAddress;
    (void)FunctionName;
    (void)Ordinal;
    if (FunctionAddress) *FunctionAddress = NULL;
    return STATUS_NOT_IMPLEMENTED;
}

/* ── Export resolution table ────────────────────────────────── */

static const SHIM_EXPORT ntdll_exports[] = {
    /* NT API */
    { "NtCreateFile",              (PVOID)NtCreateFile,              11, CC_STDCALL },
    { "NtReadFile",                (PVOID)NtReadFile,                 9, CC_STDCALL },
    { "NtWriteFile",               (PVOID)NtWriteFile,                9, CC_STDCALL },
    { "NtClose",                   (PVOID)NtClose,                    1, CC_STDCALL },
    { "NtAllocateVirtualMemory",   (PVOID)NtAllocateVirtualMemory,    6, CC_STDCALL },
    { "NtFreeVirtualMemory",       (PVOID)NtFreeVirtualMemory,        4, CC_STDCALL },
    { "NtTerminateProcess",        (PVOID)NtTerminateProcess,         2, CC_STDCALL },
    { "NtDelayExecution",          (PVOID)NtDelayExecution,           2, CC_STDCALL },
    { "NtQueryPerformanceCounter", (PVOID)NtQueryPerformanceCounter,  2, CC_STDCALL },
    { "NtQueryInformationFile",    (PVOID)NtQueryInformationFile,     5, CC_STDCALL },
    { "NtSetInformationFile",      (PVOID)NtSetInformationFile,       5, CC_STDCALL },
    { "NtDuplicateObject",         (PVOID)NtDuplicateObject,          7, CC_STDCALL },
    { "NtProtectVirtualMemory",    (PVOID)NtProtectVirtualMemory,     5, CC_STDCALL },
    { "NtQueryVirtualMemory",      (PVOID)NtQueryVirtualMemory,       6, CC_STDCALL },
    /* Section (memory-mapped files) */
    { "NtCreateSection",           (PVOID)NtCreateSection,            7, CC_STDCALL },
    { "NtMapViewOfSection",        (PVOID)NtMapViewOfSection,        10, CC_STDCALL },
    { "NtUnmapViewOfSection",      (PVOID)NtUnmapViewOfSection,       2, CC_STDCALL },
    { "ZwCreateSection",           (PVOID)NtCreateSection,            7, CC_STDCALL },
    { "ZwMapViewOfSection",        (PVOID)NtMapViewOfSection,        10, CC_STDCALL },
    { "ZwUnmapViewOfSection",      (PVOID)NtUnmapViewOfSection,       2, CC_STDCALL },
    /* Synchronization (Phase 21) */
    { "NtCreateEvent",             (PVOID)NtCreateEvent,              5, CC_STDCALL },
    { "NtSetEvent",                (PVOID)NtSetEvent,                 2, CC_STDCALL },
    { "NtResetEvent",              (PVOID)NtResetEvent,               2, CC_STDCALL },
    { "NtPulseEvent",              (PVOID)NtPulseEvent,               2, CC_STDCALL },
    { "NtWaitForSingleObject",     (PVOID)NtWaitForSingleObject,      3, CC_STDCALL },
    /* Zw aliases (identical in user mode) */
    { "ZwCreateFile",              (PVOID)NtCreateFile,              11, CC_STDCALL },
    { "ZwReadFile",                (PVOID)NtReadFile,                 9, CC_STDCALL },
    { "ZwWriteFile",               (PVOID)NtWriteFile,                9, CC_STDCALL },
    { "ZwClose",                   (PVOID)NtClose,                    1, CC_STDCALL },
    { "ZwQueryInformationFile",    (PVOID)NtQueryInformationFile,     5, CC_STDCALL },
    { "ZwSetInformationFile",      (PVOID)NtSetInformationFile,       5, CC_STDCALL },
    { "ZwDuplicateObject",         (PVOID)NtDuplicateObject,          7, CC_STDCALL },
    { "ZwCreateEvent",             (PVOID)NtCreateEvent,              5, CC_STDCALL },
    { "ZwSetEvent",                (PVOID)NtSetEvent,                 2, CC_STDCALL },
    { "ZwResetEvent",              (PVOID)NtResetEvent,               2, CC_STDCALL },
    { "ZwPulseEvent",              (PVOID)NtPulseEvent,               2, CC_STDCALL },
    { "ZwWaitForSingleObject",     (PVOID)NtWaitForSingleObject,      3, CC_STDCALL },
    /* Rtl utilities */
    { "RtlInitUnicodeString",      (PVOID)RtlInitUnicodeString,       2, CC_STDCALL },
    { "RtlCopyMemory",             (PVOID)RtlCopyMemory,              3, CC_STDCALL },
    { "RtlZeroMemory",             (PVOID)RtlZeroMemory,              2, CC_STDCALL },
    { "RtlFillMemory",             (PVOID)RtlFillMemory,              3, CC_STDCALL },
    { "RtlNtStatusToDosError",     (PVOID)RtlNtStatusToDosError,      1, CC_STDCALL },
    /* SEH support */
    { "RtlRaiseException",         (PVOID)RtlRaiseException,          1, CC_STDCALL },
    { "RtlUnwind",                 (PVOID)RtlUnwind,                  4, CC_STDCALL },
    { "RtlCaptureContext",         (PVOID)RtlCaptureContext,          1, CC_STDCALL },
    { "NtRaiseException",          (PVOID)NtRaiseException,           3, CC_STDCALL },
    { "ZwRaiseException",          (PVOID)NtRaiseException,           3, CC_STDCALL },
    /* Loader */
    { "LdrLoadDll",                (PVOID)LdrLoadDll,                 4, CC_STDCALL },
    { "LdrGetProcedureAddress",    (PVOID)LdrGetProcedureAddress,     4, CC_STDCALL },
    { NULL, NULL, 0, CC_STDCALL }
};

const WIN32_EXPORT *ntdll_abi_table(int *count) {
    *count = (int)(sizeof(ntdll_exports)/sizeof(ntdll_exports[0]));
    return (const WIN32_EXPORT *)ntdll_exports;
}

#define NTDLL_EXPORT_COUNT \
    (sizeof(ntdll_exports) / sizeof(ntdll_exports[0]) - 1)

/* ── Resolve by name ────────────────────────────────────────── */

static int shim_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

PVOID ntdll_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    if (by_ordinal) {
        /* We don't support ordinal-based import for ntdll */
        return NULL;
    }

    for (int i = 0; ntdll_exports[i].name; i++) {
        if (shim_strcmp(func_name, ntdll_exports[i].name) == 0)
            return ntdll_exports[i].func;
    }

    return NULL;
}

/* ── Shim init ──────────────────────────────────────────────── */

PVOID ntdll_shim_init(void)
{
    /* Nothing to allocate — exports are static.
     * Return a non-NULL sentinel so callers know init succeeded. */
    return (PVOID)ntdll_exports;
}
