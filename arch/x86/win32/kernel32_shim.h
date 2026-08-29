/*
 * OsitoK Windows Compatibility Layer — kernel32.dll Shim
 *
 * Provides Win32 API functions that console applications commonly use.
 * Each function translates to the corresponding NT API call.
 *
 * This is the minimum set needed to run a basic "Hello World" console
 * app compiled with MSVC or MinGW targeting Windows.
 */

#ifndef KERNEL32_SHIM_H
#define KERNEL32_SHIM_H

#include "nttypes.h"

/* ── Console handle constants (Windows convention) ──────────── */

#define WIN32_STD_INPUT_HANDLE  ((ULONG)-10)
#define WIN32_STD_OUTPUT_HANDLE ((ULONG)-11)
#define WIN32_STD_ERROR_HANDLE  ((ULONG)-12)

/* ── File API ───────────────────────────────────────────────── */

HANDLE  WINAPI CreateFileA(PCSTR lpFileName, DWORD dwDesiredAccess,
                    DWORD dwShareMode, PVOID lpSecurityAttributes,
                    DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes,
                    HANDLE hTemplateFile);

HANDLE  WINAPI CreateFileW(PCWSTR lpFileName, DWORD dwDesiredAccess,
                    DWORD dwShareMode, PVOID lpSecurityAttributes,
                    DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes,
                    HANDLE hTemplateFile);

BOOL    WINAPI ReadFile(HANDLE hFile, PVOID lpBuffer, DWORD nNumberOfBytesToRead,
                 DWORD *lpNumberOfBytesRead, PVOID lpOverlapped);

BOOL    WINAPI WriteFile(HANDLE hFile, PCVOID lpBuffer, DWORD nNumberOfBytesToWrite,
                  DWORD *lpNumberOfBytesWritten, PVOID lpOverlapped);

BOOL    WINAPI CloseHandle(HANDLE hObject);

DWORD   WINAPI GetFileSize(HANDLE hFile, DWORD *lpFileSizeHigh);
DWORD   WINAPI GetFileAttributesW(PCWSTR lpFileName);

DWORD   WINAPI SetFilePointer(HANDLE hFile, LONG lDistanceToMove,
                       LONG *lpDistanceToMoveHigh, DWORD dwMoveMethod);

BOOL    WINAPI CopyFileA(PCSTR lpExistingFileName, PCSTR lpNewFileName, BOOL bFailIfExists);
BOOL    WINAPI CopyFileW(PCWSTR lpExistingFileName, PCWSTR lpNewFileName, BOOL bFailIfExists);
BOOL    WINAPI DeleteFileA(PCSTR lpFileName);
BOOL    WINAPI DeleteFileW(PCWSTR lpFileName);
BOOL    WINAPI MoveFileA(PCSTR lpExistingFileName, PCSTR lpNewFileName);
BOOL    WINAPI MoveFileW(PCWSTR lpExistingFileName, PCWSTR lpNewFileName);

/* ── Console API ────────────────────────────────────────────── */

HANDLE  WINAPI GetStdHandle(DWORD nStdHandle);

BOOL    WINAPI WriteConsoleA(HANDLE hConsoleOutput, PCVOID lpBuffer,
                      DWORD nNumberOfCharsToWrite,
                      DWORD *lpNumberOfCharsWritten, PVOID lpReserved);

/* ── Process API ────────────────────────────────────────────── */

void    WINAPI ExitProcess(DWORD uExitCode);
HANDLE  WINAPI GetCurrentProcess(void);
HANDLE  WINAPI OpenProcess(DWORD dwDesiredAccess, BOOL bInheritHandle,
                           DWORD dwProcessId);
DWORD   WINAPI GetCurrentProcessId(void);

/* ── Memory API ─────────────────────────────────────────────── */

PVOID   WINAPI VirtualAlloc(PVOID lpAddress, SIZE_T dwSize,
                     DWORD flAllocationType, DWORD flProtect);

BOOL    WINAPI VirtualFree(PVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType);

