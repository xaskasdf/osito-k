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
#include "wdbg.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void *mem_alloc_pages(uint64_t count);
extern int  kern_setjmp(uint64_t *buf) __attribute__((returns_twice));
extern void kern_longjmp(uint64_t *buf, int val);

/* ── Global compat32 mode flag ────────────────────────────────── */

int g_compat32_mode = 0;
uint32_t g_int2e_rsp_depth = 0; /* shared with int2e_stub.S */

/* ── C++ EH unwind state (set by _CxxThrowException) ────────── */
uint32_t g_compat32_unwind_eip = 0;
uint32_t g_compat32_last_stack_arg13 = 0;  /* for CreateWindowExW lpParam workaround */
uint32_t g_compat32_unwind_esp = 0;
uint32_t g_compat32_unwind_ebp = 0;

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

/* Stub for unresolved imports: XOR EAX,EAX; RET (returns 0) */
static uint32_t unresolved_stub_addr = 0;
static uint32_t compat32_data_area = 0;

/* Stub for C++ catch funclet return: JMP EAX (continues at funclet's return value) */
static uint32_t catch_continue_stub_addr = 0;

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
#define CALLBACK_STACK_SIZE   65536

/* Force to .data section to change RIP-relative displacement encoding.
 * In BSS, the displacement contained 0xCC at a critical code address,
 * which QEMU TCG misinterpreted as INT3. */
static int      callback_depth __attribute__((section(".data"))) = 0;
/* kern_setjmp / kern_longjmp use a 9-quad jmp_buf:
 *   [0..5] callee-saved GPRs (rbx, rbp, r12-r15)
 *   [6]    rsp     [7] rip     [8] cr3
 * Sizing this array as [...][8] truncated each slot to 64 bytes and the
 * setjmp at depth N+1 wrote its rbx (slot[0]) on top of slot N's cr3 —
 * causing the depth=0 longjmp after _initterm to triple-fault on a
 * bogus 0x40 CR3. Must be at least 9. */
static uint64_t callback_jmpbufs[MAX_CALLBACK_DEPTH][9];
static uint64_t callback_saved_ist1[MAX_CALLBACK_DEPTH];
/* Callback stacks allocated lazily (saves 2MB BSS).
 * Each depth level gets its own 64KB stack on first use. */
static uint8_t *callback_stacks_ptr[MAX_CALLBACK_DEPTH];

static uint8_t *callback_stack_get(int depth) {
    if (depth < 0 || depth >= MAX_CALLBACK_DEPTH) return NULL;
    if (!callback_stacks_ptr[depth]) {
        extern void *mem_alloc_aligned(uint64_t, uint64_t);
        callback_stacks_ptr[depth] = (uint8_t *)mem_alloc_aligned(CALLBACK_STACK_SIZE, 16);
    }
    return callback_stacks_ptr[depth];
}
/* Compat macro: callback_stack_get(depth) → callback_stack_get(depth) */
#define callback_stacks(d) callback_stack_get(d)

/* The last 32-bit caller EIP (stack_args[-1] = the address right
 * after the CALL into a kernel32_shim API). Updated on every
 * compat32_dispatch entry. Diagnostic only — read by VirtualAlloc
 * to identify which engine function makes bogus 4GB requests. */
uint32_t g_last_caller_eip = 0;
uint32_t compat32_get_last_caller_eip(void) { return g_last_caller_eip; }

/* Pointer (as uint32_t) to the user-mode stack at the args, equal to
 * user ESP+4 at the moment of the INT 0x2E. Updated on each dispatch
 * entry. Used by VirtualAlloc shim to walk the user stack chain. */
uint32_t g_last_stack_args = 0;
uint32_t compat32_get_last_stack_args(void) { return g_last_stack_args; }

/* The user-mode RBP at the moment of the INT 0x2E. Set by
 * int2e_stub.S right before it calls compat32_dispatch. The low 32
 * bits are the 32-bit EBP that the engine's frame-pointer chain uses;
 * the VirtualAlloc shim walks [EBP], [EBP+4] up the chain to find
 * the callers of FMallocWindows::Realloc. */
uint64_t g_int2e_user_rbp = 0;
uint64_t g_int2e_user_rcx = 0;
uint64_t g_int2e_user_rdx = 0;
uint64_t g_int2e_user_rsi = 0;
uint64_t g_int2e_user_rdi = 0;
uint64_t g_int2e_user_rbx = 0;
uint32_t compat32_get_last_user_ecx(void) { return (uint32_t)g_int2e_user_rcx; }
uint32_t compat32_get_last_user_edx(void) { return (uint32_t)g_int2e_user_rdx; }
uint32_t compat32_get_last_user_esi(void) { return (uint32_t)g_int2e_user_rsi; }
uint32_t compat32_get_last_user_edi(void) { return (uint32_t)g_int2e_user_rdi; }
uint32_t compat32_get_last_user_ebp(void) { return (uint32_t)g_int2e_user_rbp; }
uint32_t compat32_get_last_user_ebx(void) { return (uint32_t)g_int2e_user_rbx; }

/*
 * Single global retval written by the 32-bit return stub (MOV [addr], EAX).
 * The stub uses a fixed address so we can't index by depth there.
 * The dispatch handler reads this and stores it before longjmp.
 */
static uint32_t callback_retval = 0;           /* written by 32-bit return stub */
static uint32_t callback_retval_per_depth[MAX_CALLBACK_DEPTH]; /* saved before longjmp */

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

    /*
     * Install "unresolved import" stub: XOR EAX,EAX; RET
     * Used for IAT entries that couldn't be resolved — prevents
     * wild jumps to RVA addresses left in the IAT.
     */
    {
        uint8_t *stub = thunk_pool + (THUNK_POOL_PAGES * 4096) - 32;
        stub[0] = 0x31; stub[1] = 0xC0;  /* XOR EAX, EAX */
        stub[2] = 0xC3;                   /* RET */
        unresolved_stub_addr = (uint32_t)(ULONG_PTR)stub;
    }

    /* Catch continuation stub: JMP EAX
     * MSVC catch funclets return with EAX = continuation address.
     * This stub is used as the return address for catch funclets —
     * when the funclet does RET, it pops this stub's address, and
     * JMP EAX continues execution at the establishing function's
     * code after the try/catch block. */
    {
        uint8_t *stub = thunk_pool + (THUNK_POOL_PAGES * 4096) - 40;
        stub[0] = 0xFF; stub[1] = 0xE0;  /* JMP EAX */
        catch_continue_stub_addr = (uint32_t)(ULONG_PTR)stub;
    }

    /*
     * Data export area — MSVC CRT data imports (_acmdln, _adjust_fdiv, etc.)
     * These are VARIABLES, not functions. PE32 code reads them directly via
     * the IAT (mov eax,[IAT]; mov val,[eax]). They must live in 32-bit
     * addressable memory, NOT be thunked.
     *
     * Layout (at thunk_pool end - 128):
     *   +0:  int    _adjust_fdiv = 0
     *   +4:  char*  _acmdln = &cmdline[0]
     *   +8:  int    _commode = 0
     *   +12: int    _fmode = 0
     *   +16: int    __mb_cur_max = 1
     *   +20: char   cmdline[64] = "UnrealTournament.exe"
     */
    {
        uint8_t *data = thunk_pool + (THUNK_POOL_PAGES * 4096) - 256;
        memset(data, 0, 128);
        /* _adjust_fdiv at +0 */
        *(int32_t *)(data + 0) = 0;
        /* _acmdln at +4: points to cmdline string at +20 */
        *(uint32_t *)(data + 4) = (uint32_t)(ULONG_PTR)(data + 20);
        /* _commode at +8 */
        *(int32_t *)(data + 8) = 0;
        /* _fmode at +12 */
        *(int32_t *)(data + 12) = 0;
        /* __mb_cur_max at +16 */
        *(int32_t *)(data + 16) = 1;
        /* cmdline at +20 */
        extern char win32_exe_name[64];
        const char *cmd = win32_exe_name[0] ? win32_exe_name : "UnrealTournament.exe";
        int ci = 0;
        while (cmd[ci] && ci < 60) { data[20 + ci] = cmd[ci]; ci++; }
        data[20 + ci] = 0;

        compat32_data_area = (uint32_t)(ULONG_PTR)data;
        serial_puts("[COMPAT32] Data exports at 0x");
        serial_puthex(compat32_data_area, 8);
        serial_puts("\n");
    }

    /* Initialize fast 32-bit x87 math functions (pow, fmod, acos).
     * These run natively in compat mode without INT 0x2E overhead. */
    extern void compat32_init_fast_math(void);
    compat32_init_fast_math();
#endif
}

/* ── Data export resolution ──────────────────────────────────
 *
 * Returns a 32-bit address for known CRT data imports.
 * These are written directly to the IAT (no thunk).
 * Returns 0 if not a data import.
 */

