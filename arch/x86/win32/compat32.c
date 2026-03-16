/*
 * OsitoK Windows Compatibility Layer — 32-bit Compatibility Mode
 *
 * Thunk generation for PE32 (i386) → 64-bit shim calls.
 *
 * Each thunk is a small code stub in low memory (<4GB) that:
 *   1. Reads cdecl stack arguments
 *   2. Marshals them into ms_abi registers (RCX, RDX, R8, R9 + stack)
 *   3. On bare metal: far-calls to 64-bit CS, runs shim, far-returns
 *   4. On test harness: calls directly (already in 64-bit mode)
 *   5. Returns EAX to the 32-bit caller
 *
 * Thunk memory layout (per function, 64 bytes):
 *   [0x00..0x3F]  machine code for the thunk stub
 *
 * The thunk pool is a single executable page (4KB = 64 thunks per page).
 * We allocate additional pages as needed.
 */

#include "compat32.h"
#include "dllloader.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void *mem_alloc_pages(uint64_t count);
extern int  kern_setjmp(uint64_t *buf);
extern void kern_longjmp(uint64_t *buf, int val);

/* ── Global compat32 mode flag ────────────────────────────────── */

int g_compat32_mode = 0;

/* ── Thunk state ─────────────────────────────────────────────── */

#define THUNK_STUB_SIZE  64      /* bytes per thunk stub */
#define THUNKS_PER_PAGE  (4096 / THUNK_STUB_SIZE)  /* 64 */
#define THUNK_POOL_PAGES 32      /* 32 pages = 2048 thunks */

static uint8_t *thunk_pool = NULL;
static uint32_t thunk_count = 0;

static compat32_thunk_t thunk_table[COMPAT32_MAX_THUNKS];

/* INT 0x2E now uses IST1 via TSS — no manual stack management needed */

/* ── FName::Names diagnostic ────────────────────────────────── */
/*
 * Address of FName::Names TArray<FNameEntry*> in Core.dll.
 * Stored during IAT patching for diagnostic dumps.
 * Layout: { FNameEntry** Data; INT Num; INT Max; } — 12 bytes.
 */
uint32_t g_fname_names_addr = 0;
uint32_t g_gmalloc_addr = 0;

/* ── Callback mechanism (64-bit → 32-bit → 64-bit) ─────────── */

/*
 * Magic thunk index for callback return. When the INT 0x2E handler
 * sees this index, it longjmps back to the caller instead of
 * dispatching to a shim function.
 */
#define THUNK_CALLBACK_RETURN  0xFFFFFFFE

/* Return stub address (32-bit code in thunk pool that INT 0x2Es back) */
static uint32_t callback_return_stub_addr = 0;

/*
 * Reentrant callback support.
 *
 * DllMain's CRT init calls _initterm which invokes compat32_callback()
 * for each C++ constructor — while the DllMain callback itself is still
 * active. Without nesting support, the inner callback overwrites the
 * outer's jmpbuf and stack, causing a #GP on return.
 *
 * We support up to MAX_CALLBACK_DEPTH nested callbacks, each with its
 * own jmpbuf, return value, and stack.
 */
#define MAX_CALLBACK_DEPTH    32
#define CALLBACK_STACK_SIZE   16384

/* Force to .data section to change RIP-relative displacement encoding.
 * In BSS, the displacement contained 0xCC at a critical code address,
 * which QEMU TCG misinterpreted as INT3. */
static int      callback_depth __attribute__((section(".data"))) = 0;
static uint64_t callback_jmpbufs[MAX_CALLBACK_DEPTH][8];
static uint64_t callback_saved_ist1[MAX_CALLBACK_DEPTH];  /* IST1 before LRETQ */
static uint8_t  callback_stacks[MAX_CALLBACK_DEPTH][CALLBACK_STACK_SIZE]
    __attribute__((aligned(16)));

/*
 * Single global retval written by the 32-bit return stub (MOV [addr], EAX).
 * The stub uses a fixed address so we can't index by depth there.
 * The dispatch handler reads this and stores it before longjmp.
 */
static uint32_t callback_retval = 0;

/* ── Thunk code generation ───────────────────────────────────── */

/*
 * Generate a cdecl→ms_abi thunk stub.
 *
 * For a PE32 function with N stack arguments:
 *   On entry (cdecl): [ESP+4]=arg1, [ESP+8]=arg2, ...
 *   ms_abi target:    RCX=arg1, RDX=arg2, R8=arg3, R9=arg4, [RSP+0x20..]=rest
 *
 * The thunk uses a fixed pattern:
 *   - Encode the 64-bit target address inline
 *   - Load up to 4 args into registers from the 32-bit stack
 *   - Push remaining args onto the 64-bit stack (with shadow space)
 *   - Call the target
 *   - Return (EAX has the result)
 *
 * On x86-64 test harness: PE32 code is loaded in low memory but runs
 * as 64-bit code (same CS). The thunk just marshals calling convention.
 *
 * On OsitoK bare metal: PE32 code runs in compat mode. The thunk must
 * switch to 64-bit mode (far call to 64-bit CS) then switch back.
 * This is the "Heaven's Gate" technique.
 */

static void emit_thunk(uint8_t *code, uint64_t target, uint8_t num_args, uint8_t callconv)
{
    int p = 0;

#ifdef TEST_HARNESS
    /* movabs rax, target (10 bytes) */
    code[p++] = 0x48;  /* REX.W */
    code[p++] = 0xB8;  /* MOV RAX, imm64 */
    for (int i = 0; i < 8; i++)
        code[p++] = (uint8_t)(target >> (i * 8));

    /* jmp rax (2 bytes) */
    code[p++] = 0xFF;
    code[p++] = 0xE0;

#else
    /*
     * OsitoK bare-metal path: PE32 code runs in 32-bit compat mode.
     * The thunk is 32-bit code that does INT 0x2E to enter the kernel.
     * EAX = thunk index, ECX = arg count.
     */

    /* MOV EAX, <thunk_index> */
    code[p++] = 0xB8;
    uint32_t idx = thunk_count;
    code[p++] = (uint8_t)(idx);
    code[p++] = (uint8_t)(idx >> 8);
    code[p++] = (uint8_t)(idx >> 16);
    code[p++] = (uint8_t)(idx >> 24);

    /* MOV ECX, <num_args> */
    code[p++] = 0xB9;
    code[p++] = num_args;
    code[p++] = 0;
    code[p++] = 0;
    code[p++] = 0;

    /* INT 0x2E */
    code[p++] = 0xCD;
    code[p++] = 0x2E;

    /*
     * Calling convention determines stack cleanup:
     *   stdcall (Win32 API): callee cleans → RET N  (C2 xx xx)
     *   cdecl   (MSVCRT):    caller cleans → RET    (C3)
     *
     * CRITICAL: Using RET N for cdecl functions causes double cleanup —
     * the thunk pops N bytes, then the caller also does add esp,N or pop.
     * This corrupts the stack and causes wild jumps after a few calls.
     */
    if (callconv == CC_CDECL || num_args == 0) {
        code[p++] = 0xC3;  /* RET — caller will clean up args */
    } else {
        code[p++] = 0xC2;  /* RET imm16 — callee cleans (stdcall) */
        uint16_t cleanup = (uint16_t)(num_args * 4);
        code[p++] = (uint8_t)(cleanup);
        code[p++] = (uint8_t)(cleanup >> 8);
    }
#endif

    /* Fill remainder with INT3 (debug trap) */
    while (p < THUNK_STUB_SIZE)
        code[p++] = 0xCC;
}

/* ── Public API ──────────────────────────────────────────────── */

