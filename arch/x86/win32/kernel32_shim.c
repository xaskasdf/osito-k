/*
 * OsitoK Windows Compatibility Layer — kernel32.dll Shim Implementation
 *
 * Translates Win32 API calls to NT API calls.
 * This is what makes regular Windows .exe files work.
 */

#include "kernel32_shim.h"
#include "ntdll_shim.h"
#include "dllloader.h"

/* ── Kernel interfaces (forward declarations) ─────────────── */
extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);

/* ── Per-thread last error (global for now, TEB-based later) ── */

static DWORD g_last_error = 0;

/* Sync to TEB32 so 32-bit code reading FS:[0x34] sees correct value */
extern TEB32 g_teb32;
static inline void sync_last_error(void) { g_teb32.LastErrorValue = g_last_error; }

/* ── Helpers ────────────────────────────────────────────────── */

static inline void set_last_error_from_status(NTSTATUS status)
{
    g_last_error = RtlNtStatusToDosError(status);
    sync_last_error();
}

/* Convert ASCII string to UNICODE_STRING (stack-based, temporary) */
static void ascii_to_unicode_buf(const char *src, WCHAR *buf, int max_chars)
{
    int i;
    for (i = 0; src[i] && i < max_chars - 1; i++)
        buf[i] = (WCHAR)(unsigned char)src[i];
    buf[i] = 0;
}

/* Win32 creation disposition → NT create disposition */
static ULONG win32_to_nt_disposition(DWORD dwCreationDisposition)
{
    switch (dwCreationDisposition) {
    case 1: /* CREATE_NEW */        return FILE_CREATE;
    case 2: /* CREATE_ALWAYS */     return FILE_OVERWRITE_IF;
    case 3: /* OPEN_EXISTING */     return FILE_OPEN;
    case 4: /* OPEN_ALWAYS */       return FILE_OPEN_IF;
    case 5: /* TRUNCATE_EXISTING */ return FILE_OVERWRITE;
    default:                        return FILE_OPEN;
    }
}

/* ── Console handles ────────────────────────────────────────── */

/* Map Win32 pseudo-handles (-10,-11,-12) to NT handles (4,8,12) */
static HANDLE console_handle(DWORD nStdHandle)
{
    switch (nStdHandle) {
    case WIN32_STD_INPUT_HANDLE:  return (HANDLE)(ULONG_PTR)4;
    case WIN32_STD_OUTPUT_HANDLE: return (HANDLE)(ULONG_PTR)8;
    case WIN32_STD_ERROR_HANDLE:  return (HANDLE)(ULONG_PTR)12;
    default:                      return INVALID_HANDLE_VALUE;
    }
}

/* ── File API ───────────────────────────────────────────────── */

HANDLE WINAPI CreateFileA(PCSTR lpFileName, DWORD dwDesiredAccess,
                   DWORD dwShareMode, PVOID lpSecurityAttributes,
                   DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes,
                   HANDLE hTemplateFile)
{
    (void)lpSecurityAttributes;
    (void)dwFlagsAndAttributes;
    (void)hTemplateFile;

    serial_puts("[CreateFileA] '");
    if (lpFileName) serial_puts(lpFileName);
    serial_puts("'\n");

    /* Build NT path from Win32 path */
    WCHAR name_buf[260];
    ascii_to_unicode_buf(lpFileName, name_buf, 260);

    UNICODE_STRING name;
    RtlInitUnicodeString(&name, name_buf);

    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE, NULL, NULL);

    IO_STATUS_BLOCK iosb;
    HANDLE file_handle = INVALID_HANDLE_VALUE;

    /* Map Win32 access to NT access */
    ACCESS_MASK nt_access = 0;
    if (dwDesiredAccess & GENERIC_READ)  nt_access |= FILE_GENERIC_READ;
    if (dwDesiredAccess & GENERIC_WRITE) nt_access |= FILE_GENERIC_WRITE;
    nt_access |= SYNCHRONIZE;

    NTSTATUS status = NtCreateFile(
        &file_handle,
        nt_access,
        &oa,
        &iosb,
        NULL,                                   /* AllocationSize */
        FILE_ATTRIBUTE_NORMAL,                  /* FileAttributes */
        dwShareMode,                            /* ShareAccess */
        win32_to_nt_disposition(dwCreationDisposition),
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
        NULL,                                   /* EaBuffer */
        0                                       /* EaLength */
    );

    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return INVALID_HANDLE_VALUE;
    }

    return file_handle;
}

HANDLE WINAPI CreateFileW(PCWSTR lpFileName, DWORD dwDesiredAccess,
                   DWORD dwShareMode, PVOID lpSecurityAttributes,
                   DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes,
                   HANDLE hTemplateFile)
{
    (void)lpSecurityAttributes;
    (void)dwFlagsAndAttributes;
    (void)hTemplateFile;

    /* Log wide filename as ASCII for debug */
    serial_puts("[CreateFileW] ptr=0x");
    serial_puthex((uint64_t)lpFileName, 16);
    serial_puts(" '");
    if (lpFileName) {
        for (int i = 0; i < 80 && lpFileName[i]; i++) {
            char c = (char)(lpFileName[i] & 0xFF);
            char buf[2] = { c, 0 };
            serial_puts(buf);
        }
    }
    serial_puts("'\n");

    UNICODE_STRING name;
    RtlInitUnicodeString(&name, lpFileName);

    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE, NULL, NULL);

    IO_STATUS_BLOCK iosb;
    HANDLE file_handle = INVALID_HANDLE_VALUE;

    ACCESS_MASK nt_access = 0;
    if (dwDesiredAccess & GENERIC_READ)  nt_access |= FILE_GENERIC_READ;
    if (dwDesiredAccess & GENERIC_WRITE) nt_access |= FILE_GENERIC_WRITE;
    nt_access |= SYNCHRONIZE;

    NTSTATUS status = NtCreateFile(
        &file_handle, nt_access, &oa, &iosb, NULL,
        FILE_ATTRIBUTE_NORMAL, dwShareMode,
        win32_to_nt_disposition(dwCreationDisposition),
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
        NULL, 0
    );

    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return INVALID_HANDLE_VALUE;
    }

    return file_handle;
}

BOOL WINAPI ReadFile(HANDLE hFile, PVOID lpBuffer, DWORD nNumberOfBytesToRead,
              DWORD *lpNumberOfBytesRead, PVOID lpOverlapped)
{
    (void)lpOverlapped;

    IO_STATUS_BLOCK iosb;
    NTSTATUS status = NtReadFile(hFile, NULL, NULL, NULL, &iosb,
                                 lpBuffer, nNumberOfBytesToRead, NULL, NULL);

    if (NT_SUCCESS(status)) {
        if (lpNumberOfBytesRead)
            *lpNumberOfBytesRead = (DWORD)iosb.Information;
        return TRUE;
    }

    /* EOF is not an error in Win32 — just 0 bytes read */
    if (status == STATUS_END_OF_FILE) {
        if (lpNumberOfBytesRead)
            *lpNumberOfBytesRead = 0;
        return TRUE;
    }

    set_last_error_from_status(status);
    return FALSE;
}

BOOL WINAPI WriteFile(HANDLE hFile, PCVOID lpBuffer, DWORD nNumberOfBytesToWrite,
               DWORD *lpNumberOfBytesWritten, PVOID lpOverlapped)
{
    (void)lpOverlapped;

    IO_STATUS_BLOCK iosb;
    NTSTATUS status = NtWriteFile(hFile, NULL, NULL, NULL, &iosb,
                                  (PVOID)lpBuffer, nNumberOfBytesToWrite,
                                  NULL, NULL);

    if (NT_SUCCESS(status)) {
        if (lpNumberOfBytesWritten)
            *lpNumberOfBytesWritten = (DWORD)iosb.Information;
        return TRUE;
    }

    set_last_error_from_status(status);
    return FALSE;
}

BOOL WINAPI CloseHandle(HANDLE hObject)
{
    NTSTATUS status = NtClose(hObject);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return FALSE;
    }
    return TRUE;
}

/* ── Console API ────────────────────────────────────────────── */

HANDLE WINAPI GetStdHandle(DWORD nStdHandle)
{
    return console_handle(nStdHandle);
}

BOOL WINAPI WriteConsoleA(HANDLE hConsoleOutput, PCVOID lpBuffer,
                   DWORD nNumberOfCharsToWrite,
                   DWORD *lpNumberOfCharsWritten, PVOID lpReserved)
{
    (void)lpReserved;
    return WriteFile(hConsoleOutput, lpBuffer, nNumberOfCharsToWrite,
                     lpNumberOfCharsWritten, NULL);
}

/* ── Process API ────────────────────────────────────────────── */

void WINAPI ExitProcess(DWORD uExitCode)
{
    serial_puts("[K32] ExitProcess called, code=");
    serial_putdec(uExitCode);
    serial_puts("\n");
    NtTerminateProcess(NT_CURRENT_PROCESS, (NTSTATUS)uExitCode);
    /* Never returns */
    for (;;) __asm__ volatile("hlt");
}

HANDLE WINAPI GetCurrentProcess(void)
{
    return NT_CURRENT_PROCESS;
}

DWORD WINAPI GetCurrentProcessId(void)
{
    /* Stub: return 1 (single process for now) */
    return 1;
}

/* ── Memory API ─────────────────────────────────────────────── */

PVOID WINAPI VirtualAlloc(PVOID lpAddress, SIZE_T dwSize,
                   DWORD flAllocationType, DWORD flProtect)
{
    PVOID base = lpAddress;
    SIZE_T size = dwSize;

    NTSTATUS status = NtAllocateVirtualMemory(
        NT_CURRENT_PROCESS, &base, 0, &size,
        flAllocationType, flProtect);

    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return NULL;
    }

    return base;
}

BOOL WINAPI VirtualFree(PVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType)
{
    PVOID base = lpAddress;
    SIZE_T size = dwSize;

    NTSTATUS status = NtFreeVirtualMemory(
        NT_CURRENT_PROCESS, &base, &size, dwFreeType);

    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return FALSE;
    }

    return TRUE;
}

/* ── Heap API (bump allocator with size headers) ────────────── */
/*
 * HeapReAlloc/HeapSize expect an 8-byte size header at (ptr - 8).
 * HeapAlloc MUST write this header so realloc can copy the right amount.
 * Without it, HeapReAlloc reads garbage as old_size → data loss on grow.
 * This was the root cause of UE1 FName::Names corruption: TArray::Realloc
 * called appRealloc → HeapReAlloc, which failed to copy old entries.
 */

/* Win32 heap: dynamic size from sys_caps (scales with RAM).
 * Allocated lazily on first HeapAlloc call via kmalloc.
 * Falls back to 16MB static pool if kmalloc unavailable. */
#include "../include/sys_caps.h"

static BYTE   heap_pool_static[16 * 1024 * 1024]; /* fallback */
static BYTE  *heap_pool = NULL;
static SIZE_T heap_pool_size = 0;
static SIZE_T heap_offset = 0;

static void heap_pool_init(void)
{
    if (heap_pool) return;
    uint64_t target = g_sys_caps.win32_heap_size;
    if (!target) target = 16ULL * 1024 * 1024;

    extern void *kmalloc(uint64_t size);
    heap_pool = (BYTE *)kmalloc(target);
    if (heap_pool) {
        heap_pool_size = target;
    } else {
        heap_pool = heap_pool_static;
        heap_pool_size = sizeof(heap_pool_static);
    }
}

HANDLE WINAPI GetProcessHeap(void)
{
    /* Return a sentinel — we only have one heap */
    return (HANDLE)(ULONG_PTR)0xBEEF0001;
}

PVOID WINAPI HeapAlloc(HANDLE hHeap, DWORD dwFlags, SIZE_T dwBytes)
{
    (void)hHeap;
    static int heap_log_count = 0;

    /* 8-byte header + data, aligned to 16 bytes */
    SIZE_T total = (dwBytes + 8 + 15) & ~(SIZE_T)15;

    if (!heap_pool) heap_pool_init();
    if (heap_offset + total > heap_pool_size) {
        g_last_error = 8; /* ERROR_NOT_ENOUGH_MEMORY */
        return NULL;
    }

    BYTE *block = heap_pool + heap_offset;
    heap_offset += total;

    /* Write size header (same format as crt_malloc) */
    *(SIZE_T *)block = total;
    PVOID ptr = block + 8;

    if (dwFlags & 0x00000008) /* HEAP_ZERO_MEMORY */
        RtlZeroMemory(ptr, dwBytes);

    /* Log first few allocations to identify heap_pool base address */
    if (heap_log_count < 5) {
        heap_log_count++;
        serial_puts("[HEAP] alloc 0x");
        serial_puthex(dwBytes, 8);
        serial_puts(" -> 0x");
        serial_puthex((uint64_t)(ULONG_PTR)ptr, 16);
        serial_puts(" (pool=0x");
        serial_puthex((uint64_t)(ULONG_PTR)heap_pool, 16);
        serial_puts(")\n");
    }

    return ptr;
}

BOOL WINAPI HeapFree(HANDLE hHeap, DWORD dwFlags, PVOID lpMem)
{
    (void)hHeap;
    (void)dwFlags;
    (void)lpMem;
    /* Bump allocator doesn't free — acceptable for Phase 0 */
    return TRUE;
}

/* ── Error API ──────────────────────────────────────────────── */

DWORD WINAPI GetLastError(void)
{
    return g_last_error;
}

void WINAPI SetLastError(DWORD dwErrCode)
{
    g_last_error = dwErrCode;
    sync_last_error();
}

/* ── Misc API ───────────────────────────────────────────────── */

void WINAPI Sleep(DWORD dwMilliseconds)
{
    LARGE_INTEGER delay;
    /* Negative = relative time in 100ns units */
    delay.QuadPart = -(LONGLONG)dwMilliseconds * 10000LL;
    NtDelayExecution(FALSE, &delay);
}

BOOL WINAPI QueryPerformanceCounter(PLARGE_INTEGER lpPerformanceCount)
{
    NTSTATUS status = NtQueryPerformanceCounter(lpPerformanceCount, NULL);
    return NT_SUCCESS(status);
}

BOOL WINAPI QueryPerformanceFrequency(PLARGE_INTEGER lpFrequency)
{
    NTSTATUS status = NtQueryPerformanceCounter(NULL, lpFrequency);
    return NT_SUCCESS(status);
}