/* ── Memory-Mapped File API ─────────────────────────────────── */

HANDLE  WINAPI CreateFileMappingA(HANDLE hFile, PVOID lpFileMappingAttributes,
                    DWORD flProtect, DWORD dwMaximumSizeHigh,
                    DWORD dwMaximumSizeLow, PCSTR lpName);

HANDLE  WINAPI CreateFileMappingW(HANDLE hFile, PVOID lpFileMappingAttributes,
                    DWORD flProtect, DWORD dwMaximumSizeHigh,
                    DWORD dwMaximumSizeLow, PCWSTR lpName);

HANDLE  WINAPI OpenFileMappingA(DWORD dwDesiredAccess, BOOL bInheritHandle,
                    PCSTR lpName);
HANDLE  WINAPI OpenFileMappingW(DWORD dwDesiredAccess, BOOL bInheritHandle,
                    PCWSTR lpName);

PVOID   WINAPI MapViewOfFile(HANDLE hFileMappingObject, DWORD dwDesiredAccess,
                    DWORD dwFileOffsetHigh, DWORD dwFileOffsetLow,
                    SIZE_T dwNumberOfBytesToMap);

BOOL    WINAPI UnmapViewOfFile(PCVOID lpBaseAddress);

/* ── Heap API ───────────────────────────────────────────────── */

HANDLE  WINAPI GetProcessHeap(void);
PVOID   WINAPI HeapAlloc(HANDLE hHeap, DWORD dwFlags, SIZE_T dwBytes);
BOOL    WINAPI HeapFree(HANDLE hHeap, DWORD dwFlags, PVOID lpMem);
SIZE_T  WINAPI HeapSize(HANDLE hHeap, DWORD dwFlags, PCVOID lpMem);
PVOID   WINAPI HeapReAlloc(HANDLE hHeap, DWORD dwFlags, PVOID lpMem,
                    SIZE_T dwBytes);
PVOID   WINAPI LocalAlloc(UINT uFlags, SIZE_T dwBytes);
PVOID   WINAPI LocalFree(PVOID hMem);

/* ── Error API ──────────────────────────────────────────────── */

DWORD   WINAPI GetLastError(void);
void    WINAPI SetLastError(DWORD dwErrCode);

/* ── Misc API ───────────────────────────────────────────────── */

void    WINAPI Sleep(DWORD dwMilliseconds);
BOOL    WINAPI QueryPerformanceCounter(PLARGE_INTEGER lpPerformanceCount);
BOOL    WINAPI QueryPerformanceFrequency(PLARGE_INTEGER lpFrequency);
PVOID   WINAPI GetProcAddress(HANDLE hModule, PCSTR lpProcName);
HANDLE  WINAPI GetModuleHandleA(PCSTR lpModuleName);
HANDLE  WINAPI GetModuleHandleW(PCWSTR lpModuleName);
BOOL    WINAPI GetModuleHandleExA(DWORD dwFlags, PCSTR lpModuleName,
                                  PHANDLE phModule);
BOOL    WINAPI GetModuleHandleExW(DWORD dwFlags, PCWSTR lpModuleName,
                                  PHANDLE phModule);

/* ── String API ─────────────────────────────────────────────── */

int     WINAPI lstrlenA(PCSTR lpString);
int     WINAPI lstrlenW(PCWSTR lpString);

/* ── Command line / Environment ─────────────────────────────── */

PCSTR   WINAPI GetCommandLineA(void);
PCWSTR  WINAPI GetCommandLineW(void);
PCSTR   WINAPI GetEnvironmentStringsA(void);
BOOL    WINAPI FreeEnvironmentStringsA(PCSTR lpszEnvironmentBlock);
PCWSTR  WINAPI GetEnvironmentStringsW(void);
BOOL    WINAPI FreeEnvironmentStringsW(PCWSTR lpszEnvironmentBlock);
DWORD   WINAPI GetEnvironmentVariableA(PCSTR lpName, PSTR lpBuffer,
                                       DWORD nSize);
