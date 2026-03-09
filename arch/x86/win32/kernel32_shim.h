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

BOOL    WINAPI SetFilePointer(HANDLE hFile, LONG lDistanceToMove,
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
DWORD   WINAPI GetCurrentProcessId(void);

/* ── Memory API ─────────────────────────────────────────────── */

PVOID   WINAPI VirtualAlloc(PVOID lpAddress, SIZE_T dwSize,
                     DWORD flAllocationType, DWORD flProtect);

BOOL    WINAPI VirtualFree(PVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType);

/* ── Heap API ───────────────────────────────────────────────── */

HANDLE  WINAPI GetProcessHeap(void);
PVOID   WINAPI HeapAlloc(HANDLE hHeap, DWORD dwFlags, SIZE_T dwBytes);
BOOL    WINAPI HeapFree(HANDLE hHeap, DWORD dwFlags, PVOID lpMem);

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

/* ── String API ─────────────────────────────────────────────── */

int     WINAPI lstrlenA(PCSTR lpString);
int     WINAPI lstrlenW(PCWSTR lpString);

/* ── Command line / Environment ─────────────────────────────── */

PCSTR   WINAPI GetCommandLineA(void);
PCWSTR  WINAPI GetCommandLineW(void);
PCSTR   WINAPI GetEnvironmentStringsA(void);
BOOL    WINAPI FreeEnvironmentStringsA(PCSTR lpszEnvironmentBlock);

/* ── Handle / Protection ────────────────────────────────────── */

BOOL    WINAPI DuplicateHandle(HANDLE hSourceProcessHandle, HANDLE hSourceHandle,
                        HANDLE hTargetProcessHandle, PHANDLE lpTargetHandle,
                        DWORD dwDesiredAccess, BOOL bInheritHandle,
                        DWORD dwOptions);

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
void WINAPI InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION lpCS, DWORD dwSpinCount);
void WINAPI EnterCriticalSection(LPCRITICAL_SECTION lpCS);
BOOL WINAPI TryEnterCriticalSection(LPCRITICAL_SECTION lpCS);
void WINAPI LeaveCriticalSection(LPCRITICAL_SECTION lpCS);
void WINAPI DeleteCriticalSection(LPCRITICAL_SECTION lpCS);

/* ── Thread Local Storage ──────────────────────────────────── */

DWORD  WINAPI TlsAlloc(void);
BOOL   WINAPI TlsFree(DWORD dwTlsIndex);
PVOID  WINAPI TlsGetValue(DWORD dwTlsIndex);
BOOL   WINAPI TlsSetValue(DWORD dwTlsIndex, PVOID lpTlsValue);

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
DWORD  WINAPI WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds);
DWORD  WINAPI WaitForMultipleObjects(DWORD nCount, const HANDLE *lpHandles,
                                     BOOL bWaitAll, DWORD dwMilliseconds);
BOOL   WINAPI SetThreadPriority(HANDLE hThread, int nPriority);

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

/* ── Mutex API ─────────────────────────────────────────────── */

HANDLE WINAPI CreateMutexA(PVOID lpMutexAttributes, BOOL bInitialOwner, PCSTR lpName);
HANDLE WINAPI CreateMutexW(PVOID lpMutexAttributes, BOOL bInitialOwner, PCWSTR lpName);

/* ── DLL / Module API ──────────────────────────────────────── */

HANDLE WINAPI LoadLibraryA(PCSTR lpLibFileName);
HANDLE WINAPI LoadLibraryW(PCWSTR lpLibFileName);
HANDLE WINAPI LoadLibraryExA(PCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
BOOL   WINAPI FreeLibrary(HANDLE hLibModule);
DWORD  WINAPI GetModuleFileNameA(HANDLE hModule, PSTR lpFilename, DWORD nSize);
DWORD  WINAPI GetModuleFileNameW(HANDLE hModule, PWSTR lpFilename, DWORD nSize);

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

/* ── Path / Directory ──────────────────────────────────────── */

DWORD WINAPI GetFullPathNameA(PCSTR lpFileName, DWORD nBufferLength,
                              PSTR lpBuffer, PSTR *lpFilePart);
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
DWORD WINAPI GetSystemDirectoryA(PSTR lpBuffer, DWORD uSize);
DWORD WINAPI GetSystemDirectoryW(PWSTR lpBuffer, DWORD uSize);
DWORD WINAPI GetWindowsDirectoryA(PSTR lpBuffer, DWORD uSize);
DWORD WINAPI GetWindowsDirectoryW(PWSTR lpBuffer, DWORD uSize);

/* ── Find File ─────────────────────────────────────────────── */

typedef struct _WIN32_FIND_DATAA {
    DWORD    dwFileAttributes;
    ULONGLONG ftCreationTime;
    ULONGLONG ftLastAccessTime;
    ULONGLONG ftLastWriteTime;
    DWORD    nFileSizeHigh;
    DWORD    nFileSizeLow;
    DWORD    dwReserved0;
    DWORD    dwReserved1;
    char     cFileName[260];
    char     cAlternateFileName[14];
} WIN32_FIND_DATAA, *LPWIN32_FIND_DATAA;

typedef struct _WIN32_FIND_DATAW {
    DWORD    dwFileAttributes;
    ULONGLONG ftCreationTime;
    ULONGLONG ftLastAccessTime;
    ULONGLONG ftLastWriteTime;
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

/* ── Interlocked ───────────────────────────────────────────── */

LONG WINAPI InterlockedIncrement(volatile LONG *Addend);
LONG WINAPI InterlockedDecrement(volatile LONG *Addend);
LONG WINAPI InterlockedExchange(volatile LONG *Target, LONG Value);
LONG WINAPI InterlockedCompareExchange(volatile LONG *Dest, LONG Exchange, LONG Comparand);

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

#endif /* KERNEL32_SHIM_H */