void compat32_init(void)
{
    thunk_count = 0;

    /* Allocate executable thunk pool in low memory */
    thunk_pool = (uint8_t *)mem_alloc_pages(THUNK_POOL_PAGES);
    if (!thunk_pool) {
        serial_puts("[COMPAT32] Failed to allocate thunk pool!\n");
        return;
    }

    /* Fill with INT3 */
    for (uint32_t i = 0; i < THUNK_POOL_PAGES * 4096; i++)
        thunk_pool[i] = 0xCC;

    serial_puts("[COMPAT32] Thunk pool at 0x");
    serial_puthex((uint64_t)(ULONG_PTR)thunk_pool, 16);
    serial_puts(" (");
    serial_putdec(THUNKS_PER_PAGE * THUNK_POOL_PAGES);
    serial_puts(" slots)\n");

#ifdef TEST_HARNESS
    /* On Linux test harness, make thunk pool executable */
    #include <sys/mman.h>
    mprotect(thunk_pool, THUNK_POOL_PAGES * 4096,
             PROT_READ | PROT_WRITE | PROT_EXEC);
#endif

#ifndef TEST_HARNESS
    /*
     * Install a "callback return stub" at the END of the thunk pool.
     * This is a 32-bit code snippet that a compat-mode function RETs to.
     * It saves EAX (function return value) to a known address, then
     * does INT 0x2E with a magic index to signal "callback complete".
     *
     * Code (18 bytes):
     *   A3 xx xx xx xx       MOV [callback_retval], EAX  ; save return value
     *   B8 FE FF FF FF       MOV EAX, 0xFFFFFFFE  (THUNK_CALLBACK_RETURN)
     *   B9 00 00 00 00       MOV ECX, 0
     *   CD 2E                INT 0x2E
     *   F4                   HLT  (should never reach here)
     */
    {
        uint8_t *stub = thunk_pool + (THUNK_POOL_PAGES * 4096) - 24;
        uint32_t retval_addr = (uint32_t)(ULONG_PTR)&callback_retval;
        int p = 0;
        stub[p++] = 0xA3;  /* MOV [moffs32], EAX */
        stub[p++] = (uint8_t)(retval_addr);
        stub[p++] = (uint8_t)(retval_addr >> 8);
        stub[p++] = (uint8_t)(retval_addr >> 16);
        stub[p++] = (uint8_t)(retval_addr >> 24);
        stub[p++] = 0xB8;  /* MOV EAX, imm32 */
        stub[p++] = 0xFE; stub[p++] = 0xFF; stub[p++] = 0xFF; stub[p++] = 0xFF;
        stub[p++] = 0xB9;  /* MOV ECX, 0 */
        stub[p++] = 0x00; stub[p++] = 0x00; stub[p++] = 0x00; stub[p++] = 0x00;
        stub[p++] = 0xCD; stub[p++] = 0x2E;  /* INT 0x2E */
        stub[p++] = 0xF4;  /* HLT */
        callback_return_stub_addr = (uint32_t)(ULONG_PTR)stub;
        serial_puts("[COMPAT32] Callback return stub at 0x");
        serial_puthex(callback_return_stub_addr, 8);
        serial_puts("\n");
    }
#endif
}

uint32_t compat32_make_thunk(uint64_t target, const char *name, uint8_t num_args)
{
    /* Default to stdcall for backwards compatibility */
    return compat32_make_thunk_ex(target, name, num_args, CC_STDCALL);
}

uint32_t compat32_make_thunk_ex(uint64_t target, const char *name,
                                 uint8_t num_args, uint8_t callconv)
{
    if (!thunk_pool) return 0;
    if (thunk_count >= COMPAT32_MAX_THUNKS) {
        serial_puts("[COMPAT32] Thunk table full!\n");
        return 0;
    }

    uint32_t idx = thunk_count;
    uint8_t *stub = thunk_pool + (idx * THUNK_STUB_SIZE);

    /* Generate thunk code */
    emit_thunk(stub, target, num_args, callconv);

    /* Record in table */
    thunk_table[idx].thunk_addr  = (uint32_t)(ULONG_PTR)stub;
    thunk_table[idx].target_addr = target;
    thunk_table[idx].num_args    = num_args;
    thunk_table[idx].callconv    = callconv;
    thunk_table[idx].name        = name;

    thunk_count++;

    return (uint32_t)(ULONG_PTR)stub;
}

/*
 * Guess the number of stack arguments for common Win32 functions.
 * This is a heuristic — most Win32 API functions use stdcall (callee cleans).
 * For unknown functions, we use 0 (the INT 0x2E handler can read from stack).
 */