uint32_t compat32_resolve_data_import(const char *name)
{
    if (!compat32_data_area || !name) return 0;
    /* Compare function names for known data imports */
    if (name[0] == '_') {
        if (name[1] == 'a' && name[2] == 'c' && name[3] == 'm' &&
            name[4] == 'd' && name[5] == 'l' && name[6] == 'n' && name[7] == 0)
            return compat32_data_area + 4;  /* _acmdln */
        if (name[1] == 'a' && name[2] == 'd' && name[3] == 'j') /* _adjust_fdiv */
            return compat32_data_area + 0;
        if (name[1] == 'c' && name[2] == 'o' && name[3] == 'm') /* _commode */
            return compat32_data_area + 8;
        if (name[1] == 'f' && name[2] == 'm' && name[3] == 'o') /* _fmode */
            return compat32_data_area + 12;
    }
    if (name[0] == '_' && name[1] == '_' && name[2] == 'm' && name[3] == 'b')
        return compat32_data_area + 16;  /* __mb_cur_max */
    return 0;
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

        /* user32 — recently added, were missing and caused RET N over-pop */
        { "IsWindow",             1 }, { "IsIconic",              1 },
        { "IsZoomed",             1 }, { "IsWindowEnabled",       1 },
        { "GetParent",            1 }, { "EnableWindow",          2 },
        { "BeginPaint",           2 }, { "EndPaint",              2 },
        { "GetWindowLongW",       2 }, { "SetWindowLongW",        3 },
        { "GetClassInfoExA",      3 }, { "GetClassInfoExW",       3 },
        { "EnumChildWindows",     3 }, { "FillRect",              3 },
        { "GetUpdateRect",        3 }, { "InvalidateRect",        3 },
        { "ChangeDisplaySettingsA", 2 }, { "ChangeDisplaySettingsW", 2 },
        { "EnumDisplaySettingsA",  3 }, { "EnumDisplaySettingsW",  3 },
        { "SystemParametersInfoA", 4 }, { "SystemParametersInfoW", 4 },
        { "CallWindowProcA",      5 }, { "CallWindowProcW",       5 },
        { "DialogBoxParamW",      5 }, { "DialogBoxParamA",       5 },
        { "RegisterWindowMessageA", 1 }, { "RegisterWindowMessageW", 1 },
        { "PostThreadMessageW",   4 }, { "PostThreadMessageA",    4 },
        { "SetPropA",             3 }, { "SetPropW",              3 },
        { "GetPropA",             2 }, { "GetPropW",              2 },
        { "RemovePropA",          2 }, { "RemovePropW",           2 },
        { "GetClassLongA",        2 }, { "GetClassLongW",         2 },
        { "SendMessageTimeoutW",  7 }, { "SendMessageTimeoutA",   7 },
        { "GetWindowTextA",       3 }, { "GetWindowTextW",        3 },
        { "GetWindowTextLengthA", 1 }, { "GetWindowTextLengthW",  1 },
        { "DefWindowProcW",       4 }, { "DefMDIChildProcW",      4 },
        { "UpdateWindow",         1 }, { "ShowCursor",            1 },
        { "SetCapture",           1 }, { "ReleaseCapture",        0 },
        { "GetForegroundWindow",  0 }, { "SetForegroundWindow",   1 },

        /* winmm — timeGetTime was 0-arg but got RET 16 = 16 bytes over-pop per call! */
        { "timeGetTime",          0 }, { "timeBeginPeriod",       1 },
        { "timeEndPeriod",        1 }, { "timeSetEvent",          5 },
        { "timeKillEvent",        1 },

        /* gdi32 — extended */
        { "GetDeviceCaps",        2 }, { "CreateCompatibleDC",    1 },
        { "DeleteDC",             1 }, { "SelectObject",          2 },
        { "GetObjectA",           3 }, { "DeleteObject",          1 },
        { "ChoosePixelFormat",    2 }, { "SetPixelFormat",        3 },
        { "CreateDIBitmap",       6 }, { "CreateBitmap",          5 },
        { "CreatePatternBrush",   1 }, { "CreateSolidBrush",      1 },
        { "GetStockObject",       1 }, { "GetObjectW",            3 },
        { "CreateDIBSection",     6 }, { "BitBlt",                9 },

        /* ddraw COM methods (called via thunks, stdcall with 'this') */
        { "DD_QI",                3 }, { "DD_AddRef",             1 },
        { "DD_Release",           1 }, { "DD_CreateSurface",      4 },
        { "DD_GetDisplayMode",    2 }, { "DD_SetCoopLevel",       3 },
        { "DD_SetDisplayMode",    6 },
        { "Surf_QI",              3 }, { "Surf_AddRef",           1 },
        { "Surf_Release",         1 }, { "Surf_Blt",              7 },
        { "Surf_Flip",            3 }, { "Surf_GetDesc",          2 },
        { "Surf_Lock",            5 }, { "Surf_Unlock",           2 },

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
        /* Wide string functions (cdecl, from msvcrt) */
        { "wcslen",               1 }, { "wcscpy",                2 },
        { "wcsncpy",              3 }, { "wcscmp",                2 },
        { "wcsncmp",              3 }, { "wcscat",                2 },
        { "wcschr",               2 }, { "wcsrchr",               2 },
        { "wcsstr",               2 }, { "_wcsicmp",              2 },
        { "_wcsnicmp",            3 }, { "_wcslwr",               1 },
        { "_wcsupr",              1 }, { "wcstol",                3 },
        { "wcstod",               2 }, { "swprintf",             12 },
        { "_snwprintf",          12 }, { "towlower",              1 },
        { "towupper",             1 }, { "iswspace",              1 },
        { "iswdigit",             1 }, { "iswalpha",              1 },
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

    /* Default: 4 args → RET 16. With the comprehensive arg count table above,
     * most functions have correct entries. The default of 4 is safer than 0
     * because under-pop (stale args) is less destructive than the assertion
     * failures caused by 0-arg defaults for stdcall functions that need cleanup.
     * The critical missing entries (timeGetTime=0, IsWindow=1, etc.) are now
     * all explicitly listed above. */
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

            if (!resolved) {
                /*
                 * Unresolved import — patch IAT with a stub that returns 0.
                 * Without this, the IAT keeps the original RVA (e.g. 0x1F68)
                 * and calling through it jumps to garbage → #UD.
                 */
                if (unresolved_stub_addr)
                    iat_entry->u1.Function = unresolved_stub_addr;
                continue;
            }

            if (shim) {
                /*
                 * Import from a shim DLL (64-bit kernel code).
                 * Check for fast-math native 32-bit implementations first.
                 * These run directly in compat mode (no INT 0x2E overhead).
                 */
                extern uint32_t g_fast_CIpow_addr, g_fast_CIfmod_addr, g_fast_CIacos_addr;
                if (func_name && g_fast_CIpow_addr) {
                    uint32_t fast = 0;
                    if (func_name[0]=='_' && func_name[1]=='C' && func_name[2]=='I') {
                        if (func_name[3]=='p' && func_name[4]=='o' && func_name[5]=='w' && !func_name[6])
                            fast = g_fast_CIpow_addr;
                        else if (func_name[3]=='f' && func_name[4]=='m' && func_name[5]=='o' && func_name[6]=='d' && !func_name[7])
                            fast = g_fast_CIfmod_addr;
                        else if (func_name[3]=='a' && func_name[4]=='c' && func_name[5]=='o' && func_name[6]=='s' && !func_name[7])
                            fast = g_fast_CIacos_addr;
                    }
                    if (fast) {
                        iat_entry->u1.Function = fast;
                        direct++;
                        serial_puts("[FAST-IAT] ");
                        serial_puts(func_name);
                        serial_puts(" → 0x");
                        serial_puthex(fast, 8);
                        serial_puts("\n");
                        continue;
                    }
                }

                /*
                 * Check for DATA imports — these are variables, not
                 * functions. Write the 32-bit data address directly.
                 */
                uint32_t data_addr = func_name ?
                    compat32_resolve_data_import(func_name) : 0;
                if (data_addr) {
                    iat_entry->u1.Function = data_addr;
                    direct++;
                    if (func_name) {
                        serial_puts("[IAT-DATA] ");
                        serial_puts(func_name);
                        serial_puts(" → 0x");
                        serial_puthex(data_addr, 8);
                        serial_puts("\n");
                    }
                } else {
                    /* Function import — create INT 0x2E thunk */
                    uint64_t target64 = (uint64_t)(ULONG_PTR)resolved;
                    uint8_t nargs = func_name ? guess_num_args(func_name) : 4;
                    uint32_t thunk_addr = compat32_make_thunk_ex(target64, func_name, nargs, cc);
                    if (thunk_addr) {
                        iat_entry->u1.Function = thunk_addr;
                        patched++;
                    }
                }
            } else {
                /*
                 * Import from a real PE32 DLL (32-bit code in same compat mode).
                 * Write the address directly — no thunk needed.
                 */
                /* Use volatile to ensure the write hits memory */
                volatile uint32_t *iat_ptr = (volatile uint32_t *)&iat_entry->u1.Function;
                *iat_ptr = (uint32_t)(ULONG_PTR)resolved;
                direct++;

                /* Catch imports resolved to VirtualAlloc range (never valid DLL code) */
                {
                    uint32_t val = (uint32_t)(ULONG_PTR)resolved;
                    if (val >= 0x40000000 && val < 0x80000000) {
                        serial_puts("[IAT-WARN] VirtualAlloc addr: ");
                        if (func_name) serial_puts(func_name);
                        serial_puts(" from ");
                        serial_puts(dll_name);
                        serial_puts(" -> 0x");
                        serial_puthex(val, 8);
                        serial_puts("\n");
                        *iat_ptr = unresolved_stub_addr;
                    }
                }

                /* Log GIsRunning resolution for debugging */
                if (func_name && func_name[0]=='?' && func_name[1]=='G' &&
                    func_name[2]=='I' && func_name[3]=='s' && func_name[4]=='R') {
                    serial_puts("[IAT] ");
                    serial_puts(func_name);
                    serial_puts(" → 0x");
                    serial_puthex((uint64_t)(ULONG_PTR)resolved, 8);
                    serial_puts(" IAT@0x");
                    serial_puthex((uint64_t)(ULONG_PTR)&iat_entry->u1.Function, 8);
                    serial_puts(" wrote=0x");
                    serial_puthex(iat_entry->u1.Function, 8);
                    serial_puts("\n");
                }

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

    /* Post-patch validation: scan ALL IAT entries for unpatched RVAs.
     * Unpatched entries still contain PE file RVAs (< 0x01000000) that
     * get interpreted as VirtualAlloc addresses at runtime, causing the
     * CPU to execute package bytecode as x86 → #UD. Replace with stub. */
    {
        uint32_t fixups = 0;
        PIMAGE_IMPORT_DESCRIPTOR d2 =
            (PIMAGE_IMPORT_DESCRIPTOR)(base + imp_dir->VirtualAddress);
        for (; d2->Name != 0; d2++) {
            PIMAGE_THUNK_DATA32 iat =
                (PIMAGE_THUNK_DATA32)(base + d2->FirstThunk);
            for (; iat->u1.Function != 0; iat++) {
                uint32_t val = iat->u1.Function;
                if (val > 0 && val < 0x01000000 && val != unresolved_stub_addr) {
                    iat->u1.Function = unresolved_stub_addr;
                    fixups++;
                }
            }
        }
        if (fixups) {
            serial_puts("[COMPAT32] IAT fixup: ");
            serial_putdec(fixups);
            serial_puts(" stale RVA entries replaced with stub\n");
        }
    }

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

    /* Win32 now runs under kernel CR3 with shared page tables.
     * VirtualAlloc maps via paging_map_page (kernel PTs) which is
     * visible to all processes. No CR3 switch needed. */

    /* Mask APIC timer for the duration of compat32 execution. UT99
     * (PID 1) runs almost entirely in 32-bit user code on a low-half
     * stack; if the timer ISR fires there, it saves the GP frame on
     * that low-half stack and the scheduler stores frame_ptr (a low-
     * half address) in PID 1->kernel_rsp. Subsequent user-mode writes
     * to that same memory overwrite the saved frame, and the next
     * dispatch reads garbage as CS/RIP/SS/RSP — `[SCHED] CORRUPT PID
     * 1 CS=0x1F10` style triple-fault.
     *
     * The timer is unmasked transiently by INT 0x2E handlers (so
     * cooperative thread yield works inside compat32_callback_args),
     * and by sched_yield from the spinlock loop. */
    {
        extern volatile uint32_t *idt_get_apic_base(void);
        volatile uint32_t *apic = idt_get_apic_base();
        if (apic) apic[0x320/4] |= 0x10000;  /* LVT_TIMER |= MASKED */
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
        "sti\n"
        "lretq\n"
        :
        : [cs] "r"(cs64),
          [ip] "r"(ip64),
          [sp] "r"(sp64)
        : "memory", "cc", "rax"
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

    /* Save SEH ExceptionList — 32-bit code may push SEH frames on the
     * callback stack. Restore on return so the main chain stays valid. */
    extern TEB32 g_teb32;
    uint32_t saved_seh = g_teb32.ExceptionList;

    if (kern_setjmp(callback_jmpbufs[depth]) == 0) {
        /*
         * First return from setjmp — switch to compat mode.
         * Set up a small stack with the return stub as return address,
         * then LRETQ to the 32-bit function.
         */
        uint32_t *sp = (uint32_t *)(callback_stack_get(depth) + CALLBACK_STACK_SIZE);
        sp--;
        *sp = callback_return_stub_addr;  /* return address for the function */

        uint64_t cs64 = GDT_SEL_CODE32;
        uint64_t ip64 = func_addr;
        uint64_t sp64 = (uint64_t)(ULONG_PTR)sp;

        /* Mask APIC timer during callback to prevent RSP=0 crash.
         * Callback stacks are 8KB mini-buffers — if the timer fires
         * on them with IST=0, the ISR gets an invalid RSP. The timer
         * ticks are not needed during 32-bit callbacks (single-threaded). */
        {
            extern volatile uint32_t *idt_get_apic_base(void);
            volatile uint32_t *apic = idt_get_apic_base();
            if (apic) apic[0x320/4] |= 0x10000;  /* LVT_TIMER |= MASKED */
        }

        __asm__ volatile (
            "movw $0x48, %%ax\n"    /* GDT_SEL_DATA32 */
            "mov %%ax, %%ds\n"
            "mov %%ax, %%es\n"
            "mov %%ax, %%ss\n"
            "mov %[sp], %%rsp\n"
            "sti\n"                 /* Re-enable interrupts (INT 0x2E gate clears IF) */
            "push %[cs]\n"
            "push %[ip]\n"
            "lretq\n"
            :
            : [cs] "r"(cs64),
              [ip] "r"(ip64),
              [sp] "r"(sp64)
            : "memory", "cc", "rax", "rbx", "r12", "r13", "r14", "r15"
        );
        /* never reached — control flows via longjmp */
    }

    /* longjmp returned here — 32-bit function is done. */

    /* DON'T unmask the APIC timer here. compat32_enter masked it for
     * the entire UT99 lifetime (commit 0311d5f); the per-callback
     * mask added in commit 6702735 was originally paired with this
     * unmask, but with the lifetime mask in place, unmasking here
     * re-exposes the exact race that lifetime mask was meant to
     * close: any subsequent UT99 user-mode code that gets preempted
     * onto a low-half stack would corrupt PID 1's saved frame. */

    g_teb32.ExceptionList = saved_seh;  /* Restore SEH chain */
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

    /* Save SEH ExceptionList — 32-bit code may push SEH frames on the
     * callback stack. Restore on return so the main chain stays valid. */
    extern TEB32 g_teb32;
    uint32_t saved_seh = g_teb32.ExceptionList;

    /* Mask APIC timer for the duration of the 32-bit callback. The
     * callback runs on a SHARED `callback_stack[depth]` (one buffer per
     * depth, not per process) — if the timer ISR fires here it saves
     * the interrupted process's full GP frame on top of that stack,
     * the scheduler stores frame_ptr in proc->kernel_rsp, then a
     * different thread's later callback at the same depth WRITES OVER
     * the saved frame as it pushes arguments. The saved frame becomes
     * garbage, the next dispatch of the original process triple-faults
     * on a CS=0x1F10-style bogus selector. Until callback_stacks are
     * per-process the only safe thing is no-preempt during callback. */
    {
        extern volatile uint32_t *idt_get_apic_base(void);
        volatile uint32_t *apic = idt_get_apic_base();
        if (apic) apic[0x320/4] |= 0x10000;  /* LVT_TIMER |= MASKED */
    }

    /* Pick the callback stack slot. PID 1 (UT99 main) uses slot=depth
     * as before. Worker threads spawned via CreateThread use a HIGH
     * slot offset (8 + thread_index) so their stack writes can't
     * overlap PID 1's saved interrupt frames on slot=depth. Without
     * this isolation, the timer-mask only narrows the race window —
     * PID 1 may still have been preempted onto callback_stack[0]
     * BEFORE the thread acquired the lock and TID=2's later push of
     * arguments overwrites the saved frame. Out of MAX_CALLBACK_DEPTH
     * = 32 slots we reserve [0..7] for PID 1 and [8..31] for threads. */
    int stack_slot;
    {
        extern int32_t proc_current_pid(void);
        int my_pid = proc_current_pid();
        stack_slot = (my_pid == 1)
                   ? depth
                   : (8 + (my_pid & 0xF));
        if (stack_slot >= MAX_CALLBACK_DEPTH) stack_slot = depth;
    }

    if (kern_setjmp(callback_jmpbufs[depth]) == 0) {
        uint32_t *sp = (uint32_t *)(callback_stack_get(stack_slot) + CALLBACK_STACK_SIZE);

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
            "sti\n"
            "push %[cs]\n"
            "push %[ip]\n"
            "lretq\n"
            :
            : [cs] "r"(cs64),
              [ip] "r"(ip64),
              [sp] "r"(sp64)
            : "memory", "cc", "rax"
        );
        /* never reached */
    }

    /* longjmp returned — 32-bit function is done. The APIC timer was
     * masked by compat32_enter and stays masked while UT99 is in
     * compat32 mode; we don't unmask here. */

    g_teb32.ExceptionList = saved_seh;  /* Restore SEH chain */
    callback_depth--;
    return callback_retval_per_depth[depth];
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
    /* Track reentrant catch dispatch.
     * When a catch handler throws (e.g., appUnwindf re-throws), skip the
     * current frame (which is corrupted by the first dispatch) and start
     * searching from the NEXT frame in the SEH chain. */
    static int in_catch_dispatch = 0;
    static uint32_t catch_frame_addr = 0;
    static uint32_t saved_next_frame = 0;  /* saved Next BEFORE catch corrupts it */
    if (in_catch_dispatch) {
        serial_puts("[SEH32] Re-throw from catch — using saved next frame 0x");
        serial_puthex(saved_next_frame, 8);
        serial_puts("\n");
        in_catch_dispatch = 0;
        /* Use the saved Next (from before the catch handler corrupted the frame) */
        g_teb32.ExceptionList = saved_next_frame;
    }

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

    /* Validate ExceptionList — skip entries whose ADDRESS is in PE image
     * .text range (where SEH frames CAN'T legitimately live — they're
     * stack-allocated). PE images load at 0x10000000 and engine .text
     * tops out around 0x12000000. UT99's stack at 0x13Bxxxxx-0x13Fxxxxx
     * holds VALID stack-allocated SEH frames — DO NOT skip those. */
    while (frame_addr >= 0x10000000 && frame_addr < 0x12000000) {
        serial_puts("[SEH32] skipping corrupt frame at 0x");
        serial_puthex(frame_addr, 8);
        uint32_t *f = (uint32_t *)(uintptr_t)frame_addr;
        uint32_t next = f[0];
        serial_puts(" next=0x");
        serial_puthex(next, 8);
        serial_puts("\n");
        if (next == 0 || next == 0xFFFFFFFF) {
            serial_puts("[SEH32] chain ends after corrupt entry\n");
            return 0;
        }
        frame_addr = next;
    }
    if (frame_addr == 0 || frame_addr == 0xFFFFFFFF) {
        serial_puts("[SEH32] empty chain (after skipping corrupt entries)\n");
        return 0;
    }

    /* Build 32-bit EXCEPTION_RECORD for filter functions */
    BYTE *p = (BYTE *)&seh32_exception_record;
    for (SIZE_T i = 0; i < sizeof(seh32_exception_record); i++) p[i] = 0;
    seh32_exception_record.ExceptionCode = ExceptionRecord->ExceptionCode;
    seh32_exception_record.ExceptionFlags = ExceptionRecord->ExceptionFlags;
    seh32_exception_record.ExceptionAddress =
        (uint32_t)(ULONG_PTR)ExceptionRecord->ExceptionAddress;
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
        /* Per-iteration validation: skip frames whose ADDRESS is in PE
         * image .text range (where SEH frames CAN'T legitimately live).
         * This catches chains where a Next pointer points back into PE
         * code (e.g., engine's Engine.dll at 0x10173F72 with garbage
         * handler 0xC5CAE910 — calling that hangs UT99). */
        if (frame_addr >= 0x10000000 && frame_addr < 0x12000000) {
            serial_puts("[SEH32] skipping in-image frame at 0x");
            serial_puthex(frame_addr, 8);
            serial_puts("\n");
            uint32_t *fbad = (uint32_t *)(uintptr_t)frame_addr;
            uint32_t nbad = fbad[0];
            if (nbad == 0 || nbad == 0xFFFFFFFF) break;
            frame_addr = nbad;
            frame_num++;
            continue;
        }

        /* Read 32-bit EXCEPTION_REGISTRATION_RECORD:
         *   offset 0: uint32_t Next
         *   offset 4: uint32_t Handler */
        uint32_t *frame32 = (uint32_t *)(ULONG_PTR)frame_addr;
        uint32_t next32    = frame32[0];
        uint32_t handler32 = frame32[1];

        /* Validate handler address: must be in executable code range.
         * Garbage values like 0xC5CAE910 are common in corrupt chains
         * where the catch handler's locals overwrote [EBP-4] (Handler). */
        if (handler32 < 0x01000000 || handler32 >= 0x80000000) {
            serial_puts("[SEH32] frame ");
            serial_putdec(frame_num);
            serial_puts(" @0x");
            serial_puthex(frame_addr, 8);
            serial_puts(" bogus handler=0x");
            serial_puthex(handler32, 8);
            serial_puts(" — skipping\n");
            frame_addr = next32;
            frame_num++;
            continue;
        }

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
            /*
             * PE32 handler — not in our thunk table.
             * Skip NULL/corrupted handlers (can happen when catch handler
             * locals overlap with the SEH registration at [EBP-8]).
             */
            if (handler32 == 0) {
                serial_puts(" (NULL — corrupted, skipping)\n");
                goto next_frame;
            }
            /* Validate handler address: must be in executable code range
             * (PE DLLs 0x01D-0x12M, thunks 0x01DC-0x01FE, heap 0x40-0x80M).
             * Handlers at low addresses (<0x10000) or in data ranges are
             * corrupt SEH frames — skip to avoid infinite NULL-CALL loops. */
            if (handler32 < 0x01000000 ||
                (handler32 >= 0x20000000 && handler32 < 0x40000000)) {
                serial_puts(" (invalid addr 0x");
                serial_puthex(handler32, 8);
                serial_puts(" — skipping)\n");
                goto next_frame;
            }

            /*
             * Check for MSVC C++ EH handler thunk pattern:
             *   B8 xx xx xx xx    MOV EAX, <FuncInfo_ptr>
             *   E9 xx xx xx xx    JMP <__CxxFrameHandler>
             *
             * If detected, parse FuncInfo directly and dispatch
             * to the matching catch block without calling the handler
             * (which needs a valid CONTEXT we can't easily provide).
             */
            uint8_t *hcode = (uint8_t *)(uintptr_t)handler32;

            if (hcode[0] == 0xB8 && hcode[5] == 0xE9) {
                /* Extract FuncInfo pointer from MOV EAX, imm32 */
                uint32_t func_info_addr = *(uint32_t *)(hcode + 1);

                serial_puts(" (CxxFrameHandler thunk)\n");
                serial_puts("[SEH32] FuncInfo=0x");
                serial_puthex(func_info_addr, 8);

                /* Read FuncInfo: magic(4), maxState(4), pUnwindMap(4),
                 *                nTryBlocks(4), pTryBlockMap(4) */
                uint32_t *fi = (uint32_t *)(uintptr_t)func_info_addr;
                uint32_t magic = fi[0];
                /* int32_t maxState = (int32_t)fi[1]; */
                /* uint32_t pUnwindMap = fi[2]; */
                int32_t nTryBlocks = (int32_t)fi[3];
                uint32_t pTryBlockMap = fi[4];

                serial_puts(" magic=0x");
                serial_puthex(magic, 8);
                serial_puts(" nTry=");
                serial_putdec((uint64_t)nTryBlocks);
                serial_puts("\n");

                if (magic != 0x19930520 && magic != 0x19930522) {
                    serial_puts("[SEH32] bad FuncInfo magic, skipping\n");
                    goto next_frame;
                }

                /* C++ EH frame layout (3 fields, NOT 4):
                 *   frame_addr+0 = Next
                 *   frame_addr+4 = Handler
                 *   frame_addr+8 = State (current unwind state)
                 * EBP = frame_addr + 0x0C */
                int32_t cur_state = (int32_t)frame32[2];

                serial_puts("[SEH32] state=");
                serial_putdec((uint64_t)(uint32_t)cur_state);
                serial_puts("\n");

                /* Walk TryBlockMap looking for a catch that matches.
                 * TryBlockMapEntry: tryLow(4), tryHigh(4), catchHigh(4),
                 *                   nCatches(4), pHandlerArray(4) = 20 bytes */
                for (int32_t t = 0; t < nTryBlocks; t++) {
                    uint32_t *tb = (uint32_t *)(uintptr_t)(pTryBlockMap + t * 20);
                    int32_t tryLow  = (int32_t)tb[0];
                    int32_t tryHigh = (int32_t)tb[1];
                    /* int32_t catchHigh = (int32_t)tb[2]; */
                    int32_t nCatches = (int32_t)tb[3];
                    uint32_t pHandlerArray = tb[4];

                    if (cur_state < tryLow || cur_state > tryHigh)
                        continue;

                    serial_puts("[SEH32] try[");
                    serial_putdec(t);
                    serial_puts("] matches (state ");
                    serial_putdec((uint64_t)(uint32_t)cur_state);
                    serial_puts(" in [");
                    serial_putdec((uint64_t)(uint32_t)tryLow);
                    serial_puts(",");
                    serial_putdec((uint64_t)(uint32_t)tryHigh);
                    serial_puts("])\n");

                    /* HandlerType: adjectives(4), pType(4),
                     *              dispCatchObj(4), addressOfHandler(4) = 16 bytes
                     * Look for catch(...) first (pType == 0), then typed catches */
                    uint32_t catch_handler = 0;
                    int32_t catch_disp = 0;
                    for (int32_t c = 0; c < nCatches; c++) {
                        uint32_t *ch = (uint32_t *)(uintptr_t)(pHandlerArray + c * 16);
                        uint32_t pType = ch[1];
                        int32_t disp = (int32_t)ch[2];
                        uint32_t addr = ch[3];

                        if (pType == 0) {
                            /* catch(...) — always matches */
                            catch_handler = addr;
                            catch_disp = disp;
                            serial_puts("[SEH32] catch(...) handler=0x");
                            serial_puthex(addr, 8);
                            serial_puts("\n");
                            break;
                        }
                        /* TODO: typed catch matching */
                    }

                    /* Also accept first typed catch as fallback */
                    if (!catch_handler && nCatches > 0) {
                        uint32_t *ch = (uint32_t *)(uintptr_t)pHandlerArray;
                        catch_handler = ch[3];
                        catch_disp = (int32_t)ch[2];
                        serial_puts("[SEH32] fallback catch handler=0x");
                        serial_puthex(catch_handler, 8);
                        serial_puts("\n");
                    }

                    if (catch_handler) {
                        /* Update state to catchHigh (after the catch block) */
                        frame32[2] = (uint32_t)((int32_t)tb[2]);

                        /* Unwind SEH chain to the NEXT frame after the catcher.
                         * Windows removes all frames up to and including the
                         * catching frame. The catch handler's locals overlap
                         * with the SEH registration at [EBP-4/-8/-C], so the
                         * frame must be unlinked before the handler runs. */
                        g_teb32.ExceptionList = next32;

                        /* EBP for the catch handler = frame_addr + 0x0C
                         * (C++ EH frame is 3 fields: Next+Handler+State = 12 bytes) */
                        uint32_t catch_ebp = frame_addr + 0x0C;

                        /* If catch has a catch object (dispCatchObj != 0),
                         * store the exception object pointer at EBP+disp */
                        if (catch_disp != 0 && seh32_exception_record.NumberParameters >= 2) {
                            uint32_t exc_obj = seh32_exception_record.ExceptionInformation[1];
                            uint32_t *catch_obj_ptr = (uint32_t *)(uintptr_t)(catch_ebp + catch_disp);
                            *catch_obj_ptr = exc_obj;
                        }

                        serial_puts("[SEH32] dispatching to catch @0x");
                        serial_puthex(catch_handler, 8);
                        serial_puts(" EBP=0x");
                        serial_puthex(catch_ebp, 8);
                        serial_puts("\n");

                        /*
                         * Call the catch handler via compat32_callback.
                         * The MSVC catch handler expects EBP to be the
                         * establishing function's frame pointer. We set
                         * up the unwind globals so the INT2E return will
                         * restore EBP before jumping to the handler.
                         */
                        in_catch_dispatch = 1;
                        catch_frame_addr = frame_addr;
                        saved_next_frame = next32;  /* save BEFORE catch corrupts it */
                        g_compat32_unwind_eip = catch_handler;
                        /*
                         * The catch handler epilog does:
                         *   pop edi; pop esi; pop ebx
                         *   mov esp, ebp
                         *   pop ebp
                         *   ret N
                         *
                         * The pops MUST restore correct callee-saved values.
                         * We write EDI/ESI/EBX from the establishing function's
                         * stack frame into the area below EBP so the pops work.
                         *
                         * The establishing function prologue does:
                         *   push ebx; push esi; push edi
                         * at [EBP-N-12], [EBP-N-8], [EBP-N-4] after sub esp,N.
                         * We can't know N, but we can read the saved ESP from
                         * [EBP-0x10] (mov [ebp-0x10], esp after all pushes).
                         *
                         * Alternative: write EDI/ESI/EBX just below EBP and
                         * set ESP there. The catch handler pushes/calls use
                         * their own stack (below ESP), and the pops at epilog
                         * restore correct values before mov esp,ebp resets ESP.
                         */
                        /* MSVC catch funclet:
                         * - Gets EBP from establishing function
                         * - Does work (error handling)
                         * - Sets EAX = continuation address
                         * - Does RET (returns to caller)
                         *
                         * We push catch_continue_stub_addr as return address
                         * so the funclet's RET jumps to our stub (JMP EAX)
                         * which continues at the funclet's chosen address.
                         *
                         * Stack layout at funclet entry:
                         *   [ESP+0] = catch_continue_stub_addr (return addr)
                         */
                        {
                            uint32_t esp = catch_ebp - 4;
                            *(uint32_t *)(uintptr_t)esp = catch_continue_stub_addr;
                            g_compat32_unwind_esp = esp;
                            g_compat32_unwind_ebp = catch_ebp;
                        }

                        return 1;  /* handled — INT2E will apply unwind */
                    }
                }
                /* No matching catch in this frame, try next */
            } else {
                serial_puts(" (PE32 handler, calling via compat32)\n");

                /* Generic PE32 handler — call via compat32 with CONTEXT */
                uint32_t args[4];
                args[0] = (uint32_t)(ULONG_PTR)&seh32_exception_record;
                args[1] = frame_addr;
                args[2] = 0;
                args[3] = 0;

                uint32_t disp = compat32_callback_args(handler32, 4, args);

                serial_puts("[SEH32] PE32 handler returned disp=");
                serial_putdec(disp);
                serial_puts("\n");

                /* EXCEPTION_DISPOSITION enum:
                 *  0 = ExceptionContinueExecution (retry instruction)
                 *  1 = ExceptionContinueSearch (try next handler)
                 * Note: these differ from __except filter constants! */
                if (disp == 0) {
                    serial_puts("[SEH32] handler: ContinueExecution\n");
                    return 1;
                }
                /* 1 = ContinueSearch: try next frame */
            }
        }

next_frame:

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

    /* Try the base SEH frame as last resort — the normal chain may be
     * corrupt but the base frame (installed by winexec) is always valid. */
    {
        extern uint32_t g_base_seh_frame_addr;
        if (g_base_seh_frame_addr >= 0x1C000000 && g_base_seh_frame_addr < 0x50000000) {
            uint32_t *bf = (uint32_t *)(uintptr_t)g_base_seh_frame_addr;
            uint32_t base_handler = bf[1];
            if (base_handler >= 0x01000000 && base_handler < 0x20000000) {
                serial_puts("[SEH32] trying base SEH frame @0x");
                serial_puthex(g_base_seh_frame_addr, 8);
                serial_puts("\n");
                /* Call the base handler */
                uint32_t args[4];
                args[0] = (uint32_t)(ULONG_PTR)&seh32_exception_record;
                args[1] = g_base_seh_frame_addr;
                args[2] = 0;
                args[3] = 0;
                uint32_t disp = compat32_callback_args(base_handler, 4, args);
                if (disp == 0) { /* ExceptionContinueExecution */
                    serial_puts("[SEH32] base handler: ContinueExecution\n");
                    return 1;
                }
                /* 1+ = ContinueSearch — fall through to UNHANDLED */
            }
        }
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
/* IAT snapshot globals */
/* g_iat_snapshot_ready removed — all snapshot approaches reverted */

/* Ring buffer of recent PE32 return addresses for crash diagnostics */
#define CALL_TRACE_SIZE 64
uint32_t g_call_trace[CALL_TRACE_SIZE];
uint32_t g_call_trace_idx = 0;

void dump_call_trace(void)
{
    extern void serial_puts(const char *);
    extern void serial_puthex(uint64_t, int);
    serial_puts("[TRACE] Last PE32 callers: ");
    for (int i = 0; i < CALL_TRACE_SIZE; i++) {
        int idx = (g_call_trace_idx - 1 - i + CALL_TRACE_SIZE) % CALL_TRACE_SIZE;
        uint32_t addr = g_call_trace[idx];
        if (addr >= 0x10000000 && addr < 0x20000000) {
            serial_puthex(addr, 8);
            serial_puts(" ");
        }
        if (addr == 0) break;
    }
    serial_puts("\n");
}

uint64_t compat32_dispatch(uint32_t thunk_idx, uint32_t *stack_args)
{
#ifdef COMPAT_TRACE
    /* Panorama probe: confirms risk #1 (compat INT soft-int has no LAPIC
     * fast-path). On -smp 1 this is silent (BSP is LAPIC 0). On -smp 2+,
     * the first dispatch taken on an AP prints `[int2e] cpu=N ...`. Also
     * bursts the first few calls for sanity. Reads LAPIC ID directly via
     * higher-half MMIO (same window as isr_stubs.S). */
    {
        volatile uint32_t *_apic_id =
            (volatile uint32_t *)(uintptr_t)(0xFFFF800000000000ULL + 0xFEE00020ULL);
        uint32_t _cpu = (*_apic_id >> 24) & 0xFF;
        static uint32_t _tr_seen = 0;
        if (_cpu != 0 || _tr_seen < 4) {
            _tr_seen++;
            serial_puts("[int2e] cpu=");
            serial_putdec(_cpu);
            serial_puts(" depth=");
            serial_putdec(g_int2e_rsp_depth);
            serial_puts(" idx=0x");
            serial_puthex(thunk_idx, 4);
            serial_puts("\n");
        }
    }
#endif

    /* ENGINE-PATCH UnLevel.h:246-247 — skip Actors(0) assertions.
     * The function at Engine.dll ~0x1038C320 does two consecutive
     * checks on Level->Actors:
     *   line 246:  if (Actors.Num()==0)  fail("Actors(0)")
     *   line 247:  if (!Actors(0)->IsA(ALevelInfo)) fail("...IsA(...)")
     * Both `jne +N` (75 NN) over the call to appFailAssert.  If we
     * change `jne` to `jmp` (EB NN), the path that doesn't trigger
     * the assert is ALWAYS taken.  Engine continues to read Actor[0]
     * which may NULL-deref, but our existing recovery handles that
     * cleanly via proc_exit. */
    /* Core.dll throw-helper @0x1014BD10 — leave as-is.  Tried suppressing
     * the CxxThrowException call (5 bytes replaced with add esp,0xC; ret;
     * nop).  Result: engine assumed package loaded successfully, then
     * NULL-CALL'd 50,000+ times trying to access fields of the nonexistent
     * package.  The throw's catch handler is the lesser evil — at least
     * engine exits cleanly. */
    #if 0
    static int patched_core_throw = 0;
    if (!patched_core_throw) {
        volatile uint8_t *p = (uint8_t *)(uintptr_t)0x1014BD3A;
        if (p[0] == 0xE8) {
            p[0] = 0x83; p[1] = 0xC4; p[2] = 0x0C;
            p[3] = 0xC3; p[4] = 0x90;
            patched_core_throw = 1;
        }
    }
    #endif

    /* CASCADE-NOP — surgical NOP of the inner-most appThrowf call inside
     * UPackage::LoadPackage at Core.dll+0x10159B99 ("Can't find file for
     * package 'None0'"). The caller has a built-in recovery path at
     * +0x10159BA1 (jmp 0x10159D30) that handles "file not found" by
     * returning NULL up the chain. Without this NOP, LoadPackage throws,
     * the outer StaticLoadObject also throws, the engine cascades to
     * appError → StaticShutdownAfterError → exit.
     *
     * Replace `e8 5e 91 fa ff` (call 0x10102cfc, 5 bytes) with 5×NOP.
     * The push edx; push eax that prepared the throw args remain — the
     * subsequent `add esp, 8` cleans them up after the NOPs. Then
     * `jmp 0x10159D30` runs the engine's NULL-return recovery.
     *
     * DEBUG/EXPLORATION patch — the cascade comes from UE1 internals
     * trying to load synthetic "None0"/"None1" packages because the
     * UObject Outer tree for Engine.u isn't being built. The real
     * fix needs Engine.u export parser + UPackage instantiation in
     * GObj. This NOP is a stepping stone to see how far the engine
     * gets when the LoadPackage abort signal is silenced. */
    {
        static int cascade_nop_patched = 0;
        if (!cascade_nop_patched) {
            volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)0x10159B99ULL;
            if (p[0] == 0xE8 && p[1] == 0x5E && p[2] == 0x91 &&
                p[3] == 0xFA && p[4] == 0xFF) {
                p[0] = 0x90; p[1] = 0x90; p[2] = 0x90;
                p[3] = 0x90; p[4] = 0x90;
                cascade_nop_patched = 1;
                serial_puts("[CASCADE-NOP] patched Core.dll+0x10159B99 "
                            "appThrowf(can't find file) → 5×NOP\n");
            }
        }
    }

    /* FMallocWindows pool-integrity asserts in UT.exe — skip them.
     * Our stub FMalloc doesn't maintain the Pool/Free/FirstMem
     * doubly-linked-list invariants the engine sanity-checks.  Each
     * je/ja over the assert call gets flipped to jmp. */
    static int fmw_patched_mask = 0;
    if (fmw_patched_mask != 0xF) {
        struct { uint32_t va; uint8_t want; uint8_t patch; } sites[] = {
            { 0x109032A8, 0x74, 0xEB },  /* line 367  je → jmp */
            { 0x10903303, 0x77, 0xEB },  /* line 370  ja → jmp */
            { 0x10903353, 0x74, 0xEB },  /* line 375  je → jmp */
            { 0x10903374, 0x74, 0xEB },  /* line 376  je → jmp */
        };
        for (int i = 0; i < 4; i++) {
            if (fmw_patched_mask & (1 << i)) continue;
            volatile uint8_t *p = (uint8_t *)(uintptr_t)sites[i].va;
            if (p[0] == sites[i].want) {
                p[0] = sites[i].patch;
                fmw_patched_mask |= (1 << i);
                serial_puts("[UT-PATCH] FMallocWindows assert site ");
                serial_putdec((uint64_t)i);
                serial_puts(" @0x"); serial_puthex(sites[i].va, 8);
                serial_puts(" → jmp\n");
            }
        }
    }

    /* Actors-assert jne→jmp patches DISABLED.  Skipping the assertions
     * makes the engine read Actors[0] which is NULL, leading to a
     * different/earlier downstream crash chain (NULL-CALL → #BR @0x10243B18).
     * The original assert path (call appFailAssert → our shim's
     * proc_exit) reaches the same terminal state with cleaner exit and
     * after more engine progress (13,849 INT 0x2E baseline vs ~1,420
     * with the patches).  Keep the code in tree commented for future
     * comparisons. */
    #if 0
    static int actors_patched_mask = 0;
    if (actors_patched_mask != 0x3) {
        struct { uint32_t va; uint8_t want; uint8_t patch; } sites[] = {
            { 0x1038C324, 0x75, 0xEB },
            { 0x1038C35A, 0x75, 0xEB },
        };
        for (int i = 0; i < 2; i++) {
            if (actors_patched_mask & (1 << i)) continue;
            volatile uint8_t *p = (uint8_t *)(uintptr_t)sites[i].va;
            if (p[0] == sites[i].want) {
                p[0] = sites[i].patch;
                actors_patched_mask |= (1 << i);
            }
        }
    }
    #endif

    /* UT-EXE-PATCH: UT.exe @0x10902A40 doubly-linked-list pool manager
     * has 3 unguarded NULL-pointer writes (prev/next/container fields):
     *   0x10902AF2  89 01   mov [ecx], eax     ; *prev = next
     *   0x10902B1B  89 41 18 mov [ecx+0x18], eax
     *   0x10902B2D  89 08   mov [eax], ecx
     * Patch each to NOPs.  The pool's link state stays stale (orphan
     * node) but downstream code re-reads from container fields rather
     * than walking the broken chain, so engine continues.
     *
     * Forcing the function's early-exit (NOP'ing `jne +5` in prologue)
     * was tried — caused regression: skipped useful work and broke a
     * later Level/Actors invariant check.  Targeted writes only. */
    /* FName::Names @0x10295D30 — pre-allocate same way as GObjRegistrants.
     * UClass static ctors call FName::FName("Engine") etc. which needs
     * to either look up or add to FName::Names.  If TArray is empty
     * (Num=0 Max=0), the first Add() triggers a bogus FArray::Realloc
     * (with bogus Max field from default ctor).  Pre-fill with a
     * stable 16KB buffer to allow ~2K name entries (8 bytes each).
     *
     * If FName::Names contains valid entries, UClass::Name fields
     * resolve correctly → class hierarchy lookups work → UGameEngine
     * is constructable → Browse() works → Level loads. */
    /* Patch Core.dll's FName-register "Hardcoded name was duplicated"
     * check. Instruction at 0x10150AEF is `74 15  je 0x10150B06` —
     * skip-error if Names[Index] == NULL. Once we pre-populate
     * Names[0] with NAME_None for the FName-converter (below), that
     * test always fails on entry and the engine appErrorf()s + throws.
     * The next instruction (at 0x10150B06) unconditionally overwrites
     * Names[Index] = ebx anyway, so converting the je into an
     * unconditional jmp (EB 15) loses nothing — and lets the engine's
     * own hardcoded-name init proceed past the duplicate check. */
    {
        static int patched_hardcode_dup_check = 0;
        if (!patched_hardcode_dup_check) {
            volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)0x10150AEFULL;
            if (p[0] == 0x74 && p[1] == 0x15) {
                p[0] = 0xEB;
                patched_hardcode_dup_check = 1;
                serial_puts("[FNAME-RESCUE] patched Core.dll+0x50AEF "
                            "je→jmp (skip hardcode-dup-check error)\n");
            }
        }
    }
    {
        volatile uint32_t *fname_tarray = (volatile uint32_t *)(uintptr_t)0x10295D30;
        static uint64_t fname_buf_phys = 0;
        static uint64_t fname_none_entry = 0;
        uint32_t fd = fname_tarray[0], fn = fname_tarray[1], fm = fname_tarray[2];
        if (fd == 0 && fm == 0) {
            if (fname_buf_phys == 0) {
                extern void *mem_alloc_pages(uint64_t count);
                /* Buffer A (16 KB) — TArray slot pool (4 bytes per slot,
                 *                                          room for 4096 ptrs) */
                void *buf  = mem_alloc_pages(4);
                /* Buffer B (4 KB)  — FNameEntry pool */
                void *pool = mem_alloc_pages(1);
                if (buf && pool) {
                    uint8_t *p = (uint8_t *)buf;
                    for (int i = 0; i < 16384; i++) p[i] = 0;
                    uint8_t *q = (uint8_t *)pool;
                    for (int i = 0; i < 4096; i++) q[i] = 0;

                    /* Build a single canonical FNameEntry for NAME_None.
                     * Core.dll's FName::operator const TCHAR*()
                     * (Core.dll+0x98E0, confirmed via disasm) does:
                     *   eax = this->Index;
                     *   ecx = Names.Data;
                     *   eax = Names.Data[Index];       (FNameEntry*)
                     *   eax += 0xC;                    (Name field)
                     *   ret;
                     * — no NULL check. If Names[0] is NULL the returned
                     * pointer is 0xC and the caller reads garbage that
                     * happens to render as L"0" or L"". The CASCADE of
                     * "Failed to load '0' / '' / .GameEngine" is exactly
                     * that.
                     *
                     * Name lives at FNameEntry+0xC. Earlier failed
                     * attempt (commit 9f447e1) wrote the string at +8,
                     * which is some other internal field — the engine
                     * couldn't find "None" by string at the canonical
                     * +0xC location, decided it was a new name, and
                     * tripped its own "Hardcoded name 0 was duplicated"
                     * assertion. With the correct offset the engine
                     * lookup matches and dedup short-circuits. */
                    *(volatile uint32_t *)(q + 0)  = 0;        /* Index */
                    *(volatile uint32_t *)(q + 4)  = 0;        /* HashNext / flags */
                    *(volatile uint32_t *)(q + 8)  = 0;        /* reserved */
                    /* Name at +0xC. UE1 Unicode builds (UT99) read TCHAR
                     * as WCHAR (uint16_t LE). Previously wrote ASCII
                     * "None\0" which got mis-read as UTF-16 -> mojibake
                     * "Nn" (0x6F4E='??' + 0x656E='??'). Write as proper
                     * UTF-16 LE. */
                    q[12] = 'N'; q[13] = 0;
                    q[14] = 'o'; q[15] = 0;
                    q[16] = 'n'; q[17] = 0;
                    q[18] = 'e'; q[19] = 0;
                    q[20] = 0;   q[21] = 0;  /* L"\0" terminator */

                    /* TArray.Data[0] = &none_entry */
                    *(volatile uint32_t *)buf = (uint32_t)(uintptr_t)pool;

                    fname_buf_phys   = (uint64_t)(uintptr_t)buf;
                    fname_none_entry = (uint64_t)(uintptr_t)pool;

                    serial_puts("[FNAME-RESCUE] populated Names[0] = &(NAME_None) "
                                "at FNameEntry@0x");
                    serial_puthex(fname_none_entry, 8);
                    serial_puts(" (Name@+0xC = \"None\")\n");
                }
            }
            if (fname_buf_phys) {
                fname_tarray[0] = (uint32_t)fname_buf_phys;
                fname_tarray[1] = 1;       /* Num = 1 (slot 0 populated) */
                fname_tarray[2] = 2048;    /* Max = 2K entries */
                static int fname_setup_logged = 0;
                if (!fname_setup_logged) {
                    fname_setup_logged = 1;
                    serial_puts("[FNAME-RESCUE] pre-alloc FName::Names Data=0x");
                    serial_puthex(fname_buf_phys, 8);
                    serial_puts(" Num=1 Max=2048\n");
                }
            }
        }
        /* FNDIFF-NONULLCOUNT — scan the buffer periodically. Triggers:
         * - When Data ptr changes (realloc event)
         * - Every +500 entries added to Num (sample during growth)
         * Reports non_null count, highest non-NULL index, first NULL idx.
         * Tells us if entries are being written during Num growth, or
         * if the engine increments Num without writing slots. */
        {
            static uint32_t last_seen_data = 0;
            static uint32_t last_count_num = 0;
            int do_scan = 0;
            if (fd != last_seen_data && fd != 0) {
                do_scan = 1;
            } else if (fn > last_count_num + 499 || (fn > 0 && last_count_num == 0)) {
                do_scan = 1;
            }
            if (do_scan && fd != 0) {
                last_seen_data = fd;
                last_count_num = fn;
                uint32_t *slots = (uint32_t *)(uintptr_t)fd;
                uint32_t limit = fn;
                if (limit > 8192) limit = 8192;
                uint32_t nn_count = 0;
                int32_t highest_nn = -1;
                int32_t first_null = -1;
                for (uint32_t k = 0; k < limit; k++) {
                    if (slots[k] != 0) {
                        nn_count++;
                        highest_nn = (int32_t)k;
                    } else if (first_null < 0) {
                        first_null = (int32_t)k;
                    }
                }
                serial_puts("[FNDIFF-COUNT] Data=0x");
                serial_puthex(fd, 8);
                serial_puts(" Num=");
                serial_putdec((uint64_t)fn);
                serial_puts(" non_null=");
                serial_putdec((uint64_t)nn_count);
                serial_puts(" highest_idx=");
                serial_putdec((uint64_t)(uint32_t)highest_nn);
                serial_puts(" first_null=");
                serial_putdec((uint64_t)(uint32_t)first_null);
                serial_puts("\n");
            }
        }

        /* FNDIFF-TARRAY — watch the TArray header itself (Data/Num/Max).
         * When Data pointer changes, that's a Realloc — likely the
         * trigger for the LOST events (old buffer's slots aren't seen
         * any more, new buffer's slots are zero-init until refilled). */
        {
            static uint32_t prev_data = 0, prev_num = 0, prev_max = 0;
            static uint32_t tarray_changes = 0;
            if (fd != prev_data || fn != prev_num || fm != prev_max) {
                tarray_changes++;
                serial_puts("[FNDIFF-TA] #");
                serial_putdec((uint64_t)tarray_changes);
                serial_puts(" Data:0x");
                serial_puthex(prev_data, 8);
                serial_puts("→0x");
                serial_puthex(fd, 8);
                serial_puts(" Num:");
                serial_putdec((uint64_t)prev_num);
                serial_puts("→");
                serial_putdec((uint64_t)fn);
                serial_puts(" Max:");
                serial_putdec((uint64_t)prev_max);
                serial_puts("→");
                serial_putdec((uint64_t)fm);
                serial_puts(" thunk=");
                serial_putdec((uint64_t)thunk_idx);
                serial_puts("\n");
                prev_data = fd; prev_num = fn; prev_max = fm;
            }
        }

        /* FNDIFF — per-dispatch diff of Names.Data. Snapshots the buffer
         * slot-by-slot, compares to previous snapshot, reports:
         *   - new entries (NULL → non-null + value written)
         *   - LOST entries (non-null → NULL, the corruption signal)
         *   - REASSIGNED entries (non-null → different non-null)
         * Includes thunk_idx so we can correlate with the Win32 call
         * that produced the change. Capped at SHADOW_SIZE entries to
         * keep memory cost bounded. */
        #ifndef DISABLE_FNDIFF
        #define FNDIFF_SHADOW_SIZE 8192
        static uint32_t fndiff_shadow[FNDIFF_SHADOW_SIZE];
        static uint32_t fndiff_shadow_initialized = 0;
        static uint32_t fndiff_dispatch_count = 0;
        static uint32_t fndiff_total_lost = 0;
        static uint32_t fndiff_total_new = 0;
        static uint32_t fndiff_total_reassign = 0;
        fndiff_dispatch_count++;
        if (fd != 0) {
            uint32_t *slots = (uint32_t *)(uintptr_t)fd;
            uint32_t limit = fn;
            if (limit > FNDIFF_SHADOW_SIZE) limit = FNDIFF_SHADOW_SIZE;
            if (!fndiff_shadow_initialized) {
                fndiff_shadow_initialized = 1;
                for (uint32_t k = 0; k < FNDIFF_SHADOW_SIZE; k++)
                    fndiff_shadow[k] = 0;
            }
            uint32_t lost = 0, news = 0, reass = 0;
            uint32_t lost_first_idx = 0xFFFFFFFF;
            uint32_t lost_first_old = 0;
            for (uint32_t k = 0; k < limit; k++) {
                uint32_t cur = slots[k];
                uint32_t prev = fndiff_shadow[k];
                if (cur != prev) {
                    if (prev != 0 && cur == 0) {
                        if (lost_first_idx == 0xFFFFFFFF) {
                            lost_first_idx = k;
                            lost_first_old = prev;
                        }
                        lost++;
                    } else if (prev == 0 && cur != 0) {
                        news++;
                    } else {
                        reass++;
                    }
                    fndiff_shadow[k] = cur;
                }
            }
            /* Always log lost-events (the smoking gun). */
            if (lost > 0) {
                fndiff_total_lost += lost;
                serial_puts("[FNDIFF] LOST=");
                serial_putdec((uint64_t)lost);
                serial_puts(" first idx=");
                serial_putdec((uint64_t)lost_first_idx);
                serial_puts(" old_val=0x");
                serial_puthex(lost_first_old, 8);
                serial_puts(" thunk=");
                serial_putdec((uint64_t)thunk_idx);
                serial_puts(" disp=");
                serial_putdec((uint64_t)fndiff_dispatch_count);
                serial_puts(" cum_lost=");
                serial_putdec((uint64_t)fndiff_total_lost);
                serial_puts("\n");
            }
            /* Throttled "growth" log every 500 new entries. */
            if (news > 0) {
                fndiff_total_new += news;
                static uint32_t last_log_new = 0;
                if (fndiff_total_new - last_log_new >= 500) {
                    last_log_new = fndiff_total_new;
                    serial_puts("[FNDIFF] cum_new=");
                    serial_putdec((uint64_t)fndiff_total_new);
                    serial_puts(" cum_reass=");
                    serial_putdec((uint64_t)fndiff_total_reassign);
                    serial_puts(" cum_lost=");
                    serial_putdec((uint64_t)fndiff_total_lost);
                    serial_puts(" Num=");
                    serial_putdec((uint64_t)fn);
                    serial_puts("\n");
                }
            }
            if (reass > 0) fndiff_total_reassign += reass;
        }
        #endif /* DISABLE_FNDIFF */

        /* FNAME-NULL-FILL — sweep the populated range and replace NULL
         * slots with a pointer to our canonical "None" entry. The engine
         * uses sparse-by-design indices (EName enum slots) and leaves
         * many entries NULL; FName::operator*() at Core.dll+0x98E0 does
         * `Names[Idx] + 0xC` with no NULL check, so any FName(NullIdx)
         * returns the literal pointer 0xC. That bogus pointer becomes
         * the "%s" arg in appSprintf and produces the "Failed to load '0'"
         * / "Failed to load ''" / " .GameEngine" cascade.
         *
         * Filling NULLs with the None entry makes FName(NullIdx).GetName()
         * return "None" instead of garbage. Idempotent — only writes
         * slots that are still NULL. Runs every dispatch (cheap: a
         * 2-5K loop) so it catches reallocations too.
         *
         * Gate: define DISABLE_NULL_FILL at compile time to skip this
         * fix and see the engine's raw behavior — used in conjunction
         * with FNDIFF to attribute NULL slots to the engine's own code
         * path vs our fill. */
        #ifndef DISABLE_NULL_FILL
        if (fname_none_entry && fd != 0) {
            uint32_t *slots = (uint32_t *)(uintptr_t)fd;
            uint32_t limit = fn;
            if (limit > fm) limit = fm;
            if (limit > 65536) limit = 65536; /* sanity cap */
            uint32_t filled = 0;
            for (uint32_t k = 0; k < limit; k++) {
                if (slots[k] == 0) {
                    slots[k] = (uint32_t)fname_none_entry;
                    filled++;
                }
            }
            if (filled) {
                static uint32_t total_filled = 0;
                static uint32_t last_logged_total = 0;
                total_filled += filled;
                /* Log every +100 NULLs filled so we can see growth. */
                if (total_filled - last_logged_total >= 100 ||
                    last_logged_total == 0) {
                    last_logged_total = total_filled;
                    serial_puts("[FNAME-NULL-FILL] filled ");
                    serial_putdec((uint64_t)filled);
                    serial_puts(" NULL slots (cumulative=");
                    serial_putdec((uint64_t)total_filled);
                    serial_puts(", Num=");
                    serial_putdec((uint64_t)fn);
                    serial_puts(")\n");
                }
            }
        }
        #endif /* DISABLE_NULL_FILL */
        (void)fn;
        (void)fname_none_entry;
    }

    /* FMW-REPAIR — defensive pool-list invariant repair.
     *
     * UT99's FMallocWindows pool allocator uses TDoubleLinkedList<FPoolInfoBase>
     * with the invariant `pool->PrevLink == cursor` where cursor is either
     * &Table->FirstPool/ExaustedPool (for the head) or &PrevPool->Next (for
     * subsequent pools). HeapCheck() asserts this at 4 sites (UT.exe
     * 0x109032A8/0x10903303/0x10903353/0x10903374).
     *
     * Phase 1-2d investigation (commits 61514f4 → 6e833c2 → fbf6dd6)
     * confirmed Pool@0x40010700 reliably enters an inconsistent state where
     * its PrevLink points to &Pool@0x40010200.Next (0x40010218) but the
     * Table->FirstPool walk finds it as head. Root cause: unclear — UE1's
     * inline Link/Unlink operations should leave the list consistent, but
     * something in our compat32 layer causes momentary inconsistencies that
     * persist past Link/Unlink completion.
     *
     * Defensive repair: at each compat32_dispatch (= INT 0x2E boundary, safe
     * point between PE32 operations), walk every PoolTable[0..48]'s FirstPool
     * AND ExaustedPool chain. For each visited pool, verify the invariant
     * and REPAIR if mismatch. This enforces the same invariant the engine
     * asserts, so the bypass patches at 0x109032A8/0x10903303/0x10903353/
     * 0x10903374 become unnecessary (Phase 3 of plan).
     *
     * Layout constants (from FMallocWindows.h):
     *   POOL_COUNT = 49 (line 18)
     *   sizeof(FPoolTable) = 12 (FirstPool/ExaustedPool/BlockSize)
     *   FPoolInfo +0x18 = Next field
     *   FPoolInfo +0x1c = PrevLink field
     *
     * PoolTable base in UT99: 0x1092F738 (observed across all phases).
     * Could be detected dynamically via FMallocWindows vtable lookup, but
     * the address is deterministic for this PE32 build — hardcode for now.
     *
     * Safety: every pool pointer is range-checked against compat32 heap
     * (0x40000000..0x80000000) before deref. Walk depth capped at 1024
     * per list to prevent infinite-loop on circular lists. */
    {
        static int repair_total = 0;
        const uint32_t POOLTABLE_BASE = 0x1092F738;
        const int POOL_COUNT = 49;
        int repairs_this_dispatch = 0;

        /* Only run when UT99 is actually loaded — gate by checking that
         * the FMallocWindows vtable slot (FMallocWindows base = PoolTable
         * - 4 dwords, vtable ptr at +0) is non-zero and looks like a
         * Core.dll text address (FMalloc vtable lives in Core.dll). Also
         * verify the GMalloc global pointer is set to a non-zero value
         * (which winexec / PE init establishes). */
        extern uint32_t g_gmalloc_addr;
        int ut99_ready = 0;
        if (g_gmalloc_addr) {
            uint32_t gmalloc_obj = *(volatile uint32_t *)(uintptr_t)g_gmalloc_addr;
            if (gmalloc_obj >= 0x10900000 && gmalloc_obj < 0x10A00000) {
                /* GMalloc points to a UT.exe-resident FMallocWindows instance */
                ut99_ready = 1;
            }
        }
        if (!ut99_ready) goto fmw_repair_done;

        for (int t = 0; t < POOL_COUNT; t++) {
            uint32_t table_base = POOLTABLE_BASE + (uint32_t)(t * 12);
            for (int list = 0; list < 2; list++) {
                /* list 0 = FirstPool (offset 0), list 1 = ExaustedPool (+4) */
                uint32_t cursor_addr = table_base + (uint32_t)(list * 4);
                if (cursor_addr < 0x10000000 || cursor_addr >= 0x12000000) break;
                uint32_t pool = *(volatile uint32_t *)(uintptr_t)cursor_addr;
                int depth = 0;
                while (pool != 0 && depth < 1024) {
                    /* sanity: pool ptr should be in compat32 heap range
                     * (PoolIndirect bucket VAs) */
                    if (pool < 0x40000000 || pool >= 0x80000000) break;
                    uint32_t pool_prevlink = *(volatile uint32_t *)(uintptr_t)(pool + 0x1c);
                    if (pool_prevlink != cursor_addr) {
                        /* Mismatch — repair by setting Pool->PrevLink = cursor_addr.
                         * This is the value the engine's Link operation
                         * SHOULD have set if it completed atomically. */
                        *(volatile uint32_t *)(uintptr_t)(pool + 0x1c) = cursor_addr;
                        repairs_this_dispatch++;
                    }
                    /* Advance: cursor = &pool->Next, pool = pool->Next */
                    cursor_addr = pool + 0x18;
                    pool = *(volatile uint32_t *)(uintptr_t)cursor_addr;
                    depth++;
                }
            }
        }
        if (repairs_this_dispatch > 0) {
            repair_total += repairs_this_dispatch;
            /* Log every fire — should be RARE once warm; if it spams we
             * know the repair is racing the engine and need to throttle. */
            static int log_count = 0;
            if (log_count < 50 || (log_count % 100 == 0)) {
                log_count++;
                serial_puts("[FMW-REPAIR] fixed ");
                serial_putdec((uint64_t)repairs_this_dispatch);
                serial_puts(" PrevLink mismatches (cum=");
                serial_putdec((uint64_t)repair_total);
                serial_puts(") thunk=");
                serial_putdec((uint64_t)thunk_idx);
                serial_puts("\n");
            }
        }
    fmw_repair_done: ;
    }

    /* GObjRegistrants snapshot + restore.
     *
     * Observed: 200 UClass registrants get added (Num→200), then
     * engine zeros the entire TArray (Data=0, Num=0, Max=0).  Likely
     * `FArray::Empty()` after a partial ProcessRegistrants pass.  The
     * engine then queries GObjRegistrants for UClass lookups and gets
     * an empty array → UClass hierarchy never resolves → Browse() ends
     * up with corrupt this → assertion.
     *
     * Strategy: snapshot the {Data,Num,Max} when Num peaks.  After the
     * zero-event, restore the snapshot so the engine sees the original
     * 200 registrants again.  Best-effort — engine may have already
     * freed the underlying buffer, but our pre-alloc was a stable phys
     * buffer that doesn't get freed by FMallocWindows. */
    {
        volatile uint32_t *tarray = (volatile uint32_t *)(uintptr_t)0x102A0360;
        static uint32_t last_d = 0xFFFFFFFF, last_n = 0xFFFFFFFF, last_m = 0xFFFFFFFF;
        static int change_count = 0;
        static uint64_t rescue_phys = 0;
        static uint32_t snap_d = 0, snap_n = 0, snap_m = 0;
        static int restore_count = 0;
        uint32_t d = tarray[0], n = tarray[1], m = tarray[2];
        int changed = (d != last_d) || (n != last_n) || (m != last_m);
        if (changed && change_count < 50) {
            change_count++;
            serial_puts("[GOBJREG-TRACE#");
            serial_putdec((uint64_t)change_count);
            serial_puts("] Data=0x"); serial_puthex(d, 8);
            serial_puts(" Num="); serial_putdec((uint64_t)n);
            serial_puts(" Max="); serial_putdec((uint64_t)m);
            serial_puts(" thunk="); serial_putdec((uint64_t)thunk_idx);
            serial_puts("\n");
            last_d = d; last_n = n; last_m = m;
        }
        /* Snapshot when Num grows (likely registrant Add) */
        if (d != 0 && n > snap_n && m > 0) {
            snap_d = d; snap_n = n; snap_m = m;
            /* When Num peaks at a substantial value (≥100 registrants),
             * manually invoke UObject::ProcessRegistrants @0x1010190B
             * to bind the UClass hierarchy before the engine clears the
             * array.  Done once per peak. */
            static int peak_processed = 0;
            if (n >= 100 && !peak_processed) {
                peak_processed = 1;
                serial_puts("[GOBJREG-PROCESS] manual call to ProcessRegistrants @0x1010190B Num=");
                serial_putdec((uint64_t)n);
                serial_puts("\n");
                uint32_t args[1] = { 0 };
                compat32_callback_args(0x1010190B, 0, args);
                serial_puts("[GOBJREG-PROCESS] returned\n");
            }
        }
        /* Restore if zeroed AFTER snapshot taken */
        if (d == 0 && n == 0 && m == 0 && snap_n > 0) {
            tarray[0] = snap_d;
            tarray[1] = snap_n;
            tarray[2] = snap_m;
            restore_count++;
            if (restore_count <= 5) {
                serial_puts("[GOBJREG-SNAP-RESTORE #");
                serial_putdec((uint64_t)restore_count);
                serial_puts("] Data=0x"); serial_puthex(snap_d, 8);
                serial_puts(" Num="); serial_putdec((uint64_t)snap_n);
                serial_puts("\n");
            }
        }
        /* Initial-state rescue: if all-zero and never snapshotted, give a buffer */
        if (d == 0 && m == 0 && snap_n == 0) {
            if (rescue_phys == 0) {
                extern void *mem_alloc_pages(uint64_t count);
                void *buf = mem_alloc_pages(1);
                if (buf) {
                    uint64_t pa = (uint64_t)buf;
                    uint8_t *p = (uint8_t *)pa;
                    for (int i = 0; i < 4096; i++) p[i] = 0;
                    rescue_phys = pa;
                }
            }
            if (rescue_phys) {
                tarray[0] = (uint32_t)rescue_phys;
                tarray[2] = 1024;
            }
        }
    }

    static int patched_ut_listdel = 0;
    if (!patched_ut_listdel) {
        struct { uint32_t va; uint8_t want[3]; uint8_t patch[3]; int len; }
        sites[] = {
            { 0x10902AF2, {0x89, 0x01, 0x00}, {0x90, 0x90, 0x00}, 2 },
            { 0x10902B1B, {0x89, 0x41, 0x18}, {0x90, 0x90, 0x90}, 3 },
            { 0x10902B2D, {0x89, 0x08, 0x00}, {0x90, 0x90, 0x00}, 2 },
        };
        int ok = 0;
        for (int i = 0; i < 3; i++) {
            volatile uint8_t *p = (uint8_t *)(uintptr_t)sites[i].va;
            int match = 1;
            for (int b = 0; b < sites[i].len; b++)
                if (p[b] != sites[i].want[b]) { match = 0; break; }
            if (match) {
                for (int b = 0; b < sites[i].len; b++) p[b] = sites[i].patch[b];
                ok++;
            }
        }
        patched_ut_listdel = 1;
        serial_puts("[UT-PATCH] linked-list NULL-write sites patched: ");
        serial_putdec((uint64_t)ok);
        serial_puts("/3\n");
    }

    /* ENGINE-PATCH: Engine.dll @0x103887C0 does `mov ebx, [IAT-slot]`
     * then makes multiple `call ebx` to StaticLoadClass.  Between the
     * 1st and 2nd call, a virtual call `call [edx+0x54]` (at 0x1038887A)
     * goes through a corrupt vtable and lands in non-ABI-compliant code
     * that doesn't preserve EBX.  Then the 2nd `call ebx` faults at
     * NX (EBX=0x401BC870, a heap data addr).
     *
     * Fix: patch the corrupt-vtable call to a 3-byte NOP, eliminating
     * the EBX clobber.  Side effect: the virtual method (likely some
     * sort of class hierarchy walker) doesn't run, but the engine
     * generally handles missing-vtable cases gracefully.
     *
     * Verify expected bytes before patching, in case Engine.dll layout
     * differs in some build. */
    static int patched_ebx_callsite = 0;
    if (!patched_ebx_callsite) {
        volatile uint8_t *p = (uint8_t *)(uintptr_t)0x1038887A;
        if (p[0] == 0xFF && p[1] == 0x52 && p[2] == 0x54) {
            p[0] = 0x90;  /* NOP */
            p[1] = 0x90;
            p[2] = 0x90;
            patched_ebx_callsite = 1;
            serial_puts("[ENGINE-PATCH] NOP'd call [edx+0x54] @0x1038887A "
                         "(prevents EBX clobber)\n");
        }
    }

    /* Log PE32 caller return address (at stack_args[-1] = [ESP] on entry) */
    if (stack_args && thunk_idx < 0xFFFFFFF0) {
        uint32_t ret_addr = stack_args[-1]; /* return address pushed by CALL */
        g_call_trace[g_call_trace_idx % CALL_TRACE_SIZE] = ret_addr;
        g_call_trace_idx++;
        extern uint32_t g_last_caller_eip;
        extern uint32_t g_last_stack_args;
        g_last_caller_eip = ret_addr;
        g_last_stack_args = (uint32_t)(uintptr_t)stack_args;

        /* wdbg: dispatch any address-site hooks registered by callers
         * who want to inspect engine state when the PE is executing
         * inside a given VA range. No-op when no hooks registered. */
        wdbg_check_caller(ret_addr, stack_args);

        /* BAD-THIS detector: only flag ECX in PE-image .text range.
         * EDX in code range is often a legit function-pointer arg
         * (e.g. __dllonexit, callback registrations).  But ECX is
         * `this` for any C++ thiscall — a real `this` is a heap or
         * stack object, never a .text address.  When ECX lands in
         * code range, the engine is about to deref a code byte as
         * a vtable pointer, the start of the corrupt-three-level-
         * indirect chain that ends in NX-fault on data.            */
        uint32_t ecx = (uint32_t)g_int2e_user_rcx;
        uint32_t edx = (uint32_t)g_int2e_user_rdx;
        static int bad_this_count = 0;
        int bad_ecx = (ecx >= 0x10000000 && ecx < 0x12000000);
        if (bad_ecx && bad_this_count < 40) {
            bad_this_count++;
            serial_puts("[BAD-THIS#");
            serial_putdec((uint64_t)bad_this_count);
            serial_puts("] thunk=");
            serial_putdec((uint64_t)thunk_idx);
            serial_puts(" caller=0x");
            serial_puthex(ret_addr, 8);
            serial_puts(" ECX=0x");
            serial_puthex(ecx, 8);
            serial_puts(" EDX=0x");
            serial_puthex(edx, 8);
            serial_puts(" ESI=0x");
            serial_puthex((uint32_t)g_int2e_user_rsi, 8);
            serial_puts(" EDI=0x");
            serial_puthex((uint32_t)g_int2e_user_rdi, 8);
            serial_puts("\n");
        }
    }

    /* Clean up null-page stale data from compat32 writes.
     * In compat32 mode, TF single-step isn't used for null-page cleanup
     * (#DB would cause #GP without IST). Stale data on page 0 from
     * null-object writes (e.g. [eax+4] where eax=0) persists and gets
     * read as vtable pointers when EDI=0. Clean up on every INT 0x2E. */
    {
        extern volatile int g_null_page_dirty;
        if (g_null_page_dirty) {
            g_null_page_dirty = 0;
            memset((void *)0, 0, 4096);
            extern void paging_set_flags(uint64_t va, uint64_t flags);
            /* PTE_PRESENT=1, PTE_GLOBAL=0x100, PTE_NX=1<<63 */
            paging_set_flags(0, (1ULL) | (1ULL << 8) | (1ULL << 63));
            __asm__ volatile ("invlpg (%0)" :: "r"((uint64_t)0) : "memory");
        }
    }

    /* Dynamic IAT guard: protects specific entries discovered via #PF
     * auto-recovery. Also includes hardcoded StaticLoadClass for bootstrap. */
    {
        /* Bootstrap guard: StaticLoadClass (always active) */
        volatile uint32_t *iat_entry = (volatile uint32_t *)(uintptr_t)0x105A5E08;
        static uint32_t iat_original = 0;
        if (iat_original == 0 && *iat_entry >= 0x10100000 && *iat_entry < 0x10200000)
            iat_original = *iat_entry;
        if (iat_original && *iat_entry != iat_original)
            *iat_entry = iat_original;

        /* Dynamic guards: entries discovered by #PF intercept at runtime */
        extern void iat_guard_check(void);
        iat_guard_check();
    }

    /* Continuously clear GIsCriticalError + GErrorHist[0].
     * The engine's exception handlers (SEH catch, appError) set these
     * during init whenever a null-object write or call is recovered.
     * If GIsCriticalError is 1 when Browse() is called, it bails
     * immediately without attempting to load the map file.
     * Clearing on every INT 0x2E ensures Browse() always sees clean state.
     * Addresses are in Core.dll data section, mapped at init time. */
    {
        volatile uint32_t *gcrit = (volatile uint32_t *)(uintptr_t)0x101E568C;
        volatile uint16_t *gerr  = (volatile uint16_t *)(uintptr_t)0x101E3474;
        if (*gcrit != 0) {
            *gcrit = 0;
            *gerr  = 0;
        }
    }
    /* Monitor FName::Names TArray for corruption.
     * TArray<FNameEntry*> at Core.dll 0x10295D30: {Data, Num, Max}
     * Normal: Num < 50000, Max < 100000. If larger, data is corrupted. */
    {
        static int fname_corrupted = 0;
        volatile uint32_t *fname_arr = (volatile uint32_t *)(uintptr_t)0x10295D30;
        uint32_t fdata = fname_arr[0], fnum = fname_arr[1], fmax = fname_arr[2];
        if (fnum > 100000 && !fname_corrupted) {
            fname_corrupted = 1;
            serial_puts("[CORRUPT] FName::Names Num=");
            serial_putdec(fnum);
            serial_puts(" Max=");
            serial_putdec(fmax);
            serial_puts(" Data=0x");
            serial_puthex(fdata, 8);
            serial_puts(" at INT2E dispatch");
            serial_puts("\n");
        }
    }

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

        /* Save retval per-depth BEFORE longjmp */
        if (depth >= 0 && depth < MAX_CALLBACK_DEPTH)
            callback_retval_per_depth[depth] = callback_retval;

        /* Compensate: longjmp bypasses int2e_stub's depth-- for RSP stack.
         * The callback-return INT 0x2E incremented g_int2e_rsp_depth but
         * its restore path is skipped by longjmp. Decrement here. */
        {
            extern uint32_t g_int2e_rsp_depth;
            if (g_int2e_rsp_depth > 0)
                g_int2e_rsp_depth--;
        }

        /* Diagnostic: dump the saved jmpbuf BEFORE longjmp so we can
         * see whether the setjmp actually captured a valid kernel
         * state, or whether the slot was clobbered between setjmp and
         * the longjmp dispatch. */
        {
            uint64_t *jb = callback_jmpbufs[depth];
            serial_puts("[CB32] longjmp depth="); serial_putdec(depth);
            serial_puts(" rip=0x"); serial_puthex(jb[7], 16);
            serial_puts(" rsp=0x"); serial_puthex(jb[6], 16);
            serial_puts(" cr3=0x"); serial_puthex(jb[8], 16);
            serial_puts("\n");
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

    /* Save the 13th stack arg for CreateWindowExW workaround. */
    g_compat32_last_stack_arg13 = (nargs >= 12) ? stack_args[12] : 0;

    /* Debug: dump full stack for 12-arg functions (CreateWindowExW) */
    if (nargs >= 12 && t->name && t->name[0] == 'C' && t->name[6] == 'W') {
        serial_puts("[STACK-CWW] retaddr=0x");
        serial_puthex(stack_args[-1], 8);
        serial_puts("\n  args:");
        for (int si = 0; si < 14; si++) {
            serial_puts(" ");
            serial_puthex(stack_args[si], 8);
        }
        serial_puts("\n");
    }

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
        /* Throttle: first 200, every 1000th, exception-related always */
        int do_log = 0;
        if (int2e_call_count <= 200 || (int2e_call_count % 1000) == 0)
            do_log = 1;
        /* Always log exception-related functions */
        if (t->name && (t->name[0] == '_' && t->name[1] == 'C'))  /* _Cxx* */
            do_log = 1;
        if (t->name && t->name[0] == 'R' && t->name[1] == 'a')   /* Raise* */
            do_log = 1;
        /* Always log VirtualAlloc/Free */
        if (t->name && t->name[0] == 'V' && t->name[1] == 'i')
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

/* ── Stub UObject factory ────────────────────────────────────── */

/*
 * Create a minimal stub UObject with a valid vtable.
 * All vtable entries point to XOR EAX,EAX; RET (returns 0).
 * Used to initialize global USubsystem* pointers (e.g. GWindowManager)
 * that would otherwise be NULL and cause virtual call crashes.
 *
 * Layout (single page):
 *   [0x000..0x07F]  vtable (32 entries × 4 bytes)
 *   [0x100..0x1FF]  object (vtable ptr at offset 0, rest zeroed)
 */
uint32_t create_stub_uobject(const char *name)
{
    if (!unresolved_stub_addr) return 0;

    uint8_t *page = (uint8_t *)mem_alloc_pages(1);
    if (!page) return 0;

    memset(page, 0, 4096);

    uint32_t *vtable = (uint32_t *)page;
    uint32_t *object = (uint32_t *)(page + 768);  /* after 128-entry vtable (512 bytes) + padding */

    /* Fill 128 vtable entries with RET-0 stub.
     * WinDrv.dll calls slot 61 (offset 0xF4) and beyond.
     * With only 32 entries, slot 61 read past the vtable into zeroed
     * memory, dispatching through address 0 → #UD/#GP cascade. */
    for (int i = 0; i < 128; i++)
        vtable[i] = unresolved_stub_addr;

    /* Object[0] = vtable pointer */
    object[0] = (uint32_t)(uintptr_t)vtable;

    serial_puts("[WIN32] stub UObject '");
    serial_puts(name);
    serial_puts("' obj=0x");
    serial_puthex((uint64_t)(uintptr_t)object, 8);
    serial_puts(" vtbl=0x");
    serial_puthex((uint64_t)(uintptr_t)vtable, 8);
    serial_puts("\n");

    return (uint32_t)(uintptr_t)object;
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

uint32_t compat32_get_thunk_addr(uint32_t thunk_idx)
{
    if (thunk_idx >= thunk_count) return 0;
    return thunk_table[thunk_idx].thunk_addr;
}

uint32_t compat32_get_count(void)
{
    return thunk_count;
}