PVOID WINAPI GetProcAddress(HANDLE hModule, PCSTR lpProcName)
{
    if (!lpProcName) return NULL;

    /* If hModule is a loaded PE module, search its exports */
    if (hModule && (ULONG_PTR)hModule != 0xD1100001 &&
        (ULONG_PTR)hModule != 0x00400000) {
        /* Check if this handle matches a loaded module's base */
        LOADED_MODULE *mod = NULL;
        /* Scan modules by base address */
        extern LOADED_MODULE *dll_find_module(const char *);
        /* Direct export directory search */
        LOADED_MODULE search_mod;
        search_mod.image.ImageBase = hModule;
        PVOID fn = dll_resolve_export(&search_mod, lpProcName, 0, FALSE);
        if (fn) return fn;
    }

    /* Fall back to searching all shims and modules */
    PVOID result = dll_resolve_import("", lpProcName, 0, FALSE);
    if (!result) {
        serial_puts("[GPA] UNRESOLVED: ");
        serial_puts(lpProcName);
        serial_puts(" hMod=0x");
        serial_puthex((uint64_t)(ULONG_PTR)hModule, 8);
        serial_puts("\n");
    }
    return result;
}

HANDLE WINAPI GetModuleHandleA(PCSTR lpModuleName)
{
    (void)lpModuleName;
    /* Stub: return sentinel for the main exe */
    return (HANDLE)(ULONG_PTR)0x00400000;
}

HANDLE WINAPI GetModuleHandleW(PCWSTR lpModuleName)
{
    (void)lpModuleName;
    return (HANDLE)(ULONG_PTR)0x00400000;
}

/* ── File extended API ──────────────────────────────────────── */

DWORD WINAPI GetFileSize(HANDLE hFile, DWORD *lpFileSizeHigh)
{
    IO_STATUS_BLOCK iosb;
    FILE_STANDARD_INFORMATION info;

    NTSTATUS status = NtQueryInformationFile(hFile, &iosb, &info,
                                              sizeof(info),
                                              FileStandardInformation);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return (DWORD)-1;
    }

    if (lpFileSizeHigh)
        *lpFileSizeHigh = (DWORD)(info.EndOfFile.QuadPart >> 32);

    return (DWORD)(info.EndOfFile.QuadPart & 0xFFFFFFFF);
}

#define FILE_BEGIN   0
#define FILE_CURRENT 1
#define FILE_END     2

BOOL WINAPI SetFilePointer(HANDLE hFile, LONG lDistanceToMove,
                    LONG *lpDistanceToMoveHigh, DWORD dwMoveMethod)
{
    /* First get current position and size */
    IO_STATUS_BLOCK iosb;
    FILE_POSITION_INFORMATION pos_info;
    FILE_STANDARD_INFORMATION std_info;

    LONGLONG new_pos;

    switch (dwMoveMethod) {
    case FILE_BEGIN:
        new_pos = (LONGLONG)lDistanceToMove;
        if (lpDistanceToMoveHigh)
            new_pos |= ((LONGLONG)*lpDistanceToMoveHigh) << 32;
        break;

    case FILE_CURRENT:
        NtQueryInformationFile(hFile, &iosb, &pos_info,
                                sizeof(pos_info), FilePositionInformation);
        new_pos = pos_info.CurrentByteOffset.QuadPart + lDistanceToMove;
        break;

    case FILE_END:
        NtQueryInformationFile(hFile, &iosb, &std_info,
                                sizeof(std_info), FileStandardInformation);
        new_pos = std_info.EndOfFile.QuadPart + lDistanceToMove;
        break;

    default:
        g_last_error = 87; /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    pos_info.CurrentByteOffset.QuadPart = new_pos;
    NTSTATUS status = NtSetInformationFile(hFile, &iosb, &pos_info,
                                            sizeof(pos_info),
                                            FilePositionInformation);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return FALSE;
    }

    if (lpDistanceToMoveHigh)
        *lpDistanceToMoveHigh = (LONG)(new_pos >> 32);

    return TRUE;
}

/* ── File copy/delete/move stubs (UT99) ─────────────────────── */

BOOL WINAPI CopyFileA(PCSTR lpExistingFileName, PCSTR lpNewFileName, BOOL bFailIfExists)
{
    (void)lpExistingFileName;
    (void)lpNewFileName;
    (void)bFailIfExists;
    g_last_error = 2; /* ERROR_FILE_NOT_FOUND */
    return FALSE;
}

BOOL WINAPI CopyFileW(PCWSTR lpExistingFileName, PCWSTR lpNewFileName, BOOL bFailIfExists)
{
    (void)lpExistingFileName;
    (void)lpNewFileName;
    (void)bFailIfExists;
    g_last_error = 2; /* ERROR_FILE_NOT_FOUND */
    return FALSE;
}

BOOL WINAPI DeleteFileA(PCSTR lpFileName)
{
    (void)lpFileName;
    g_last_error = 2; /* ERROR_FILE_NOT_FOUND */
    return FALSE;
}

BOOL WINAPI DeleteFileW(PCWSTR lpFileName)
{
    (void)lpFileName;
    g_last_error = 2; /* ERROR_FILE_NOT_FOUND */
    return FALSE;
}

BOOL WINAPI MoveFileA(PCSTR lpExistingFileName, PCSTR lpNewFileName)
{
    (void)lpExistingFileName;
    (void)lpNewFileName;
    g_last_error = 2; /* ERROR_FILE_NOT_FOUND */
    return FALSE;
}

BOOL WINAPI MoveFileW(PCWSTR lpExistingFileName, PCWSTR lpNewFileName)
{
    (void)lpExistingFileName;
    (void)lpNewFileName;
    g_last_error = 2; /* ERROR_FILE_NOT_FOUND */
    return FALSE;
}

/* ── Handle duplication ─────────────────────────────────────── */

BOOL WINAPI DuplicateHandle(HANDLE hSourceProcessHandle, HANDLE hSourceHandle,
                     HANDLE hTargetProcessHandle, PHANDLE lpTargetHandle,
                     DWORD dwDesiredAccess, BOOL bInheritHandle,
                     DWORD dwOptions)
{
    (void)bInheritHandle;
    NTSTATUS status = NtDuplicateObject(hSourceProcessHandle, hSourceHandle,
                                         hTargetProcessHandle, lpTargetHandle,
                                         dwDesiredAccess, 0, dwOptions);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return FALSE;
    }
    return TRUE;
}

/* ── Memory protection ──────────────────────────────────────── */

BOOL WINAPI VirtualProtect(PVOID lpAddress, SIZE_T dwSize,
                    DWORD flNewProtect, DWORD *lpflOldProtect)
{
    PVOID base = lpAddress;
    SIZE_T size = dwSize;
    ULONG old_prot = 0;

    NTSTATUS status = NtProtectVirtualMemory(NT_CURRENT_PROCESS,
                                              &base, &size,
                                              flNewProtect, &old_prot);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return FALSE;
    }

    if (lpflOldProtect)
        *lpflOldProtect = old_prot;

    return TRUE;
}

/* ── Memory query ──────────────────────────────────────────── */

typedef struct _MEMORY_BASIC_INFORMATION_K32 {
    PVOID       BaseAddress;
    PVOID       AllocationBase;
    ULONG       AllocationProtect;
    USHORT      PartitionId;
    USHORT      Padding0;
    SIZE_T      RegionSize;
    ULONG       State;
    ULONG       Protect;
    ULONG       Type;
    ULONG       Padding1;
} MEMORY_BASIC_INFORMATION_K32;

SIZE_T WINAPI VirtualQuery(PVOID lpAddress, PVOID lpBuffer, SIZE_T dwLength)
{
    if (!lpBuffer || dwLength < sizeof(MEMORY_BASIC_INFORMATION_K32))
        return 0;

    SIZE_T ret_len = 0;
    NTSTATUS status = NtQueryVirtualMemory(NT_CURRENT_PROCESS,
                                            lpAddress, 0 /* MemoryBasicInformation */,
                                            lpBuffer, dwLength, &ret_len);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return 0;
    }

    return ret_len;
}

/* ── String API (commonly needed by CRT) ────────────────────── */

int WINAPI lstrlenA(PCSTR lpString)
{
    if (!lpString) return 0;
    int len = 0;
    while (lpString[len]) len++;
    return len;
}

int WINAPI lstrlenW(PCWSTR lpString)
{
    if (!lpString) return 0;
    int len = 0;
    while (lpString[len]) len++;
    return len;
}

/* ── Command line (stub) ────────────────────────────────────── */

static char  g_cmdline_a[] = "UnrealTournament.exe CityIntro.unr";
static WCHAR g_cmdline_w[] = {'U','n','r','e','a','l','T','o','u','r','n','a','m','e','n','t','.','e','x','e',' ','C','i','t','y','I','n','t','r','o','.','u','n','r',0};

PCSTR WINAPI GetCommandLineA(void)
{
    return g_cmdline_a;
}

PCWSTR WINAPI GetCommandLineW(void)
{
    return g_cmdline_w;
}

/* ── Environment (stub) ─────────────────────────────────────── */

PCSTR WINAPI GetEnvironmentStringsA(void)
{
    /* Empty environment: double-null terminated */
    static char env[] = "\0";
    return env;
}

BOOL WINAPI FreeEnvironmentStringsA(PCSTR lpszEnvironmentBlock)
{
    (void)lpszEnvironmentBlock;
    return TRUE;
}

/* ── Critical Section ───────────────────────────────────────── */
/*
 * Single-threaded for now — no actual locking needed.
 * When OsitoK gets real threads, these become spinlocks + futex.
 */

void WINAPI InitializeCriticalSection(LPCRITICAL_SECTION lpCS)
{
    if (!lpCS) return;
    lpCS->DebugInfo      = NULL;
    lpCS->LockCount      = -1;
    lpCS->RecursionCount = 0;
    lpCS->OwningThread   = NULL;
    lpCS->LockSemaphore  = NULL;
    lpCS->SpinCount      = 0;
}

void WINAPI InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION lpCS, DWORD dwSpinCount)
{
    InitializeCriticalSection(lpCS);
    if (lpCS) lpCS->SpinCount = dwSpinCount;
}

void WINAPI EnterCriticalSection(LPCRITICAL_SECTION lpCS)
{
    if (!lpCS) return;
    lpCS->LockCount++;
    lpCS->RecursionCount++;
    lpCS->OwningThread = (HANDLE)(ULONG_PTR)1; /* current thread */
}

BOOL WINAPI TryEnterCriticalSection(LPCRITICAL_SECTION lpCS)
{
    EnterCriticalSection(lpCS);
    return TRUE;
}

void WINAPI LeaveCriticalSection(LPCRITICAL_SECTION lpCS)
{
    if (!lpCS) return;
    lpCS->RecursionCount--;
    if (lpCS->RecursionCount == 0) {
        lpCS->OwningThread = NULL;
    }
    lpCS->LockCount--;
}

void WINAPI DeleteCriticalSection(LPCRITICAL_SECTION lpCS)
{
    if (!lpCS) return;
    lpCS->LockCount      = -1;
    lpCS->RecursionCount = 0;
    lpCS->OwningThread   = NULL;
}

/* ── Thread Local Storage ──────────────────────────────────── */

#define TLS_MAX_SLOTS 64
static PVOID tls_slots[TLS_MAX_SLOTS];
static BOOL  tls_used[TLS_MAX_SLOTS];

DWORD WINAPI TlsAlloc(void)
{
    for (DWORD i = 0; i < TLS_MAX_SLOTS; i++) {
        if (!tls_used[i]) {
            tls_used[i] = TRUE;
            tls_slots[i] = NULL;
            return i;
        }
    }
    g_last_error = 87; /* ERROR_INVALID_PARAMETER */
    return (DWORD)-1; /* TLS_OUT_OF_INDEXES */
}

BOOL WINAPI TlsFree(DWORD dwTlsIndex)
{
    if (dwTlsIndex >= TLS_MAX_SLOTS) return FALSE;
    tls_used[dwTlsIndex] = FALSE;
    tls_slots[dwTlsIndex] = NULL;
    return TRUE;
}

PVOID WINAPI TlsGetValue(DWORD dwTlsIndex)
{
    if (dwTlsIndex >= TLS_MAX_SLOTS) return NULL;
    g_last_error = 0;
    return tls_slots[dwTlsIndex];
}

BOOL WINAPI TlsSetValue(DWORD dwTlsIndex, PVOID lpTlsValue)
{
    if (dwTlsIndex >= TLS_MAX_SLOTS) return FALSE;
    tls_slots[dwTlsIndex] = lpTlsValue;
    return TRUE;
}

/* ── Thread API ────────────────────────────────────────────── */

static DWORD g_thread_id_counter = 1;

HANDLE WINAPI CreateThread(PVOID lpThreadAttributes, SIZE_T dwStackSize,
                           LPTHREAD_START_ROUTINE lpStartAddress,
                           PVOID lpParameter, DWORD dwCreationFlags,
                           DWORD *lpThreadId)
{
    (void)lpThreadAttributes;
    (void)dwStackSize;
    (void)dwCreationFlags;

    /*
     * Stub: single-threaded — run the thread function synchronously.
     * Real implementation needs OsitoK scheduler integration.
     */
    DWORD tid = ++g_thread_id_counter;
    if (lpThreadId) *lpThreadId = tid;

    /* Run immediately in current context */
    lpStartAddress(lpParameter);

    /* Return a pseudo-handle */
    return (HANDLE)(ULONG_PTR)tid;
}

DWORD WINAPI GetCurrentThreadId(void) { return 1; }
HANDLE WINAPI GetCurrentThread(void) { return NT_CURRENT_THREAD; }

DWORD WINAPI SuspendThread(HANDLE hThread)
{
    (void)hThread;
    return 0; /* previous suspend count */
}

DWORD WINAPI ResumeThread(HANDLE hThread)
{
    (void)hThread;
    return 0;
}

BOOL WINAPI TerminateThread(HANDLE hThread, DWORD dwExitCode)
{
    (void)hThread;
    (void)dwExitCode;
    return TRUE;
}