DWORD   WINAPI GetEnvironmentVariableW(PCWSTR lpName, PWSTR lpBuffer,
                                       DWORD nSize);
BOOL    WINAPI SetEnvironmentVariableA(PCSTR lpName, PCSTR lpValue);
BOOL    WINAPI SetEnvironmentVariableW(PCWSTR lpName, PCWSTR lpValue);

/* ── Handle / Protection ────────────────────────────────────── */

BOOL    WINAPI DuplicateHandle(HANDLE hSourceProcessHandle, HANDLE hSourceHandle,
                        HANDLE hTargetProcessHandle, PHANDLE lpTargetHandle,
                        DWORD dwDesiredAccess, BOOL bInheritHandle,
                        DWORD dwOptions);
BOOL    WINAPI SetHandleInformation(HANDLE hObject, DWORD dwMask, DWORD dwFlags);

BOOL    WINAPI VirtualProtect(PVOID lpAddress, SIZE_T dwSize,
                       DWORD flNewProtect, DWORD *lpflOldProtect);

/* ── Critical Section ───────────────────────────────────────── */

typedef struct _RTL_CRITICAL_SECTION {
    PVOID       DebugInfo;
    LONG        LockCount;
    LONG        RecursionCount;
    HANDLE      OwningThread;
    HANDLE      LockSemaphore;
    ULONG_PTR   SpinCount;
} RTL_CRITICAL_SECTION, *PRTL_CRITICAL_SECTION, CRITICAL_SECTION, *LPCRITICAL_SECTION;

void WINAPI InitializeCriticalSection(LPCRITICAL_SECTION lpCS);
BOOL WINAPI InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION lpCS, DWORD dwSpinCount);
DWORD WINAPI SetCriticalSectionSpinCount(LPCRITICAL_SECTION lpCS,
                                         DWORD dwSpinCount);
void WINAPI EnterCriticalSection(LPCRITICAL_SECTION lpCS);
BOOL WINAPI TryEnterCriticalSection(LPCRITICAL_SECTION lpCS);
void WINAPI LeaveCriticalSection(LPCRITICAL_SECTION lpCS);
void WINAPI DeleteCriticalSection(LPCRITICAL_SECTION lpCS);

/* ── Thread Local Storage ──────────────────────────────────── */

DWORD  WINAPI TlsAlloc(void);
BOOL   WINAPI TlsFree(DWORD dwTlsIndex);
PVOID  WINAPI TlsGetValue(DWORD dwTlsIndex);
BOOL   WINAPI TlsSetValue(DWORD dwTlsIndex, PVOID lpTlsValue);
DWORD  WINAPI FlsAlloc(PVOID lpCallback);
BOOL   WINAPI FlsFree(DWORD dwFlsIndex);
PVOID  WINAPI FlsGetValue(DWORD dwFlsIndex);
BOOL   WINAPI FlsSetValue(DWORD dwFlsIndex, PVOID lpFlsData);
void   win32_tls_reset(void);

/* Fiber API */
typedef void (WINAPI *LPFIBER_START_ROUTINE)(PVOID);

PVOID  WINAPI ConvertThreadToFiber(PVOID lpParameter);
PVOID  WINAPI ConvertThreadToFiberEx(PVOID lpParameter, DWORD dwFlags);
BOOL   WINAPI ConvertFiberToThread(void);
PVOID  WINAPI CreateFiber(SIZE_T dwStackSize,
                          LPFIBER_START_ROUTINE lpStartAddress,
                          PVOID lpParameter);
PVOID  WINAPI CreateFiberEx(SIZE_T dwStackCommitSize,
                            SIZE_T dwStackReserveSize, DWORD dwFlags,
                            LPFIBER_START_ROUTINE lpStartAddress,
                            PVOID lpParameter);
void   WINAPI DeleteFiber(PVOID lpFiber);
void   WINAPI SwitchToFiber(PVOID lpFiber);
BOOL   WINAPI IsThreadAFiber(void);

/* ── Thread API ────────────────────────────────────────────── */