static uint8_t guess_num_args(const char *name)
{
    if (!name) return 0;

    /* Common functions with known arg counts */
    struct { const char *n; uint8_t args; } known[] = {
        /* kernel32 */
        { "GetModuleHandleA",      1 }, { "GetModuleHandleW",      1 },
        { "GetProcAddress",        2 }, { "LoadLibraryA",          1 },
        { "LoadLibraryW",          1 }, { "FreeLibrary",           1 },
        { "GetLastError",          0 }, { "SetLastError",          1 },
        { "ExitProcess",           1 }, { "GetCurrentProcess",     0 },
        { "GetCurrentThread",      0 }, { "GetCurrentThreadId",    0 },
        { "GetCurrentProcessId",   0 }, { "CloseHandle",           1 },
        { "GetCommandLineA",       0 }, { "GetCommandLineW",       0 },
        { "GetTickCount",          0 }, { "Sleep",                 1 },
        { "CreateFileA",           7 }, { "CreateFileW",           7 },
        { "ReadFile",              5 }, { "WriteFile",             5 },
        { "SetFilePointer",        4 }, { "GetFileSize",           2 },
        { "CreateEventA",          4 }, { "CreateEventW",          4 },
        { "SetEvent",              1 }, { "ResetEvent",            1 },
        { "WaitForSingleObject",   2 }, { "WaitForMultipleObjects",4 },
        { "CreateMutexA",          3 }, { "ReleaseMutex",          1 },
        { "InitializeCriticalSection",      1 },
        { "EnterCriticalSection",           1 },
        { "LeaveCriticalSection",           1 },
        { "DeleteCriticalSection",          1 },
        { "TlsAlloc",             0 }, { "TlsGetValue",           1 },
        { "TlsSetValue",          2 }, { "TlsFree",               1 },
        { "VirtualAlloc",         4 }, { "VirtualFree",           3 },
        { "HeapAlloc",            3 }, { "HeapFree",              3 },
        { "HeapCreate",           3 }, { "GetProcessHeap",        0 },
        { "GetSystemInfo",        1 }, { "GetVersionExA",         1 },
        { "QueryPerformanceCounter",   1 },
        { "QueryPerformanceFrequency", 1 },
        { "GetStartupInfoA",      1 }, { "GetStartupInfoW",       1 },
        { "GetEnvironmentVariableA", 3 },
        { "GetSystemDirectoryA",  2 }, { "GetWindowsDirectoryA",  2 },
        { "FindFirstFileA",       2 }, { "FindNextFileA",         2 },
        { "FindClose",            1 }, { "DeleteFileA",           1 },
        { "CreateDirectoryA",     2 }, { "SetCurrentDirectoryA",  1 },
        { "GetCurrentDirectoryA", 2 }, { "GetFullPathNameA",      4 },
        { "OutputDebugStringA",   1 },
        { "MultiByteToWideChar",  6 }, { "WideCharToMultiByte",  8 },
        { "GetModuleFileNameA",   3 }, { "GetModuleFileNameW",   3 },
        { "GetStdHandle",         1 }, { "SetStdHandle",         2 },
        { "GetFileType",          1 }, { "SetHandleCount",       1 },
        { "GetEnvironmentStringsW", 0 }, { "FreeEnvironmentStringsW", 1 },
        { "GetACP",               0 }, { "GetOEMCP",             0 },
        { "GetCPInfo",            2 }, { "IsValidCodePage",      1 },
        { "GetStringTypeW",       5 }, { "LCMapStringW",         6 },
        { "GetLocaleInfoA",       4 }, { "GetLocaleInfoW",       4 },
        { "GetUserDefaultLCID",   0 }, { "IsDBCSLeadByte",       1 },
        { "FlushFileBuffers",     1 }, { "SetEndOfFile",         1 },
        { "GetConsoleMode",       2 }, { "SetConsoleMode",       2 },
        { "WriteConsoleA",        5 }, { "WriteConsoleW",        5 },
        { "SetUnhandledExceptionFilter", 1 },
        { "UnhandledExceptionFilter",    1 },
        { "IsBadReadPtr",         2 }, { "IsBadWritePtr",        2 },
        { "IsBadCodePtr",         1 },
        { "HeapReAlloc",          4 }, { "HeapSize",             3 },
        { "RtlUnwind",            4 },
        { "CreateThread",         6 }, { "ExitThread",           1 },
        { "ResumeThread",         1 }, { "SuspendThread",        1 },
        { "SetThreadPriority",    2 }, { "GetThreadPriority",    1 },
        { "GetExitCodeThread",    2 }, { "TerminateThread",      2 },
        { "GetPrivateProfileStringA", 6 },
        { "GetPrivateProfileIntA",    4 },
        { "WritePrivateProfileStringA", 4 },
        { "GlobalAlloc",          2 }, { "GlobalFree",           1 },
        { "GlobalLock",           1 }, { "GlobalUnlock",         1 },
        { "LocalAlloc",           2 }, { "LocalFree",            1 },
        { "GetModuleHandleExA",   3 },
        { "IsProcessorFeaturePresent", 1 },
        { "GetTimeZoneInformation",    1 },
        { "FormatMessageA",       7 }, { "FormatMessageW",        7 },
        { "CompareStringA",       6 }, { "CompareStringW",        6 },
        { "GetDiskFreeSpaceA",    5 }, { "GetVolumeInformationA", 8 },
        { "GetTempPathA",         2 }, { "GetTempFileNameA",      4 },
        { "MoveFileA",            2 }, { "CopyFileA",             3 },
        { "GetFileAttributesA",   1 }, { "SetFileAttributesA",    2 },
        { "GetPrivateProfileSectionNamesA", 3 },
        { "GetComputerNameA",     2 }, { "GetComputerNameW",     2 },
        { "GetVersionExA",        1 }, { "GetVersionExW",        1 },
        { "FormatMessageA",       7 }, { "FormatMessageW",       7 },
        { "GetUserNameA",         2 }, { "GetUserNameW",         2 },
        /* W variants of existing A-only entries */
        { "GetEnvironmentVariableW", 3 },
        { "GetSystemDirectoryW",  2 }, { "GetWindowsDirectoryW",  2 },
        { "FindFirstFileW",       2 }, { "FindNextFileW",         2 },
        { "DeleteFileW",          1 },
        { "CreateDirectoryW",     2 }, { "SetCurrentDirectoryW",  1 },
        { "GetCurrentDirectoryW", 2 }, { "GetFullPathNameW",      4 },
        { "OutputDebugStringW",   1 },
        { "GetDiskFreeSpaceW",    5 }, { "GetVolumeInformationW", 8 },
        { "GetTempPathW",         2 }, { "GetTempFileNameW",      4 },
        { "MoveFileW",            2 }, { "CopyFileW",             3 },
        { "GetFileAttributesW",   1 }, { "SetFileAttributesW",    2 },
        { "GetPrivateProfileStringW", 6 },
        { "GetPrivateProfileIntW",    4 },
        { "WritePrivateProfileStringW", 4 },
        { "GetPrivateProfileSectionNamesW", 3 },
        { "RegisterClassW",       1 }, { "RegisterClassExW",      1 },
        { "CreateWindowExW",     12 },
        { "GetMessageW",          4 }, { "PeekMessageW",          5 },
        { "DispatchMessageW",     1 }, { "DefWindowProcW",        4 },
        { "SendMessageW",         4 }, { "PostMessageW",          4 },
        { "SetWindowTextW",       2 },
        { "LoadCursorW",          2 }, { "LoadIconW",             2 },
        { "MapVirtualKeyW",       2 }, { "MessageBoxW",           4 },
        { "GetObjectW",           3 },
        /* MSVCRT — critical: _initterm with wrong args crashes! */
        { "_initterm",            2 }, { "_initterm_e",           2 },
        { "__dllonexit",          3 }, { "_onexit",               1 },
        { "_atexit",              1 }, { "atexit",                1 },
        { "_controlfp",           2 }, { "__set_app_type",        1 },
        { "__p__fmode",           0 }, { "__p__commode",          0 },
        { "_adjust_fdiv",         0 }, { "__setusermatherr",      1 },
        { "_except_handler3",     4 }, { "_except_handler4",      4 },
        { "RegOpenKeyExW",        5 }, { "RegQueryValueExW",      6 },
        { "RegSetValueExW",       6 }, { "RegCreateKeyExW",       9 },
        /* Completely missing functions */
        { "GetLogicalDrives",     0 },
        { "GetDriveTypeW",        1 }, { "GetDriveTypeA",         1 },
        { "CreateSemaphoreW",     4 }, { "CreateSemaphoreA",      4 },
        { "CreateMutexW",         3 },
        { "OpenEventW",           3 }, { "OpenEventA",            3 },
        { "LockFile",             5 }, { "UnlockFile",            5 },
        { "GetShortPathNameA",    3 }, { "GetShortPathNameW",     3 },
        { "SearchPathA",          6 }, { "SearchPathW",           6 },
        { "GetLocalTime",         1 },
        { "GetForegroundWindow",  0 }, { "SetForegroundWindow",   1 },

        /* user32 */
        { "RegisterClassA",       1 }, { "RegisterClassExA",      1 },
        { "CreateWindowExA",     12 }, { "DestroyWindow",         1 },
        { "ShowWindow",           2 }, { "UpdateWindow",          1 },
        { "GetMessageA",          4 }, { "PeekMessageA",          5 },
        { "TranslateMessage",     1 }, { "DispatchMessageA",      1 },
        { "PostQuitMessage",      1 }, { "DefWindowProcA",        4 },
        { "SendMessageA",         4 }, { "PostMessageA",          4 },
        { "SetWindowTextA",       2 }, { "GetClientRect",         2 },
        { "GetWindowRect",        2 }, { "AdjustWindowRect",      3 },
        { "SetCursor",            1 }, { "ShowCursor",            1 },
        { "LoadCursorA",          2 }, { "LoadIconA",             2 },
        { "GetDC",                1 }, { "ReleaseDC",             2 },
        { "GetFocus",             0 }, { "SetFocus",              1 },
        { "GetActiveWindow",      0 }, { "SetActiveWindow",       1 },
        { "GetDesktopWindow",     0 }, { "GetSystemMetrics",      1 },
        { "MoveWindow",           6 }, { "SetWindowPos",          7 },
        { "MessageBoxA",          4 }, { "GetAsyncKeyState",      1 },
        { "GetKeyState",          1 }, { "MapVirtualKeyA",        2 },
        { "SetTimer",             4 }, { "KillTimer",             2 },
        { "ClipCursor",           1 }, { "SetCursorPos",          2 },
        { "GetCursorPos",         1 }, { "ScreenToClient",        2 },
        { "ClientToScreen",       2 },

        /* gdi32 */
        { "GetDeviceCaps",        2 }, { "CreateCompatibleDC",    1 },
        { "DeleteDC",             1 }, { "SelectObject",          2 },
        { "GetObjectA",           3 }, { "DeleteObject",          1 },
        { "ChoosePixelFormat",    2 }, { "SetPixelFormat",        3 },

        /* advapi32 */
        { "RegOpenKeyExA",        5 }, { "RegCloseKey",           1 },
        { "RegQueryValueExA",     6 }, { "RegSetValueExA",        6 },
        { "RegCreateKeyExA",      9 },

        /* msvcrt */
        { "malloc",               1 }, { "free",                  1 },
        { "calloc",               2 }, { "realloc",               2 },
        { "memcpy",               3 }, { "memset",                3 },
        { "memmove",              3 }, { "memcmp",                3 },
        { "strlen",               1 }, { "strcpy",                2 },
        { "strncpy",              3 }, { "strcmp",                 2 },
        { "strncmp",              3 }, { "strcat",                2 },
        { "strchr",               2 }, { "strrchr",               2 },
        { "strstr",               2 },
        /* Variadic printf: nargs=12 to capture all possible args from
         * the 32-bit stack. ms_va_start/ms_va_arg on the zero-extended
         * args works correctly for int and pointer types. */
        { "sprintf",             12 }, { "_snprintf",             12 },
        { "printf",              12 }, { "fprintf",               12 },
        { "sscanf",              12 },
        /* v*printf: va_list is a 32-bit pointer (1 arg). The 64-bit
         * shim walks it with uint32_t* via do_vformat32. */
        { "vprintf",              2 }, { "vsprintf",               3 },
        { "_vsnprintf",           4 }, { "vfprintf",               3 },
        { "_vsnwprintf",          4 },
        { "atoi",                  1 },
        { "atof",                 1 }, { "strtol",                3 },
        { "strtod",               2 }, { "abs",                   1 },
        { "fopen",                2 }, { "_wfopen",               2 },
        { "_access",              2 }, { "_waccess",              2 },
        { "_stat",                2 }, { "_wstat",                2 },
        { "fclose",                1 },
        { "fread",                4 }, { "fwrite",                4 },
        { "fseek",                3 }, { "ftell",                 1 },
        { "fgets",                3 }, { "fputs",                 2 },
        { "exit",                 1 }, { "_exit",                 1 },
        { "time",                 1 }, { "clock",                 0 },
        { "srand",                1 }, { "rand",                  0 },
        { "_beginthreadex",       6 }, { "_endthreadex",          1 },
        { "_CxxThrowException",   2 }, { "__CxxFrameHandler",     4 },
        { "__CxxFrameHandler3",   4 }, { "__CxxFrameHandler4",    4 },
        { "_except_handler3",     4 }, { "_except_handler4",      4 },
        { "RaiseException",       4 }, { "_XcptFilter",           2 },
        { "_purecall",            0 }, { "abort",                 0 },
        { "_amsg_exit",           1 },

        /* ddraw */
        { "DirectDrawCreate",     3 }, { "DirectDrawCreateEx",    4 },

        /* dsound */
        { "DirectSoundCreate",    3 },

        /* wsock32 */
        { "WSAStartup",           2 }, { "WSACleanup",            0 },

        { NULL, 0 }
    };

    for (int i = 0; known[i].n; i++) {
        /* Simple case-insensitive compare */
        const char *a = name;
        const char *b = known[i].n;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca += 32;
            if (cb >= 'A' && cb <= 'Z') cb += 32;
            if (ca != cb) break;
            a++; b++;
        }
        if (*a == '\0' && *b == '\0')
            return known[i].args;
    }

    /* Default: assume 4 args (common for many Win32 APIs) */
    return 4;
}