#define WAIT_OBJECT_0   0x00000000
#define WAIT_TIMEOUT    0x00000102
#define WAIT_FAILED     0xFFFFFFFF
#define INFINITE        0xFFFFFFFF

DWORD WINAPI WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds)
{
    extern NTSTATUS NtWaitForSingleObject(HANDLE, BOOL, PLARGE_INTEGER);

    LARGE_INTEGER timeout;
    PLARGE_INTEGER p_timeout = NULL;

    if (dwMilliseconds != INFINITE) {
        /* Convert milliseconds to 100ns units, negative = relative */
        timeout.QuadPart = -(LONGLONG)dwMilliseconds * 10000;
        p_timeout = &timeout;
    }

    NTSTATUS status = NtWaitForSingleObject(hHandle, FALSE, p_timeout);

    if (status == STATUS_TIMEOUT)
        return WAIT_TIMEOUT;
    if (NT_SUCCESS(status))
        return WAIT_OBJECT_0;

    set_last_error_from_status(status);
    return WAIT_FAILED;
}

DWORD WINAPI WaitForMultipleObjects(DWORD nCount, const HANDLE *lpHandles,
                                     BOOL bWaitAll, DWORD dwMilliseconds)
{
    extern NTSTATUS sys_NtWaitForMultipleObjects(ULONG_PTR *args);

    LARGE_INTEGER timeout;
    PLARGE_INTEGER p_timeout = NULL;

    if (dwMilliseconds != INFINITE) {
        timeout.QuadPart = -(LONGLONG)dwMilliseconds * 10000;
        p_timeout = &timeout;
    }

    ULONG_PTR args[5] = {
        (ULONG_PTR)nCount,
        (ULONG_PTR)lpHandles,
        (ULONG_PTR)(bWaitAll ? 0 : 1),  /* NT: 0=WaitAll, 1=WaitAny */
        (ULONG_PTR)FALSE,               /* Alertable */
        (ULONG_PTR)p_timeout
    };

    NTSTATUS status = sys_NtWaitForMultipleObjects(args);

    if (status == STATUS_TIMEOUT)
        return WAIT_TIMEOUT;
    if (NT_SUCCESS(status))
        return (DWORD)status;  /* STATUS_WAIT_0 + index */

    set_last_error_from_status(status);
    return WAIT_FAILED;
}

BOOL WINAPI SetThreadPriority(HANDLE hThread, int nPriority)
{
    (void)hThread;
    (void)nPriority;
    return TRUE;
}

/* ── Event API (Phase 21) ──────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);

extern NTSTATUS NtCreateEvent(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
                              ULONG, BOOL);
extern NTSTATUS NtSetEvent(HANDLE, LONG *);
extern NTSTATUS NtResetEvent(HANDLE, LONG *);
extern NTSTATUS NtPulseEvent(HANDLE, LONG *);

#define EVENT_TYPE_NOTIFICATION_K32     0   /* manual-reset */
#define EVENT_TYPE_SYNCHRONIZATION_K32  1   /* auto-reset */

HANDLE WINAPI CreateEventA(PVOID lpEventAttributes, BOOL bManualReset,
                           BOOL bInitialState, PCSTR lpName)
{
    (void)lpEventAttributes;
    (void)lpName;

    serial_puts("[K32] CreateEventA: manual=");
    serial_puthex(bManualReset, 1);
    serial_puts(" initial=");
    serial_puthex(bInitialState, 1);
    serial_puts("\n");

    HANDLE h = NULL;
    ULONG event_type = bManualReset ? EVENT_TYPE_NOTIFICATION_K32
                                    : EVENT_TYPE_SYNCHRONIZATION_K32;

    NTSTATUS status = NtCreateEvent(&h, GENERIC_ALL, NULL,
                                    event_type, bInitialState);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return NULL;
    }

    return h;
}

HANDLE WINAPI CreateEventW(PVOID lpEventAttributes, BOOL bManualReset,
                           BOOL bInitialState, PCWSTR lpName)
{
    (void)lpEventAttributes;
    (void)lpName;

    serial_puts("[K32] CreateEventW: manual=");
    serial_puthex(bManualReset, 1);
    serial_puts(" initial=");
    serial_puthex(bInitialState, 1);
    serial_puts("\n");

    HANDLE h = NULL;
    ULONG event_type = bManualReset ? EVENT_TYPE_NOTIFICATION_K32
                                    : EVENT_TYPE_SYNCHRONIZATION_K32;

    NTSTATUS status = NtCreateEvent(&h, GENERIC_ALL, NULL,
                                    event_type, bInitialState);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return NULL;
    }

    return h;
}

