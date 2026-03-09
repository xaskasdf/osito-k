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

/* ── Thunk state ─────────────────────────────────────────────── */

#define THUNK_STUB_SIZE  64      /* bytes per thunk stub */
#define THUNKS_PER_PAGE  (4096 / THUNK_STUB_SIZE)  /* 64 */
#define THUNK_POOL_PAGES 8       /* 8 pages = 512 thunks */

static uint8_t *thunk_pool = NULL;
static uint32_t thunk_count = 0;

static compat32_thunk_t thunk_table[COMPAT32_MAX_THUNKS];

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

static void emit_thunk(uint8_t *code, uint64_t target, uint8_t num_args)
{
    int p = 0;

    /*
     * Strategy: generate a small 64-bit stub that:
     *   1. Saves the stack frame
     *   2. Reads cdecl args from the old stack
     *   3. Sets up ms_abi registers + shadow space
     *   4. Calls the 64-bit target
     *   5. Returns to caller
     *
     * On the test harness, this runs as 64-bit code directly.
     * On OsitoK bare metal, the INT 0x2E path handles mode switching
     * so the thunk itself is still 64-bit code placed in the IAT.
     *
     * For bare-metal compat mode, each IAT entry actually points to a
     * small 32-bit stub that does:
     *   push <syscall_number>
     *   int 0x2E
     *   ret <N*4>
     * And the INT 0x2E handler on the kernel side does the marshaling.
     * This is the Windows WoW64/ntdll model.
     */

#ifdef TEST_HARNESS
    /*
     * Test harness path: PE32 runs in 64-bit mode, IAT has 64-bit ptrs
     * truncated to 32 bits. The PE32 code does CALL [IAT] which jumps here.
     * We're in 64-bit mode, so we can just do the register marshaling.
     *
     * cdecl on x86-64: args are still on stack (PE32 code pushes them).
     * Since PE32 code uses 32-bit PUSH, they're at [RSP+8], [RSP+12], etc.
     * (after the 4-byte return address, zero-extended to 8 bytes by CPU).
     *
     * Actually: on x86-64 host, the PE32 code is loaded but runs as 64-bit.
     * The CALL [IAT_entry] pushes an 8-byte return address.
     * PE32 cdecl args were pushed as 4-byte values, but in 64-bit mode
     * PUSH imm32 sign-extends to 8 bytes. So stack layout is:
     *   [RSP+0]: return address (8 bytes)
     *   [RSP+8]: arg1 (8 bytes, only low 32 valid)
     *   [RSP+16]: arg2 ...
     *
     * We generate: movabs rax, <target>; jmp rax
     * This works because the PE32 code already pushed args in the right
     * order for cdecl, and our shim functions handle both conventions
     * due to the ms_abi attribute (which on Linux x86-64 test harness
     * is also the standard convention).
     */

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
     * We use a convention: EAX = thunk index, ECX = arg count.
     * The kernel's INT 0x2E handler reads the index, looks up the
     * target function, marshals args, calls it in 64-bit mode.
     *
     * Alternatively (simpler): each thunk encodes a unique syscall
     * number that maps to the shim function via a dispatch table.
     */

    /* MOV EAX, <thunk_index> — tells the kernel which function to call */
    code[p++] = 0xB8;
    uint32_t idx = thunk_count;  /* will be incremented after emit */
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

    /* RET <num_args * 4> (cdecl caller cleanup — some Win32 funcs are stdcall) */
    if (num_args > 0) {
        code[p++] = 0xC2;
        uint16_t cleanup = (uint16_t)(num_args * 4);
        code[p++] = (uint8_t)(cleanup);
        code[p++] = (uint8_t)(cleanup >> 8);
    } else {
        code[p++] = 0xC3;  /* RET */
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
}

uint32_t compat32_make_thunk(uint64_t target, const char *name, uint8_t num_args)
{
    if (!thunk_pool) return 0;
    if (thunk_count >= COMPAT32_MAX_THUNKS) {
        serial_puts("[COMPAT32] Thunk table full!\n");
        return 0;
    }

    uint32_t idx = thunk_count;
    uint8_t *stub = thunk_pool + (idx * THUNK_STUB_SIZE);

    /* Generate thunk code */
    emit_thunk(stub, target, num_args);

    /* Record in table */
    thunk_table[idx].thunk_addr  = (uint32_t)(ULONG_PTR)stub;
    thunk_table[idx].target_addr = target;
    thunk_table[idx].num_args    = num_args;
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
        { "strstr",               2 }, { "sprintf",               0 },
        { "printf",               0 }, { "fprintf",               0 },
        { "sscanf",               0 }, { "atoi",                  1 },
        { "atof",                 1 }, { "strtol",                3 },
        { "strtod",               2 }, { "abs",                   1 },
        { "fopen",                2 }, { "fclose",                1 },
        { "fread",                4 }, { "fwrite",                4 },
        { "fseek",                3 }, { "ftell",                 1 },
        { "fgets",                3 }, { "fputs",                 2 },
        { "exit",                 1 }, { "_exit",                 1 },
        { "time",                 1 }, { "clock",                 0 },
        { "srand",                1 }, { "rand",                  0 },
        { "_beginthreadex",       6 }, { "_endthreadex",          1 },

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

    PIMAGE_IMPORT_DESCRIPTOR desc =
        (PIMAGE_IMPORT_DESCRIPTOR)(base + imp_dir->VirtualAddress);

    uint32_t patched = 0;

    for (; desc->Name != 0; desc++) {
        const char *dll_name = (const char *)(base + desc->Name);

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

            /* Get the 64-bit shim address (currently truncated in IAT) */
            uint64_t target64 = 0;
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

            target64 = (uint64_t)(ULONG_PTR)resolved;

            /* Create thunk */
            uint8_t nargs = func_name ? guess_num_args(func_name) : 4;
            uint32_t thunk_addr = compat32_make_thunk(target64, func_name, nargs);

            if (thunk_addr) {
                iat_entry->u1.Function = thunk_addr;
                patched++;
            }
        }
    }

    serial_puts("[COMPAT32] Patched ");
    serial_putdec(patched);
    serial_puts(" IAT entries with thunks\n");

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
    if (thunk_idx >= thunk_count) {
        serial_puts("[COMPAT32] Invalid thunk index ");
        serial_putdec(thunk_idx);
        serial_puts("\n");
        return 0;
    }

    compat32_thunk_t *t = &thunk_table[thunk_idx];
    uint64_t target = t->target_addr;
    uint8_t nargs = t->num_args;

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