typedef DWORD (WINAPI *LPTHREAD_START_ROUTINE)(PVOID);

HANDLE WINAPI CreateThread(PVOID lpThreadAttributes, SIZE_T dwStackSize,
                           LPTHREAD_START_ROUTINE lpStartAddress,
                           PVOID lpParameter, DWORD dwCreationFlags,
                           DWORD *lpThreadId);
DWORD  WINAPI GetCurrentThreadId(void);
HANDLE WINAPI GetCurrentThread(void);
DWORD  WINAPI SuspendThread(HANDLE hThread);
DWORD  WINAPI ResumeThread(HANDLE hThread);
BOOL   WINAPI TerminateThread(HANDLE hThread, DWORD dwExitCode);
void   WINAPI ExitThread(DWORD dwExitCode) __attribute__((noreturn));
DWORD  WINAPI WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds);
DWORD  WINAPI WaitForMultipleObjects(DWORD nCount, const HANDLE *lpHandles,
                                     BOOL bWaitAll, DWORD dwMilliseconds);
BOOL   WINAPI SetThreadPriority(HANDLE hThread, int nPriority);
BOOL   WINAPI SetPriorityClass(HANDLE hProcess, DWORD dwPriorityClass);
DWORD  WINAPI GetPriorityClass(HANDLE hProcess);

/* ── Event API ─────────────────────────────────────────────── */

HANDLE WINAPI CreateEventA(PVOID lpEventAttributes, BOOL bManualReset,
                           BOOL bInitialState, PCSTR lpName);
HANDLE WINAPI CreateEventW(PVOID lpEventAttributes, BOOL bManualReset,
                           BOOL bInitialState, PCWSTR lpName);
BOOL   WINAPI SetEvent(HANDLE hEvent);
BOOL   WINAPI ResetEvent(HANDLE hEvent);
BOOL   WINAPI PulseEvent(HANDLE hEvent);
HANDLE WINAPI OpenEventA(DWORD dwDesiredAccess, BOOL bInheritHandle, PCSTR lpName);
HANDLE WINAPI OpenEventW(DWORD dwDesiredAccess, BOOL bInheritHandle, PCWSTR lpName);
HANDLE WINAPI CreateIoCompletionPort(HANDLE file, HANDLE existing_port,
                                     ULONG_PTR completion_key,
                                     DWORD concurrent_threads);
BOOL   WINAPI PostQueuedCompletionStatus(HANDLE port, DWORD bytes,
                                         ULONG_PTR completion_key,
                                         PVOID overlapped);
BOOL   WINAPI GetQueuedCompletionStatus(HANDLE port, DWORD *bytes,
                                        ULONG_PTR *completion_key,
                                        PVOID *overlapped,
                                        DWORD timeout_ms);

/* Internal bridge used by overlapped-capable shims such as Winsock. */
BOOL   k32_iocp_complete_handle(HANDLE file, DWORD bytes, PVOID overlapped);
BOOL   k32_iocp_complete_handle_for_owner(HANDLE file, DWORD bytes,
                                           PVOID overlapped, DWORD owner_pid,
                                           BOOL compat32);
BOOL   k32_iocp_complete_handle_status_for_owner(
           HANDLE file, DWORD bytes, PVOID overlapped, DWORD owner_pid,
           BOOL compat32, NTSTATUS completion_status);
BOOL   k32_iocp_wake_handle_for_owner(HANDLE file, DWORD owner_pid);
void   k32_iocp_forget_file(HANDLE file);
void   k32_iocp_thread_blocking(void);
BOOL   WINAPI GetQueuedCompletionStatusEx(HANDLE port, PVOID entries,
                                          ULONG count, ULONG *removed,
                                          DWORD timeout_ms, BOOL alertable);
BOOL   WINAPI SetFileCompletionNotificationModes(HANDLE file, BYTE flags);

/* ── Mutex API ─────────────────────────────────────────────── */

HANDLE WINAPI CreateMutexA(PVOID lpMutexAttributes, BOOL bInitialOwner, PCSTR lpName);
HANDLE WINAPI CreateMutexW(PVOID lpMutexAttributes, BOOL bInitialOwner, PCWSTR lpName);