BOOL WINAPI SetEvent(HANDLE hEvent)
{
    serial_puts("[K32] SetEvent\n");
    NTSTATUS status = NtSetEvent(hEvent, NULL);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI ResetEvent(HANDLE hEvent)
{
    serial_puts("[K32] ResetEvent\n");
    NTSTATUS status = NtResetEvent(hEvent, NULL);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI PulseEvent(HANDLE hEvent)
{
    serial_puts("[K32] PulseEvent\n");
    NTSTATUS status = NtPulseEvent(hEvent, NULL);
    if (!NT_SUCCESS(status)) {
        set_last_error_from_status(status);
        return FALSE;
    }
    return TRUE;
}

HANDLE WINAPI OpenEventA(DWORD dwDesiredAccess, BOOL bInheritHandle, PCSTR lpName)
{
    (void)dwDesiredAccess;
    (void)bInheritHandle;
    (void)lpName;
    serial_puts("[K32] OpenEventA: stub, returning NULL\n");
    g_last_error = 2;  /* ERROR_FILE_NOT_FOUND */
    return NULL;
}

HANDLE WINAPI OpenEventW(DWORD dwDesiredAccess, BOOL bInheritHandle, PCWSTR lpName)
{
    (void)dwDesiredAccess;
    (void)bInheritHandle;
    (void)lpName;
    serial_puts("[K32] OpenEventW: stub, returning NULL\n");
    g_last_error = 2;  /* ERROR_FILE_NOT_FOUND */
    return NULL;
}

/* ── Mutex API (UT99) ──────────────────────────────────────── */

HANDLE WINAPI CreateMutexA(PVOID lpMutexAttributes, BOOL bInitialOwner, PCSTR lpName)
{
    (void)lpMutexAttributes;
    (void)bInitialOwner;
    (void)lpName;
    /* Return a sentinel handle — single-threaded, no real mutex needed */
    return (HANDLE)(ULONG_PTR)0xBEEF0002;
}

HANDLE WINAPI CreateMutexW(PVOID lpMutexAttributes, BOOL bInitialOwner, PCWSTR lpName)
{
    (void)lpMutexAttributes;
    (void)bInitialOwner;
    serial_puts("[K32] CreateMutexW called\n");
    if (lpName) {
        /* Print wide name as narrow for debug */
        char nbuf[64]; int i;
        for (i = 0; lpName[i] && i < 63; i++) nbuf[i] = (char)(lpName[i] & 0xFF);
        nbuf[i] = 0;
        serial_puts("[K32] Mutex name: ");
        serial_puts(nbuf);
        serial_puts("\n");
    }
    g_last_error = 0; /* NOT ERROR_ALREADY_EXISTS */
    return (HANDLE)(ULONG_PTR)0xBEEF0002;
}

/* ── DLL / Module API ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);

/* File reading for DLL loading */
extern void *osfs2_find(const char *name);
extern int   osfs2_read(void *file, uint64_t offset, void *buf, uint64_t len);
extern uint64_t osfs2_file_size(void *file);

HANDLE WINAPI LoadLibraryA(PCSTR lpLibFileName)
{
    if (!lpLibFileName) return NULL;

    serial_puts("[K32] LoadLibraryA: ");
    serial_puts(lpLibFileName);
    serial_puts("\n");

    /* Check if it's a built-in shim DLL — return a sentinel handle */
    LOADED_MODULE *mod = dll_find_module(lpLibFileName);
    if (mod)
        return mod->image.ImageBase ? mod->image.ImageBase
                                    : (HANDLE)(ULONG_PTR)0xD1100001;

    /* Check if there's a shim registered for this DLL */
    /* (For ntdll/kernel32/msvcrt that don't have real PE modules) */
    PVOID test = dll_resolve_import(lpLibFileName, NULL, 0, FALSE);
    (void)test; /* Just checking if shim exists */

    /* Try to load the DLL file from the filesystem */
    /* Extract basename and try with/without .dll extension */
    const char *basename = lpLibFileName;
    for (const char *p = lpLibFileName; *p; p++) {
        if (*p == '\\' || *p == '/') basename = p + 1;
    }

    void *fsfile = osfs2_find(basename);
    if (!fsfile)
        fsfile = osfs2_find(lpLibFileName);

    /* If not found, try appending .dll (engine often omits extension) */
    if (!fsfile) {
        char with_dll[128];
        int j = 0;
        int has_dot = 0;
        for (const char *p = basename; *p && j < 120; p++, j++) {
            with_dll[j] = *p;
            if (*p == '.') has_dot = 1;
        }
        if (!has_dot) {
            with_dll[j++] = '.'; with_dll[j++] = 'd';
            with_dll[j++] = 'l'; with_dll[j++] = 'l';
        }
        with_dll[j] = 0;
        fsfile = osfs2_find(with_dll);
    }

    if (fsfile) {
        uint64_t fsize = osfs2_file_size(fsfile);
        if (fsize > 0) {
            /* Allocate buffer and read */
            extern void *mem_alloc_pages(uint64_t count);
            extern void  mem_free_pages(void *addr, uint64_t count);
            uint64_t pages = (fsize + 0xFFF) / 4096;
            uint8_t *buf = (uint8_t *)mem_alloc_pages(pages);
            if (buf) {
                osfs2_read(fsfile, 0, buf, fsize);
                PVOID base = dll_load(lpLibFileName, buf, fsize);
                mem_free_pages(buf, pages);
                if (base) return (HANDLE)base;
            }
        }
    }

    /* Not found — return sentinel so callers don't crash */
    serial_puts("[K32] DLL not found, returning sentinel\n");
    return (HANDLE)(ULONG_PTR)0xD1100001;
}

HANDLE WINAPI LoadLibraryW(PCWSTR lpLibFileName)
{
    if (!lpLibFileName) return NULL;

    /* Convert wide to ASCII */
    char name[260];
    int i;
    for (i = 0; i < 259 && lpLibFileName[i]; i++)
        name[i] = (char)(lpLibFileName[i] & 0xFF);
    name[i] = 0;

    serial_puts("[K32] LoadLibraryW: ");
    serial_puts(name);
    serial_puts("\n");

    return LoadLibraryA(name);
}

HANDLE WINAPI LoadLibraryExA(PCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
{
    (void)hFile;
    (void)dwFlags;
    return LoadLibraryA(lpLibFileName);
}

BOOL WINAPI FreeLibrary(HANDLE hLibModule)
{
    (void)hLibModule;
    return TRUE;
}

DWORD WINAPI GetModuleFileNameA(HANDLE hModule, PSTR lpFilename, DWORD nSize)
{
    (void)hModule;
    /* Build full path: "C:\System\<exe_name>" so engine can derive install dir */
    extern char win32_exe_name[64];
    static const char prefix[] = "C:\\System\\";
    DWORD pos = 0;

    for (int i = 0; prefix[i] && pos < nSize - 1; i++)
        lpFilename[pos++] = prefix[i];
    for (int i = 0; win32_exe_name[i] && pos < nSize - 1; i++)
        lpFilename[pos++] = win32_exe_name[i];
    lpFilename[pos] = 0;
    return pos;
}

DWORD WINAPI GetModuleFileNameW(HANDLE hModule, PWSTR lpFilename, DWORD nSize)
{
    (void)hModule;
    /* Build wide path: "C:\System\<name>" matching GetModuleFileNameA */
    extern char win32_exe_name[64];
    static const WCHAR prefix[] = {'C',':','\\','S','y','s','t','e','m','\\'};
    DWORD pos = 0;

    for (DWORD i = 0; i < 10 && pos < nSize - 1; i++)
        lpFilename[pos++] = prefix[i];
    for (int i = 0; win32_exe_name[i] && pos < nSize - 1; i++)
        lpFilename[pos++] = (WCHAR)(unsigned char)win32_exe_name[i];

    lpFilename[pos] = 0;
    return pos;
}

/* ── Timing ────────────────────────────────────────────────── */

extern uint64_t idt_get_ticks(void);

DWORD WINAPI GetTickCount(void)
{
    return (DWORD)idt_get_ticks();
}

ULONGLONG WINAPI GetTickCount64(void)
{
    return (ULONGLONG)idt_get_ticks();
}

void WINAPI GetSystemTimeAsFileTime(PVOID lpSystemTimeAsFileTime)
{
    /* Return a plausible FILETIME (Jan 1 2024 as 100ns intervals since 1601) */
    if (lpSystemTimeAsFileTime) {
        ULONGLONG *ft = (ULONGLONG *)lpSystemTimeAsFileTime;
        *ft = 133500000000000000ULL + (ULONGLONG)idt_get_ticks() * 10000ULL;
    }
}

/* ── System Info ───────────────────────────────────────────── */

void WINAPI GetSystemInfo(LPSYSTEM_INFO lpSystemInfo)
{
    if (!lpSystemInfo) return;

    /*
     * PE32 (i386) code allocates a 32-bit SYSTEM_INFO (36 bytes) on the
     * stack. Our 64-bit struct is 48 bytes (PVOID/ULONG_PTR are 8 bytes).
     * Writing 48 bytes to a 36-byte buffer overflows 12 bytes, corrupting
     * the caller's stack frame. Bytes 44-45 contain wProcessorLevel=6,
     * which lands on the saved SEH ExceptionList → value 6 in SEH chain.
     *
     * Fix: write using 32-bit struct layout (all fields 4 bytes or less).
     *
     * 32-bit SYSTEM_INFO layout (36 bytes):
     *   +0:  WORD  wProcessorArchitecture
     *   +2:  WORD  wReserved
     *   +4:  DWORD dwPageSize
     *   +8:  DWORD lpMinimumApplicationAddress  (4-byte ptr!)
     *   +12: DWORD lpMaximumApplicationAddress  (4-byte ptr!)
     *   +16: DWORD dwActiveProcessorMask        (4-byte!)
     *   +20: DWORD dwNumberOfProcessors
     *   +24: DWORD dwProcessorType
     *   +28: DWORD dwAllocationGranularity
     *   +32: WORD  wProcessorLevel
     *   +34: WORD  wProcessorRevision
     */
    extern int g_compat32_mode;
    if (g_compat32_mode) {
        uint8_t *p = (uint8_t *)lpSystemInfo;
        for (int i = 0; i < 36; i++) p[i] = 0;
        *(uint16_t *)(p + 0)  = 0;     /* PROCESSOR_ARCHITECTURE_INTEL (i386) */
        *(uint32_t *)(p + 4)  = 4096;  /* dwPageSize */
        *(uint32_t *)(p + 8)  = 0x10000;    /* lpMinimumApplicationAddress */
        *(uint32_t *)(p + 12) = 0x7FFEFFFF; /* lpMaximumApplicationAddress */
        *(uint32_t *)(p + 16) = 1;     /* dwActiveProcessorMask */
        *(uint32_t *)(p + 20) = 1;     /* dwNumberOfProcessors */
        *(uint32_t *)(p + 24) = 586;   /* dwProcessorType (PROCESSOR_INTEL_PENTIUM) */
        *(uint32_t *)(p + 28) = 65536; /* dwAllocationGranularity */
        *(uint16_t *)(p + 32) = 6;     /* wProcessorLevel */
        *(uint16_t *)(p + 34) = 0;     /* wProcessorRevision */
        return;
    }

    lpSystemInfo->wProcessorArchitecture  = 9; /* PROCESSOR_ARCHITECTURE_AMD64 */
    lpSystemInfo->dwPageSize              = 4096;
    lpSystemInfo->lpMinimumApplicationAddress = (PVOID)(ULONG_PTR)0x10000;
    lpSystemInfo->lpMaximumApplicationAddress = (PVOID)(ULONG_PTR)0x7FFFFFFEFFFF;
    lpSystemInfo->dwActiveProcessorMask   = 1;
    lpSystemInfo->dwNumberOfProcessors    = 1;
    lpSystemInfo->dwProcessorType         = 8664; /* AMD64 */
    lpSystemInfo->dwAllocationGranularity = 65536;
    lpSystemInfo->wProcessorLevel         = 6;
    lpSystemInfo->wProcessorRevision      = 0;
}

BOOL WINAPI GetVersionExA(LPOSVERSIONINFOA lpVersionInformation)
{
    if (!lpVersionInformation) return FALSE;
    /* Report as Windows XP x64 (5.2) — what UT99 expects */
    lpVersionInformation->dwMajorVersion = 5;
    lpVersionInformation->dwMinorVersion = 2;
    lpVersionInformation->dwBuildNumber  = 3790;
    lpVersionInformation->dwPlatformId   = 2; /* VER_PLATFORM_WIN32_NT */
    for (int i = 0; i < 128; i++)
        lpVersionInformation->szCSDVersion[i] = 0;
    lpVersionInformation->szCSDVersion[0] = 'S';
    lpVersionInformation->szCSDVersion[1] = 'P';
    lpVersionInformation->szCSDVersion[2] = '2';
    return TRUE;
}

/* ── Path / Directory ──────────────────────────────────────── */

DWORD WINAPI GetFullPathNameA(PCSTR lpFileName, DWORD nBufferLength,
                              PSTR lpBuffer, PSTR *lpFilePart)
{
    if (!lpFileName || !lpBuffer) return 0;
    DWORD len = 0;
    while (lpFileName[len]) len++;
    if (len >= nBufferLength) return len + 1;
    for (DWORD i = 0; i <= len; i++) lpBuffer[i] = lpFileName[i];
    if (lpFilePart) {
        /* Find last backslash or slash */
        *lpFilePart = lpBuffer;
        for (DWORD i = 0; i < len; i++) {
            if (lpBuffer[i] == '\\' || lpBuffer[i] == '/')
                *lpFilePart = lpBuffer + i + 1;
        }
    }
    return len;
}

DWORD WINAPI GetCurrentDirectoryA(DWORD nBufferLength, PSTR lpBuffer)
{
    const char *dir = "C:\\System";
    DWORD len = 9;
    if (nBufferLength > len) {
        for (DWORD i = 0; i <= len; i++) lpBuffer[i] = dir[i];
    }
    return len;
}

DWORD WINAPI GetFileAttributesA(PCSTR lpFileName)
{
    if (!lpFileName) return (DWORD)-1;

    /* Extract basename (OsitoFS is flat) */
    const char *base = lpFileName;
    int has_dot = 0;
    for (const char *p = lpFileName; *p; p++) {
        if (*p == '\\' || *p == '/') base = p + 1;
        if (*p == '.') has_dot = 1;
    }

    /* Empty basename after stripping path = directory reference */
    if (!*base || !has_dot) {
        /* Treat as directory — UT99 checks ".", "..", "..\Maps", etc. */
        return 0x10; /* FILE_ATTRIBUTE_DIRECTORY */
    }

    /* Check OsitoFS for the file */
    void *f = osfs2_find(base);
    if (f) return 0x80; /* FILE_ATTRIBUTE_NORMAL */

    /* Also try the full relative path as-is */
    if (base != lpFileName) {
        f = osfs2_find(lpFileName);
        if (f) return 0x80;
    }

    serial_puts("[K32] GetFileAttributesA NOT_FOUND: '");
    serial_puts(lpFileName);
    serial_puts("'\n");
    g_last_error = 2; /* ERROR_FILE_NOT_FOUND */
    return (DWORD)-1; /* INVALID_FILE_ATTRIBUTES */
}

BOOL WINAPI SetFileAttributesA(PCSTR lpFileName, DWORD dwFileAttributes)
{
    (void)lpFileName;
    (void)dwFileAttributes;
    return TRUE;
}

BOOL WINAPI CreateDirectoryA(PCSTR lpPathName, PVOID lpSecurityAttributes)
{
    (void)lpPathName;
    (void)lpSecurityAttributes;
    return TRUE;
}

BOOL WINAPI RemoveDirectoryA(PCSTR lpPathName)
{
    (void)lpPathName;
    return TRUE;
}

BOOL WINAPI CreateDirectoryW(PCWSTR lpPathName, PVOID lpSecurityAttributes)
{
    (void)lpPathName;
    (void)lpSecurityAttributes;
    return TRUE;
}

BOOL WINAPI RemoveDirectoryW(PCWSTR lpPathName)
{
    (void)lpPathName;
    g_last_error = 2; /* ERROR_FILE_NOT_FOUND */
    return FALSE;
}

DWORD WINAPI GetCurrentDirectoryW(DWORD nBufferLength, PWSTR lpBuffer)
{
    static const WCHAR dir[] = {'C',':','\\','S','y','s','t','e','m',0};
    DWORD len = 9;
    if (nBufferLength > len && lpBuffer) {
        for (DWORD i = 0; i <= len; i++) lpBuffer[i] = dir[i];
    }
    return len;
}

BOOL WINAPI SetCurrentDirectoryA(PCSTR lpPathName)
{
    (void)lpPathName;
    return TRUE;
}

BOOL WINAPI SetCurrentDirectoryW(PCWSTR lpPathName)
{
    (void)lpPathName;
    return TRUE;
}

BOOL WINAPI SetFileAttributesW(PCWSTR lpFileName, DWORD dwFileAttributes)
{
    (void)lpFileName;
    (void)dwFileAttributes;
    return TRUE;
}

/* ── System / Windows Directory (UT99) ─────────────────────── */

DWORD WINAPI GetSystemDirectoryA(PSTR lpBuffer, DWORD uSize)
{
    const char *dir = "C:\\Windows\\System32";
    DWORD len = 0;
    while (dir[len]) len++;
    if (lpBuffer && uSize > len) {
        for (DWORD i = 0; i <= len; i++) lpBuffer[i] = dir[i];
    }
    return len;
}

DWORD WINAPI GetSystemDirectoryW(PWSTR lpBuffer, DWORD uSize)
{
    static const WCHAR dir[] = {'C',':','\\','W','i','n','d','o','w','s','\\',
                                 'S','y','s','t','e','m','3','2',0};
    DWORD len = 0;
    while (dir[len]) len++;
    if (lpBuffer && uSize > len) {
        for (DWORD i = 0; i <= len; i++) lpBuffer[i] = dir[i];
    }
    return len;
}

DWORD WINAPI GetWindowsDirectoryA(PSTR lpBuffer, DWORD uSize)
{
    const char *dir = "C:\\Windows";
    DWORD len = 0;
    while (dir[len]) len++;
    if (lpBuffer && uSize > len) {
        for (DWORD i = 0; i <= len; i++) lpBuffer[i] = dir[i];
    }
    return len;
}

DWORD WINAPI GetWindowsDirectoryW(PWSTR lpBuffer, DWORD uSize)
{
    static const WCHAR dir[] = {'C',':','\\','W','i','n','d','o','w','s',0};
    DWORD len = 0;
    while (dir[len]) len++;
    if (lpBuffer && uSize > len) {
        for (DWORD i = 0; i <= len; i++) lpBuffer[i] = dir[i];
    }
    return len;
}

/* ── Find File ─────────────────────────────────────────────── */

/* ── FindFirstFile/FindNextFile backed by OsitoFS ──────────── */

extern int osfs2_find_first(const char *pattern, int start_idx);
typedef struct { char name[64]; uint64_t size; } osfs2_file_t;
extern osfs2_file_t *osfs2_get_file(int index);

/* Find handle: store pattern + current index */
#define MAX_FIND_HANDLES 8
static struct {
    char pattern[64];   /* e.g. "*.u" */
    int  next_idx;      /* next OsitoFS index to search */
    bool in_use;
} find_handles[MAX_FIND_HANDLES];

static void fill_find_data_a(LPWIN32_FIND_DATAA fd, osfs2_file_t *f)
{
    memset(fd, 0, sizeof(*fd));
    fd->dwFileAttributes = 0x80; /* FILE_ATTRIBUTE_NORMAL */
    fd->nFileSizeLow = (uint32_t)(f->size & 0xFFFFFFFF);
    fd->nFileSizeHigh = (uint32_t)(f->size >> 32);
    /* Copy name (max 260 chars) */
    int i = 0;
    while (f->name[i] && i < 259) { fd->cFileName[i] = f->name[i]; i++; }
    fd->cFileName[i] = 0;
}

/* Extract basename pattern from "C:\System\*.u" → "*.u" */
static const char *extract_pattern(const char *path)
{
    const char *p = path;
    const char *last = path;
    while (*p) { if (*p == '\\' || *p == '/') last = p + 1; p++; }
    return last;
}

HANDLE WINAPI FindFirstFileA(PCSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData)
{
    if (!lpFileName || !lpFindFileData) {
        g_last_error = 87; /* ERROR_INVALID_PARAMETER */
        return INVALID_HANDLE_VALUE;
    }

    const char *pattern = extract_pattern(lpFileName);

    serial_puts("[K32] FindFirstFileA: '");
    serial_puts(lpFileName);
    serial_puts("' pattern='");
    serial_puts(pattern);
    serial_puts("'\n");

    int idx = osfs2_find_first(pattern, 0);
    if (idx < 0) {
        serial_puts("[K32] FindFirstFileA: no match\n");
        g_last_error = 2; /* ERROR_FILE_NOT_FOUND */
        return INVALID_HANDLE_VALUE;
    }

    /* Allocate find handle */
    int slot = -1;
    for (int i = 0; i < MAX_FIND_HANDLES; i++) {
        if (!find_handles[i].in_use) { slot = i; break; }
    }
    if (slot < 0) {
        g_last_error = 4; /* ERROR_TOO_MANY_OPEN_FILES */
        return INVALID_HANDLE_VALUE;
    }

    find_handles[slot].in_use = true;
    int j = 0;
    while (pattern[j] && j < 63) { find_handles[slot].pattern[j] = pattern[j]; j++; }
    find_handles[slot].pattern[j] = 0;

    osfs2_file_t *f = osfs2_get_file(idx);
    fill_find_data_a(lpFindFileData, f);
    find_handles[slot].next_idx = idx + 1;

    return (HANDLE)(ULONG_PTR)(slot + 0x100);  /* offset to avoid NULL */
}

BOOL WINAPI FindNextFileA(HANDLE hFindFile, LPWIN32_FIND_DATAA lpFindFileData)
{
    int slot = (int)(ULONG_PTR)hFindFile - 0x100;
    if (slot < 0 || slot >= MAX_FIND_HANDLES || !find_handles[slot].in_use) {
        g_last_error = 6; /* ERROR_INVALID_HANDLE */
        return FALSE;
    }

    int idx = osfs2_find_first(find_handles[slot].pattern, find_handles[slot].next_idx);
    if (idx < 0) {
        g_last_error = 18; /* ERROR_NO_MORE_FILES */
        return FALSE;
    }

    osfs2_file_t *f = osfs2_get_file(idx);
    fill_find_data_a(lpFindFileData, f);
    find_handles[slot].next_idx = idx + 1;
    return TRUE;
}

BOOL WINAPI FindClose(HANDLE hFindFile)
{
    int slot = (int)(ULONG_PTR)hFindFile - 0x100;
    if (slot >= 0 && slot < MAX_FIND_HANDLES)
        find_handles[slot].in_use = false;
    return TRUE;
}

HANDLE WINAPI FindFirstFileW(PCWSTR lpFileName, LPWIN32_FIND_DATAW lpFindFileData)
{
    /* Convert wide to ASCII and delegate */
    char narrow[260];
    int i = 0;
    while (lpFileName[i] && i < 259) { narrow[i] = (char)lpFileName[i]; i++; }
    narrow[i] = 0;

    WIN32_FIND_DATAA fdA;
    HANDLE h = FindFirstFileA(narrow, &fdA);
    if (h == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    /* Convert result to wide */
    if (lpFindFileData) {
        memset(lpFindFileData, 0, sizeof(*lpFindFileData));
        lpFindFileData->dwFileAttributes = fdA.dwFileAttributes;
        lpFindFileData->nFileSizeLow = fdA.nFileSizeLow;
        lpFindFileData->nFileSizeHigh = fdA.nFileSizeHigh;
        for (int j = 0; fdA.cFileName[j] && j < 259; j++)
            lpFindFileData->cFileName[j] = (uint16_t)fdA.cFileName[j];
    }
    return h;
}

BOOL WINAPI FindNextFileW(HANDLE hFindFile, LPWIN32_FIND_DATAW lpFindFileData)
{
    WIN32_FIND_DATAA fdA;
    BOOL ok = FindNextFileA(hFindFile, &fdA);
    if (!ok) return FALSE;
    if (lpFindFileData) {
        memset(lpFindFileData, 0, sizeof(*lpFindFileData));
        lpFindFileData->dwFileAttributes = fdA.dwFileAttributes;
        lpFindFileData->nFileSizeLow = fdA.nFileSizeLow;
        lpFindFileData->nFileSizeHigh = fdA.nFileSizeHigh;
        for (int j = 0; fdA.cFileName[j] && j < 259; j++)
            lpFindFileData->cFileName[j] = (uint16_t)fdA.cFileName[j];
    }
    return TRUE;
}

/* ── Startup / Debug ───────────────────────────────────────── */

void WINAPI GetStartupInfoA(LPSTARTUPINFOA lpStartupInfo)
{
    if (!lpStartupInfo) return;

    /*
     * PE32 (i386) STARTUPINFOA is 68 bytes (4-byte pointers/handles).
     * Our 64-bit version is 104 bytes (8-byte pointers/handles).
     * Must write 32-bit layout to avoid 36-byte stack overflow.
     *
     * 32-bit layout (68 bytes):
     *   +0:  DWORD cb               +4:  LPSTR lpReserved
     *   +8:  LPSTR lpDesktop        +12: LPSTR lpTitle
     *   +16: DWORD dwX              +20: DWORD dwY
     *   +24: DWORD dwXSize          +28: DWORD dwYSize
     *   +32: DWORD dwXCountChars    +36: DWORD dwYCountChars
     *   +40: DWORD dwFillAttribute  +44: DWORD dwFlags
     *   +48: WORD wShowWindow       +50: WORD cbReserved2
     *   +52: LPBYTE lpReserved2     +56: HANDLE hStdInput
     *   +60: HANDLE hStdOutput      +64: HANDLE hStdError
     */
    extern int g_compat32_mode;
    if (g_compat32_mode) {
        uint8_t *p = (uint8_t *)lpStartupInfo;
        for (int i = 0; i < 68; i++) p[i] = 0;
        *(uint32_t *)(p + 0) = 68;  /* cb = 32-bit sizeof */
        return;
    }

    BYTE *p = (BYTE *)lpStartupInfo;
    for (SIZE_T i = 0; i < sizeof(STARTUPINFOA); i++) p[i] = 0;
    lpStartupInfo->cb = sizeof(STARTUPINFOA);
}

BOOL WINAPI IsDebuggerPresent(void) { return FALSE; }

PVOID g_unhandled_filter = NULL;  /* non-static: accessed by ntdll_shim.c for SEH dispatch */

PVOID WINAPI SetUnhandledExceptionFilter(PVOID lpTopLevelExceptionFilter)
{
    PVOID old = g_unhandled_filter;
    g_unhandled_filter = lpTopLevelExceptionFilter;
    return old;
}

void WINAPI OutputDebugStringA(PCSTR lpOutputString)
{
    if (lpOutputString) {
        serial_puts("[DEBUG] ");
        serial_puts(lpOutputString);
        serial_puts("\n");
    }
}

/*
 * RaiseException — Win32 wrapper around RtlRaiseException.
 */
extern void RtlRaiseException(PEXCEPTION_RECORD ExceptionRecord);

void WINAPI RaiseException(DWORD dwExceptionCode, DWORD dwExceptionFlags,
                           DWORD nNumberOfArguments,
                           const ULONG_PTR *lpArguments)
{
    EXCEPTION_RECORD rec;
    BYTE *p = (BYTE *)&rec;
    for (SIZE_T i = 0; i < sizeof(EXCEPTION_RECORD); i++) p[i] = 0;

    rec.ExceptionCode    = dwExceptionCode;
    rec.ExceptionFlags   = dwExceptionFlags;
    rec.ExceptionRecord  = NULL;
    rec.ExceptionAddress = NULL;

    if (lpArguments && nNumberOfArguments > 0) {
        if (nNumberOfArguments > EXCEPTION_MAXIMUM_PARAMETERS)
            nNumberOfArguments = EXCEPTION_MAXIMUM_PARAMETERS;
        rec.NumberParameters = nNumberOfArguments;
        for (DWORD i = 0; i < nNumberOfArguments; i++)
            rec.ExceptionInformation[i] = lpArguments[i];
    }

    RtlRaiseException(&rec);
}

/*
 * UnhandledExceptionFilter — default top-level filter.
 * Returns EXCEPTION_EXECUTE_HANDLER to terminate the process.
 */
LONG WINAPI UnhandledExceptionFilter(PEXCEPTION_POINTERS ExceptionInfo)
{
    if (ExceptionInfo && ExceptionInfo->ExceptionRecord) {
        serial_puts("[SEH] UnhandledExceptionFilter: code=0x");
        serial_puthex(ExceptionInfo->ExceptionRecord->ExceptionCode, 8);
        serial_puts("\n");
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

/* ── String Conversion ─────────────────────────────────────── */

#define CP_ACP  0
#define CP_UTF8 65001

int WINAPI MultiByteToWideChar(DWORD CodePage, DWORD dwFlags,
                               PCSTR lpMultiByteStr, int cbMultiByte,
                               PWSTR lpWideCharStr, int cchWideChar)
{
    (void)CodePage;
    (void)dwFlags;

    int len = 0;
    if (cbMultiByte == -1) {
        while (lpMultiByteStr[len]) len++;
        len++; /* include null */
    } else {
        len = cbMultiByte;
    }

    if (cchWideChar == 0) return len; /* query size */

    int copy = len < cchWideChar ? len : cchWideChar;
    for (int i = 0; i < copy; i++)
        lpWideCharStr[i] = (WCHAR)(unsigned char)lpMultiByteStr[i];

    return copy;
}

int WINAPI WideCharToMultiByte(DWORD CodePage, DWORD dwFlags,
                               PCWSTR lpWideCharStr, int cchWideChar,
                               PSTR lpMultiByteStr, int cbMultiByte,
                               PCSTR lpDefaultChar, BOOL *lpUsedDefaultChar)
{
    (void)CodePage;
    (void)dwFlags;
    (void)lpDefaultChar;
    if (lpUsedDefaultChar) *lpUsedDefaultChar = FALSE;

    int len = 0;
    if (cchWideChar == -1) {
        while (lpWideCharStr[len]) len++;
        len++;
    } else {
        len = cchWideChar;
    }

    if (cbMultiByte == 0) return len;

    int copy = len < cbMultiByte ? len : cbMultiByte;
    for (int i = 0; i < copy; i++)
        lpMultiByteStr[i] = (char)(lpWideCharStr[i] & 0xFF);

    return copy;
}

/* ── Interlocked ───────────────────────────────────────────── */

LONG WINAPI InterlockedIncrement(volatile LONG *Addend)
{
    return ++(*Addend);
}

LONG WINAPI InterlockedDecrement(volatile LONG *Addend)
{
    return --(*Addend);
}

LONG WINAPI InterlockedExchange(volatile LONG *Target, LONG Value)
{
    LONG old = *Target;
    *Target = Value;
    return old;
}

LONG WINAPI InterlockedCompareExchange(volatile LONG *Dest, LONG Exchange, LONG Comparand)
{
    LONG old = *Dest;
    if (old == Comparand) *Dest = Exchange;
    return old;
}

/* ── HeapSize / HeapReAlloc (CRT sometimes needs these) ────── */

SIZE_T WINAPI HeapSize(HANDLE hHeap, DWORD dwFlags, PCVOID lpMem)
{
    (void)hHeap;
    (void)dwFlags;
    if (!lpMem) return (SIZE_T)-1;
    /* Read size header from our bump allocator */
    const BYTE *block = (const BYTE *)lpMem - 8;
    return *(const SIZE_T *)block - 8;
}

PVOID WINAPI HeapReAlloc(HANDLE hHeap, DWORD dwFlags, PVOID lpMem, SIZE_T dwBytes)
{
    (void)hHeap;
    if (!lpMem) return HeapAlloc(hHeap, dwFlags, dwBytes);

    /* Check if ptr is from our heap pool (has valid header) */
    BYTE *block = (BYTE *)lpMem - 8;
    int from_heap = (heap_pool && block >= heap_pool && block < heap_pool + heap_pool_size);

    SIZE_T old_size;
    if (from_heap) {
        old_size = *(SIZE_T *)block - 8;
    } else {
        /* Not from heap pool — try VirtualAlloc page-aligned heuristic.
         * Cannot determine exact size; use dwBytes as copy size (grow = copy all old) */
        old_size = dwBytes;
        serial_puts("[HEAP-RA] ptr not from heap_pool: 0x");
        serial_puthex((uint64_t)(ULONG_PTR)lpMem, 8);
        serial_puts(" new_size=0x");
        serial_puthex(dwBytes, 8);
        serial_puts("\n");
    }

    PVOID new_mem = HeapAlloc(hHeap, dwFlags, dwBytes);
    if (!new_mem) return NULL;

    SIZE_T copy = old_size < dwBytes ? old_size : dwBytes;
    BYTE *d = (BYTE *)new_mem, *s = (BYTE *)lpMem;
    for (SIZE_T i = 0; i < copy; i++) d[i] = s[i];

    /* Diagnostic: log realloc details for FName array debugging */
    serial_puts("[HEAP-RA] 0x");
    serial_puthex((uint64_t)(ULONG_PTR)lpMem, 8);
    serial_puts(" -> 0x");
    serial_puthex((uint64_t)(ULONG_PTR)new_mem, 8);
    serial_puts(" old_sz=0x");
    serial_puthex(old_size, 8);
    serial_puts(" new_sz=0x");
    serial_puthex(dwBytes, 8);
    serial_puts(" copy=0x");
    serial_puthex(copy, 8);
    serial_puts("\n");

    return new_mem;
}

/* ── UT99 stubs: Process, Memory, System, Console ─────────── */

typedef struct _MEMORYSTATUS {
    DWORD  dwLength;
    DWORD  dwMemoryLoad;
    SIZE_T dwTotalPhys;
    SIZE_T dwAvailPhys;
    SIZE_T dwTotalPageFile;
    SIZE_T dwAvailPageFile;
    SIZE_T dwTotalVirtual;
    SIZE_T dwAvailVirtual;
} MEMORYSTATUS;

void WINAPI GlobalMemoryStatus(MEMORYSTATUS *lpBuffer)
{
    if (!lpBuffer) return;
    lpBuffer->dwLength         = sizeof(MEMORYSTATUS);
    lpBuffer->dwMemoryLoad     = 25;
    /* Values must fit in 32-bit SIZE_T (UT99 reads 4 bytes).
     * 4GB = 0x100000000 overflows to 0 → "Phys=0" → no rendering. */
    lpBuffer->dwTotalPhys      = 512 * 1024 * 1024;  /* 512 MB */
    lpBuffer->dwAvailPhys      = 384 * 1024 * 1024;
    lpBuffer->dwTotalPageFile  = 1024 * 1024 * 1024;  /* 1 GB */
    lpBuffer->dwAvailPageFile  = 768 * 1024 * 1024;
    lpBuffer->dwTotalVirtual   = 2047 * 1024 * 1024;  /* ~2 GB */
    lpBuffer->dwAvailVirtual   = 1536 * 1024 * 1024;
}

BOOL WINAPI SetConsoleCtrlHandler(PVOID HandlerRoutine, BOOL Add)
{
    (void)HandlerRoutine; (void)Add;
    return TRUE;
}

BOOL WINAPI GetProcessWorkingSetSize(HANDLE hProcess, SIZE_T *lpMin, SIZE_T *lpMax)
{
    (void)hProcess;
    if (lpMin) *lpMin = 204800;
    if (lpMax) *lpMax = 1413120;
    return TRUE;
}

PVOID WINAPI GlobalAlloc(UINT uFlags, SIZE_T dwBytes)
{
    (void)uFlags;
    return HeapAlloc((HANDLE)(ULONG_PTR)0xBEEF0001, 0, dwBytes);
}

BOOL WINAPI CreateProcessA(PCSTR lpApp, PSTR lpCmd, PVOID a, PVOID b,
                            BOOL c, DWORD d, PVOID e, PCSTR f, PVOID g, PVOID h)
{
    (void)lpApp; (void)lpCmd; (void)a; (void)b; (void)c;
    (void)d; (void)e; (void)f; (void)g; (void)h;
    serial_puts("[K32] CreateProcessA (stub — FALSE)\n");
    g_last_error = 2;
    return FALSE;
}

BOOL WINAPI CreateProcessW(PCWSTR lpApp, PWSTR lpCmd, PVOID a, PVOID b,
                             BOOL c, DWORD d, PVOID e, PCWSTR f, PVOID g, PVOID h)
{
    (void)lpApp; (void)lpCmd; (void)a; (void)b; (void)c;
    (void)d; (void)e; (void)f; (void)g; (void)h;
    serial_puts("[K32] CreateProcessW (stub — FALSE)\n");
    g_last_error = 2;
    return FALSE;
}

DWORD WINAPI FormatMessageA(DWORD dwFlags, PCVOID lpSource, DWORD dwMessageId,
                             DWORD dwLanguageId, PSTR lpBuffer, DWORD nSize,
                             PVOID Arguments)
{
    (void)dwFlags; (void)lpSource; (void)dwMessageId;
    (void)dwLanguageId; (void)Arguments;
    if (lpBuffer && nSize > 0) lpBuffer[0] = 0;
    return 0;
}

DWORD WINAPI FormatMessageW(DWORD dwFlags, PCVOID lpSource, DWORD dwMessageId,
                              DWORD dwLanguageId, PWSTR lpBuffer, DWORD nSize,
                              PVOID Arguments)
{
    (void)dwFlags; (void)lpSource; (void)dwMessageId;
    (void)dwLanguageId; (void)Arguments;
    if (lpBuffer && nSize > 0) lpBuffer[0] = 0;
    return 0;
}

BOOL WINAPI GetComputerNameA(PSTR lpBuffer, DWORD *nSize)
{
    const char *name = "OSITOK";
    DWORD len = 6;
    if (!lpBuffer || !nSize || *nSize <= len) {
        if (nSize) *nSize = len + 1;
        return FALSE;
    }
    for (DWORD i = 0; i <= len; i++) lpBuffer[i] = name[i];
    *nSize = len;
    return TRUE;
}

BOOL WINAPI GetComputerNameW(PWSTR lpBuffer, DWORD *nSize)
{
    static const WCHAR name[] = {'O','S','I','T','O','K',0};
    DWORD len = 6;
    if (!lpBuffer || !nSize || *nSize <= len) {
        if (nSize) *nSize = len + 1;
        return FALSE;
    }
    for (DWORD i = 0; i <= len; i++) lpBuffer[i] = name[i];
    *nSize = len;
    return TRUE;
}

BOOL WINAPI GetExitCodeProcess(HANDLE hProcess, DWORD *lpExitCode)
{
    (void)hProcess;
    if (lpExitCode) *lpExitCode = 0;
    return TRUE;
}

typedef struct _SYSTEMTIME {
    WORD wYear;
    WORD wMonth;
    WORD wDayOfWeek;
    WORD wDay;
    WORD wHour;
    WORD wMinute;
    WORD wSecond;
    WORD wMilliseconds;
} SYSTEMTIME;

void WINAPI GetLocalTime(SYSTEMTIME *lpSystemTime)
{
    if (!lpSystemTime) return;
    lpSystemTime->wYear         = 2026;
    lpSystemTime->wMonth        = 3;
    lpSystemTime->wDayOfWeek    = 0;
    lpSystemTime->wDay          = 9;
    lpSystemTime->wHour         = 12;
    lpSystemTime->wMinute       = 0;
    lpSystemTime->wSecond       = 0;
    lpSystemTime->wMilliseconds = 0;
}

DWORD WINAPI GetVersion(void)
{
    /* Windows XP: major=5, minor=1, build=2600 */
    return 0x0A280105;
}

BOOL WINAPI GetVersionExW(PVOID lpVersionInformation)
{
    if (!lpVersionInformation) return FALSE;
    BYTE *p = (BYTE *)lpVersionInformation;
    /* Zero everything first (at least 276 bytes for OSVERSIONINFOW) */
    for (int i = 0; i < 276; i++) p[i] = 0;
    *(DWORD *)(p + 0)  = 276;  /* dwOSVersionInfoSize */
    *(DWORD *)(p + 4)  = 5;    /* dwMajorVersion */
    *(DWORD *)(p + 8)  = 2;    /* dwMinorVersion */
    *(DWORD *)(p + 12) = 3790; /* dwBuildNumber */
    *(DWORD *)(p + 16) = 2;    /* dwPlatformId = VER_PLATFORM_WIN32_NT */
    return TRUE;
}

BOOL WINAPI TerminateProcess(HANDLE hProcess, UINT uExitCode)
{
    (void)hProcess;
    ExitProcess(uExitCode);
    return TRUE;
}

HANDLE WINAPI HeapCreate(DWORD flOptions, SIZE_T dwInitialSize, SIZE_T dwMaximumSize)
{
    (void)flOptions; (void)dwInitialSize; (void)dwMaximumSize;
    return (HANDLE)(ULONG_PTR)0xBEEF0002;
}

BOOL WINAPI HeapDestroy(HANDLE hHeap)
{
    (void)hHeap;
    return TRUE;
}

BOOL WINAPI HeapValidate(HANDLE hHeap, DWORD dwFlags, PCVOID lpMem)
{
    (void)hHeap; (void)dwFlags; (void)lpMem;
    return TRUE;
}

void WINAPI ExitThread(DWORD dwExitCode)
{
    (void)dwExitCode;
    /* In single-threaded mode, just return */
}

BOOL WINAPI SetErrorMode(UINT uMode)
{
    (void)uMode;
    return 0;
}

UINT WINAPI SetHandleCount(UINT uNumber)
{
    return uNumber; /* no-op, return requested count */
}

BOOL WINAPI SetStdHandle(DWORD nStdHandle, HANDLE hHandle)
{
    (void)nStdHandle; (void)hHandle;
    return TRUE;
}

BOOL WINAPI FlushFileBuffers(HANDLE hFile)
{
    (void)hFile;
    return TRUE;
}

DWORD WINAPI GetFileType(HANDLE hFile)
{
    (void)hFile;
    return 1; /* FILE_TYPE_DISK */
}

BOOL WINAPI SetEnvironmentVariableA(PCSTR lpName, PCSTR lpValue)
{
    (void)lpName; (void)lpValue;
    return TRUE;
}

BOOL WINAPI SetEnvironmentVariableW(PCWSTR lpName, PCWSTR lpValue)
{
    (void)lpName; (void)lpValue;
    return TRUE;
}

PCSTR WINAPI GetEnvironmentStrings(void) { return GetEnvironmentStringsA(); }
PCWSTR WINAPI GetEnvironmentStringsW(void) { return (PCWSTR)L"\0"; }
BOOL WINAPI FreeEnvironmentStringsW(PCWSTR lpszEnvironmentBlock) { (void)lpszEnvironmentBlock; return TRUE; }

void WINAPI Beep_stub(DWORD dwFreq, DWORD dwDuration)
{
    (void)dwFreq; (void)dwDuration;
}

/* Locale / codepage stubs */
UINT WINAPI GetACP(void) { return 1252; }
UINT WINAPI GetOEMCP(void) { return 437; }
BOOL WINAPI GetCPInfo(UINT cp, PVOID info)
{
    (void)cp;
    if (info) {
        BYTE *p = (BYTE *)info;
        for (int i = 0; i < 20; i++) p[i] = 0;
        *(UINT *)p = 1; /* MaxCharSize */
    }
    return TRUE;
}
DWORD WINAPI GetUserDefaultLCID(void) { return 0x0409; } /* en-US */
BOOL WINAPI IsValidCodePage(UINT cp) { (void)cp; return TRUE; }
BOOL WINAPI IsValidLocale(DWORD lcid, DWORD flags) { (void)lcid; (void)flags; return TRUE; }
BOOL WINAPI EnumSystemLocalesA(PVOID fn, DWORD flags) { (void)fn; (void)flags; return TRUE; }

int WINAPI GetLocaleInfoA(DWORD Locale, DWORD LCType, PSTR lpLCData, int cchData)
{
    (void)Locale; (void)LCType;
    if (lpLCData && cchData > 0) lpLCData[0] = 0;
    return 0;
}
int WINAPI GetLocaleInfoW(DWORD Locale, DWORD LCType, PWSTR lpLCData, int cchData)
{
    (void)Locale; (void)LCType;
    if (lpLCData && cchData > 0) lpLCData[0] = 0;
    return 0;
}
int WINAPI CompareStringA(DWORD loc, DWORD flags, PCSTR s1, int c1, PCSTR s2, int c2)
{
    (void)loc; (void)flags;
    if (c1 < 0) { int n=0; while(s1[n]) n++; c1 = n; }
    if (c2 < 0) { int n=0; while(s2[n]) n++; c2 = n; }
    int n = c1 < c2 ? c1 : c2;
    for (int i = 0; i < n; i++) {
        if ((unsigned char)s1[i] < (unsigned char)s2[i]) return 1; /* CSTR_LESS_THAN */
        if ((unsigned char)s1[i] > (unsigned char)s2[i]) return 3; /* CSTR_GREATER_THAN */
    }
    if (c1 < c2) return 1;
    if (c1 > c2) return 3;
    return 2; /* CSTR_EQUAL */
}
int WINAPI CompareStringW(DWORD loc, DWORD flags, PCWSTR s1, int c1, PCWSTR s2, int c2)
{
    (void)loc; (void)flags;
    if (c1 < 0) { int n=0; while(s1[n]) n++; c1 = n; }
    if (c2 < 0) { int n=0; while(s2[n]) n++; c2 = n; }
    int n = c1 < c2 ? c1 : c2;
    for (int i = 0; i < n; i++) {
        if (s1[i] < s2[i]) return 1;
        if (s1[i] > s2[i]) return 3;
    }
    if (c1 < c2) return 1;
    if (c1 > c2) return 3;
    return 2;
}
BOOL WINAPI GetStringTypeA(DWORD dwInfoType, PCSTR lpSrcStr, int cchSrc, WORD *lpCharType)
{
    (void)dwInfoType; (void)lpSrcStr;
    if (lpCharType && cchSrc > 0) {
        for (int i = 0; i < cchSrc; i++) lpCharType[i] = 0;
    }
    return TRUE;
}
BOOL WINAPI GetStringTypeW(DWORD dwInfoType, PCWSTR lpSrcStr, int cchSrc, WORD *lpCharType)
{
    (void)dwInfoType; (void)lpSrcStr;
    if (lpCharType && cchSrc > 0) {
        for (int i = 0; i < cchSrc; i++) lpCharType[i] = 0;
    }
    return TRUE;
}
int WINAPI LCMapStringA(DWORD loc, DWORD flags, PCSTR src, int srclen, PSTR dst, int dstlen)
{
    (void)loc; (void)flags;
    if (srclen < 0) { int n=0; while(src[n]) n++; srclen = n+1; }
    if (dstlen == 0) return srclen;
    int n = srclen < dstlen ? srclen : dstlen;
    for (int i = 0; i < n; i++) dst[i] = src[i];
    return n;
}
int WINAPI LCMapStringW(DWORD loc, DWORD flags, PCWSTR src, int srclen, PWSTR dst, int dstlen)
{
    (void)loc; (void)flags;
    if (srclen < 0) { int n=0; while(src[n]) n++; srclen = n+1; }
    if (dstlen == 0) return srclen;
    int n = srclen < dstlen ? srclen : dstlen;
    for (int i = 0; i < n; i++) dst[i] = src[i];
    return n;
}
BOOL WINAPI IsBadReadPtr(PCVOID lp, SIZE_T ucb) { (void)lp; (void)ucb; return FALSE; }
BOOL WINAPI IsBadWritePtr(PVOID lp, SIZE_T ucb) { (void)lp; (void)ucb; return FALSE; }
BOOL WINAPI IsBadCodePtr(PVOID lpfn) { (void)lpfn; return FALSE; }

DWORD WINAPI GetFullPathNameW(PCWSTR lpFileName, DWORD nBufferLength,
                               PWSTR lpBuffer, PWSTR *lpFilePart)
{
    if (!lpFileName || !lpBuffer) return 0;
    DWORD len = 0;
    while (lpFileName[len]) len++;
    if (len >= nBufferLength) return len + 1;
    for (DWORD i = 0; i <= len; i++) lpBuffer[i] = lpFileName[i];
    if (lpFilePart) *lpFilePart = lpBuffer;
    return len;
}

DWORD WINAPI GetFileAttributesW(PCWSTR lpFileName)
{
    if (!lpFileName) return (DWORD)-1;
    /* Convert wide to narrow and delegate */
    char narrow[260];
    int i = 0;
    for (; lpFileName[i] && i < 259; i++)
        narrow[i] = (char)(lpFileName[i] & 0xFF);
    narrow[i] = 0;
    return GetFileAttributesA(narrow);
}

BOOL WINAPI FileTimeToLocalFileTime(PCVOID lpFileTime, PVOID lpLocalFileTime)
{
    if (lpFileTime && lpLocalFileTime) {
        BYTE *d = (BYTE *)lpLocalFileTime;
        const BYTE *s = (const BYTE *)lpFileTime;
        for (int i = 0; i < 8; i++) d[i] = s[i];
    }
    return TRUE;
}

BOOL WINAPI FileTimeToSystemTime(PCVOID lpFileTime, SYSTEMTIME *lpSystemTime)
{
    (void)lpFileTime;
    if (lpSystemTime) GetLocalTime(lpSystemTime);
    return TRUE;
}

DWORD WINAPI GetTimeZoneInformation(PVOID lpTimeZoneInformation)
{
    if (lpTimeZoneInformation) {
        BYTE *p = (BYTE *)lpTimeZoneInformation;
        for (int i = 0; i < 172; i++) p[i] = 0;
    }
    return 0; /* TIME_ZONE_ID_UNKNOWN */
}

DWORD WINAPI GetLogicalDrives(void) { return 0x04; } /* C: drive */
UINT WINAPI GetDriveTypeA(PCSTR lpRootPathName) { (void)lpRootPathName; return 3; } /* DRIVE_FIXED */
UINT WINAPI GetDriveTypeW(PCWSTR lpRootPathName) { (void)lpRootPathName; return 3; }

BOOL WINAPI GetDiskFreeSpaceA(PCSTR lpRoot, DWORD *lpSPC, DWORD *lpBPS,
                                DWORD *lpFC, DWORD *lpTC)
{
    (void)lpRoot;
    if (lpSPC) *lpSPC = 8;
    if (lpBPS) *lpBPS = 512;
    if (lpFC)  *lpFC  = 1048576;
    if (lpTC)  *lpTC  = 2097152;
    return TRUE;
}

/* ── INI File (Private Profile) ────────────────────────────── */
/*
 * In-memory INI store. UT99 reads UnrealTournament.ini, User.ini, etc.
 * We keep a flat array of section+key→value entries.
 * File parameter is ignored (all INI data is in one global store).
 */

#define INI_MAX_ENTRIES 1024
#define INI_MAX_SECTION  64
#define INI_MAX_KEY      64
#define INI_MAX_VALUE   512

typedef struct {
    char section[INI_MAX_SECTION];
    char key[INI_MAX_KEY];
    char value[INI_MAX_VALUE];
} INI_ENTRY;

static INI_ENTRY g_ini_store[INI_MAX_ENTRIES];
static int       g_ini_count = 0;

/* Track which INI files have been loaded from OsitoFS */
#define INI_FILES_MAX 8
static char ini_loaded_files[INI_FILES_MAX][64];
static int  ini_loaded_count = 0;

static int ini_stricmp(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static void ini_strcpy(char *dst, const char *src, int max)
{
    int i;
    for (i = 0; src[i] && i < max - 1; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

static int ini_strlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static INI_ENTRY *ini_find(const char *section, const char *key)
{
    for (int i = 0; i < g_ini_count; i++) {
        if (ini_stricmp(g_ini_store[i].section, section) == 0 &&
            ini_stricmp(g_ini_store[i].key, key) == 0)
            return &g_ini_store[i];
    }
    return NULL;
}

/* Add INI entry (allows duplicates — UT99 uses multi-value keys like Paths=) */
static void ini_add(const char *section, const char *key, const char *value)
{
    if (g_ini_count >= INI_MAX_ENTRIES) return;
    INI_ENTRY *e = &g_ini_store[g_ini_count++];
    ini_strcpy(e->section, section, INI_MAX_SECTION);
    ini_strcpy(e->key, key, INI_MAX_KEY);
    ini_strcpy(e->value, value, INI_MAX_VALUE);
}

/* Parse and load an INI file from OsitoFS into the INI store */
static void ini_load_from_osfs(const char *filename)
{
    /* Check if already loaded */
    const char *base = filename;
    for (const char *p = filename; *p; p++) {
        if (*p == '\\' || *p == '/') base = p + 1;
    }
    for (int i = 0; i < ini_loaded_count; i++) {
        if (ini_stricmp(ini_loaded_files[i], base) == 0) return;
    }

    void *f = osfs2_find(base);
    if (!f) return;

    uint64_t fsize = osfs2_file_size(f);
    if (fsize == 0 || fsize > 64 * 1024) return; /* sanity limit */

    /* Allocate temp buffer and read */
    extern void *kmalloc(uint64_t size);
    extern void kfree(void *ptr);
    char *buf = (char *)kmalloc(fsize + 1);
    if (!buf) return;
    osfs2_read(f, 0, buf, fsize);
    buf[fsize] = 0;

    serial_puts("[INI] Loading ");
    serial_puts(base);
    serial_puts(" (");
    serial_putdec(fsize);
    serial_puts(" bytes)\n");

    /* Track as loaded */
    if (ini_loaded_count < INI_FILES_MAX)
        ini_strcpy(ini_loaded_files[ini_loaded_count++], base, 64);

    /* Parse: [Section] and Key=Value lines */
    char cur_section[INI_MAX_SECTION] = "";
    char *p = buf;
    while (*p) {
        /* Skip whitespace */
        while (*p == ' ' || *p == '\t') p++;

        if (*p == '[') {
            /* Section header */
            p++;
            char *start = p;
            while (*p && *p != ']' && *p != '\r' && *p != '\n') p++;
            int len = (int)(p - start);
            if (len >= INI_MAX_SECTION) len = INI_MAX_SECTION - 1;
            for (int i = 0; i < len; i++) cur_section[i] = start[i];
            cur_section[len] = 0;
            if (*p == ']') p++;
        } else if (*p == ';' || *p == '#' || *p == '\r' || *p == '\n') {
            /* Comment or empty line — skip */
        } else if (cur_section[0]) {
            /* Key=Value */
            char key[INI_MAX_KEY] = "";
            char val[INI_MAX_VALUE] = "";
            char *start = p;
            while (*p && *p != '=' && *p != '\r' && *p != '\n') p++;
            if (*p == '=') {
                int klen = (int)(p - start);
                if (klen >= INI_MAX_KEY) klen = INI_MAX_KEY - 1;
                for (int i = 0; i < klen; i++) key[i] = start[i];
                key[klen] = 0;
                p++; /* skip '=' */
                start = p;
                while (*p && *p != '\r' && *p != '\n') p++;
                int vlen = (int)(p - start);
                if (vlen >= INI_MAX_VALUE) vlen = INI_MAX_VALUE - 1;
                for (int i = 0; i < vlen; i++) val[i] = start[i];
                val[vlen] = 0;
                ini_add(cur_section, key, val);
            }
        }
        /* Skip to end of line */
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }

    serial_puts("[INI] Loaded ");
    serial_putdec(g_ini_count);
    serial_puts(" total entries\n");
    kfree(buf);
}

DWORD WINAPI GetPrivateProfileStringA(PCSTR lpAppName, PCSTR lpKeyName,
                                       PCSTR lpDefault, PSTR lpReturnedString,
                                       DWORD nSize, PCSTR lpFileName)
{
    /* Auto-load INI file from OsitoFS on first access */
    if (lpFileName)
        ini_load_from_osfs(lpFileName);

    /* If section is NULL, enumerate section names */
    if (!lpAppName) {
        if (lpReturnedString && nSize > 0)
            lpReturnedString[0] = 0;
        return 0;
    }

    /* If key is NULL, enumerate keys in section */
    if (!lpKeyName) {
        if (lpReturnedString && nSize > 0)
            lpReturnedString[0] = 0;
        return 0;
    }

    const char *result = lpDefault ? lpDefault : "";
    INI_ENTRY *entry = ini_find(lpAppName, lpKeyName);
    if (entry)
        result = entry->value;

    int len = ini_strlen(result);
    if ((DWORD)len >= nSize) len = nSize - 1;
    if (lpReturnedString && nSize > 0) {
        for (int i = 0; i < len; i++)
            lpReturnedString[i] = result[i];
        lpReturnedString[len] = 0;
    }
    return (DWORD)len;
}

BOOL WINAPI WritePrivateProfileStringA(PCSTR lpAppName, PCSTR lpKeyName,
                                        PCSTR lpString, PCSTR lpFileName)
{
    (void)lpFileName;

    if (!lpAppName) return FALSE;

    /* Delete key if lpString is NULL */
    if (!lpKeyName || !lpString) return TRUE;

    INI_ENTRY *entry = ini_find(lpAppName, lpKeyName);
    if (entry) {
        ini_strcpy(entry->value, lpString, INI_MAX_VALUE);
        return TRUE;
    }

    if (g_ini_count >= INI_MAX_ENTRIES) return FALSE;

    entry = &g_ini_store[g_ini_count++];
    ini_strcpy(entry->section, lpAppName, INI_MAX_SECTION);
    ini_strcpy(entry->key, lpKeyName, INI_MAX_KEY);
    ini_strcpy(entry->value, lpString, INI_MAX_VALUE);
    return TRUE;
}

UINT WINAPI GetPrivateProfileIntA(PCSTR lpAppName, PCSTR lpKeyName,
                                   int nDefault, PCSTR lpFileName)
{
    char buf[32];
    DWORD len = GetPrivateProfileStringA(lpAppName, lpKeyName, NULL, buf, 32, lpFileName);
    if (len == 0) return (UINT)nDefault;

    /* Simple atoi */
    int result = 0, sign = 1, i = 0;
    if (buf[0] == '-') { sign = -1; i = 1; }
    for (; buf[i] >= '0' && buf[i] <= '9'; i++)
        result = result * 10 + (buf[i] - '0');
    return (UINT)(result * sign);
}

DWORD WINAPI GetPrivateProfileSectionNamesA(PSTR lpszReturnBuffer,
                                             DWORD nSize, PCSTR lpFileName)
{
    (void)lpFileName;
    /* Return empty double-null-terminated buffer */
    if (lpszReturnBuffer && nSize >= 2) {
        lpszReturnBuffer[0] = 0;
        lpszReturnBuffer[1] = 0;
    }
    return 0;
}

/* ── Stubs for MSVCRT.dll CRT init dependencies ───────────── */

/* These are called by the real MSVCRT.dll during CRT initialization.
 * They need to exist as stubs to prevent NULL IAT entries → crashes. */

static BOOL WINAPI HeapCompact_stub(HANDLE hHeap, DWORD dwFlags)
{
    (void)hHeap; (void)dwFlags;
    return 1;  /* report success */
}

static BOOL WINAPI HeapWalk_stub(HANDLE hHeap, void *lpEntry)
{
    (void)hHeap; (void)lpEntry;
    g_last_error = 0x12; /* ERROR_NO_MORE_ITEMS */
    sync_last_error();
    return FALSE;
}

static BOOL WINAPI ReadConsoleA_stub(HANDLE h, void *buf, DWORD n, DWORD *read, void *r)
{
    (void)h; (void)buf; (void)n; (void)r;
    if (read) *read = 0;
    return FALSE;
}

static BOOL WINAPI SetConsoleMode_stub(HANDLE h, DWORD mode)
{
    (void)h; (void)mode;
    return TRUE;
}

static BOOL WINAPI GetConsoleMode_stub(HANDLE h, DWORD *mode)
{
    (void)h;
    if (mode) *mode = 0x3; /* ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT */
    return TRUE;
}

static BOOL WINAPI SetEndOfFile_stub(HANDLE h)
{
    (void)h;
    return TRUE;
}

typedef struct _BY_HANDLE_FILE_INFORMATION {
    DWORD dwFileAttributes;
    uint64_t ftCreationTime;
    uint64_t ftLastAccessTime;
    uint64_t ftLastWriteTime;
    DWORD dwVolumeSerialNumber;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD nNumberOfLinks;
    DWORD nFileIndexHigh;
    DWORD nFileIndexLow;
} BY_HANDLE_FILE_INFORMATION;

static BOOL WINAPI GetFileInformationByHandle_stub(HANDLE h, BY_HANDLE_FILE_INFORMATION *info)
{
    (void)h;
    if (info) memset(info, 0, sizeof(*info));
    return TRUE;
}

static BOOL WINAPI PeekNamedPipe_stub(HANDLE h, void *buf, DWORD sz,
                                       DWORD *read, DWORD *avail, DWORD *left)
{
    (void)h; (void)buf; (void)sz;
    if (read)  *read = 0;
    if (avail) *avail = 0;
    if (left)  *left = 0;
    return TRUE;
}

typedef struct _INPUT_RECORD { WORD EventType; char pad[18]; } INPUT_RECORD;

static BOOL WINAPI ReadConsoleInputA_stub(HANDLE h, INPUT_RECORD *buf, DWORD len, DWORD *read)
{
    (void)h; (void)buf; (void)len;
    if (read) *read = 0;
    return FALSE;
}

static BOOL WINAPI PeekConsoleInputA_stub(HANDLE h, INPUT_RECORD *buf, DWORD len, DWORD *read)
{
    (void)h; (void)buf; (void)len;
    if (read) *read = 0;
    return TRUE;
}

static BOOL WINAPI GetNumberOfConsoleInputEvents_stub(HANDLE h, DWORD *num)
{
    (void)h;
    if (num) *num = 0;
    return TRUE;
}

static BOOL WINAPI LockFile_stub(HANDLE h, DWORD lo, DWORD hi, DWORD nlo, DWORD nhi)
{
    (void)h; (void)lo; (void)hi; (void)nlo; (void)nhi;
    return TRUE;
}

static BOOL WINAPI UnlockFile_stub(HANDLE h, DWORD lo, DWORD hi, DWORD nlo, DWORD nhi)
{
    (void)h; (void)lo; (void)hi; (void)nlo; (void)nhi;
    return TRUE;
}

static BOOL WINAPI CreatePipe_stub(HANDLE *hRead, HANDLE *hWrite,
                                    void *lpAttr, DWORD nSize)
{
    (void)lpAttr; (void)nSize;
    if (hRead)  *hRead  = (HANDLE)(ULONG_PTR)0xDEAD0001;
    if (hWrite) *hWrite = (HANDLE)(ULONG_PTR)0xDEAD0002;
    return TRUE;
}

static BOOL WINAPI SetFileTime_stub(HANDLE h, const void *c, const void *a, const void *w)
{
    (void)h; (void)c; (void)a; (void)w;
    return TRUE;
}

static BOOL WINAPI LocalFileTimeToFileTime_stub(const void *local, void *utc)
{
    if (utc) memset(utc, 0, 8);
    (void)local;
    return TRUE;
}

static BOOL WINAPI SystemTimeToFileTime_stub(const void *st, void *ft)
{
    if (ft) memset(ft, 0, 8);
    (void)st;
    return TRUE;
}

static void WINAPI GetSystemTime_stub(SYSTEMTIME *st)
{
    if (st) {
        memset(st, 0, sizeof(*st));
        st->wYear = 2026;
        st->wMonth = 3;
        st->wDay = 13;
    }
}

static BOOL WINAPI SetLocalTime_stub(const SYSTEMTIME *st)
{
    (void)st;
    return TRUE;
}

static BOOL WINAPI GlobalFree_stub(void *hMem)
{
    (void)hMem;
    return 0;  /* success = NULL */
}

static BOOL WINAPI ReleaseMutex_stub(HANDLE h)
{
    (void)h;
    return TRUE;
}

static void WINAPI OutputDebugStringW_stub(const WCHAR *s)
{
    (void)s;
    /* Silent */
}

static DWORD WINAPI GlobalAddAtomW_stub(const WCHAR *s)
{
    (void)s;
    return 0xC000;  /* fake atom */
}

/* ── Export resolution table ────────────────────────────────── */

typedef struct {
    const char *name;
    PVOID       func;
} K32_EXPORT;

static const K32_EXPORT k32_exports[] = {
    { "CreateFileA",             (PVOID)CreateFileA },
    { "CreateFileW",             (PVOID)CreateFileW },
    { "ReadFile",                (PVOID)ReadFile },
    { "WriteFile",               (PVOID)WriteFile },
    { "CloseHandle",             (PVOID)CloseHandle },
    { "GetStdHandle",            (PVOID)GetStdHandle },
    { "WriteConsoleA",           (PVOID)WriteConsoleA },
    { "ExitProcess",             (PVOID)ExitProcess },
    { "GetCurrentProcess",       (PVOID)GetCurrentProcess },
    { "GetCurrentProcessId",     (PVOID)GetCurrentProcessId },
    { "VirtualAlloc",            (PVOID)VirtualAlloc },
    { "VirtualFree",             (PVOID)VirtualFree },
    { "GetProcessHeap",          (PVOID)GetProcessHeap },
    { "HeapAlloc",               (PVOID)HeapAlloc },
    { "HeapFree",                (PVOID)HeapFree },
    { "GetLastError",            (PVOID)GetLastError },
    { "SetLastError",            (PVOID)SetLastError },
    { "Sleep",                   (PVOID)Sleep },
    { "QueryPerformanceCounter", (PVOID)QueryPerformanceCounter },
    { "QueryPerformanceFrequency",(PVOID)QueryPerformanceFrequency },
    { "GetProcAddress",          (PVOID)GetProcAddress },
    { "GetModuleHandleA",        (PVOID)GetModuleHandleA },
    { "GetModuleHandleW",        (PVOID)GetModuleHandleW },
    { "GetFileSize",             (PVOID)GetFileSize },
    { "SetFilePointer",          (PVOID)SetFilePointer },
    { "DuplicateHandle",         (PVOID)DuplicateHandle },
    { "VirtualProtect",          (PVOID)VirtualProtect },
    { "VirtualQuery",            (PVOID)VirtualQuery },
    { "lstrlenA",                (PVOID)lstrlenA },
    { "lstrlenW",                (PVOID)lstrlenW },
    { "GetCommandLineA",         (PVOID)GetCommandLineA },
    { "GetCommandLineW",         (PVOID)GetCommandLineW },
    { "GetEnvironmentStringsA",  (PVOID)GetEnvironmentStringsA },
    { "FreeEnvironmentStringsA", (PVOID)FreeEnvironmentStringsA },
    /* Phase 11: Critical Section */
    { "InitializeCriticalSection",          (PVOID)InitializeCriticalSection },
    { "InitializeCriticalSectionAndSpinCount",(PVOID)InitializeCriticalSectionAndSpinCount },
    { "EnterCriticalSection",               (PVOID)EnterCriticalSection },
    { "TryEnterCriticalSection",            (PVOID)TryEnterCriticalSection },
    { "LeaveCriticalSection",               (PVOID)LeaveCriticalSection },
    { "DeleteCriticalSection",              (PVOID)DeleteCriticalSection },
    /* TLS */
    { "TlsAlloc",               (PVOID)TlsAlloc },
    { "TlsFree",                (PVOID)TlsFree },
    { "TlsGetValue",            (PVOID)TlsGetValue },
    { "TlsSetValue",            (PVOID)TlsSetValue },
    /* Thread */
    { "CreateThread",            (PVOID)CreateThread },
    { "GetCurrentThreadId",      (PVOID)GetCurrentThreadId },
    { "GetCurrentThread",        (PVOID)GetCurrentThread },
    { "SuspendThread",           (PVOID)SuspendThread },
    { "ResumeThread",            (PVOID)ResumeThread },
    { "TerminateThread",         (PVOID)TerminateThread },
    { "WaitForSingleObject",     (PVOID)WaitForSingleObject },
    { "WaitForMultipleObjects",  (PVOID)WaitForMultipleObjects },
    /* DLL / Module */
    { "LoadLibraryA",            (PVOID)LoadLibraryA },
    { "LoadLibraryW",            (PVOID)LoadLibraryW },
    { "LoadLibraryExA",          (PVOID)LoadLibraryExA },
    { "FreeLibrary",             (PVOID)FreeLibrary },
    { "GetModuleFileNameA",      (PVOID)GetModuleFileNameA },
    /* Timing */
    { "GetTickCount",            (PVOID)GetTickCount },
    { "GetTickCount64",          (PVOID)GetTickCount64 },
    { "GetSystemTimeAsFileTime", (PVOID)GetSystemTimeAsFileTime },
    /* System */
    { "GetSystemInfo",           (PVOID)GetSystemInfo },
    { "GetVersionExA",           (PVOID)GetVersionExA },
    /* Path / Dir */
    { "GetFullPathNameA",        (PVOID)GetFullPathNameA },
    { "GetCurrentDirectoryA",    (PVOID)GetCurrentDirectoryA },
    { "GetFileAttributesA",      (PVOID)GetFileAttributesA },
    { "SetFileAttributesA",      (PVOID)SetFileAttributesA },
    { "CreateDirectoryA",        (PVOID)CreateDirectoryA },
    { "RemoveDirectoryA",        (PVOID)RemoveDirectoryA },
    /* Find File */
    { "FindFirstFileA",          (PVOID)FindFirstFileA },
    { "FindNextFileA",           (PVOID)FindNextFileA },
    { "FindClose",               (PVOID)FindClose },
    /* Startup / Debug */
    { "GetStartupInfoA",         (PVOID)GetStartupInfoA },
    { "IsDebuggerPresent",       (PVOID)IsDebuggerPresent },
    { "SetUnhandledExceptionFilter",(PVOID)SetUnhandledExceptionFilter },
    { "UnhandledExceptionFilter",(PVOID)UnhandledExceptionFilter },
    { "RaiseException",          (PVOID)RaiseException },
    { "OutputDebugStringA",      (PVOID)OutputDebugStringA },
    /* String Conversion */
    { "MultiByteToWideChar",     (PVOID)MultiByteToWideChar },
    { "WideCharToMultiByte",     (PVOID)WideCharToMultiByte },
    /* Interlocked */
    { "InterlockedIncrement",    (PVOID)InterlockedIncrement },
    { "InterlockedDecrement",    (PVOID)InterlockedDecrement },
    { "InterlockedExchange",     (PVOID)InterlockedExchange },
    { "InterlockedCompareExchange",(PVOID)InterlockedCompareExchange },
    /* Heap extended */
    { "HeapSize",                (PVOID)HeapSize },
    { "HeapReAlloc",             (PVOID)HeapReAlloc },
    /* UT99: File copy/delete/move */
    { "CopyFileA",               (PVOID)CopyFileA },
    { "CopyFileW",               (PVOID)CopyFileW },
    { "DeleteFileA",             (PVOID)DeleteFileA },
    { "DeleteFileW",             (PVOID)DeleteFileW },
    { "MoveFileA",               (PVOID)MoveFileA },
    { "MoveFileW",               (PVOID)MoveFileW },
    /* UT99: Directory W variants */
    { "CreateDirectoryW",        (PVOID)CreateDirectoryW },
    { "RemoveDirectoryW",        (PVOID)RemoveDirectoryW },
    { "GetCurrentDirectoryW",    (PVOID)GetCurrentDirectoryW },
    { "SetCurrentDirectoryA",    (PVOID)SetCurrentDirectoryA },
    { "SetCurrentDirectoryW",    (PVOID)SetCurrentDirectoryW },
    { "SetFileAttributesW",      (PVOID)SetFileAttributesW },
    /* UT99: System/Windows directory */
    { "GetSystemDirectoryA",     (PVOID)GetSystemDirectoryA },
    { "GetSystemDirectoryW",     (PVOID)GetSystemDirectoryW },
    { "GetWindowsDirectoryA",    (PVOID)GetWindowsDirectoryA },
    { "GetWindowsDirectoryW",    (PVOID)GetWindowsDirectoryW },
    /* UT99: Find file W variants */
    { "FindFirstFileW",          (PVOID)FindFirstFileW },
    { "FindNextFileW",           (PVOID)FindNextFileW },
    /* UT99: Module filename W */
    { "GetModuleFileNameW",      (PVOID)GetModuleFileNameW },
    /* UT99: Mutex */
    { "CreateMutexA",            (PVOID)CreateMutexA },
    { "CreateMutexW",            (PVOID)CreateMutexW },
    /* UT99: Thread priority */
    { "SetThreadPriority",       (PVOID)SetThreadPriority },
    /* Phase 21: Event objects */
    { "CreateEventA",            (PVOID)CreateEventA },
    { "CreateEventW",            (PVOID)CreateEventW },
    { "SetEvent",                (PVOID)SetEvent },
    { "ResetEvent",              (PVOID)ResetEvent },
    { "PulseEvent",              (PVOID)PulseEvent },
    { "OpenEventA",              (PVOID)OpenEventA },
    { "OpenEventW",              (PVOID)OpenEventW },
    /* INI file (Private Profile) */
    { "GetPrivateProfileStringA",      (PVOID)GetPrivateProfileStringA },
    { "WritePrivateProfileStringA",    (PVOID)WritePrivateProfileStringA },
    { "GetPrivateProfileIntA",         (PVOID)GetPrivateProfileIntA },
    { "GetPrivateProfileSectionNamesA",(PVOID)GetPrivateProfileSectionNamesA },
    /* UT99: Process/Memory/System */
    { "GlobalMemoryStatus",      (PVOID)GlobalMemoryStatus },
    { "SetConsoleCtrlHandler",   (PVOID)SetConsoleCtrlHandler },
    { "GetProcessWorkingSetSize",(PVOID)GetProcessWorkingSetSize },
    { "GlobalAlloc",             (PVOID)GlobalAlloc },
    { "CreateProcessA",          (PVOID)CreateProcessA },
    { "CreateProcessW",          (PVOID)CreateProcessW },
    { "FormatMessageA",          (PVOID)FormatMessageA },
    { "FormatMessageW",          (PVOID)FormatMessageW },
    { "GetComputerNameA",        (PVOID)GetComputerNameA },
    { "GetComputerNameW",        (PVOID)GetComputerNameW },
    { "GetExitCodeProcess",      (PVOID)GetExitCodeProcess },
    { "GetLocalTime",            (PVOID)GetLocalTime },
    { "GetVersion",              (PVOID)GetVersion },
    { "GetVersionExW",           (PVOID)GetVersionExW },
    { "TerminateProcess",        (PVOID)TerminateProcess },
    { "HeapCreate",              (PVOID)HeapCreate },
    { "HeapDestroy",             (PVOID)HeapDestroy },
    { "HeapValidate",            (PVOID)HeapValidate },
    { "ExitThread",              (PVOID)ExitThread },
    { "SetErrorMode",            (PVOID)SetErrorMode },
    { "SetHandleCount",          (PVOID)SetHandleCount },
    { "SetStdHandle",            (PVOID)SetStdHandle },
    { "FlushFileBuffers",        (PVOID)FlushFileBuffers },
    { "GetFileType",             (PVOID)GetFileType },
    { "SetEnvironmentVariableA", (PVOID)SetEnvironmentVariableA },
    { "SetEnvironmentVariableW", (PVOID)SetEnvironmentVariableW },
    { "GetEnvironmentStrings",   (PVOID)GetEnvironmentStrings },
    { "GetEnvironmentStringsW",  (PVOID)GetEnvironmentStringsW },
    { "FreeEnvironmentStringsW", (PVOID)FreeEnvironmentStringsW },
    { "Beep",                    (PVOID)Beep_stub },
    { "GetACP",                  (PVOID)GetACP },
    { "GetOEMCP",                (PVOID)GetOEMCP },
    { "GetCPInfo",               (PVOID)GetCPInfo },
    { "GetUserDefaultLCID",      (PVOID)GetUserDefaultLCID },
    { "IsValidCodePage",         (PVOID)IsValidCodePage },
    { "IsValidLocale",           (PVOID)IsValidLocale },
    { "EnumSystemLocalesA",      (PVOID)EnumSystemLocalesA },
    { "GetLocaleInfoA",          (PVOID)GetLocaleInfoA },
    { "GetLocaleInfoW",          (PVOID)GetLocaleInfoW },
    { "CompareStringA",          (PVOID)CompareStringA },
    { "CompareStringW",          (PVOID)CompareStringW },
    { "GetStringTypeA",          (PVOID)GetStringTypeA },
    { "GetStringTypeW",          (PVOID)GetStringTypeW },
    { "LCMapStringA",            (PVOID)LCMapStringA },
    { "LCMapStringW",            (PVOID)LCMapStringW },
    { "IsBadReadPtr",            (PVOID)IsBadReadPtr },
    { "IsBadWritePtr",           (PVOID)IsBadWritePtr },
    { "IsBadCodePtr",            (PVOID)IsBadCodePtr },
    { "GetFullPathNameW",        (PVOID)GetFullPathNameW },
    { "GetFileAttributesW",      (PVOID)GetFileAttributesW },
    { "FileTimeToLocalFileTime", (PVOID)FileTimeToLocalFileTime },
    { "FileTimeToSystemTime",    (PVOID)FileTimeToSystemTime },
    { "GetTimeZoneInformation",  (PVOID)GetTimeZoneInformation },
    { "GetLogicalDrives",        (PVOID)GetLogicalDrives },
    { "GetDriveTypeA",           (PVOID)GetDriveTypeA },
    { "GetDriveTypeW",           (PVOID)GetDriveTypeW },
    { "GetDiskFreeSpaceA",       (PVOID)GetDiskFreeSpaceA },
    /* MSVCRT CRT init stubs */
    { "HeapCompact",             (PVOID)HeapCompact_stub },
    { "HeapWalk",                (PVOID)HeapWalk_stub },
    { "ReadConsoleA",            (PVOID)ReadConsoleA_stub },
    { "SetConsoleMode",          (PVOID)SetConsoleMode_stub },
    { "GetConsoleMode",          (PVOID)GetConsoleMode_stub },
    { "SetEndOfFile",            (PVOID)SetEndOfFile_stub },
    { "GetFileInformationByHandle",(PVOID)GetFileInformationByHandle_stub },
    { "PeekNamedPipe",           (PVOID)PeekNamedPipe_stub },
    { "ReadConsoleInputA",       (PVOID)ReadConsoleInputA_stub },
    { "PeekConsoleInputA",       (PVOID)PeekConsoleInputA_stub },
    { "GetNumberOfConsoleInputEvents",(PVOID)GetNumberOfConsoleInputEvents_stub },
    { "LockFile",                (PVOID)LockFile_stub },
    { "UnlockFile",              (PVOID)UnlockFile_stub },
    { "CreatePipe",              (PVOID)CreatePipe_stub },
    { "SetFileTime",             (PVOID)SetFileTime_stub },
    { "LocalFileTimeToFileTime", (PVOID)LocalFileTimeToFileTime_stub },
    { "SystemTimeToFileTime",    (PVOID)SystemTimeToFileTime_stub },
    { "GetSystemTime",           (PVOID)GetSystemTime_stub },
    { "SetLocalTime",            (PVOID)SetLocalTime_stub },
    { "GlobalFree",              (PVOID)GlobalFree_stub },
    { "ReleaseMutex",            (PVOID)ReleaseMutex_stub },
    { "OutputDebugStringW",      (PVOID)OutputDebugStringW_stub },
    { "GlobalAddAtomW",          (PVOID)GlobalAddAtomW_stub },
    { NULL, NULL }
};

static int k32_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

PVOID kernel32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    if (by_ordinal) return NULL;

    for (int i = 0; k32_exports[i].name; i++) {
        if (k32_strcmp(func_name, k32_exports[i].name) == 0)
            return k32_exports[i].func;
    }

    return NULL;
}

PVOID kernel32_shim_init(void)
{
    return (PVOID)k32_exports;
}