/*
 * Determine calling convention from DLL name.
 * Win32 API DLLs use stdcall (callee cleanup).
 * MSVCRT and C runtime DLLs use cdecl (caller cleanup).
 */
static uint8_t dll_calling_convention(const char *dll_name)
{
    if (!dll_name) return CC_STDCALL;

    /* Case-insensitive prefix check for MSVCRT variants */
    const char *d = dll_name;
    char low[16];
    int i;
    for (i = 0; i < 15 && d[i]; i++) {
        char c = d[i];
        low[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
    }
    low[i] = '\0';

    /* MSVCRT, MSVCR70, MSVCR71, MSVCR80, MSVCR90, MSVCR100, MSVCR110, MSVCR120, MSVCR140 */
    if (low[0]=='m' && low[1]=='s' && low[2]=='v' && low[3]=='c')
        return CC_CDECL;

    /* ucrtbase.dll (Universal CRT) */
    if (low[0]=='u' && low[1]=='c' && low[2]=='r' && low[3]=='t')
        return CC_CDECL;

    return CC_STDCALL;
}

/*
 * Check if a DLL name is one of our built-in shims (64-bit kernel code).
 * Imports from shim DLLs need INT 0x2E thunks.
 * Imports from real PE32 DLLs (e.g. Core.dll, Engine.dll) are direct 32-bit calls.
 */
static int is_shim_dll(const char *dll_name)
{
    if (!dll_name) return 0;

    /* Lowercase first 16 chars */
    char low[16];
    int i;
    for (i = 0; i < 15 && dll_name[i]; i++) {
        char c = dll_name[i];
        low[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
    }
    low[i] = '\0';

    /* Known shim DLLs (our 64-bit implementations) */
    static const char *shim_names[] = {
        "kernel32", "ntdll", "msvcrt", "msvcr",
        "user32", "gdi32", "advapi32",
        "ddraw", "dsound", "ole32", "oleaut32",
        "shell32", "comctl32", "comdlg32",
        "winmm", "wsock32", "ws2_32",
        "ucrtbase", "vcruntime",
        NULL
    };

    /* Strip .dll extension from low */
    int len = i;
    if (len > 4 && low[len-4]=='.' && low[len-3]=='d' &&
        low[len-2]=='l' && low[len-1]=='l')
        low[len-4] = '\0';

    for (int j = 0; shim_names[j]; j++) {
        const char *a = low, *b = shim_names[j];
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0') return 1;
        /* Also check prefix match for msvcr* */
        if (shim_names[j][0]=='m' && shim_names[j][4]=='r' &&
            shim_names[j][5]=='\0') {
            /* "msvcr" prefix — check if low starts with it */
            if (low[0]=='m' && low[1]=='s' && low[2]=='v' &&
                low[3]=='c' && low[4]=='r')
                return 1;
        }
    }
    return 0;
}

/*
 * Patch the IAT of a PE32 image to use thunk stubs.
 * Walks the import directory, finds each resolved entry, and replaces
 * the truncated 64-bit pointer with a proper 32-bit thunk address.
 */
NTSTATUS compat32_patch_iat(PE_IMAGE_INFO *info)
{
    if (!info || !info->Is32Bit || !info->ImageBase)
        return STATUS_INVALID_PARAMETER;

    if (!thunk_pool) {
        serial_puts("[COMPAT32] Thunk pool not initialized\n");
        return STATUS_UNSUCCESSFUL;
    }

    BYTE *base = (BYTE *)info->ImageBase;

    /* Parse NT headers to find import directory */
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return STATUS_UNSUCCESSFUL;

    IMAGE_NT_HEADERS32 *nt32 = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    if (nt32->Signature != IMAGE_NT_SIGNATURE)
        return STATUS_UNSUCCESSFUL;

    if (nt32->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT)
        return STATUS_SUCCESS;  /* no imports */

    IMAGE_DATA_DIRECTORY *imp_dir =
        &nt32->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

    if (imp_dir->VirtualAddress == 0 || imp_dir->Size == 0)
        return STATUS_SUCCESS;

    /* IAT patch trace removed — debug serial_puts changes code layout
     * and can introduce 0xCC displacement bytes that QEMU TCG misinterprets */

    PIMAGE_IMPORT_DESCRIPTOR desc =
        (PIMAGE_IMPORT_DESCRIPTOR)(base + imp_dir->VirtualAddress);

    uint32_t patched = 0;

    uint32_t direct = 0;  /* imports from real PE32 DLLs (no thunk) */

    for (; desc->Name != 0; desc++) {
        const char *dll_name = (const char *)(base + desc->Name);
        uint8_t cc = dll_calling_convention(dll_name);
        int shim = is_shim_dll(dll_name);

        PIMAGE_THUNK_DATA32 int_entry = (PIMAGE_THUNK_DATA32)(
            base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk
                                             : desc->FirstThunk));
        PIMAGE_THUNK_DATA32 iat_entry =
            (PIMAGE_THUNK_DATA32)(base + desc->FirstThunk);

        for (; int_entry->u1.AddressOfData != 0; int_entry++, iat_entry++) {
            /* Get the function name for arg count lookup */
            const char *func_name = NULL;
            if (!IMAGE_SNAP_BY_ORDINAL32(int_entry->u1.Ordinal)) {
                PIMAGE_IMPORT_BY_NAME name_entry =
                    (PIMAGE_IMPORT_BY_NAME)(base + int_entry->u1.AddressOfData);
                func_name = name_entry->Name;
            }

            /* Resolve the import */
            PVOID resolved = NULL;
            if (func_name) {
                USHORT hint = 0;
                if (!IMAGE_SNAP_BY_ORDINAL32(int_entry->u1.Ordinal)) {
                    PIMAGE_IMPORT_BY_NAME n =
                        (PIMAGE_IMPORT_BY_NAME)(base + int_entry->u1.AddressOfData);
                    hint = n->Hint;
                }
                resolved = dll_resolve_import(dll_name, func_name, hint, FALSE);
            } else {
                USHORT ordinal = (USHORT)IMAGE_ORDINAL32(int_entry->u1.Ordinal);
                resolved = dll_resolve_import(dll_name, NULL, ordinal, TRUE);
            }

            if (!resolved) continue;

            if (shim) {
                /*
                 * Import from a shim DLL (64-bit kernel code).
                 * Create an INT 0x2E thunk to bridge 32-bit → 64-bit.
                 */
                uint64_t target64 = (uint64_t)(ULONG_PTR)resolved;
                uint8_t nargs = func_name ? guess_num_args(func_name) : 4;
                uint32_t thunk_addr = compat32_make_thunk_ex(target64, func_name, nargs, cc);
                if (thunk_addr) {
                    iat_entry->u1.Function = thunk_addr;
                    patched++;
                }
            } else {
                /*
                 * Import from a real PE32 DLL (32-bit code in same compat mode).
                 * Write the address directly — no thunk needed.
                 */
                iat_entry->u1.Function = (ULONG)(ULONG_PTR)resolved;
                direct++;

                /* Capture key data import addresses for diagnostics */
                if (func_name) {
                    /* Check for "Names@FName" substring */
                    for (const char *p = func_name; *p; p++) {
                        if (p[0]=='N' && p[1]=='a' && p[2]=='m' && p[3]=='e' &&
                            p[4]=='s' && p[5]=='@' && p[6]=='F') {
                            g_fname_names_addr = (uint32_t)(ULONG_PTR)resolved;
                            serial_puts("[DIAG] FName::Names resolved at 0x");
                            serial_puthex((uint64_t)g_fname_names_addr, 8);
                            serial_puts(" IAT@0x");
                            serial_puthex((uint64_t)(ULONG_PTR)&iat_entry->u1.Function, 8);
                            serial_puts("\n");
                            break;
                        }
                    }
                    /* Check for "GMalloc" substring */
                    for (const char *p = func_name; *p; p++) {
                        if (p[0]=='G' && p[1]=='M' && p[2]=='a' && p[3]=='l' &&
                            p[4]=='l' && p[5]=='o' && p[6]=='c') {
                            g_gmalloc_addr = (uint32_t)(ULONG_PTR)resolved;
                            serial_puts("[DIAG] GMalloc resolved at 0x");
                            serial_puthex((uint64_t)g_gmalloc_addr, 8);
                            serial_puts("\n");
                            break;
                        }
                    }
                }
            }
        }
    }

    serial_puts("[COMPAT32] IAT: ");
    serial_putdec(patched);
    serial_puts(" thunked (shim), ");
    serial_putdec(direct);
    serial_puts(" direct (PE32 DLL)\n");

    return STATUS_SUCCESS;
}

/* ── TEB setup for 32-bit code ───────────────────────────────── */

void compat32_setup_teb(void *teb_addr)
{
    /*
     * Windows i386 uses FS:0 → TEB.
     * Set FS base to point to our TEB structure.
     * On x86-64, FS base is set via MSR 0xC0000100.
     */
#ifndef TEST_HARNESS
    uint64_t addr = (uint64_t)(ULONG_PTR)teb_addr;
    __asm__ volatile (
        "mov $0xC0000100, %%ecx\n"   /* MSR_FS_BASE */
        "mov %0, %%rax\n"
        "mov %0, %%rdx\n"
        "shr $32, %%rdx\n"
        "wrmsr\n"
        :
        : "r"(addr)
        : "rax", "rcx", "rdx"
    );
#else
    /* On Linux, use arch_prctl to set FS base for 32-bit TEB access */
    {
        #include <asm/prctl.h>
        extern int arch_prctl(int code, unsigned long addr);
        arch_prctl(ARCH_SET_FS, (unsigned long)teb_addr);
    }
#endif

    serial_puts("[COMPAT32] FS base set to 0x");
    serial_puthex((uint64_t)(ULONG_PTR)teb_addr, 16);
    serial_puts(" (TEB for 32-bit code)\n");
}

/* ── Enter 32-bit compatibility mode ─────────────────────────── */

void compat32_enter(uint32_t entry, uint32_t stack_top)
{
#ifndef TEST_HARNESS
    /*
     * On bare metal, switch to 32-bit compat mode via RETF.
     *
     * Push the 32-bit code selector and entry point onto the stack,
     * then execute RETF to load CS with the 32-bit segment and
     * EIP with the entry point.
     *
     * Before RETF, set ESP to the PE32's stack.
     */
    serial_puts("[COMPAT32] Entering 32-bit mode at 0x");
    serial_puthex(entry, 8);
    serial_puts("\n");

    /* Switch to Win32 page table (VirtualAlloc VAs only visible here) */
    {
        extern uint64_t paging_get_win32_cr3(void);
        uint64_t w32cr3 = paging_get_win32_cr3();
        if (w32cr3) {
            __asm__ volatile ("mov %0, %%cr3" : : "r"(w32cr3) : "memory");
        }
    }

    /* Set data segments to 32-bit data selector, then RETF to compat mode.
     * Hardcode 0x48 (GDT_SEL_DATA32) because GAS doesn't like C macros
     * in mov-to-segment operands with PIE. */
    uint64_t cs64 = GDT_SEL_CODE32;
    uint64_t ip64 = entry;
    uint64_t sp64 = stack_top;
    __asm__ volatile (
        "movw $0x48, %%ax\n"    /* GDT_SEL_DATA32 = 0x48 */
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%ss\n"
        "mov %[sp], %%rsp\n"
        "push %[cs]\n"
        "push %[ip]\n"
        "lretq\n"
        :
        : [cs] "r"(cs64),
          [ip] "r"(ip64),
          [sp] "r"(sp64)
        : "memory", "rax"
    );
#else
    /*
     * Test harness: PE32 code is loaded in low memory but runs as
     * 64-bit code. Just call the entry point directly.
     */
    serial_puts("[COMPAT32] Calling 32-bit entry at 0x");
    serial_puthex(entry, 8);
    serial_puts(" (test harness, running as 64-bit)\n");

    typedef void (*entry_fn)(void);
    entry_fn fn = (entry_fn)(ULONG_PTR)entry;
    fn();
#endif
}

/* ── Callback: call a 32-bit function from 64-bit code ─────── */

/*
 * Call a 32-bit function pointer from 64-bit kernel code.
 * Used by _initterm to execute CRT initializers / C++ constructors.
 *
 * Mechanism:
 *   1. kern_setjmp saves 64-bit state
 *   2. LRETQ switches to compat mode at func_addr
 *   3. 32-bit function executes and RETs
 *   4. RET lands on callback_return_stub (pushed as return addr)
 *   5. Stub does INT 0x2E with magic THUNK_CALLBACK_RETURN index
 *   6. compat32_dispatch sees magic, restores segments, longjmps back
 *   7. kern_setjmp returns 1, we continue
 */
void compat32_callback(uint32_t func_addr)
{
#ifndef TEST_HARNESS
    if (!callback_return_stub_addr) return;

    /* Clamp depth — callbacks that don't return via the stub leak depth.
     * UT99's message loop (PeekMessage/DispatchMessage/WndProc) does this.
     * Always use slot 0 when overflowed — safe because the old callbacks
     * are already gone (their stack frames were unwound by the game loop). */
    if (callback_depth >= MAX_CALLBACK_DEPTH)
        callback_depth = 1;  /* Reserve slot 0 for overflow reuse */

    int depth = callback_depth++;

    serial_puts("[CB32] depth=");
    serial_putdec(depth);
    serial_puts(" calling 0x");
    serial_puthex(func_addr, 8);
    serial_puts("\n");

    /* Save IST1 before callback — longjmp bypasses int2e_stub's restore */
    {
        extern uint64_t *tss_ist1_ptr;
        if (tss_ist1_ptr) callback_saved_ist1[depth] = *tss_ist1_ptr;
    }

    if (kern_setjmp(callback_jmpbufs[depth]) == 0) {
        /*
         * First return from setjmp — switch to compat mode.
         * Set up a small stack with the return stub as return address,
         * then LRETQ to the 32-bit function.
         */
        uint32_t *sp = (uint32_t *)(callback_stacks[depth] + CALLBACK_STACK_SIZE);
        sp--;
        *sp = callback_return_stub_addr;  /* return address for the function */

        uint64_t cs64 = GDT_SEL_CODE32;
        uint64_t ip64 = func_addr;
        uint64_t sp64 = (uint64_t)(ULONG_PTR)sp;

        __asm__ volatile (
            "movw $0x48, %%ax\n"    /* GDT_SEL_DATA32 */
            "mov %%ax, %%ds\n"
            "mov %%ax, %%es\n"
            "mov %%ax, %%ss\n"
            "mov %[sp], %%rsp\n"
            "push %[cs]\n"
            "push %[ip]\n"
            "lretq\n"
            :
            : [cs] "r"(cs64),
              [ip] "r"(ip64),
              [sp] "r"(sp64)
            : "memory", "rax"
        );
        /* never reached — control flows via longjmp */
    }

    /* longjmp returned here — 32-bit function is done. */
    callback_depth--;
    serial_puts("[CB32] depth=");
    serial_putdec(depth);
    serial_puts(" returned\n");
#else
    /* Test harness: call directly */
    typedef void (*void_fn)(void);
    ((void_fn)(ULONG_PTR)func_addr)();
#endif
}

/*
 * Call a 32-bit function with arguments, returning EAX.
 * nargs: number of uint32_t arguments (0-8)
 * args:  array of uint32_t arguments (pushed right-to-left)
 * Returns: EAX from the 32-bit function.
 */
uint32_t compat32_callback_args(uint32_t func_addr, int nargs, const uint32_t *args)
{
#ifndef TEST_HARNESS
    if (!callback_return_stub_addr) return 0;

    if (callback_depth >= MAX_CALLBACK_DEPTH)
        callback_depth = 1;

    int depth = callback_depth++;

    callback_retval = 0;

    /* Save IST1 before callback — longjmp bypasses int2e_stub's restore */
    {
        extern uint64_t *tss_ist1_ptr;
        if (tss_ist1_ptr) callback_saved_ist1[depth] = *tss_ist1_ptr;
    }

    if (kern_setjmp(callback_jmpbufs[depth]) == 0) {
        uint32_t *sp = (uint32_t *)(callback_stacks[depth] + CALLBACK_STACK_SIZE);

        /* Push arguments right-to-left (cdecl/stdcall convention) */
        for (int i = nargs - 1; i >= 0; i--) {
            sp--;
            *sp = args[i];
        }

        /* Push return stub as return address */
        sp--;
        *sp = callback_return_stub_addr;

        uint64_t cs64 = GDT_SEL_CODE32;
        uint64_t ip64 = func_addr;
        uint64_t sp64 = (uint64_t)(ULONG_PTR)sp;

        __asm__ volatile (
            "movw $0x48, %%ax\n"    /* GDT_SEL_DATA32 */
            "mov %%ax, %%ds\n"
            "mov %%ax, %%es\n"
            "mov %%ax, %%ss\n"
            "mov %[sp], %%rsp\n"
            "push %[cs]\n"
            "push %[ip]\n"
            "lretq\n"
            :
            : [cs] "r"(cs64),
              [ip] "r"(ip64),
              [sp] "r"(sp64)
            : "memory", "rax"
        );
        /* never reached */
    }

    /* longjmp returned — 32-bit function is done. */
    callback_depth--;
    return callback_retval;
#else
    /* Test harness: call directly */
    typedef uint32_t (*fn0)(void);
    typedef uint32_t (*fn1)(uint32_t);
    typedef uint32_t (*fn2)(uint32_t, uint32_t);
    typedef uint32_t (*fn3)(uint32_t, uint32_t, uint32_t);
    typedef uint32_t (*fn4)(uint32_t, uint32_t, uint32_t, uint32_t);
    uint64_t f = (uint64_t)(ULONG_PTR)func_addr;
    switch (nargs) {
    case 0:  return ((fn0)f)();
    case 1:  return ((fn1)f)(args[0]);
    case 2:  return ((fn2)f)(args[0], args[1]);
    case 3:  return ((fn3)f)(args[0], args[1], args[2]);
    default: return ((fn4)f)(args[0], args[1], args[2], args[3]);
    }
#endif
}

/*
 * Look up a thunk entry by its 32-bit stub address.
 * Returns the thunk index, or -1 if not found.
 */
int32_t compat32_find_thunk(uint32_t addr)
{
    for (uint32_t i = 0; i < thunk_count; i++) {
        if (thunk_table[i].thunk_addr == addr)
            return (int32_t)i;
    }
    return -1;
}

/*
 * Get the name of a thunk by index.
 */
const char *compat32_get_name(uint32_t thunk_idx)
{
    if (thunk_idx >= thunk_count) return NULL;
    return thunk_table[thunk_idx].name;
}

/* ── 32-bit SEH exception dispatch ────────────────────────────── */

/*
 * Walk the 32-bit SEH chain and dispatch an exception.
 *
 * PE32 (i386) code maintains the SEH chain via FS:[0] (= TEB.ExceptionList).
 * The chain uses 32-bit structs:
 *   EXCEPTION_REGISTRATION_RECORD32: { uint32_t Next; uint32_t Handler; }
 *   EH3_EXCEPTION_REGISTRATION32:    { Next(4), Handler(4), ScopeTable(4), TryLevel(4) }
 *   SCOPETABLE_ENTRY32:              { EnclosingLevel(4), FilterFunc(4), HandlerFunc(4) }
 *
 * Handlers in the chain are thunk addresses (32-bit stubs pointing to our
 * 64-bit shims). We look up each handler in the thunk table to find the
 * 64-bit target, then dispatch appropriately.
 *
 * For _except_handler3 targets: we read the 32-bit scopetable, call the
 * 32-bit filter functions via compat32_callback_args, and if a filter returns
 * EXCEPTION_EXECUTE_HANDLER, call the handler function (which does a local
 * goto and never returns to us).
 *
 * Returns: 1 if handled (ContinueExecution), 0 if unhandled.
 */

extern TEB g_teb;
extern TEB32 g_teb32;
extern PVOID g_unhandled_filter;

/* 32-bit EXCEPTION_RECORD for passing to 32-bit filter functions */
typedef struct __attribute__((packed)) {
    uint32_t ExceptionCode;
    uint32_t ExceptionFlags;
    uint32_t ExceptionRecord;     /* 32-bit ptr (self-referential) */
    uint32_t ExceptionAddress;    /* 32-bit ptr */
    uint32_t NumberParameters;
    uint32_t ExceptionInformation[15];
} EXCEPTION_RECORD32;

/* 32-bit EXCEPTION_POINTERS for passing to 32-bit filter functions */
typedef struct __attribute__((packed)) {
    uint32_t ExceptionRecord;     /* 32-bit ptr to EXCEPTION_RECORD32 */
    uint32_t ContextRecord;       /* 32-bit ptr (NULL for us) */
} EXCEPTION_POINTERS32;

/* Static buffers for 32-bit exception data (must be <4GB accessible) */
static EXCEPTION_RECORD32 seh32_exception_record;
static EXCEPTION_POINTERS32 seh32_exception_pointers;

/* Accessor for msvcrt_shim.c's crt_except_handler3 compat32 path */
PVOID seh32_ep_addr_for_filter(void)
{
    return (PVOID)&seh32_exception_pointers;
}

int compat32_seh_dispatch(PEXCEPTION_RECORD ExceptionRecord)
{
    /* Read the 32-bit ExceptionList from TEB32.
     * 32-bit code writes FS:[0] — since FS base points to g_teb32,
     * the SEH chain is at g_teb32.ExceptionList (4 bytes). */
    uint32_t frame_addr = g_teb32.ExceptionList;

    /* Diagnostic: read FS base from MSR to verify it points to g_teb32 */
#ifndef TEST_HARNESS
    {
        uint32_t lo, hi;
        __asm__ volatile (
            "mov $0xC0000100, %%ecx\n"  /* MSR_FS_BASE */
            "rdmsr\n"
            : "=a"(lo), "=d"(hi)
            :
            : "ecx"
        );
        uint64_t fs_base = ((uint64_t)hi << 32) | lo;
        serial_puts("[SEH32] FS_BASE MSR = 0x");
        serial_puthex(fs_base, 16);
        serial_puts(" g_teb32 = 0x");
        serial_puthex((uint64_t)(ULONG_PTR)&g_teb32, 16);
        uint32_t *fs0 = (uint32_t *)fs_base;
        serial_puts(" *FS[0] = 0x");
        serial_puthex(*fs0, 8);
        serial_puts("\n");
    }
#endif

    serial_puts("[SEH32] dispatch code=0x");
    serial_puthex(ExceptionRecord->ExceptionCode, 8);
    serial_puts(" chain=0x");
    serial_puthex(frame_addr, 8);
    serial_puts("\n");

    if (frame_addr == 0 || frame_addr == 0xFFFFFFFF) {
        serial_puts("[SEH32] empty chain\n");
        return 0;
    }

    /* Build 32-bit EXCEPTION_RECORD for filter functions */
    BYTE *p = (BYTE *)&seh32_exception_record;
    for (SIZE_T i = 0; i < sizeof(seh32_exception_record); i++) p[i] = 0;
    seh32_exception_record.ExceptionCode = ExceptionRecord->ExceptionCode;
    seh32_exception_record.ExceptionFlags = ExceptionRecord->ExceptionFlags;
    seh32_exception_record.NumberParameters = ExceptionRecord->NumberParameters;
    for (DWORD i = 0; i < ExceptionRecord->NumberParameters && i < 15; i++)
        seh32_exception_record.ExceptionInformation[i] =
            (uint32_t)ExceptionRecord->ExceptionInformation[i];

    /* Build 32-bit EXCEPTION_POINTERS */
    seh32_exception_pointers.ExceptionRecord =
        (uint32_t)(ULONG_PTR)&seh32_exception_record;
    seh32_exception_pointers.ContextRecord = 0;  /* no context */

    int frame_num = 0;
    while (frame_addr != 0xFFFFFFFF && frame_addr != 0 && frame_num < 64) {
        /* Read 32-bit EXCEPTION_REGISTRATION_RECORD:
         *   offset 0: uint32_t Next
         *   offset 4: uint32_t Handler */
        uint32_t *frame32 = (uint32_t *)(ULONG_PTR)frame_addr;
        uint32_t next32    = frame32[0];
        uint32_t handler32 = frame32[1];

        serial_puts("[SEH32] frame ");
        serial_putdec(frame_num);
        serial_puts(" @0x");
        serial_puthex(frame_addr, 8);
        serial_puts(" handler=0x");
        serial_puthex(handler32, 8);

        /* Look up handler in thunk table */
        int32_t thunk_idx = compat32_find_thunk(handler32);

        if (thunk_idx >= 0) {
            const char *name = thunk_table[thunk_idx].name;
            serial_puts(" → ");
            if (name) serial_puts(name);
            serial_puts("\n");

            uint64_t target = thunk_table[thunk_idx].target_addr;

            /*
             * Check if this is _except_handler3 (our C SEH handler).
             * For _except_handler3, the EH3 frame has additional fields:
             *   +8:  uint32_t ScopeTable (pointer to 32-bit scopetable)
             *   +12: uint32_t TryLevel
             *
             * We read the 32-bit scopetable and call filter functions
             * via compat32_callback_args (in 32-bit compat mode).
             */
            extern EXCEPTION_DISPOSITION WINAPI crt_except_handler3(
                PEXCEPTION_RECORD, PEH3_EXCEPTION_REGISTRATION,
                PCONTEXT, PVOID);
            extern EXCEPTION_DISPOSITION WINAPI crt_except_handler4(
                PEXCEPTION_RECORD, PEH3_EXCEPTION_REGISTRATION,
                PCONTEXT, PVOID);

            if ((void *)(ULONG_PTR)target == (void *)crt_except_handler3 ||
                (void *)(ULONG_PTR)target == (void *)crt_except_handler4)
            {
                /* Read 32-bit EH3 extra fields */
                uint32_t scopetable32 = frame32[2];
                uint32_t trylevel32   = frame32[3];

                serial_puts("[SEH32] _except_handler3: scope=0x");
                serial_puthex(scopetable32, 8);
                serial_puts(" tryLevel=");
                serial_putdec(trylevel32);
                serial_puts("\n");

                /* Walk scopetable (32-bit entries: 12 bytes each) */
                uint32_t level = trylevel32;
                while (level != (uint32_t)-1 && scopetable32 != 0) {
                    /* 32-bit SCOPETABLE_ENTRY:
                     *   +0: uint32_t EnclosingLevel
                     *   +4: uint32_t FilterFunc (32-bit code ptr)
                     *   +8: uint32_t HandlerFunc (32-bit code ptr) */
                    uint32_t *se = (uint32_t *)(ULONG_PTR)(scopetable32 + level * 12);
                    uint32_t enclosing = se[0];
                    uint32_t filter32  = se[1];
                    uint32_t handler_func32 = se[2];

                    if (filter32) {
                        serial_puts("[SEH32] calling filter @0x");
                        serial_puthex(filter32, 8);
                        serial_puts("\n");

                        /* Call 32-bit filter: int filter(EXCEPTION_POINTERS *) */
                        uint32_t ep_addr = (uint32_t)(ULONG_PTR)&seh32_exception_pointers;
                        uint32_t filter_args[1] = { ep_addr };
                        uint32_t result = compat32_callback_args(filter32, 1, filter_args);

                        serial_puts("[SEH32] filter returned ");
                        serial_putdec(result);
                        serial_puts("\n");

                        if ((int32_t)result == 1 /* EXCEPTION_EXECUTE_HANDLER */) {
                            serial_puts("[SEH32] EXECUTE_HANDLER — calling handler @0x");
                            serial_puthex(handler_func32, 8);
                            serial_puts("\n");

                            /* Update TryLevel on the 32-bit stack */
                            frame32[3] = enclosing;

                            /*
                             * Unwind frames between chain head and this frame.
                             * Send EXCEPTION_UNWINDING to each handler above us.
                             */
                            uint32_t uw_addr = g_teb32.ExceptionList;
                            while (uw_addr != 0xFFFFFFFF && uw_addr != frame_addr) {
                                uint32_t *uw32 = (uint32_t *)(ULONG_PTR)uw_addr;
                                uw_addr = uw32[0]; /* skip to next */
                            }
                            /* Set this frame as new chain head */
                            g_teb32.ExceptionList = frame_addr;

                            /*
                             * Call the 32-bit handler function.
                             * In MSVC _except_handler3, this is a longjmp-style
                             * transfer: the handler restores EBP to the establishing
                             * frame and continues execution at the __except block.
                             * It does NOT return to us.
                             *
                             * We call it via compat32_callback. If it does return
                             * (unusual), we treat the exception as handled.
                             */
                            compat32_callback(handler_func32);

                            /* If handler returned (unusual), exception is handled */
                            serial_puts("[SEH32] handler returned — continuing\n");
                            return 1;
                        }
                        else if ((int32_t)result == -1 /* EXCEPTION_CONTINUE_EXECUTION */) {
                            serial_puts("[SEH32] CONTINUE_EXECUTION\n");
                            return 1;
                        }
                        /* EXCEPTION_CONTINUE_SEARCH → try enclosing scope */
                    }

                    level = enclosing;
                }
            } else {
                /* Other handler (e.g., __CxxFrameHandler3).
                 * We can't easily dispatch C++ EH from here.
                 * Try calling the 64-bit shim with a temporary 64-bit frame. */
                serial_puts("[SEH32] calling 64-bit handler shim\n");

                /* Build minimal temporary EH3 frame with 64-bit pointers */
                EH3_EXCEPTION_REGISTRATION temp_eh3;
                temp_eh3.registration.Next = EXCEPTION_CHAIN_END;
                temp_eh3.registration.Handler = (PVOID)(ULONG_PTR)target;
                temp_eh3.ScopeTable = NULL;
                temp_eh3.TryLevel = (DWORD)-1;

                CONTEXT ctx;
                BYTE *cp = (BYTE *)&ctx;
                for (SIZE_T ci = 0; ci < sizeof(ctx); ci++) cp[ci] = 0;
                ctx.ContextFlags = CONTEXT_FULL;

                typedef EXCEPTION_DISPOSITION (WINAPI *seh_handler_fn)(
                    PEXCEPTION_RECORD, PEH3_EXCEPTION_REGISTRATION,
                    PCONTEXT, PVOID);
                seh_handler_fn handler = (seh_handler_fn)(ULONG_PTR)target;
                EXCEPTION_DISPOSITION disp = handler(
                    ExceptionRecord, &temp_eh3, &ctx, NULL);

                if (disp == ExceptionContinueExecution) {
                    serial_puts("[SEH32] handler: ContinueExecution\n");
                    return 1;
                }
                serial_puts("[SEH32] handler: ContinueSearch\n");
            }
        } else {
            serial_puts(" (PE32 handler, calling via compat32)\n");

            /*
             * This is a 32-bit handler installed by PE32 code directly
             * (e.g., __CxxFrameHandler3 for MSVC C++ EH).
             *
             * 32-bit SEH handler signature (cdecl):
             *   EXCEPTION_DISPOSITION handler(
             *       EXCEPTION_RECORD *ExceptionRecord,
             *       void *EstablisherFrame,
             *       CONTEXT *ContextRecord,
             *       void *DispatcherContext);
             *
             * Returns: 0=ContinueExecution, 1=ContinueSearch
             * May also longjmp directly to catch block (no return).
             */
            uint32_t args[4];
            args[0] = (uint32_t)(ULONG_PTR)&seh32_exception_record;
            args[1] = frame_addr;
            args[2] = 0;   /* no CONTEXT */
            args[3] = 0;   /* no DispatcherContext */

            uint32_t disp = compat32_callback_args(handler32, 4, args);

            serial_puts("[SEH32] PE32 handler returned disp=");
            serial_putdec(disp);
            serial_puts("\n");

            if (disp == 0 /* ExceptionContinueExecution */) {
                serial_puts("[SEH32] PE32 handler: ContinueExecution\n");
                return 1;
            }
            /* disp==1 → ContinueSearch, try next frame */
        }

        frame_addr = next32;
        frame_num++;
    }

    /* Try unhandled exception filter */
    if (g_unhandled_filter) {
        serial_puts("[SEH32] calling UnhandledExceptionFilter\n");
        EXCEPTION_POINTERS ep;
        ep.ExceptionRecord = ExceptionRecord;
        ep.ContextRecord   = NULL;

        typedef LONG (WINAPI *uef_fn)(PEXCEPTION_POINTERS);
        uef_fn filter = (uef_fn)g_unhandled_filter;
        LONG result = filter(&ep);

        if (result == EXCEPTION_CONTINUE_EXECUTION)
            return 1;
    }

    serial_puts("[SEH32] UNHANDLED — no handler caught the exception\n");
    return 0;
}

/* ── Kernel-side thunk dispatch (called from INT 0x2E handler) ── */

/*
 * For bare-metal mode: the kernel's INT 0x2E handler calls this
 * function when it detects a compat32 thunk invocation.
 *
 * thunk_idx: the thunk table index (from EAX)
 * stack_args: pointer to the 32-bit caller's stack arguments
 * num_args:   number of arguments (from ECX, or from thunk_table)
 *
 * Returns: the 64-bit result from the shim function (truncated to
 *          EAX for the 32-bit caller by the INT handler).
 */
uint64_t compat32_dispatch(uint32_t thunk_idx, uint32_t *stack_args)
{
    /* Callback return: 32-bit function completed, longjmp back */
    if (thunk_idx == THUNK_CALLBACK_RETURN) {
        /* Restore 64-bit data segments (compat mode set them to 0x48) */
        __asm__ volatile (
            "mov $0x30, %%ax\n"
            "mov %%ax, %%ds\n"
            "mov %%ax, %%es\n"
            "mov %%ax, %%ss\n"
            ::: "ax"
        );

        int depth = callback_depth - 1;
        serial_puts("[INT2E] callback return depth=");
        serial_putdec(depth);
        serial_puts("\n");

        if (depth < 0 || depth >= MAX_CALLBACK_DEPTH) {
            serial_puts("[INT2E] FATAL: invalid callback depth!\n");
            return 0;
        }

        /* CRITICAL: Restore IST1 before longjmp. The INT 0x2E handler
         * saved old IST1 on stack and shifted it by -8192. kern_longjmp
         * bypasses the handler's pop → IST1 drifts 8KB per callback.
         * After 338 callbacks: 338×8192 = 2.76MB drift → stack corruption. */
        {
            extern uint64_t *tss_ist1_ptr;
            if (tss_ist1_ptr && depth >= 0 && depth < MAX_CALLBACK_DEPTH)
                *tss_ist1_ptr = callback_saved_ist1[depth];
        }

        kern_longjmp(callback_jmpbufs[depth], 1);
        /* never reached */
        return 0;
    }

    if (thunk_idx >= thunk_count) {
        serial_puts("[COMPAT32] Invalid thunk index ");
        serial_putdec(thunk_idx);
        serial_puts("\n");
        return 0;
    }

    compat32_thunk_t *t = &thunk_table[thunk_idx];
    uint64_t target = t->target_addr;
    uint8_t nargs = t->num_args;

    /* Stack alignment check: 32-bit caller's ESP must be 4-byte aligned.
     * If misaligned, a stdcall RET N shifted the stack incorrectly. */
    {
        uint32_t esp32 = (uint32_t)(uintptr_t)stack_args - 4; /* stack_args = ESP+4 */
        if (esp32 & 3) {
            serial_puts("[COMPAT32] *** ESP MISALIGNED: 0x");
            serial_puthex(esp32, 8);
            serial_puts(" thunk=");
            if (t->name) serial_puts(t->name);
            serial_puts("\n");
        }
    }

    /* Debug: log INT 0x2E dispatch (throttled to reduce log noise) */
    {
        static uint32_t int2e_call_count = 0;
        int2e_call_count++;
        /* Log first 200, every 100th, AND last calls before crash
         * (always log _CxxThrowException and RaiseException) */
        /* Log everything except timeGetTime (always log non-timeGetTime) */
        int do_log = 1;
        if (t->name && t->name[0] == 't' && t->name[1] == 'i' && t->name[2] == 'm' && t->name[3] == 'e')
            do_log = (int2e_call_count <= 10 || (int2e_call_count % 500000) == 0);
        /* Always log exception-related functions */
        if (t->name && (t->name[0] == '_' && t->name[1] == 'C'))  /* _Cxx* */
            do_log = 1;
        if (t->name && t->name[0] == 'R' && t->name[1] == 'a')   /* Raise* */
            do_log = 1;
        /* Log last 20 calls before throttle boundary */
        if (int2e_call_count > 1300)
            do_log = 1;
        if (do_log) {
            serial_puts("[INT2E] #");
            serial_putdec(thunk_idx);
            serial_puts(" ");
            if (t->name) serial_puts(t->name);
            serial_puts(" (");
            serial_putdec(nargs);
            serial_puts(" args) [call ");
            serial_putdec(int2e_call_count);
            serial_puts("]\n");
        }
    }

    /*
     * Call the 64-bit shim function with marshaled arguments.
     * ms_abi: first 4 args in RCX, RDX, R8, R9; rest on stack.
     *
     * We use a generic dispatcher that supports up to 12 args.
     * This is sufficient for all known Win32 API functions.
     */
    typedef uint64_t (WINAPI *fn0)(void);
    typedef uint64_t (WINAPI *fn1)(uint64_t);
    typedef uint64_t (WINAPI *fn2)(uint64_t, uint64_t);
    typedef uint64_t (WINAPI *fn3)(uint64_t, uint64_t, uint64_t);
    typedef uint64_t (WINAPI *fn4)(uint64_t, uint64_t, uint64_t, uint64_t);
    typedef uint64_t (WINAPI *fn5)(uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t);
    typedef uint64_t (WINAPI *fn6)(uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t);
    typedef uint64_t (WINAPI *fn7)(uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t);
    typedef uint64_t (WINAPI *fn8)(uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
    typedef uint64_t (WINAPI *fn9)(uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t);
    typedef uint64_t (WINAPI *fn12)(uint64_t, uint64_t, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t, uint64_t);

    /* Zero-extend 32-bit stack args to 64-bit */
    uint64_t a[12] = {0};
    for (int i = 0; i < nargs && i < 12; i++)
        a[i] = (uint64_t)stack_args[i];

    switch (nargs) {
    case 0:  return ((fn0)(ULONG_PTR)target)();
    case 1:  return ((fn1)(ULONG_PTR)target)(a[0]);
    case 2:  return ((fn2)(ULONG_PTR)target)(a[0], a[1]);
    case 3:  return ((fn3)(ULONG_PTR)target)(a[0], a[1], a[2]);
    case 4:  return ((fn4)(ULONG_PTR)target)(a[0], a[1], a[2], a[3]);
    case 5:  return ((fn5)(ULONG_PTR)target)(a[0], a[1], a[2], a[3], a[4]);
    case 6:  return ((fn6)(ULONG_PTR)target)(a[0], a[1], a[2], a[3], a[4], a[5]);
    case 7:  return ((fn7)(ULONG_PTR)target)(a[0], a[1], a[2], a[3], a[4], a[5], a[6]);
    case 8:  return ((fn8)(ULONG_PTR)target)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
    case 9:  return ((fn9)(ULONG_PTR)target)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]);
    default: return ((fn12)(ULONG_PTR)target)(a[0], a[1], a[2], a[3], a[4], a[5],
                                                a[6], a[7], a[8], a[9], a[10], a[11]);
    }
}

/* ── Query thunk table (for kernel INT 0x2E handler) ─────────── */

uint64_t compat32_get_target(uint32_t thunk_idx)
{
    if (thunk_idx >= thunk_count) return 0;
    return thunk_table[thunk_idx].target_addr;
}

uint8_t compat32_get_nargs(uint32_t thunk_idx)
{
    if (thunk_idx >= thunk_count) return 0;
    return thunk_table[thunk_idx].num_args;
}

uint32_t compat32_get_count(void)
{
    return thunk_count;
}