/* ── DLL / Module API ──────────────────────────────────────── */

HANDLE WINAPI LoadLibraryA(PCSTR lpLibFileName);
HANDLE WINAPI LoadLibraryW(PCWSTR lpLibFileName);
HANDLE WINAPI LoadLibraryExA(PCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
HANDLE WINAPI LoadLibraryExW(PCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
BOOL   WINAPI FreeLibrary(HANDLE hLibModule);
BOOL   WINAPI DisableThreadLibraryCalls(HANDLE hLibModule);
DWORD  WINAPI GetModuleFileNameA(HANDLE hModule, PSTR lpFilename, DWORD nSize);
DWORD  WINAPI GetModuleFileNameW(HANDLE hModule, PWSTR lpFilename, DWORD nSize);
int kernel32_module_selftest(void);

/* ── Timing ────────────────────────────────────────────────── */

DWORD     WINAPI GetTickCount(void);
ULONGLONG WINAPI GetTickCount64(void);
void      WINAPI GetSystemTimeAsFileTime(PVOID lpSystemTimeAsFileTime);

/* ── System Info ───────────────────────────────────────────── */

typedef struct _SYSTEM_INFO {
    WORD      wProcessorArchitecture;
    WORD      wReserved;
    DWORD     dwPageSize;
    PVOID     lpMinimumApplicationAddress;
    PVOID     lpMaximumApplicationAddress;
    ULONG_PTR dwActiveProcessorMask;
    DWORD     dwNumberOfProcessors;
    DWORD     dwProcessorType;
    DWORD     dwAllocationGranularity;
    WORD      wProcessorLevel;
    WORD      wProcessorRevision;
} SYSTEM_INFO, *LPSYSTEM_INFO;

typedef struct _OSVERSIONINFOA {
    DWORD dwOSVersionInfoSize;
    DWORD dwMajorVersion;
    DWORD dwMinorVersion;
    DWORD dwBuildNumber;
    DWORD dwPlatformId;
    char  szCSDVersion[128];
} OSVERSIONINFOA, *LPOSVERSIONINFOA;

void WINAPI GetSystemInfo(LPSYSTEM_INFO lpSystemInfo);
BOOL WINAPI GetVersionExA(LPOSVERSIONINFOA lpVersionInformation);
BOOL WINAPI GetProductInfo(DWORD dwOSMajorVersion, DWORD dwOSMinorVersion,
                           DWORD dwSpMajorVersion, DWORD dwSpMinorVersion,
                           DWORD *pdwReturnedProductType);

/* ── Path / Directory ──────────────────────────────────────── */

DWORD WINAPI GetFullPathNameA(PCSTR lpFileName, DWORD nBufferLength,
                              PSTR lpBuffer, PSTR *lpFilePart);
DWORD WINAPI GetFullPathNameW(PCWSTR lpFileName, DWORD nBufferLength,
                              PWSTR lpBuffer, PWSTR *lpFilePart);
DWORD WINAPI GetCurrentDirectoryA(DWORD nBufferLength, PSTR lpBuffer);
DWORD WINAPI GetCurrentDirectoryW(DWORD nBufferLength, PWSTR lpBuffer);
BOOL  WINAPI SetCurrentDirectoryA(PCSTR lpPathName);
BOOL  WINAPI SetCurrentDirectoryW(PCWSTR lpPathName);
DWORD WINAPI GetFileAttributesA(PCSTR lpFileName);
BOOL  WINAPI SetFileAttributesA(PCSTR lpFileName, DWORD dwFileAttributes);
BOOL  WINAPI SetFileAttributesW(PCWSTR lpFileName, DWORD dwFileAttributes);
BOOL  WINAPI CreateDirectoryA(PCSTR lpPathName, PVOID lpSecurityAttributes);
BOOL  WINAPI CreateDirectoryW(PCWSTR lpPathName, PVOID lpSecurityAttributes);
BOOL  WINAPI RemoveDirectoryA(PCSTR lpPathName);
BOOL  WINAPI RemoveDirectoryW(PCWSTR lpPathName);
BOOL  WINAPI CreateSymbolicLinkW(PCWSTR lpSymlinkFileName,
                                 PCWSTR lpTargetFileName, DWORD dwFlags);
DWORD WINAPI GetSystemDirectoryA(PSTR lpBuffer, DWORD uSize);
DWORD WINAPI GetSystemDirectoryW(PWSTR lpBuffer, DWORD uSize);
DWORD WINAPI GetWindowsDirectoryA(PSTR lpBuffer, DWORD uSize);
DWORD WINAPI GetWindowsDirectoryW(PWSTR lpBuffer, DWORD uSize);

/* ── Find File ─────────────────────────────────────────────── */

/* CRITICAL: These structs are written by 64-bit code but read by 32-bit PE.
 * FILETIME on 32-bit is two DWORDs (4-byte aligned), but ULONGLONG on 64-bit
 * requires 8-byte alignment → 4 bytes padding after dwFileAttributes → cFileName
 * shifts from offset 44 to 48 → 32-bit code reads empty filenames.
 * Fix: use packed attribute to match 32-bit layout exactly. */
typedef struct __attribute__((packed)) _WIN32_FIND_DATAA {
    DWORD    dwFileAttributes;
    DWORD    ftCreationTimeLo, ftCreationTimeHi;
    DWORD    ftLastAccessTimeLo, ftLastAccessTimeHi;
    DWORD    ftLastWriteTimeLo, ftLastWriteTimeHi;
    DWORD    nFileSizeHigh;
    DWORD    nFileSizeLow;
    DWORD    dwReserved0;
    DWORD    dwReserved1;
    char     cFileName[260];
    char     cAlternateFileName[14];
} WIN32_FIND_DATAA, *LPWIN32_FIND_DATAA;

typedef struct __attribute__((packed)) _WIN32_FIND_DATAW {
    DWORD    dwFileAttributes;
    DWORD    ftCreationTimeLo, ftCreationTimeHi;
    DWORD    ftLastAccessTimeLo, ftLastAccessTimeHi;
    DWORD    ftLastWriteTimeLo, ftLastWriteTimeHi;
    DWORD    nFileSizeHigh;
    DWORD    nFileSizeLow;
    DWORD    dwReserved0;
    DWORD    dwReserved1;
    WCHAR    cFileName[260];
    WCHAR    cAlternateFileName[14];
} WIN32_FIND_DATAW, *LPWIN32_FIND_DATAW;

HANDLE WINAPI FindFirstFileA(PCSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData);
BOOL   WINAPI FindNextFileA(HANDLE hFindFile, LPWIN32_FIND_DATAA lpFindFileData);
HANDLE WINAPI FindFirstFileW(PCWSTR lpFileName, LPWIN32_FIND_DATAW lpFindFileData);
BOOL   WINAPI FindNextFileW(HANDLE hFindFile, LPWIN32_FIND_DATAW lpFindFileData);
BOOL   WINAPI FindClose(HANDLE hFindFile);

/* ── Startup / Debug ───────────────────────────────────────── */

typedef struct _STARTUPINFOA {
    DWORD  cb;
    PSTR   lpReserved;
    PSTR   lpDesktop;
    PSTR   lpTitle;
    DWORD  dwX, dwY, dwXSize, dwYSize;
    DWORD  dwXCountChars, dwYCountChars;
    DWORD  dwFillAttribute;
    DWORD  dwFlags;
    WORD   wShowWindow;
    WORD   cbReserved2;
    PVOID  lpReserved2;
    HANDLE hStdInput;
    HANDLE hStdOutput;
    HANDLE hStdError;
} STARTUPINFOA, *LPSTARTUPINFOA;

void   WINAPI GetStartupInfoA(LPSTARTUPINFOA lpStartupInfo);
BOOL   WINAPI IsDebuggerPresent(void);
PVOID  WINAPI SetUnhandledExceptionFilter(PVOID lpTopLevelExceptionFilter);
PVOID  kernel32_get_unhandled_exception_filter(void);
void   kernel32_release_process_exception_state(DWORD process_id);
void   WINAPI OutputDebugStringA(PCSTR lpOutputString);
void   WINAPI RaiseException(DWORD dwExceptionCode, DWORD dwExceptionFlags,
                             DWORD nNumberOfArguments, const ULONG_PTR *lpArguments);
LONG   WINAPI UnhandledExceptionFilter(PEXCEPTION_POINTERS ExceptionInfo);

/* ── String Conversion ─────────────────────────────────────── */

int WINAPI MultiByteToWideChar(DWORD CodePage, DWORD dwFlags,
                               PCSTR lpMultiByteStr, int cbMultiByte,
                               PWSTR lpWideCharStr, int cchWideChar);

int WINAPI WideCharToMultiByte(DWORD CodePage, DWORD dwFlags,
                               PCWSTR lpWideCharStr, int cchWideChar,
                               PSTR lpMultiByteStr, int cbMultiByte,
                               PCSTR lpDefaultChar, BOOL *lpUsedDefaultChar);

/* String/path API-set entry points currently provided by the kernel32 shim. */
int  WINAPI CompareStringOrdinal(PCWSTR string1, int count1,
                                 PCWSTR string2, int count2,
                                 BOOL ignore_case);
LONG WINAPI PathCchSkipRoot(PCWSTR pszPath, PCWSTR *ppszRootEnd);
LONG WINAPI PathCchCombineEx(PWSTR pszPathOut, SIZE_T cchPathOut,
                             PCWSTR pszPathIn, PCWSTR pszMore,
                             ULONG dwFlags);

/* ── Interlocked ───────────────────────────────────────────── */

LONG WINAPI InterlockedIncrement(volatile LONG *Addend);
LONG WINAPI InterlockedDecrement(volatile LONG *Addend);
LONG WINAPI InterlockedExchange(volatile LONG *Target, LONG Value);
LONG WINAPI InterlockedCompareExchange(volatile LONG *Dest, LONG Exchange, LONG Comparand);
PVOID WINAPI InterlockedFlushSList(PVOID list_head);

/* ── INI File API (Private Profile) ────────────────────────── */

DWORD WINAPI GetPrivateProfileStringA(PCSTR lpAppName, PCSTR lpKeyName,
                                       PCSTR lpDefault, PSTR lpReturnedString,
                                       DWORD nSize, PCSTR lpFileName);
BOOL  WINAPI WritePrivateProfileStringA(PCSTR lpAppName, PCSTR lpKeyName,
                                         PCSTR lpString, PCSTR lpFileName);
UINT  WINAPI GetPrivateProfileIntA(PCSTR lpAppName, PCSTR lpKeyName,
                                    int nDefault, PCSTR lpFileName);
DWORD WINAPI GetPrivateProfileSectionNamesA(PSTR lpszReturnBuffer,
                                             DWORD nSize, PCSTR lpFileName);

/* ── Shim resolution ────────────────────────────────────────── */

PVOID   kernel32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal);
PVOID   kernel32_shim_init(void);

/* Internal process state inherited by CreateProcess children. */
const char *kernel32_current_directory_relative(void);

/* Internal NT/Win32 bridge for OsitoFS virtual directory handles. Paths are
 * already normalized and do not include a drive prefix. */
bool    win32_normalize_path(PCSTR path, char out[260]);
bool    win32_directory_exists_normalized(const char *path);
DWORD   win32_directory_create_normalized(const char *path);
void    k32_pipe_service_pending(void);
void    kernel32_inherit_process_environment(DWORD parent_pid,
                                             DWORD child_pid);
void    kernel32_release_process_environment(DWORD process_id);
SIZE_T  kernel32_build_environment_block_w(DWORD process_id, PWSTR buffer,
                                           SIZE_T capacity);
void    k32_power_request_release(PVOID object);

#endif /* KERNEL32_SHIM_H */
