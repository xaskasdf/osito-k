/*
 * OsitoK Windows Compatibility Layer — msvcrt.dll Shim Implementation
 *
 * Microsoft C Runtime functions used by CRT-linked Windows executables.
 * printf uses a custom formatter; memory uses our heap; stdio wraps NT I/O.
 */

#include "msvcrt_shim.h"
#include "kernel32_shim.h"
#include "ntdll_shim.h"
#include "compat32.h"
#include "win32_abi.h"
#include "wintime.h"

#ifdef TEST_HARNESS
#include <math.h>
#endif

/*
 * IMPORTANT: ms_abi variadic functions must use __builtin_ms_va_list,
 * NOT the standard va_list (which is for System V ABI). GCC provides
 * dedicated builtins for Microsoft ABI va_list handling.
 */
typedef __builtin_ms_va_list ms_va_list;
#define ms_va_start(ap, param) __builtin_ms_va_start(ap, param)
#define ms_va_arg(ap, type)    __builtin_va_arg(ap, type)
#define ms_va_end(ap)          __builtin_ms_va_end(ap)
#define ms_va_copy(dst, src)   __builtin_ms_va_copy(dst, src)

/* ── Internal helpers ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_putchar(char c);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void proc_exit(int32_t code);
extern void compat32_callback(uint32_t func_addr);
extern void sched_yield(void);
extern DWORD win32_current_process_id(void);
extern const char *win32_current_exe_name(void);

static int msvcrt_running_ut99(void)
{
    const char *name = win32_current_exe_name();
    const char *expected = "UnrealTournament.exe";
    if (!name) return 0;

    for (const char *p = name; *p; p++)
        if (*p == '\\' || *p == '/') name = p + 1;

    while (*name && *expected) {
        char a = *name++;
        char b = *expected++;
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return 0;
    }
    return *name == 0 && *expected == 0;
}

/* ── CRT Initialization ────────────────────────────────────── */

/*
 * _initterm walks an array of function pointers and calls each non-NULL one.
 * CRITICAL: PE32 (i386) arrays contain 4-byte function pointers, but our
 * 64-bit code would read them as 8-byte pointers, combining pairs and
 * calling garbage addresses. We detect this by checking if the pointers
 * are in low memory (<4GB) and iterate with 4-byte steps.
 *
 * Additionally, these function pointers are 32-bit compat-mode code that
 * cannot be called directly from 64-bit. We use compat32_callback() which
 * switches to compat mode, calls the function, and returns.
 */
/* ── Stub FMalloc for early _initterm (before appInit sets up GMalloc) ── */

extern void *kmalloc(uint64_t size);
extern void kfree(void *ptr);

/* Stub FMalloc implementations using Win32 HeapAlloc (NOT kmalloc).
 * CRITICAL: FMallocWindows uses HeapReAlloc/HeapFree on these pointers
 * after it takes over from the stub. If the stub used kmalloc (different
 * pool), HeapReAlloc would fail → "FMallocWindows::Realloc" error.
 * Using HeapAlloc ensures all allocations are on the same Win32 heap. */
extern PVOID WINAPI HeapAlloc(HANDLE hHeap, DWORD dwFlags, SIZE_T dwBytes);
extern BOOL  WINAPI HeapFree(HANDLE hHeap, DWORD dwFlags, PVOID lpMem);
extern PVOID WINAPI HeapReAlloc(HANDLE hHeap, DWORD dwFlags, PVOID lpMem, SIZE_T dwBytes);
extern SIZE_T WINAPI HeapSize(HANDLE hHeap, DWORD dwFlags, PCVOID lpMem);
extern HANDLE WINAPI GetProcessHeap(void);

static int fmalloc_log_count = 0;
static uint64_t WINAPI stub_fmalloc_malloc(uint64_t _this, uint64_t count, uint64_t tag)
{
    (void)_this; (void)tag;
    HANDLE heap = GetProcessHeap();
    PVOID result = HeapAlloc(heap, 0, count ? count : 1);
    if (fmalloc_log_count < 20 || (count > 4096 && fmalloc_log_count < 200)) {
        fmalloc_log_count++;
        serial_puts("[FMALLOC] size=");
        serial_putdec(count);
        serial_puts(" -> 0x");
        serial_puthex((uint64_t)(uintptr_t)result, 8);
        serial_puts("\n");
    }
    return (uint64_t)(uintptr_t)result;
}

static uint64_t WINAPI stub_fmalloc_realloc(uint64_t _this, uint64_t orig,
                                              uint64_t count, uint64_t tag)
{
    (void)_this; (void)tag;
    HANDLE heap = GetProcessHeap();
    if (!orig) return (uint64_t)(uintptr_t)HeapAlloc(heap, 0, count ? count : 1);
    return (uint64_t)(uintptr_t)HeapReAlloc(heap, 0,
        (PVOID)(uintptr_t)(uint32_t)orig, count ? count : 1);
}

static uint64_t WINAPI stub_fmalloc_free(uint64_t _this, uint64_t ptr)
{
    (void)_this;
    if (ptr) HeapFree(GetProcessHeap(), 0, (PVOID)(uintptr_t)(uint32_t)ptr);
    return 0;
}

static uint64_t WINAPI stub_fmalloc_nop(uint64_t _this)
{
    (void)_this;
    return 1; /* HeapCheck returns TRUE, others return 0/void */
}

/* Static stub object: [0]=vtable_ptr. Vtable: [Malloc,Realloc,Free,nop×4] */
static uint32_t stub_fmalloc_vtbl[8];
static uint32_t stub_fmalloc_obj[4]; /* [0]=vtbl ptr, [1-3]=padding */
static int stub_gmalloc_installed = 0;

/* Set by winexec around winexec_preload_dlls(). During preload, the UE1
 * native-class _initterm constructors (IMPLEMENT_CLASS) run and may call
 * appMalloc — but the EXE's appInit (which calls FMallocWindows::Init to
 * create the HeapAlloc heap) has NOT run yet. On Windows the file order of
 * the OsitoFS image makes Core.dll's FMallocWindows global constructor run
 * BEFORE OpenGlDrv's _initterm, so the real vtable is already installed but
 * its Heap is still NULL → appMalloc hits "Called appMalloc before memory
 * init" and faults. While this flag is set we force our stub vtable even
 * over an already-installed real vtable; the stub routes Malloc/Realloc/Free
 * to HeapAlloc/HeapReAlloc/HeapFree, the SAME pool the real FMallocWindows
 * uses (HeapAlloc ignores the heap handle), so the handoff is transparent. */
int g_gmalloc_preload_phase = 0;

static void ensure_gmalloc_stub(void)
{
    if (!msvcrt_running_ut99()) return;

    /* GMalloc is at Core.dll + RVA 0xA7B90 (VA 0x101A7B90 when base=0x10100000).
     * It's a FMalloc* pointer. On disk, it points to a BSS object (0x101E3450)
     * whose vtable starts as 0 (zero-initialized). The pointer is NON-NULL but
     * the vtable is NULL — so we check the vtable, not the pointer. */
    volatile uint32_t *gmalloc = (volatile uint32_t *)(uintptr_t)0x101A7B90;

    /* Diagnostic: trace the GMalloc/vtbl state across calls.  Log only
     * every Nth call to avoid spam, plus always-log when state changes. */
    static uint32_t last_obj  = 0xFFFFFFFF;
    static uint32_t last_vtbl = 0xFFFFFFFF;
    static int call_n = 0;
    call_n++;
    uint32_t cur_obj  = *gmalloc;
    uint32_t cur_vtbl = (cur_obj && cur_obj < 0x80000000)
                       ? *(volatile uint32_t *)(uintptr_t)cur_obj : 0;
    int changed = (cur_obj != last_obj) || (cur_vtbl != last_vtbl);
    if (changed || call_n < 10 || (call_n % 25) == 0) {
        serial_puts("[GMSTATE#");
        serial_putdec((uint64_t)call_n);
        serial_puts("] *0x101A7B90=0x");
        serial_puthex(cur_obj, 8);
        serial_puts(" *obj=0x");
        serial_puthex(cur_vtbl, 8);
        if (changed && call_n > 1) serial_puts(" CHANGED");
        serial_puts("\n");
        last_obj  = cur_obj;
        last_vtbl = cur_vtbl;
    }

    /* Check if Core.dll is loaded */
    uint32_t obj_addr = cur_obj;
    if (obj_addr < 0x10000000 || obj_addr >= 0x20000000) {
        serial_puts("[CRT] GMalloc not in DLL range: 0x");
        serial_puthex(obj_addr, 8);
        serial_puts("\n");
        return;
    }

    /* Build the stub vtable thunks once (lazily). */
    if (!stub_gmalloc_installed) {
        extern uint32_t compat32_make_thunk_ex(uint64_t target, const char *name,
                                                 uint8_t num_args, uint8_t callconv);
        uint32_t t_malloc  = compat32_make_thunk_ex((uint64_t)(uintptr_t)stub_fmalloc_malloc,
                                                      "GMalloc_Malloc", 2, 0);
        uint32_t t_realloc = compat32_make_thunk_ex((uint64_t)(uintptr_t)stub_fmalloc_realloc,
                                                      "GMalloc_Realloc", 3, 0);
        uint32_t t_free    = compat32_make_thunk_ex((uint64_t)(uintptr_t)stub_fmalloc_free,
                                                      "GMalloc_Free", 1, 0);
        uint32_t t_nop     = compat32_make_thunk_ex((uint64_t)(uintptr_t)stub_fmalloc_nop,
                                                      "GMalloc_nop", 0, 0);
        /* FMalloc vtable: [Malloc, Realloc, Free, DumpAllocs, HeapCheck, Init, Exit] */
        stub_fmalloc_vtbl[0] = t_malloc;
        stub_fmalloc_vtbl[1] = t_realloc;
        stub_fmalloc_vtbl[2] = t_free;
        stub_fmalloc_vtbl[3] = t_nop;
        stub_fmalloc_vtbl[4] = t_nop;
        stub_fmalloc_vtbl[5] = t_nop;
        stub_fmalloc_vtbl[6] = t_nop;
        stub_fmalloc_vtbl[7] = t_nop;  /* no NULL entries — causes crash if called */
        stub_gmalloc_installed = 1;
    }

    volatile uint32_t *obj_vtbl  = (volatile uint32_t *)(uintptr_t)obj_addr;
    uint32_t stub_vtbl_addr      = (uint32_t)(uintptr_t)stub_fmalloc_vtbl;

    /* Already routed through our stub — nothing to do. */
    if (*obj_vtbl == stub_vtbl_addr)
        return;

    /* A real (Core.dll-resident) FMallocWindows vtable is installed. Outside
     * the preload window we trust it: appInit has run FMallocWindows::Init so
     * its Heap is live. */
    if (*obj_vtbl != 0 && !g_gmalloc_preload_phase) {
        if (changed) {
            serial_puts("[CRT] GMalloc vtable already set: 0x");
            serial_puthex(*obj_vtbl, 8);
            serial_puts("\n");
        }
        return;  /* Already constructed AND initialized by appInit */
    }

    /* Install our stub vtable. Two cases reach here:
     *   (a) *obj_vtbl == 0  — object in BSS, real ctor hasn't run yet.
     *   (b) *obj_vtbl != 0 AND preload phase — real ctor ran but Heap is still
     *       NULL (appInit hasn't run); the real Malloc would fault with
     *       "Called appMalloc before memory init". We override it so preload
     *       allocations succeed via HeapAlloc (same pool the real allocator
     *       uses once it takes over). */
    if (*obj_vtbl != 0) {
        serial_puts("[CRT] preload: overriding real GMalloc vtbl 0x");
        serial_puthex(*obj_vtbl, 8);
        serial_puts(" with stub (Heap not yet init)\n");
    }
    *obj_vtbl = stub_vtbl_addr;
    serial_puts("[CRT] Stub GMalloc vtable at 0x");
    serial_puthex(obj_addr, 8);
    serial_puts(" -> vtbl 0x");
    serial_puthex(*obj_vtbl, 8);
    serial_puts("\n");
}

/* ── B8 fix: FMallocWindows Free router (allocator-mismatch) ────────
 * This build's FMallocWindows is a custom pool allocator: it frees by
 * PoolIndirect[ptr>>24][(ptr>>16)&0xff]. Blocks our stub/Heap/CRT
 * allocators hand out during preload live in LOW memory (< 0x40000000,
 * the kmalloc pool) which FMallocWindows never registered -> the lookup
 * returns an empty FPoolInfo -> wild NULL/garbage writes (what the
 * FMW-POOL-SKIP band-aid masks). Fix: replace the FMallocWindows vtable's
 * Free slot (vtbl[2]) with a 32-bit router that no-op-LEAKS foreign (low)
 * pointers (they were bump-pool allocated; leaking the bounded preload set
 * is harmless) and tail-calls the REAL Free for native (>= 0x40000000)
 * pointers. FMalloc::Free is __thiscall (ecx=this, [esp+4]=ptr, ret 4).
 * Native blocks are always >= 0x40000000 (win32_va_alloc base), foreign
 * always < 0x40000000, so the threshold cleanly separates them. */
extern void *mem_alloc_pages(uint64_t count);
static uint8_t *fmw_router_pool = NULL;
static uint32_t fmw_router_used = 0;

static uint32_t fmw_build_free_router(uint32_t real_free)
{
    if (!fmw_router_pool) {
        fmw_router_pool = (uint8_t *)mem_alloc_pages(1);
        if (!fmw_router_pool) return 0;
    }
    if (fmw_router_used + 32 > 4096) return 0;
    uint8_t *p = fmw_router_pool + fmw_router_used;
    int o = 0;
    p[o++]=0x8B; p[o++]=0x44; p[o++]=0x24; p[o++]=0x04;              /* mov eax,[esp+4]   ; ptr */
    p[o++]=0x3D; p[o++]=0x00; p[o++]=0x00; p[o++]=0x00; p[o++]=0x40; /* cmp eax,0x40000000     */
    p[o++]=0x73; p[o++]=0x05;                                        /* jae +5 (native)        */
    p[o++]=0x31; p[o++]=0xC0;                                        /* xor eax,eax            */
    p[o++]=0xC2; p[o++]=0x04; p[o++]=0x00;                           /* ret 4   (foreign no-op)*/
    uint32_t site = (uint32_t)(uintptr_t)(p + o);
    int32_t rel = (int32_t)(real_free - (site + 5));
    p[o++]=0xE9; p[o++]=rel & 0xff; p[o++]=(rel>>8)&0xff;
    p[o++]=(rel>>16)&0xff; p[o++]=(rel>>24)&0xff;                    /* jmp real_free          */
    fmw_router_used += (uint32_t)((o + 15) & ~15);
    return (uint32_t)(uintptr_t)p;
}

static void fmw_install_router(uint32_t obj_addr)
{
    if (obj_addr < 0x10000 || obj_addr >= 0x80000000) return;
    volatile uint32_t *obj = (volatile uint32_t *)(uintptr_t)obj_addr;
    uint32_t vtbl = *obj;
    if (vtbl < 0x10000 || vtbl >= 0x80000000) return;
    /* Never router our own stub vtable (its HeapFree path is correct). */
    if (vtbl == (uint32_t)(uintptr_t)stub_fmalloc_vtbl) return;
    volatile uint32_t *vt = (volatile uint32_t *)(uintptr_t)vtbl;
    uint32_t real_free = vt[2];
    /* Already routed? (vtbl[2] points into our router pool) */
    if (fmw_router_pool) {
        uint32_t lo = (uint32_t)(uintptr_t)fmw_router_pool;
        if (real_free >= lo && real_free < lo + 4096) return;
    }
    /* Sanity: the real Free must be PE code (Core.dll/UT.exe range). */
    if (real_free < 0x10100000 || real_free >= 0x11000000) return;
    uint32_t router = fmw_build_free_router(real_free);
    if (!router) return;
    vt[2] = router;
    serial_puts("[FMW-ROUTER] obj=0x"); serial_puthex(obj_addr, 8);
    serial_puts(" vtbl=0x"); serial_puthex(vtbl, 8);
    serial_puts(" realFree=0x"); serial_puthex(real_free, 8);
    serial_puts(" router=0x"); serial_puthex(router, 8);
    serial_puts("\n");
}

/* Install the Free router on every live FMallocWindows object we know of:
 * the UT.exe instance (0x1092F738, the one the probe caught corrupting) and
 * the Core.dll GMalloc object (*0x101A7B90). Idempotent + cheap; called from
 * ensure_gmalloc_stub so it fires as soon as each real vtable is set. */
static void fmw_install_routers(void)
{
    if (!msvcrt_running_ut99()) return;

    fmw_install_router(0x1092F738);
    volatile uint32_t *gmalloc = (volatile uint32_t *)(uintptr_t)0x101A7B90;
    fmw_install_router(*gmalloc);
}

void WINAPI _initterm(_PVFV *pfbegin, _PVFV *pfend)
{
#ifndef TEST_HARNESS
    if (!g_compat32_mode) {
        typedef void (WINAPI *init_fn64_t)(void);
        uint64_t *begin64 = (uint64_t *)(ULONG_PTR)pfbegin;
        uint64_t *end64   = (uint64_t *)(ULONG_PTR)pfend;

        serial_puts("[MSVCRT] _initterm64: ");
        serial_putdec((uint64_t)(end64 - begin64));
        serial_puts(" entries at 0x");
        serial_puthex((uint64_t)(ULONG_PTR)begin64, 16);
        serial_puts("\n");

        for (uint64_t *p = begin64; p < end64; p++) {
            if (*p)
                ((init_fn64_t)(ULONG_PTR)*p)();
        }
        return;
    }

    /* Ensure GMalloc is valid before any callback can call appMalloc.
     * Core.dll's global constructors and DLL _initterm callbacks may
     * use appMalloc BEFORE the EXE's appInit() sets up FMallocWindows. */
    ensure_gmalloc_stub();
    fmw_install_routers();   /* B8: route foreign frees away from FMallocWindows */

    /* PE32 mode: treat as array of uint32_t function pointers */
    uint32_t *begin32 = (uint32_t *)(ULONG_PTR)pfbegin;
    uint32_t *end32   = (uint32_t *)(ULONG_PTR)pfend;

    serial_puts("[MSVCRT] _initterm: ");
    serial_putdec((uint64_t)(end32 - begin32));
    serial_puts(" entries at 0x");
    serial_puthex((uint64_t)(ULONG_PTR)begin32, 8);
    serial_puts("\n");

    int cb_count = 0;
    int idx = 0;
    /* Audit mode: trace each callback when the array is small or when the
     * skip ratio is suspicious. Prints "INIT[N] @0x... → ok" per callback. */
    int audit_mode = (end32 - begin32 < 200);
    for (uint32_t *p = begin32; p < end32; p++, idx++) {
        if (*p) {
            ensure_gmalloc_stub();  /* re-check before EACH callback */
            fmw_install_routers();  /* B8: (re)install Free router once vtable is live */
            if (audit_mode) {
                serial_puts("[INIT] [");
                serial_putdec((uint64_t)idx);
                serial_puts("] @0x");
                serial_puthex(*p, 8);
                serial_puts("\n");
            }
            compat32_callback(*p);
            cb_count++;
            if (audit_mode) {
                serial_puts("[INIT] [");
                serial_putdec((uint64_t)idx);
                serial_puts("] returned\n");
            }
        }
    }
    serial_puts("[MSVCRT] _initterm done: ");
    serial_putdec(cb_count);
    serial_puts(" callbacks executed\n");

    /* Clear GErrorHist after each _initterm batch. Null-pointer faults
     * during global constructors (handled by our write-through handler)
     * cause the engine's error handler to set GErrorHist="General
     * protection fault!". If GErrorHist is set when WinMain's Browse()
     * runs, the engine skips rendering → error exit. Clear it so the
     * engine starts WinMain with clean error state. */
    if (msvcrt_running_ut99()) {
        volatile uint16_t *gerr = (volatile uint16_t *)(uintptr_t)0x101E3474;
        volatile uint32_t *gcrit = (volatile uint32_t *)(uintptr_t)0x101E568C;
        if (*gerr != 0) {
            *gerr = 0;
            *gcrit = 0;
            serial_puts("[CRT] Cleared GErrorHist after _initterm\n");
        }
    }

    /* Diagnostic: check FMallocWindows vtable after EXE _initterm */
    if ((uint64_t)(ULONG_PTR)begin32 >= 0x10920000 &&
        (uint64_t)(ULONG_PTR)begin32 <= 0x10930000) {
        volatile uint32_t *vtable = (volatile uint32_t *)(uintptr_t)0x1092F738;
        serial_puts("[DIAG] FMallocWindows vtable = 0x");
        serial_puthex(*vtable, 8);
        serial_puts("\n");
    }
#else
    for (_PVFV *pfn = pfbegin; pfn < pfend; pfn++) {
        if (*pfn)
            (*pfn)();
    }
#endif
}

int WINAPI _initterm_e(_PIFV *pfbegin, _PIFV *pfend)
{
#ifndef TEST_HARNESS
    if (!g_compat32_mode) {
        typedef int (WINAPI *init_fn64_t)(void);
        uint64_t *begin64 = (uint64_t *)(ULONG_PTR)pfbegin;
        uint64_t *end64   = (uint64_t *)(ULONG_PTR)pfend;

        serial_puts("[MSVCRT] _initterm_e64: ");
        serial_putdec((uint64_t)(end64 - begin64));
        serial_puts(" entries at 0x");
        serial_puthex((uint64_t)(ULONG_PTR)begin64, 16);
        serial_puts("\n");

        for (uint64_t *p = begin64; p < end64; p++) {
            if (*p) {
                int ret = ((init_fn64_t)(ULONG_PTR)*p)();
                if (ret != 0)
                    return ret;
            }
        }
        return 0;
    }

    /* PE32 mode: same 32-bit pointer handling */
    uint32_t *begin32 = (uint32_t *)(ULONG_PTR)pfbegin;
    uint32_t *end32   = (uint32_t *)(ULONG_PTR)pfend;

    serial_puts("[MSVCRT] _initterm_e: ");
    serial_putdec((uint64_t)(end32 - begin32));
    serial_puts(" entries at 0x");
    serial_puthex((uint64_t)(ULONG_PTR)begin32, 8);
    serial_puts("\n");

    for (uint32_t *p = begin32; p < end32; p++) {
        if (*p) {
            /* For _initterm_e, we can't easily get the return value
             * from a 32-bit callback. Treat as void for now. */
            compat32_callback(*p);
        }
    }
    return 0;
#else
    for (_PIFV *pfn = pfbegin; pfn < pfend; pfn++) {
        if (*pfn) {
            int ret = (*pfn)();
            if (ret != 0)
                return ret;
        }
    }
    return 0;
#endif
}

static char *g_argv0 = "program.exe";
static char *g_argv[] = { NULL, NULL };
static char *g_envp[] = { NULL };

int WINAPI __getmainargs(int *argc, char ***argv, char ***env,
                          int do_wildcard, void *startinfo)
{
    (void)do_wildcard;
    (void)startinfo;
    g_argv[0] = g_argv0;
    *argc = 1;
    *argv = g_argv;
    *env  = g_envp;
    return 0;
}

static WCHAR *g_wargv0 = (WCHAR[]){'p','r','o','g','r','a','m','.','e','x','e',0};
static WCHAR *g_wargv[] = { NULL, NULL };
static WCHAR *g_wenvp[] = { NULL };

int WINAPI __wgetmainargs(int *argc, WCHAR ***argv, WCHAR ***env,
                           int do_wildcard, void *startinfo)
{
    (void)do_wildcard;
    (void)startinfo;
    g_wargv[0] = g_wargv0;
    *argc = 1;
    *argv = g_wargv;
    *env  = g_wenvp;
    return 0;
}

int WINAPI __crtGetShowWindowMode(void) { return 10; /* SW_SHOWDEFAULT */ }
void WINAPI __set_app_type(int type) { (void)type; serial_puts("[CRT] __set_app_type\n"); }
static int crt_exchange_new_mode(int mode);
static PVOID crt_exchange_new_handler(PVOID handler);

int WINAPI _set_new_mode(int mode)
{
    return crt_exchange_new_mode(mode ? 1 : 0);
}

PVOID WINAPI crt_set_new_handler(PVOID handler)
{
    return crt_exchange_new_handler(handler);
}

/* ── Heap-backed malloc/free ───────────────────────────────── */

/* CRT allocations use the Win32 process heap. PE32 callers therefore receive
 * mapped user addresses instead of truncated kernel-heap pointers. */

PVOID WINAPI crt_malloc(SIZE_T size)
{
    PVOID result = HeapAlloc(GetProcessHeap(), 0, size ? size : 1);
    if (size == 0x60 || size == 0x68) {
        static uint32_t raw_monitor_alloc_logs;
        if (raw_monitor_alloc_logs++ < 8) {
            serial_puts("[CRT-MALLOC-MON] size=");
            serial_putdec(size);
            serial_puts(" -> 0x");
            serial_puthex((ULONG_PTR)result, 16);
            serial_puts("\n");
        }
    }
    return result;
}

PVOID WINAPI crt_calloc(SIZE_T count, SIZE_T size)
{
    if (size && count > (SIZE_T)-1 / size)
        return NULL;
    SIZE_T total = count * size;
    PVOID result = HeapAlloc(GetProcessHeap(),
                             0x00000008 /* HEAP_ZERO_MEMORY */,
                             total ? total : 1);
    static uint32_t calloc120_logs;
    if (total == 0x78 && calloc120_logs++ < 8) {
        serial_puts("[CRT-CALLOC120] count=");
        serial_putdec(count);
        serial_puts(" size=");
        serial_putdec(size);
        serial_puts(" -> 0x");
        serial_puthex((ULONG_PTR)result, 16);
        serial_puts("\n");
    }
    return result;
}

void WINAPI crt_free(PVOID ptr)
{
    if (ptr)
        HeapFree(GetProcessHeap(), 0, ptr);
}

PVOID WINAPI crt_realloc(PVOID ptr, SIZE_T size)
{
    if (!ptr) return crt_malloc(size);
    if (size == 0) { crt_free(ptr); return NULL; }
    return HeapReAlloc(GetProcessHeap(), 0, ptr, size);
}

SIZE_T WINAPI crt_msize(PVOID ptr)
{
    return HeapSize(GetProcessHeap(), 0, ptr);
}

/* ── String functions ──────────────────────────────────────── */

SIZE_T WINAPI crt_strlen(const char *s)
{
    SIZE_T len = 0;
    while (s[len]) len++;
    return len;
}

int WINAPI crt_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int WINAPI crt_strncmp(const char *a, const char *b, SIZE_T n)
{
    for (SIZE_T i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == 0) break;
    }
    return 0;
}

static char to_lower(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

int WINAPI crt_stricmp(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = to_lower(*a), cb = to_lower(*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)to_lower(*a) - (unsigned char)to_lower(*b);
}

int WINAPI crt_strnicmp(const char *a, const char *b, SIZE_T n)
{
    for (SIZE_T i = 0; i < n; i++) {
        char ca = to_lower(a[i]), cb = to_lower(b[i]);
        if (ca != cb) return ca - cb;
        if (ca == 0) break;
    }
    return 0;
}

char* WINAPI crt_strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}

char* WINAPI crt_strncpy(char *dst, const char *src, SIZE_T n)
{
    SIZE_T i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

char* WINAPI crt_strcat(char *dst, const char *src)
{
    char *d = dst;
    while (*d) d++;
    while ((*d++ = *src++));
    return dst;
}

char* WINAPI crt_strstr(const char *haystack, const char *needle)
{
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)haystack;
    }
    return NULL;
}

char* WINAPI crt_strchr(const char *s, int c)
{
    for (; *s; s++)
        if (*s == (char)c) return (char *)s;
    return (c == 0) ? (char *)s : NULL;
}

char* WINAPI crt_strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (; *s; s++)
        if (*s == (char)c) last = s;
    if (c == 0) return (char *)s;
    return (char *)last;
}

/* ── Memory ops ────────────────────────────────────────────── */

static BOOL crt_char_is_delimiter(char c, const char *delimiters)
{
    for (const char *d = delimiters; *d; d++)
        if (*d == c) return TRUE;
    return FALSE;
}

char* WINAPI crt_strtok_s(char *str, const char *delimiters, char **context)
{
    if (!delimiters || !context || (!str && !*context)) {
        *crt_errno() = 22; /* EINVAL */
        return NULL;
    }

    char *cursor = str ? str : *context;
    while (*cursor && crt_char_is_delimiter(*cursor, delimiters)) cursor++;
    if (!*cursor) {
        *context = cursor;
        return NULL;
    }

    char *token = cursor;
    while (*cursor && !crt_char_is_delimiter(*cursor, delimiters)) cursor++;
    if (*cursor) *cursor++ = 0;
    *context = cursor;
    return token;
}

PVOID WINAPI crt_memcpy(PVOID dst, PCVOID src, SIZE_T n)
{
    /* Defensive: NULL dst/src after FCriticalError suppression */
    if (n == 0 || !dst || !src) return dst;

    /* Log copies involving VirtualAlloc range (0x40000000+) for TArray debug */
    {
        uint64_t d64 = (uint64_t)(ULONG_PTR)dst;
        uint64_t s64 = (uint64_t)(ULONG_PTR)src;
        static int mc_log = 0;
        if ((d64 >= 0x40000000 && d64 < 0x50000000) ||
            (s64 >= 0x40000000 && s64 < 0x50000000)) {
            if (mc_log < 50) {
                mc_log++;
                serial_puts("[MC] dst=0x");
                serial_puthex(d64, 8);
                serial_puts(" src=0x");
                serial_puthex(s64, 8);
                serial_puts(" n=");
                serial_putdec(n);
                serial_puts("\n");
            }
        }
    }
    BYTE *d = (BYTE *)dst;
    const BYTE *s = (const BYTE *)src;
    while (n--) *d++ = *s++;
    return dst;
}

PVOID WINAPI crt_memset(PVOID dst, int c, SIZE_T n)
{
    BYTE *d = (BYTE *)dst;
    while (n--) *d++ = (BYTE)c;
    return dst;
}

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern uint32_t g_fname_names_addr;

PVOID WINAPI crt_memmove(PVOID dst, PCVOID src, SIZE_T n)
{
    /* Defensive: post-suppression code paths can call memmove with
     * NULL dst or src (e.g., FArray::Realloc returned 0, but caller
     * proceeds anyway after FCriticalError was suppressed). Avoid the
     * NULL-deref kernel #PF — return early. */
    if (n == 0 || !dst || !src) return dst;

    /* Log memmove calls with src/dst/size for debugging FName issue */
    static int mm_log = 0;
    if (mm_log < 20) {
        mm_log++;
        serial_puts("[MM] dst=0x");
        serial_puthex((uint64_t)(ULONG_PTR)dst, 8);
        serial_puts(" src=0x");
        serial_puthex((uint64_t)(ULONG_PTR)src, 8);
        serial_puts(" n=0x");
        serial_puthex(n, 8);
        serial_puts("\n");
    }
    /* Check if this memmove touches the FName::Names Data buffer */
    if (g_fname_names_addr) {
        uint32_t *tarray = (uint32_t *)(uintptr_t)g_fname_names_addr;
        uint32_t data_ptr = tarray[0];
        if (data_ptr >= 0x10000 && data_ptr < 0x20000000) {
            uint32_t num = tarray[1];
            uint32_t buf_size = num * 4;
            uint64_t d = (uint64_t)(ULONG_PTR)dst;
            uint64_t s = (uint64_t)(ULONG_PTR)src;
            /* Check if src or dst overlaps with Data buffer */
            if ((d >= data_ptr && d < data_ptr + buf_size) ||
                (d + n > data_ptr && d < data_ptr + buf_size) ||
                (s >= data_ptr && s < data_ptr + buf_size) ||
                (s + n > data_ptr && s < data_ptr + buf_size)) {
                serial_puts("[MM-FNAME!] TOUCHES FName Data=0x");
                serial_puthex(data_ptr, 8);
                serial_puts(" Num=");
                serial_putdec(num);
                serial_puts("\n");
                /* Dump entries[0..3] before copy */
                uint32_t *entries = (uint32_t *)(uintptr_t)data_ptr;
                serial_puts("  PRE: [0]=0x");
                serial_puthex(entries[0], 8);
                serial_puts(" [1]=0x");
                serial_puthex(entries[1], 8);
                serial_puts(" [2]=0x");
                serial_puthex(entries[2], 8);
                serial_puts(" [3]=0x");
                serial_puthex(entries[3], 8);
                serial_puts("\n");
            }
        }
    }

    BYTE *d = (BYTE *)dst;
    const BYTE *s = (const BYTE *)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

int WINAPI crt_memcmp(PCVOID a, PCVOID b, SIZE_T n)
{
    const BYTE *pa = (const BYTE *)a, *pb = (const BYTE *)b;
    for (SIZE_T i = 0; i < n; i++)
        if (pa[i] != pb[i]) return pa[i] - pb[i];
    return 0;
}

PVOID WINAPI crt_memchr(PCVOID ptr, int value, SIZE_T n)
{
    const BYTE *p = (const BYTE *)ptr;
    BYTE needle = (BYTE)value;
    for (SIZE_T i = 0; i < n; i++) {
        if (p[i] == needle) return (PVOID)(ULONG_PTR)&p[i];
    }
    return NULL;
}

/* ── Format I/O engine ─────────────────────────────────────── */

/*
 * Minimal printf engine supporting:
 *   %d %i %u %x %X %o %p %s %c %f %% %ld %lld %lu %llu %lx %llx
 *   Width, precision, padding (0 and space), left-align (-), sign (+/ )
 *   %f with configurable precision (default 6), rounding
 *   %e/%g consume the arg and format as %f (no scientific notation)
 */

typedef struct {
    char *buf;
    SIZE_T size;
    SIZE_T pos;
} FMT_CTX;

static void fmt_putc(FMT_CTX *ctx, char c)
{
    if (ctx->buf) {
        if (ctx->pos < ctx->size - 1)
            ctx->buf[ctx->pos] = c;
    } else {
        /* Direct to console stdout */
        DWORD written;
        WriteFile((HANDLE)(ULONG_PTR)8, &c, 1, &written, NULL);
    }
    ctx->pos++;
}

static void fmt_puts(FMT_CTX *ctx, const char *s, SIZE_T len)
{
    for (SIZE_T i = 0; i < len; i++)
        fmt_putc(ctx, s[i]);
}

static void fmt_pad(FMT_CTX *ctx, int count, char pad_char)
{
    while (count-- > 0) fmt_putc(ctx, pad_char);
}

static SIZE_T uint_to_str(char *buf, unsigned long long val, int base, int upper)
{
    static const char digits_lower[] = "0123456789abcdef";
    static const char digits_upper[] = "0123456789ABCDEF";
    const char *digits = upper ? digits_upper : digits_lower;
    char tmp[24];
    int i = 0;

    if (val == 0) { tmp[i++] = '0'; }
    else { while (val) { tmp[i++] = digits[val % base]; val /= base; } }

    for (int j = 0; j < i; j++) buf[j] = tmp[i - 1 - j];
    buf[i] = 0;
    return i;
}

static void fmt_integer(FMT_CTX *ctx, const char *digits, SIZE_T digit_count,
                        int width, int precision, int left_align,
                        int zero_pad, char sign)
{
    if (precision == 0 && digit_count == 1 && digits[0] == '0')
        digit_count = 0;

    int precision_zeroes = 0;
    if (precision > (int)digit_count)
        precision_zeroes = precision - (int)digit_count;

    int content = (sign ? 1 : 0) + precision_zeroes + (int)digit_count;
    int width_pad = width > content ? width - content : 0;

    /* An explicit integer precision disables the zero flag. */
    if (!left_align && (!zero_pad || precision >= 0))
        fmt_pad(ctx, width_pad, ' ');
    if (sign)
        fmt_putc(ctx, sign);
    if (!left_align && zero_pad && precision < 0)
        fmt_pad(ctx, width_pad, '0');
    fmt_pad(ctx, precision_zeroes, '0');
    fmt_puts(ctx, digits, digit_count);
    if (left_align)
        fmt_pad(ctx, width_pad, ' ');
}

static int do_vformat(FMT_CTX *ctx, const char *fmt, ms_va_list ap)
{
    while (*fmt) {
        if (*fmt != '%') {
            fmt_putc(ctx, *fmt++);
            continue;
        }
        fmt++; /* skip '%' */

        /* Flags */
        int left_align = 0, zero_pad = 0, plus_sign = 0, space_sign = 0;
        for (;;) {
            if (*fmt == '-') { left_align = 1; fmt++; }
            else if (*fmt == '0') { zero_pad = 1; fmt++; }
            else if (*fmt == '+') { plus_sign = 1; fmt++; }
            else if (*fmt == ' ') { space_sign = 1; fmt++; }
            else break;
        }

        /* Width */
        int width = 0;
        if (*fmt == '*') { width = ms_va_arg(ap, int); fmt++; }
        else { while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; } }

        /* Precision */
        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            if (*fmt == '*') { precision = ms_va_arg(ap, int); fmt++; }
            else { while (*fmt >= '0' && *fmt <= '9') { precision = precision * 10 + (*fmt - '0'); fmt++; } }
        }

        /* Length modifier */
        int len_mod = 0; /* 0=int, 1=long, 2=long long, 3=size_t */
        if (*fmt == 'l') {
            fmt++; len_mod = 1;
            if (*fmt == 'l') { fmt++; len_mod = 2; }
        } else if (*fmt == 'z') {
            fmt++; len_mod = 3;
        } else if (*fmt == 'h') {
            fmt++;
            if (*fmt == 'h') fmt++;
            /* treat as int */
        } else if (*fmt == 'I') {
            /* MSVC I64 prefix */
            if (fmt[1] == '6' && fmt[2] == '4') {
                fmt += 3; len_mod = 2;
            }
        }

        /* Conversion */
        char num_buf[24];
        SIZE_T num_len;
        const char *str;
        SIZE_T str_len;
        int negative = 0;

        switch (*fmt) {
        case 'd': case 'i': {
            long long val;
            if (len_mod == 2) val = ms_va_arg(ap, long long);
            else if (len_mod == 1 || len_mod == 3) val = ms_va_arg(ap, long);
            else val = ms_va_arg(ap, int);

            unsigned long long magnitude;
            if (val < 0) {
                negative = 1;
                magnitude = (unsigned long long)(-(val + 1)) + 1;
            } else {
                magnitude = (unsigned long long)val;
            }
            num_len = uint_to_str(num_buf, magnitude, 10, 0);
            char sign = negative ? '-' : (plus_sign ? '+' :
                                           (space_sign ? ' ' : 0));
            fmt_integer(ctx, num_buf, num_len, width, precision, left_align,
                        zero_pad, sign);
            break;
        }
        case 'u': {
            unsigned long long val;
            if (len_mod == 2) val = ms_va_arg(ap, unsigned long long);
            else if (len_mod == 1 || len_mod == 3) val = ms_va_arg(ap, unsigned long);
            else val = ms_va_arg(ap, unsigned int);

            num_len = uint_to_str(num_buf, val, 10, 0);
            fmt_integer(ctx, num_buf, num_len, width, precision, left_align,
                        zero_pad, 0);
            break;
        }
        case 'x': case 'X': {
            unsigned long long val;
            if (len_mod == 2) val = ms_va_arg(ap, unsigned long long);
            else if (len_mod == 1 || len_mod == 3) val = ms_va_arg(ap, unsigned long);
            else val = ms_va_arg(ap, unsigned int);

            num_len = uint_to_str(num_buf, val, 16, (*fmt == 'X'));
            fmt_integer(ctx, num_buf, num_len, width, precision, left_align,
                        zero_pad, 0);
            break;
        }
        case 'o': {
            unsigned long long val;
            if (len_mod >= 1) val = ms_va_arg(ap, unsigned long long);
            else val = ms_va_arg(ap, unsigned int);

            num_len = uint_to_str(num_buf, val, 8, 0);
            fmt_integer(ctx, num_buf, num_len, width, precision, left_align,
                        zero_pad, 0);
            break;
        }
        case 'p': {
            unsigned long long val = (unsigned long long)(ULONG_PTR)ms_va_arg(ap, PVOID);
            num_len = uint_to_str(num_buf, val, 16, 0);
            /* Pad to pointer width */
            int total = (int)num_len + 2; /* "0x" prefix */
            if (!left_align) fmt_pad(ctx, width - total, ' ');
            fmt_putc(ctx, '0'); fmt_putc(ctx, 'x');
            fmt_pad(ctx, 16 - (int)num_len, '0');
            fmt_puts(ctx, num_buf, num_len);
            if (left_align) fmt_pad(ctx, width - total, ' ');
            break;
        }
        case 's':
            str = ms_va_arg(ap, const char *);
            if (!str) str = "(null)";
            str_len = crt_strlen(str);
            if (precision >= 0 && (SIZE_T)precision < str_len)
                str_len = (SIZE_T)precision;
            if (!left_align) fmt_pad(ctx, width - (int)str_len, ' ');
            fmt_puts(ctx, str, str_len);
            if (left_align) fmt_pad(ctx, width - (int)str_len, ' ');
            break;

        case 'c': {
            char ch = (char)ms_va_arg(ap, int);
            if (!left_align) fmt_pad(ctx, width - 1, ' ');
            fmt_putc(ctx, ch);
            if (left_align) fmt_pad(ctx, width - 1, ' ');
            break;
        }
        case 'f': case 'e': case 'g': {
            double val = ms_va_arg(ap, double);
            int prec = (precision >= 0) ? precision : 6;

            /* Handle negative / sign */
            int f_neg = 0;
            if (val < 0) { f_neg = 1; val = -val; }

            /* Decompose into integer and fractional parts.
             * We work with unsigned 64-bit for the integer portion
             * and compute fractional digits via repeated multiply. */
            unsigned long long int_part = (unsigned long long)val;
            double frac_part = val - (double)int_part;

            /* Build integer-part string */
            char f_buf[80];
            int f_pos = 0;
            SIZE_T ip_len = uint_to_str(f_buf, int_part, 10, 0);
            f_pos = (int)ip_len;

            /* Decimal point + fractional digits */
            if (prec > 0) {
                f_buf[f_pos++] = '.';
                for (int fi = 0; fi < prec; fi++) {
                    frac_part *= 10.0;
                    int fdigit = (int)frac_part;
                    if (fdigit > 9) fdigit = 9;
                    f_buf[f_pos++] = '0' + fdigit;
                    frac_part -= fdigit;
                }
                /* Round: check if remaining frac >= 0.5 */
                if (frac_part >= 0.5) {
                    /* Propagate carry backwards through frac digits */
                    int ci = f_pos - 1;
                    while (ci >= 0) {
                        if (f_buf[ci] == '.') { ci--; continue; }
                        if (f_buf[ci] < '9') { f_buf[ci]++; break; }
                        f_buf[ci] = '0';
                        ci--;
                    }
                    if (ci < 0) {
                        /* Carry overflowed past all digits — shift right and insert '1' */
                        for (int si = f_pos; si > 0; si--)
                            f_buf[si] = f_buf[si - 1];
                        f_buf[0] = '1';
                        f_pos++;
                    }
                }
            } else if (precision == 0) {
                /* No decimal point when precision is explicitly 0 */
                /* Round the integer part */
                if (frac_part >= 0.5) {
                    int_part++;
                    f_pos = (int)uint_to_str(f_buf, int_part, 10, 0);
                }
            }
            f_buf[f_pos] = 0;

            int f_total = f_pos + f_neg;
            if (!f_neg && plus_sign) f_total++;
            else if (!f_neg && space_sign) f_total++;
            char f_pad = (zero_pad && !left_align) ? '0' : ' ';

            if (!left_align && f_pad == ' ') fmt_pad(ctx, width - f_total, ' ');
            if (f_neg) fmt_putc(ctx, '-');
            else if (plus_sign) fmt_putc(ctx, '+');
            else if (space_sign) fmt_putc(ctx, ' ');
            if (!left_align && f_pad == '0') fmt_pad(ctx, width - f_total, '0');
            fmt_puts(ctx, f_buf, f_pos);
            if (left_align) fmt_pad(ctx, width - f_total, ' ');
            break;
        }
        case '%':
            fmt_putc(ctx, '%');
            break;

        case 'n':
            /* Store chars written */
            if (len_mod == 2) *ms_va_arg(ap, long long *) = (long long)ctx->pos;
            else if (len_mod == 1) *ms_va_arg(ap, long *) = (long)ctx->pos;
            else *ms_va_arg(ap, int *) = (int)ctx->pos;
            break;

        case 0:
            goto done;

        default:
            /* Unknown specifier — print literally */
            fmt_putc(ctx, '%');
            fmt_putc(ctx, *fmt);
            break;
        }
        fmt++;
    }
done:
    if (ctx->buf && ctx->size > 0) {
        SIZE_T end = ctx->pos < ctx->size - 1 ? ctx->pos : ctx->size - 1;
        ctx->buf[end] = 0;
    }
    return (int)ctx->pos;
}

/*
 * do_vformat32 — Format walker for 32-bit va_list (4-byte arg slots).
 *
 * When a 32-bit PE32 program calls vsprintf/vprintf/etc via INT 0x2E thunk,
 * the va_list parameter is a pointer into the 32-bit stack where args are
 * packed in 4-byte slots. ms_va_arg reads 8-byte slots which is WRONG.
 * This version manually walks the 32-bit stack with uint32_t* increments.
 */
static int do_vformat32(FMT_CTX *ctx, const char *fmt, uint32_t *vp)
{
    while (*fmt) {
        if (*fmt != '%') {
            fmt_putc(ctx, *fmt++);
            continue;
        }
        fmt++; /* skip '%' */

        /* Flags */
        int left_align = 0, zero_pad = 0, plus_sign = 0, space_sign = 0;
        for (;;) {
            if (*fmt == '-') { left_align = 1; fmt++; }
            else if (*fmt == '0') { zero_pad = 1; fmt++; }
            else if (*fmt == '+') { plus_sign = 1; fmt++; }
            else if (*fmt == ' ') { space_sign = 1; fmt++; }
            else break;
        }

        /* Width */
        int width = 0;
        if (*fmt == '*') { width = (int)(*vp++); fmt++; }
        else { while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; } }

        /* Precision */
        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            if (*fmt == '*') { precision = (int)(*vp++); fmt++; }
            else { while (*fmt >= '0' && *fmt <= '9') { precision = precision * 10 + (*fmt - '0'); fmt++; } }
        }

        /* Length modifier */
        int len_mod = 0; /* 0=int, 1=long, 2=long long, 3=size_t */
        if (*fmt == 'l') {
            fmt++; len_mod = 1;
            if (*fmt == 'l') { fmt++; len_mod = 2; }
        } else if (*fmt == 'z') {
            fmt++; len_mod = 3;
        } else if (*fmt == 'h') {
            fmt++;
            if (*fmt == 'h') fmt++;
            /* treat as int */
        } else if (*fmt == 'I') {
            /* MSVC I64 prefix */
            if (fmt[1] == '6' && fmt[2] == '4') {
                fmt += 3; len_mod = 2;
            }
        }

        /* Conversion */
        char num_buf[24];
        SIZE_T num_len;
        const char *str;
        SIZE_T str_len;
        int negative = 0;

        switch (*fmt) {
        case 'd': case 'i': {
            long long val;
            if (len_mod == 2) {
                /* 64-bit: two consecutive 32-bit values (lo, hi) */
                uint32_t lo = *vp++, hi = *vp++;
                val = (long long)((uint64_t)hi << 32 | lo);
            } else {
                val = (long long)(int32_t)(*vp++);
            }

            unsigned long long magnitude;
            if (val < 0) {
                negative = 1;
                magnitude = (unsigned long long)(-(val + 1)) + 1;
            } else {
                magnitude = (unsigned long long)val;
            }
            num_len = uint_to_str(num_buf, magnitude, 10, 0);
            char sign = negative ? '-' : (plus_sign ? '+' :
                                           (space_sign ? ' ' : 0));
            fmt_integer(ctx, num_buf, num_len, width, precision, left_align,
                        zero_pad, sign);
            break;
        }
        case 'u': {
            unsigned long long val;
            if (len_mod == 2) {
                uint32_t lo = *vp++, hi = *vp++;
                val = ((uint64_t)hi << 32) | lo;
            } else {
                val = (unsigned long long)(*vp++);
            }

            num_len = uint_to_str(num_buf, val, 10, 0);
            fmt_integer(ctx, num_buf, num_len, width, precision, left_align,
                        zero_pad, 0);
            break;
        }
        case 'x': case 'X': {
            unsigned long long val;
            if (len_mod == 2) {
                uint32_t lo = *vp++, hi = *vp++;
                val = ((uint64_t)hi << 32) | lo;
            } else {
                val = (unsigned long long)(*vp++);
            }

            num_len = uint_to_str(num_buf, val, 16, (*fmt == 'X'));
            fmt_integer(ctx, num_buf, num_len, width, precision, left_align,
                        zero_pad, 0);
            break;
        }
        case 'o': {
            unsigned long long val;
            if (len_mod >= 1) {
                uint32_t lo = *vp++, hi = *vp++;
                val = ((uint64_t)hi << 32) | lo;
            } else {
                val = (unsigned long long)(*vp++);
            }

            num_len = uint_to_str(num_buf, val, 8, 0);
            fmt_integer(ctx, num_buf, num_len, width, precision, left_align,
                        zero_pad, 0);
            break;
        }
        case 'p': {
            /* 32-bit pointer */
            uint32_t val32 = *vp++;
            num_len = uint_to_str(num_buf, (unsigned long long)val32, 16, 0);
            int total = (int)num_len + 2; /* "0x" prefix */
            if (!left_align) fmt_pad(ctx, width - total, ' ');
            fmt_putc(ctx, '0'); fmt_putc(ctx, 'x');
            fmt_pad(ctx, 8 - (int)num_len, '0');
            fmt_puts(ctx, num_buf, num_len);
            if (left_align) fmt_pad(ctx, width - total, ' ');
            break;
        }
        case 's':
            str = (const char *)(uintptr_t)(*vp++);
            if (!str) str = "(null)";
            str_len = crt_strlen(str);
            if (precision >= 0 && (SIZE_T)precision < str_len)
                str_len = (SIZE_T)precision;
            if (!left_align) fmt_pad(ctx, width - (int)str_len, ' ');
            fmt_puts(ctx, str, str_len);
            if (left_align) fmt_pad(ctx, width - (int)str_len, ' ');
            break;

        case 'c': {
            char ch = (char)(*vp++);
            if (!left_align) fmt_pad(ctx, width - 1, ' ');
            fmt_putc(ctx, ch);
            if (left_align) fmt_pad(ctx, width - 1, ' ');
            break;
        }
        case 'f': case 'e': case 'g': {
            /* Double on 32-bit stack: 8 bytes = two consecutive uint32_t */
            uint32_t lo = *vp++, hi = *vp++;
            uint64_t bits = ((uint64_t)hi << 32) | lo;
            double val;
            __builtin_memcpy(&val, &bits, 8);
            int prec = (precision >= 0) ? precision : 6;

            int f_neg = 0;
            if (val < 0) { f_neg = 1; val = -val; }

            unsigned long long int_part = (unsigned long long)val;
            double frac_part = val - (double)int_part;

            char f_buf[80];
            int f_pos = 0;
            SIZE_T ip_len = uint_to_str(f_buf, int_part, 10, 0);
            f_pos = (int)ip_len;

            if (prec > 0) {
                f_buf[f_pos++] = '.';
                for (int fi = 0; fi < prec; fi++) {
                    frac_part *= 10.0;
                    int fdigit = (int)frac_part;
                    if (fdigit > 9) fdigit = 9;
                    f_buf[f_pos++] = '0' + fdigit;
                    frac_part -= fdigit;
                }
                if (frac_part >= 0.5) {
                    int ci = f_pos - 1;
                    while (ci >= 0) {
                        if (f_buf[ci] == '.') { ci--; continue; }
                        if (f_buf[ci] < '9') { f_buf[ci]++; break; }
                        f_buf[ci] = '0';
                        ci--;
                    }
                    if (ci < 0) {
                        for (int si = f_pos; si > 0; si--)
                            f_buf[si] = f_buf[si - 1];
                        f_buf[0] = '1';
                        f_pos++;
                    }
                }
            } else if (precision == 0) {
                if (frac_part >= 0.5) {
                    int_part++;
                    f_pos = (int)uint_to_str(f_buf, int_part, 10, 0);
                }
            }
            f_buf[f_pos] = 0;

            int f_total = f_pos + f_neg;
            if (!f_neg && plus_sign) f_total++;
            else if (!f_neg && space_sign) f_total++;
            char f_pad = (zero_pad && !left_align) ? '0' : ' ';

            if (!left_align && f_pad == ' ') fmt_pad(ctx, width - f_total, ' ');
            if (f_neg) fmt_putc(ctx, '-');
            else if (plus_sign) fmt_putc(ctx, '+');
            else if (space_sign) fmt_putc(ctx, ' ');
            if (!left_align && f_pad == '0') fmt_pad(ctx, width - f_total, '0');
            fmt_puts(ctx, f_buf, f_pos);
            if (left_align) fmt_pad(ctx, width - f_total, ' ');
            break;
        }
        case '%':
            fmt_putc(ctx, '%');
            break;

        case 'n':
            if (len_mod == 2) {
                uint32_t addr32 = *vp++;
                long long *p = (long long *)(uintptr_t)addr32;
                *p = (long long)ctx->pos;
            } else {
                uint32_t addr32 = *vp++;
                int *p = (int *)(uintptr_t)addr32;
                *p = (int)ctx->pos;
            }
            break;

        case 0:
            goto done32;

        default:
            fmt_putc(ctx, '%');
            fmt_putc(ctx, *fmt);
            break;
        }
        fmt++;
    }
done32:
    if (ctx->buf && ctx->size > 0) {
        SIZE_T end = ctx->pos < ctx->size - 1 ? ctx->pos : ctx->size - 1;
        ctx->buf[end] = 0;
    }
    return (int)ctx->pos;
}

static int WINAPI crt_printf_compat32(const char *fmt, uint32_t *args)
{
    FMT_CTX ctx = { NULL, 0, 0 };
    return do_vformat32(&ctx, fmt, args);
}

static int WINAPI crt_sprintf_compat32(char *buf, const char *fmt,
                                       uint32_t *args)
{
    FMT_CTX ctx = { buf, (SIZE_T)-1, 0 };
    return do_vformat32(&ctx, fmt, args);
}

static int WINAPI crt_snprintf_compat32(char *buf, SIZE_T size,
                                        const char *fmt, uint32_t *args)
{
    FMT_CTX ctx = { buf, size, 0 };
    return do_vformat32(&ctx, fmt, args);
}

static int WINAPI crt_fprintf_compat32(PVOID stream, const char *fmt,
                                       uint32_t *args)
{
    (void)stream;
    FMT_CTX ctx = { NULL, 0, 0 };
    return do_vformat32(&ctx, fmt, args);
}

int WINAPI crt_printf(const char *fmt, ...)
{
    ms_va_list ap;
    ms_va_start(ap, fmt);
    FMT_CTX ctx = { NULL, 0, 0 };
    int ret = do_vformat(&ctx, fmt, ap);
    ms_va_end(ap);
    return ret;
}

int WINAPI crt_sprintf(char *buf, const char *fmt, ...)
{
    ms_va_list ap;
    ms_va_start(ap, fmt);
    FMT_CTX ctx = { buf, (SIZE_T)-1, 0 };
    int ret = do_vformat(&ctx, fmt, ap);
    ms_va_end(ap);
    return ret;
}

int WINAPI crt_snprintf(char *buf, SIZE_T size, const char *fmt, ...)
{
    ms_va_list ap;
    ms_va_start(ap, fmt);
    FMT_CTX ctx = { buf, size, 0 };
    int ret = do_vformat(&ctx, fmt, ap);
    ms_va_end(ap);
    return ret;
}

int WINAPI crt_fprintf(PVOID stream, const char *fmt, ...)
{
    /* Route to printf for now — stream is ignored, goes to stdout */
    ms_va_list ap;
    ms_va_start(ap, fmt);
    FMT_CTX ctx = { NULL, 0, 0 };
    int ret = do_vformat(&ctx, fmt, ap);
    ms_va_end(ap);
    return ret;
}

typedef struct {
    ms_va_list *native;
    uint32_t *compat32;
} CRT_SCAN_ARGS;

static PVOID crt_scan_output_arg(CRT_SCAN_ARGS *args)
{
    if (args->compat32) {
        PVOID output = (PVOID)(ULONG_PTR)*args->compat32;
        args->compat32++;
        return output;
    }
    return ms_va_arg(*args->native, PVOID);
}

static int crt_vsscanf_core(const char *buf, const char *fmt,
                            CRT_SCAN_ARGS *args)
{
    if (!buf || !fmt) return -1;

    const char *p = buf;  /* current position in input */
    int matched = 0;      /* number of successfully assigned items */

    while (*fmt) {
        /* Literal whitespace in format: skip any whitespace in input */
        if (*fmt == ' ' || *fmt == '\t' || *fmt == '\n') {
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            fmt++;
            continue;
        }

        /* Non-% literal: must match exactly */
        if (*fmt != '%') {
            if (*p != *fmt) goto done;
            p++; fmt++;
            continue;
        }

        fmt++; /* skip '%' */

        /* %% — literal percent */
        if (*fmt == '%') {
            if (*p != '%') goto done;
            p++; fmt++;
            continue;
        }

        /* Suppress assignment flag */
        int suppress = 0;
        if (*fmt == '*') { suppress = 1; fmt++; }

        /* Field width */
        int fw = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            fw = fw * 10 + (*fmt - '0');
            fmt++;
        }

        /* Length modifier */
        int s_len_mod = 0; /* 0=int, 1=long, 2=short, 3=long long */
        if (*fmt == 'l') {
            fmt++; s_len_mod = 1;
            if (*fmt == 'l') { fmt++; s_len_mod = 3; }
        } else if (*fmt == 'h') {
            fmt++; s_len_mod = 2;
            if (*fmt == 'h') { fmt++; s_len_mod = 2; }
        } else if (*fmt == 'I') {
            /* Microsoft CRT integer widths: %I32d / %I64u. */
            if (fmt[1] == '6' && fmt[2] == '4') {
                fmt += 3;
                s_len_mod = 3;
            } else if (fmt[1] == '3' && fmt[2] == '2') {
                fmt += 3;
                s_len_mod = 0;
            }
        }

        switch (*fmt) {
        case 'd': case 'i': {
            /* Skip leading whitespace */
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            if (!*p) goto done;

            const char *start = p;
            int neg = 0;
            if (*p == '-') { neg = 1; p++; }
            else if (*p == '+') p++;

            int base = 10;
            int had_prefix = 0; /* tracks if we consumed 0/0x prefix for %i */
            if (*fmt == 'i') {
                /* %i auto-detects base */
                if (*p == '0') {
                    had_prefix = 1;
                    p++;
                    if (*p == 'x' || *p == 'X') { base = 16; p++; }
                    else base = 8;
                }
            }

            /* Check for at least one valid digit (unless %i already consumed '0') */
            if (!had_prefix) {
                if (!((*p >= '0' && *p <= '9') ||
                      (base == 16 && ((*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))))) {
                    goto done;
                }
            }

            long long val = 0;
            int count = 0;
            while (*p && (fw == 0 || count < fw)) {
                int digit;
                if (*p >= '0' && *p <= '9') digit = *p - '0';
                else if (*p >= 'a' && *p <= 'f') digit = *p - 'a' + 10;
                else if (*p >= 'A' && *p <= 'F') digit = *p - 'A' + 10;
                else break;
                if (digit >= base) break;
                val = val * base + digit;
                p++; count++;
            }
            if (neg) val = -val;

            if (p == start) goto done; /* no chars consumed */
            if (!suppress) {
                if (s_len_mod == 3) *(long long *)crt_scan_output_arg(args) = val;
                else if (s_len_mod == 1) *(long *)crt_scan_output_arg(args) = (long)val;
                else if (s_len_mod == 2) *(short *)crt_scan_output_arg(args) = (short)val;
                else *(int *)crt_scan_output_arg(args) = (int)val;
                matched++;
            }
            break;
        }
        case 'u': {
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            if (!*p) goto done;

            if (!(*p >= '0' && *p <= '9')) goto done;

            unsigned long long val = 0;
            int count = 0;
            while (*p >= '0' && *p <= '9' && (fw == 0 || count < fw)) {
                val = val * 10 + (*p - '0');
                p++; count++;
            }
            if (!suppress) {
                if (s_len_mod == 3) *(unsigned long long *)crt_scan_output_arg(args) = val;
                else if (s_len_mod == 1) *(unsigned long *)crt_scan_output_arg(args) = (unsigned long)val;
                else if (s_len_mod == 2) *(unsigned short *)crt_scan_output_arg(args) = (unsigned short)val;
                else *(unsigned int *)crt_scan_output_arg(args) = (unsigned int)val;
                matched++;
            }
            break;
        }
        case 'x': case 'X': {
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            if (!*p) goto done;

            /* Skip optional 0x prefix */
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;

            if (!((*p >= '0' && *p <= '9') ||
                  (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))) goto done;

            unsigned long long val = 0;
            int count = 0;
            while (*p && (fw == 0 || count < fw)) {
                int digit;
                if (*p >= '0' && *p <= '9') digit = *p - '0';
                else if (*p >= 'a' && *p <= 'f') digit = *p - 'a' + 10;
                else if (*p >= 'A' && *p <= 'F') digit = *p - 'A' + 10;
                else break;
                val = val * 16 + digit;
                p++; count++;
            }
            if (!suppress) {
                if (s_len_mod == 3) *(unsigned long long *)crt_scan_output_arg(args) = val;
                else if (s_len_mod == 1) *(unsigned long *)crt_scan_output_arg(args) = (unsigned long)val;
                else *(unsigned int *)crt_scan_output_arg(args) = (unsigned int)val;
                matched++;
            }
            break;
        }
        case 'o': {
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            if (!*p) goto done;
            if (!(*p >= '0' && *p <= '7')) goto done;

            unsigned long long val = 0;
            int count = 0;
            while (*p >= '0' && *p <= '7' && (fw == 0 || count < fw)) {
                val = val * 8 + (*p - '0');
                p++; count++;
            }
            if (!suppress) {
                if (s_len_mod == 3) *(unsigned long long *)crt_scan_output_arg(args) = val;
                else if (s_len_mod == 1) *(unsigned long *)crt_scan_output_arg(args) = (unsigned long)val;
                else if (s_len_mod == 2) *(unsigned short *)crt_scan_output_arg(args) = (unsigned short)val;
                else *(unsigned int *)crt_scan_output_arg(args) = (unsigned int)val;
                matched++;
            }
            break;
        }
        case 's': {
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            if (!*p) goto done;

            char *dst = suppress ? NULL : (char *)crt_scan_output_arg(args);
            int count = 0;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' &&
                   (fw == 0 || count < fw)) {
                if (dst) dst[count] = *p;
                p++; count++;
            }
            if (dst) dst[count] = '\0';
            if (!suppress) matched++;
            break;
        }
        case 'c': {
            /* %c does NOT skip whitespace */
            int count = (fw > 0) ? fw : 1;
            if (!*p) goto done;

            char *dst = suppress ? NULL : (char *)crt_scan_output_arg(args);
            for (int ci = 0; ci < count && *p; ci++) {
                if (dst) dst[ci] = *p;
                p++;
            }
            if (!suppress) matched++;
            break;
        }
        case 'f': {
            /* Parse floating-point number */
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            if (!*p) goto done;

            const char *fstart = p;
            double sign = 1.0;
            if (*p == '-') { sign = -1.0; p++; }
            else if (*p == '+') p++;

            if (!(*p >= '0' && *p <= '9') && *p != '.') goto done;

            double fval = 0.0;
            while (*p >= '0' && *p <= '9') {
                fval = fval * 10.0 + (*p - '0');
                p++;
            }
            if (*p == '.') {
                p++;
                double divisor = 10.0;
                while (*p >= '0' && *p <= '9') {
                    fval += (*p - '0') / divisor;
                    divisor *= 10.0;
                    p++;
                }
            }
            fval *= sign;

            if (p == fstart) goto done;
            if (!suppress) {
                if (s_len_mod == 1) *(double *)crt_scan_output_arg(args) = fval;
                else *(float *)crt_scan_output_arg(args) = (float)fval;
                matched++;
            }
            break;
        }
        case 'n': {
            /* Store number of characters consumed so far */
            if (!suppress) {
                *(int *)crt_scan_output_arg(args) = (int)(p - buf);
                /* %n does not increment matched count per C standard */
            }
            break;
        }
        case '[': {
            /* Scanset: %[...] — read characters matching a set */
            fmt++; /* skip '[' */
            int negate = 0;
            if (*fmt == '^') { negate = 1; fmt++; }

            /* Build a 256-bit set of accepted characters */
            char set[256];
            crt_memset(set, 0, 256);
            /* Handle ']' as first character in set */
            if (*fmt == ']') { set[(unsigned char)']'] = 1; fmt++; }
            while (*fmt && *fmt != ']') {
                set[(unsigned char)*fmt] = 1;
                fmt++;
            }
            if (*fmt == ']') fmt++; /* skip closing ']' */

            if (!*p) goto done;
            char *dst = suppress ? NULL : (char *)crt_scan_output_arg(args);
            int count = 0;
            while (*p && (fw == 0 || count < fw)) {
                int in_set = set[(unsigned char)*p];
                if (negate) in_set = !in_set;
                if (!in_set) break;
                if (dst) dst[count] = *p;
                p++; count++;
            }
            if (count == 0) goto done;
            if (dst) dst[count] = '\0';
            if (!suppress) matched++;
            /* fmt already advanced past ']', skip the fmt++ at end of switch */
            continue;
        }
        default:
            /* Unknown specifier — stop */
            goto done;
        }
        fmt++;
    }

done:
    return matched;
}

int WINAPI crt_sscanf(const char *buf, const char *fmt, ...)
{
    ms_va_list ap;
    ms_va_start(ap, fmt);
    CRT_SCAN_ARGS args = { &ap, NULL };
    int matched = crt_vsscanf_core(buf, fmt, &args);
    ms_va_end(ap);
    return matched;
}

static int WINAPI crt_sscanf_compat32(const char *buf, const char *fmt,
                                      uint32_t *args32)
{
    CRT_SCAN_ARGS args = { NULL, args32 };
    return crt_vsscanf_core(buf, fmt, &args);
}

int WINAPI crt_puts(const char *s)
{
    DWORD written;
    SIZE_T len = crt_strlen(s);
    WriteFile((HANDLE)(ULONG_PTR)8, s, (DWORD)len, &written, NULL);
    char nl = '\n';
    WriteFile((HANDLE)(ULONG_PTR)8, &nl, 1, &written, NULL);
    return 0;
}

int WINAPI crt_putchar(int c)
{
    char ch = (char)c;
    DWORD written;
    WriteFile((HANDLE)(ULONG_PTR)8, &ch, 1, &written, NULL);
    return c;
}

/* ── stdio FILE* ───────────────────────────────────────────── */

struct _CRT_FILE {
    HANDLE  nt_handle;
    int     flags;      /* 1=read, 2=write, 4=eof, 8=error, 16=ungetc valid */
    int     ungetc_ch;
    int     open_flags;
    int     owns_handle;
};

#define CRT_FILE_MAX 32
#define CRT_FILE_PROXY_PROCESS_SLOTS 512

/* PE32 programs can inspect FILE directly. Keep the ABI-visible object in
 * process-owned low memory and translate it to the native backing table at
 * the shim boundary. This is the MSVCR100/UCRT x86 _iobuf layout. */
typedef struct {
    uint32_t ptr;
    int32_t  cnt;
    uint32_t base;
    int32_t  flag;
    int32_t  file;
    int32_t  charbuf;
    int32_t  bufsiz;
    uint32_t tmpfname;
} CRT_FILE32;

_Static_assert(sizeof(CRT_FILE32) == 32, "PE32 _iobuf layout changed");

typedef struct {
    DWORD owner_pid;
    CRT_FILE32 *files;
} CRT_FILE_PROXY_SLOT;

#define CRT_EBADF       9
#define CRT_ENOMEM     12
#define CRT_EINVAL     22
#define CRT_EMFILE     24
#define CRT_ENOENT      2
#define CRT_ENAMETOOLONG 38
#define CRT_EOVERFLOW 132
#define CRT_ERANGE     34
#define CRT_STRUNCATE  80

#define CRT_O_WRONLY   0x0001
#define CRT_O_RDWR     0x0002
#define CRT_O_APPEND   0x0008
#define CRT_O_CREAT    0x0100
#define CRT_O_TRUNC    0x0200
#define CRT_O_EXCL     0x0400
#define CRT_O_TEXT     0x4000
#define CRT_O_BINARY   0x8000

#define CRT_FILE_BEGIN   0
#define CRT_FILE_CURRENT 1
#define CRT_FILE_END     2

#define CRT_DUPLICATE_SAME_ACCESS 0x00000002

extern BOOL WINAPI FlushFileBuffers(HANDLE hFile);

static CRT_FILE crt_files[CRT_FILE_MAX];
static int crt_files_init = 0;
static CRT_FILE_PROXY_SLOT crt_file_proxies[CRT_FILE_PROXY_PROCESS_SLOTS];
static volatile uint32_t crt_file_proxy_lock;
static volatile uint32_t crt_open_trace_count;
static volatile uint32_t crt_close_trace_count;
static volatile uint32_t crt_read_trace_count;
static volatile uint32_t crt_lseek_trace_count;
static volatile uint32_t crt_lseek32_trace_count;
static volatile uint32_t crt_stat_trace_count;

static int crt_io_trace_take(volatile uint32_t *counter, uint32_t limit)
{
    if (limit > 8) limit = 8;
    return __sync_fetch_and_add(counter, 1) < limit;
}

/* Standard streams as FILE indices */
#define CRT_STDIN   (&crt_files[0])
#define CRT_STDOUT  (&crt_files[1])
#define CRT_STDERR  (&crt_files[2])

static void ensure_stdio_init(void)
{
    if (crt_files_init) return;
    crt_files_init = 1;
    crt_files[0].nt_handle = (HANDLE)(ULONG_PTR)4;   /* stdin */
    crt_files[0].flags = 1;
    crt_files[0].open_flags = CRT_O_TEXT;
    crt_files[1].nt_handle = (HANDLE)(ULONG_PTR)8;   /* stdout */
    crt_files[1].flags = 2;
    crt_files[1].open_flags = CRT_O_TEXT | CRT_O_WRONLY;
    crt_files[2].nt_handle = (HANDLE)(ULONG_PTR)12;  /* stderr */
    crt_files[2].flags = 2;
    crt_files[2].open_flags = CRT_O_TEXT | CRT_O_WRONLY;
}

static void crt_file_proxy_lock_acquire(void)
{
    while (__sync_lock_test_and_set(&crt_file_proxy_lock, 1)) {
        for (int spin = 0; spin < 100; spin++)
            __asm__ volatile ("pause" ::: "memory");
        sched_yield();
    }
}

static void crt_file_proxy_lock_release(void)
{
    __sync_lock_release(&crt_file_proxy_lock);
}

static int crt_file_proxy_flags(const CRT_FILE *file)
{
    int flags = 0;
    if (file->flags & 1) flags |= 0x0001; /* _IOREAD */
    if (file->flags & 2) flags |= 0x0002; /* _IOWRT */
    if (file->flags & 4) flags |= 0x0010; /* _IOEOF */
    if (file->flags & 8) flags |= 0x0020; /* _IOERR */
    return flags;
}

static void crt_file_proxy_init(CRT_FILE32 *files)
{
    ensure_stdio_init();
    for (int fd = 0; fd < CRT_FILE_MAX; fd++) {
        files[fd].flag = crt_file_proxy_flags(&crt_files[fd]);
        files[fd].file = fd;
    }
}

static CRT_FILE32 *crt_file_proxy_array(BOOL create)
{
    DWORD owner_pid = win32_current_process_id();
    if (!owner_pid) owner_pid = 1;

    CRT_FILE32 *files = NULL;
    BOOL have_free_slot = FALSE;
    crt_file_proxy_lock_acquire();
    for (uint32_t i = 0; i < CRT_FILE_PROXY_PROCESS_SLOTS; i++) {
        if (crt_file_proxies[i].owner_pid == owner_pid) {
            files = crt_file_proxies[i].files;
            break;
        }
        if (!crt_file_proxies[i].owner_pid)
            have_free_slot = TRUE;
    }
    crt_file_proxy_lock_release();

    if (files || !create || !have_free_slot)
        return files;

    CRT_FILE32 *candidate = (CRT_FILE32 *)VirtualAlloc(
        NULL, sizeof(CRT_FILE32) * CRT_FILE_MAX,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ULONG_PTR candidate_end = (ULONG_PTR)candidate +
                              sizeof(CRT_FILE32) * CRT_FILE_MAX - 1;
    if (!candidate || (ULONG_PTR)candidate > (ULONG_PTR)UINT32_MAX ||
        candidate_end > (ULONG_PTR)UINT32_MAX) {
        if (candidate)
            VirtualFree(candidate, 0, MEM_RELEASE);
        return NULL;
    }
    crt_file_proxy_init(candidate);

    CRT_FILE32 *unused = candidate;
    crt_file_proxy_lock_acquire();
    CRT_FILE_PROXY_SLOT *free_slot = NULL;
    for (uint32_t i = 0; i < CRT_FILE_PROXY_PROCESS_SLOTS; i++) {
        CRT_FILE_PROXY_SLOT *slot = &crt_file_proxies[i];
        if (slot->owner_pid == owner_pid) {
            files = slot->files;
            break;
        }
        if (!slot->owner_pid && !free_slot)
            free_slot = slot;
    }
    if (!files && free_slot) {
        free_slot->owner_pid = owner_pid;
        free_slot->files = candidate;
        files = candidate;
        unused = NULL;
    }
    crt_file_proxy_lock_release();

    if (unused)
        VirtualFree(unused, 0, MEM_RELEASE);
    if (files == candidate) {
        serial_puts("[CRT] PE32 stdio proxy pid=");
        serial_putdec(owner_pid);
        serial_puts(" va=0x");
        serial_puthex((ULONG_PTR)files, 8);
        serial_puts("\n");
    }
    return files;
}

static void crt_file_proxy_sync(int fd)
{
    if (fd < 0 || fd >= CRT_FILE_MAX) return;
    CRT_FILE32 *files = crt_file_proxy_array(FALSE);
    if (!files) return;
    files[fd].flag = crt_file_proxy_flags(&crt_files[fd]);
    files[fd].file = fd;
}

static void crt_file_proxy_release_process(DWORD process_id)
{
    crt_file_proxy_lock_acquire();
    for (uint32_t i = 0; i < CRT_FILE_PROXY_PROCESS_SLOTS; i++) {
        if (crt_file_proxies[i].owner_pid == process_id) {
            crt_file_proxies[i].owner_pid = 0;
            crt_file_proxies[i].files = NULL;
        }
    }
    crt_file_proxy_lock_release();
}

static int crt_file_index(CRT_FILE *file)
{
    uintptr_t address = (uintptr_t)file;
    uintptr_t first = (uintptr_t)&crt_files[0];
    uintptr_t end = (uintptr_t)&crt_files[CRT_FILE_MAX];

    ensure_stdio_init();
    if (address >= first && address < end &&
        ((address - first) % sizeof(CRT_FILE)) == 0) {
        int fd = (int)((address - first) / sizeof(CRT_FILE));
        return crt_files[fd].nt_handle ? fd : -1;
    }

    CRT_FILE32 *files = crt_file_proxy_array(FALSE);
    first = (uintptr_t)files;
    end = first + sizeof(CRT_FILE32) * CRT_FILE_MAX;
    if (!files || address < first || address >= end ||
        ((address - first) % sizeof(CRT_FILE32)) != 0)
        return -1;

    int fd = (int)((address - first) / sizeof(CRT_FILE32));
    return crt_files[fd].nt_handle ? fd : -1;
}

static CRT_FILE *crt_file_resolve(CRT_FILE *file, int *fd_out)
{
    int fd = crt_file_index(file);
    if (fd_out) *fd_out = fd;
    return fd >= 0 ? &crt_files[fd] : NULL;
}

static CRT_FILE *crt_file_from_fd(int fd)
{
    ensure_stdio_init();
    if (fd < 0 || fd >= CRT_FILE_MAX || !crt_files[fd].nt_handle)
        return NULL;
    return &crt_files[fd];
}

static CRT_FILE *crt_file_for_caller(int fd)
{
    CRT_FILE *file = crt_file_from_fd(fd);
    if (!file || !g_compat32_mode)
        return file;

    CRT_FILE32 *files = crt_file_proxy_array(TRUE);
    if (!files) {
        *crt_errno() = CRT_ENOMEM;
        return NULL;
    }
    crt_file_proxy_sync(fd);
    return (CRT_FILE *)(ULONG_PTR)&files[fd];
}

static int crt_file_attach(HANDLE handle, int open_flags)
{
    if (!handle || handle == INVALID_HANDLE_VALUE) {
        *crt_errno() = CRT_EBADF;
        return -1;
    }

    ensure_stdio_init();
    for (int fd = 3; fd < CRT_FILE_MAX; fd++) {
        if (crt_files[fd].nt_handle) continue;
        crt_files[fd].nt_handle = handle;
        crt_files[fd].flags = (open_flags & CRT_O_RDWR) ? 3 :
                              (open_flags & CRT_O_WRONLY) ? 2 : 1;
        crt_files[fd].ungetc_ch = -1;
        crt_files[fd].open_flags = open_flags;
        crt_files[fd].owns_handle = 1;
        crt_file_proxy_sync(fd);
        return fd;
    }

    *crt_errno() = CRT_EMFILE;
    return -1;
}

CRT_FILE* WINAPI crt_iob_func(void)
{
    ensure_stdio_init();
    if (!g_compat32_mode)
        return &crt_files[0];

    CRT_FILE32 *files = crt_file_proxy_array(TRUE);
    return (CRT_FILE *)(ULONG_PTR)files;
}

CRT_FILE* WINAPI crt_acrt_iob_func(unsigned int index)
{
    if (index >= 3)
        return NULL;
    return crt_file_for_caller((int)index);
}

CRT_FILE* WINAPI crt_fopen(const char *path, const char *mode)
{
    if (!path || !mode || !mode[0]) {
        *crt_errno() = CRT_EINVAL;
        return NULL;
    }

    int open_flags;
    if (mode[0] == 'r') open_flags = 0;
    else if (mode[0] == 'w')
        open_flags = CRT_O_WRONLY | CRT_O_CREAT | CRT_O_TRUNC;
    else if (mode[0] == 'a')
        open_flags = CRT_O_WRONLY | CRT_O_CREAT | CRT_O_APPEND;
    else {
        *crt_errno() = CRT_EINVAL;
        return NULL;
    }

    open_flags |= CRT_O_TEXT;
    for (const char *option = mode + 1; *option; option++) {
        if (*option == '+') {
            open_flags &= ~CRT_O_WRONLY;
            open_flags |= CRT_O_RDWR;
        } else if (*option == 'b') {
            open_flags &= ~CRT_O_TEXT;
            open_flags |= CRT_O_BINARY;
        }
    }

    int fd = crt_open(path, open_flags, 0666);
    return fd >= 0 ? crt_file_for_caller(fd) : NULL;
}

CRT_FILE* WINAPI crt_wfopen(const uint16_t *wpath, const uint16_t *wmode)
{
    /* Convert wide strings to narrow */
    char path[260], mode[16];
    int i;
    for (i = 0; i < 259 && wpath[i]; i++) path[i] = (char)wpath[i];
    path[i] = 0;
    for (i = 0; i < 15 && wmode[i]; i++) mode[i] = (char)wmode[i];
    mode[i] = 0;
    return crt_fopen(path, mode);
}

SIZE_T WINAPI crt_fread(PVOID buf, SIZE_T size, SIZE_T count, CRT_FILE *f)
{
    int fd;
    CRT_FILE *file = crt_file_resolve(f, &fd);
    if (!size || !count) return 0;
    if (!buf || !file || !(file->flags & 1)) {
        if (file) {
            file->flags |= 8;
            crt_file_proxy_sync(fd);
        }
        return 0;
    }
    DWORD total = (DWORD)(size * count);
    DWORD bytes_read = 0;
    BOOL ok = ReadFile(file->nt_handle, buf, total, &bytes_read, NULL);
    if (!ok) file->flags |= 8;
    else if (bytes_read == 0) file->flags |= 4;
    crt_file_proxy_sync(fd);
    return bytes_read / size;
}

SIZE_T WINAPI crt_fwrite(PCVOID buf, SIZE_T size, SIZE_T count, CRT_FILE *f)
{
    int fd;
    CRT_FILE *file = crt_file_resolve(f, &fd);
    if (!size || !count) return 0;
    if (!buf || !file || !(file->flags & 2)) {
        if (file) {
            file->flags |= 8;
            crt_file_proxy_sync(fd);
        }
        return 0;
    }
    DWORD total = (DWORD)(size * count);
    DWORD bytes_written = 0;
    if (!WriteFile(file->nt_handle, buf, total, &bytes_written, NULL)) {
        file->flags |= 8;
        crt_file_proxy_sync(fd);
    }

    /* Echo text writes to serial (captures engine log output) */
    if (total > 0 && total < 4096) {
        const char *s = (const char *)buf;
        serial_puts("[LOG] ");
        for (DWORD i = 0; i < total && i < 200; i++) {
            char c = s[i];
            if (c >= 32 && c < 127) {
                char b[2] = { c, 0 };
                serial_puts(b);
            } else if (c == '\n') {
                serial_puts("\n[LOG] ");
            }
        }
        serial_puts("\n");
    }

    return bytes_written / size;
}

int WINAPI crt_fclose(CRT_FILE *f)
{
    int fd = crt_file_index(f);
    return fd < 0 ? -1 : crt_close(fd);
}

int WINAPI crt_fseek(CRT_FILE *f, long offset, int whence)
{
    int fd = crt_file_index(f);
    if (fd < 0 || crt_lseeki64(fd, (LONGLONG)offset, whence) < 0)
        return -1;
    return 0;
}

long WINAPI crt_ftell(CRT_FILE *f)
{
    CRT_FILE *file = crt_file_resolve(f, NULL);
    if (!file) return -1;
    /* Get current position by seeking 0 from current */
    IO_STATUS_BLOCK iosb;
    FILE_POSITION_INFORMATION pos_info;
    NTSTATUS status = NtQueryInformationFile(file->nt_handle, &iosb, &pos_info,
                                              sizeof(pos_info),
                                              FilePositionInformation);
    if (!NT_SUCCESS(status)) return -1;
    return (long)pos_info.CurrentByteOffset.QuadPart;
}

int WINAPI crt_fflush(CRT_FILE *f)
{
    if (!f) return 0;
    CRT_FILE *file = crt_file_resolve(f, NULL);
    if (!file) return -1;
    if (!(file->flags & 2)) return 0;
    return FlushFileBuffers(file->nt_handle) ? 0 : -1;
}
int WINAPI crt_feof(CRT_FILE *f)
{
    CRT_FILE *file = crt_file_resolve(f, NULL);
    return file ? (file->flags & 4) : 0;
}

int WINAPI crt_ferror(CRT_FILE *f)
{
    CRT_FILE *file = crt_file_resolve(f, NULL);
    return file ? (file->flags & 8) : 0;
}

int WINAPI crt_fgetc(CRT_FILE *f)
{
    int fd;
    CRT_FILE *file = crt_file_resolve(f, &fd);
    if (!file) return -1;
    if (file->flags & 16) {
        file->flags &= ~16;
        crt_file_proxy_sync(fd);
        return file->ungetc_ch;
    }
    char c;
    DWORD read;
    if (!ReadFile(file->nt_handle, &c, 1, &read, NULL) || read == 0) {
        file->flags |= 4;
        crt_file_proxy_sync(fd);
        return -1;
    }
    return (unsigned char)c;
}

int WINAPI crt_fputc(int c, CRT_FILE *f)
{
    int fd;
    CRT_FILE *file = crt_file_resolve(f, &fd);
    if (!file) return -1;
    char ch = (char)c;
    DWORD written = 0;
    if (!WriteFile(file->nt_handle, &ch, 1, &written, NULL) || written != 1) {
        file->flags |= 8;
        crt_file_proxy_sync(fd);
        return -1;
    }
    return (unsigned char)ch;
}

char* WINAPI crt_fgets(char *buf, int n, CRT_FILE *f)
{
    if (!buf || !crt_file_resolve(f, NULL) || n <= 0) return NULL;
    int i;
    for (i = 0; i < n - 1; i++) {
        int c = crt_fgetc(f);
        if (c == -1) {
            if (i == 0) return NULL;
            break;
        }
        buf[i] = (char)c;
        if (c == '\n') { i++; break; }
    }
    buf[i] = 0;
    return buf;
}

int WINAPI crt_fputs(const char *s, CRT_FILE *f)
{
    int fd;
    CRT_FILE *file = crt_file_resolve(f, &fd);
    if (!file || !s) return -1;
    SIZE_T len = crt_strlen(s);
    DWORD written = 0;
    if (!WriteFile(file->nt_handle, s, (DWORD)len, &written, NULL)) {
        file->flags |= 8;
        crt_file_proxy_sync(fd);
        return -1;
    }
    return (int)written;
}

int WINAPI crt_ungetc(int c, CRT_FILE *f)
{
    int fd;
    CRT_FILE *file = crt_file_resolve(f, &fd);
    if (!file || c == -1) return -1;
    file->ungetc_ch = c;
    file->flags |= 16;
    file->flags &= ~4;  /* clear EOF */
    crt_file_proxy_sync(fd);
    return c;
}

int WINAPI crt_fileno(CRT_FILE *f)
{
    int fd = crt_file_index(f);
    if (fd < 0) *crt_errno() = CRT_EBADF;
    return fd;
}

static void WINAPI crt_clearerr(CRT_FILE *file)
{
    int fd;
    CRT_FILE *native = crt_file_resolve(file, &fd);
    if (native) {
        native->flags &= ~(4 | 8);
        crt_file_proxy_sync(fd);
    }
}

static void WINAPI crt_rewind(CRT_FILE *file)
{
    if (file && crt_fseek(file, 0, CRT_FILE_BEGIN) == 0)
        crt_clearerr(file);
}

static int WINAPI crt_setvbuf(CRT_FILE *file, char *buffer, int mode,
                              SIZE_T size)
{
    (void)buffer;
    (void)size;
    if (!crt_file_resolve(file, NULL) ||
        (mode != 0 && mode != 4 && mode != 64)) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }
    /* Osito's CRT streams are unbuffered, so all valid policies are already
     * synchronized with the backing NT handle. */
    *crt_errno() = 0;
    return 0;
}

/* ── Conversion ────────────────────────────────────────────── */

static int crt_errno_from_last_error(void)
{
    switch (GetLastError()) {
    case 2:  /* ERROR_FILE_NOT_FOUND */
    case 3:  /* ERROR_PATH_NOT_FOUND */
        return 2;
    case 5:  /* ERROR_ACCESS_DENIED */
    case 32: /* ERROR_SHARING_VIOLATION */
    case 33: /* ERROR_LOCK_VIOLATION */
        return 13;
    case 6:  /* ERROR_INVALID_HANDLE */
        return CRT_EBADF;
    case 80:  /* ERROR_FILE_EXISTS */
    case 183: /* ERROR_ALREADY_EXISTS */
        return 17;
    case 112: /* ERROR_DISK_FULL */
        return 28;
    default:
        return CRT_EINVAL;
    }
}

static DWORD crt_open_access(int flags)
{
    if (flags & CRT_O_RDWR) return GENERIC_READ | GENERIC_WRITE;
    if (flags & CRT_O_WRONLY) return GENERIC_WRITE;
    return GENERIC_READ;
}

static DWORD crt_open_disposition(int flags)
{
    if ((flags & (CRT_O_CREAT | CRT_O_EXCL)) ==
        (CRT_O_CREAT | CRT_O_EXCL))
        return 1; /* CREATE_NEW */
    if ((flags & (CRT_O_CREAT | CRT_O_TRUNC)) ==
        (CRT_O_CREAT | CRT_O_TRUNC))
        return 2; /* CREATE_ALWAYS */
    if (flags & CRT_O_CREAT) return 4; /* OPEN_ALWAYS */
    if (flags & CRT_O_TRUNC) return 5; /* TRUNCATE_EXISTING */
    return 3; /* OPEN_EXISTING */
}

static int crt_open_handle(HANDLE handle, int flags)
{
    if (handle == INVALID_HANDLE_VALUE) {
        *crt_errno() = crt_errno_from_last_error();
        return -1;
    }

    int fd = crt_file_attach(handle, flags);
    if (fd < 0) {
        CloseHandle(handle);
        return -1;
    }
    if ((flags & CRT_O_APPEND) && crt_lseeki64(fd, 0, CRT_FILE_END) < 0) {
        crt_close(fd);
        return -1;
    }
    *crt_errno() = 0;
    return fd;
}

int WINAPI crt_open(const char *path, int flags, int mode)
{
    (void)mode;
    if (!path || !path[0]) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }

    HANDLE handle = CreateFileA(path, crt_open_access(flags),
                                FILE_SHARE_READ | FILE_SHARE_WRITE |
                                    FILE_SHARE_DELETE,
                                NULL, crt_open_disposition(flags), 0, NULL);
    int fd = crt_open_handle(handle, flags);
    if (crt_io_trace_take(&crt_open_trace_count, 64)) {
        serial_puts("[CRT-OPEN] fd=");
        serial_putdec((uint64_t)(uint32_t)fd);
        serial_puts(" flags=0x");
        serial_puthex((uint32_t)flags, 8);
        serial_puts(" mode=0x");
        serial_puthex((uint32_t)mode, 8);
        serial_puts(" handle=0x");
        serial_puthex((ULONG_PTR)handle, 8);
        serial_puts(" errno=");
        serial_putdec((uint64_t)(uint32_t)*crt_errno());
        serial_puts(" path='");
        serial_puts(path);
        serial_puts("'\n");
    }
    return fd;
}

int WINAPI crt_wopen(const WCHAR *path, int flags, int mode)
{
    (void)mode;
    if (!path || !path[0]) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }

    HANDLE handle = CreateFileW(path, crt_open_access(flags),
                                FILE_SHARE_READ | FILE_SHARE_WRITE |
                                    FILE_SHARE_DELETE,
                                NULL, crt_open_disposition(flags), 0, NULL);
    return crt_open_handle(handle, flags);
}

LONG_PTR WINAPI crt_get_osfhandle(int fd)
{
    CRT_FILE *file = crt_file_from_fd(fd);
    if (!file) {
        *crt_errno() = CRT_EBADF;
        return -1;
    }
    return (LONG_PTR)(ULONG_PTR)file->nt_handle;
}

int WINAPI crt_open_osfhandle(LONG_PTR handle, int flags)
{
    int fd = crt_file_attach((HANDLE)(ULONG_PTR)handle, flags);
    if (fd >= 0) *crt_errno() = 0;
    return fd;
}

int WINAPI crt_close(int fd)
{
    int trace = crt_io_trace_take(&crt_close_trace_count, 64);
    CRT_FILE *file = crt_file_from_fd(fd);
    if (!file) {
        *crt_errno() = CRT_EBADF;
        if (trace) {
            serial_puts("[CRT-CLOSE] invalid fd=");
            serial_putdec((uint64_t)(uint32_t)fd);
            serial_puts("\n");
        }
        return -1;
    }

    /* Standard handles are kernel pseudo-handles shared by every Win32
     * process. Keep their backing objects alive. */
    if (fd < 3) {
        *crt_errno() = 0;
        return 0;
    }
    if (file->owns_handle && !CloseHandle(file->nt_handle)) {
        *crt_errno() = crt_errno_from_last_error();
        return -1;
    }

    file->nt_handle = NULL;
    file->flags = 0;
    file->ungetc_ch = -1;
    file->open_flags = 0;
    file->owns_handle = 0;
    crt_file_proxy_sync(fd);
    *crt_errno() = 0;
    if (trace) {
        serial_puts("[CRT-CLOSE] fd=");
        serial_putdec((uint64_t)(uint32_t)fd);
        serial_puts(" ok\n");
    }
    return 0;
}

int WINAPI crt_read(int fd, PVOID buffer, unsigned int count)
{
    int trace = count != 1 && crt_io_trace_take(&crt_read_trace_count, 96);
    CRT_FILE *file = crt_file_from_fd(fd);
    if (!file || !(file->flags & 1)) {
        *crt_errno() = CRT_EBADF;
        if (trace) {
            serial_puts("[CRT-READ] invalid fd=");
            serial_putdec((uint64_t)(uint32_t)fd);
            serial_puts(" count=");
            serial_putdec(count);
            serial_puts("\n");
        }
        return -1;
    }
    if (!buffer && count) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }

    DWORD transferred = 0;
    if (!ReadFile(file->nt_handle, buffer, count, &transferred, NULL)) {
        file->flags |= 8;
        crt_file_proxy_sync(fd);
        *crt_errno() = crt_errno_from_last_error();
        if (trace) {
            serial_puts("[CRT-READ] fd=");
            serial_putdec((uint64_t)(uint32_t)fd);
            serial_puts(" count=");
            serial_putdec(count);
            serial_puts(" failed errno=");
            serial_putdec((uint64_t)(uint32_t)*crt_errno());
            serial_puts("\n");
        }
        return -1;
    }
    if (!transferred && count) file->flags |= 4;
    crt_file_proxy_sync(fd);
    *crt_errno() = 0;
    if (trace) {
        serial_puts("[CRT-READ] fd=");
        serial_putdec((uint64_t)(uint32_t)fd);
        serial_puts(" count=");
        serial_putdec(count);
        serial_puts(" -> ");
        serial_putdec(transferred);
        serial_puts("\n");
    }
    return (int)transferred;
}

int WINAPI crt_write(int fd, PCVOID buffer, unsigned int count)
{
    CRT_FILE *file = crt_file_from_fd(fd);
    if (!file || !(file->flags & 2)) {
        *crt_errno() = CRT_EBADF;
        return -1;
    }
    if (!buffer && count) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }

    DWORD transferred = 0;
    if (!WriteFile(file->nt_handle, buffer, count, &transferred, NULL)) {
        file->flags |= 8;
        crt_file_proxy_sync(fd);
        *crt_errno() = crt_errno_from_last_error();
        return -1;
    }
    *crt_errno() = 0;
    return (int)transferred;
}

LONGLONG WINAPI crt_lseeki64(int fd, LONGLONG offset, int origin)
{
    int trace = crt_io_trace_take(&crt_lseek_trace_count, 96);
    CRT_FILE *file = crt_file_from_fd(fd);
    if (!file) {
        *crt_errno() = CRT_EBADF;
        if (trace) {
            serial_puts("[CRT-LSEEK] invalid fd=");
            serial_putdec((uint64_t)(uint32_t)fd);
            serial_puts("\n");
        }
        return -1;
    }
    if (origin < CRT_FILE_BEGIN || origin > CRT_FILE_END) {
        *crt_errno() = CRT_EINVAL;
        if (trace) {
            serial_puts("[CRT-LSEEK] fd=");
            serial_putdec((uint64_t)(uint32_t)fd);
            serial_puts(" invalid origin=");
            serial_putdec((uint64_t)(uint32_t)origin);
            serial_puts(" offset=0x");
            serial_puthex((uint64_t)offset, 16);
            serial_puts("\n");
        }
        return -1;
    }

    LONG high = (LONG)((ULONGLONG)offset >> 32);
    SetLastError(0);
    DWORD low = SetFilePointer(file->nt_handle, (LONG)offset, &high,
                               (DWORD)origin);
    if (low == 0xFFFFFFFFU && GetLastError() != 0) {
        *crt_errno() = crt_errno_from_last_error();
        if (trace) {
            serial_puts("[CRT-LSEEK] fd=");
            serial_putdec((uint64_t)(uint32_t)fd);
            serial_puts(" failed errno=");
            serial_putdec((uint64_t)(uint32_t)*crt_errno());
            serial_puts("\n");
        }
        return -1;
    }

    file->flags &= ~4;
    crt_file_proxy_sync(fd);
    *crt_errno() = 0;
    LONGLONG result = (LONGLONG)(((ULONGLONG)(ULONG)high << 32) | low);
    if (trace) {
        serial_puts("[CRT-LSEEK] fd=");
        serial_putdec((uint64_t)(uint32_t)fd);
        serial_puts(" origin=");
        serial_putdec((uint64_t)(uint32_t)origin);
        serial_puts(" offset=0x");
        serial_puthex((uint64_t)offset, 16);
        serial_puts(" -> 0x");
        serial_puthex((uint64_t)result, 16);
        serial_puts("\n");
    }
    return result;
}

static LONGLONG WINAPI crt_lseeki64_compat32(uint32_t fd,
                                              uint32_t offset_low,
                                              uint32_t offset_high,
                                              uint32_t origin)
{
    ULONGLONG bits = ((ULONGLONG)offset_high << 32) | offset_low;
    if (crt_io_trace_take(&crt_lseek32_trace_count, 96)) {
        serial_puts("[CRT-LSEEK32] fd=");
        serial_putdec(fd);
        serial_puts(" raw=0x");
        serial_puthex(offset_high, 8);
        serial_puthex(offset_low, 8);
        serial_puts(" origin=");
        serial_putdec(origin);
        serial_puts("\n");
    }
    return crt_lseeki64((int)fd, (LONGLONG)bits, (int)origin);
}

LONG WINAPI crt_lseek(int fd, LONG offset, int origin)
{
    LONGLONG result = crt_lseeki64(fd, offset, origin);
    if (result < (LONGLONG)(-2147483647 - 1) || result > 2147483647) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }
    return (LONG)result;
}

int WINAPI crt_dup(int fd)
{
    CRT_FILE *source = crt_file_from_fd(fd);
    if (!source) {
        *crt_errno() = CRT_EBADF;
        return -1;
    }

    HANDLE duplicate = source->nt_handle;
    BOOL owns_handle = FALSE;
    if (source->owns_handle) {
        if (!DuplicateHandle(GetCurrentProcess(), source->nt_handle,
                             GetCurrentProcess(), &duplicate, 0, FALSE,
                             CRT_DUPLICATE_SAME_ACCESS)) {
            *crt_errno() = crt_errno_from_last_error();
            return -1;
        }
        owns_handle = TRUE;
    }

    int new_fd = crt_file_attach(duplicate, source->open_flags);
    if (new_fd < 0) {
        if (owns_handle) CloseHandle(duplicate);
        return -1;
    }
    crt_files[new_fd].owns_handle = owns_handle;
    *crt_errno() = 0;
    return new_fd;
}

int WINAPI crt_dup2(int source_fd, int target_fd)
{
    CRT_FILE *source = crt_file_from_fd(source_fd);
    if (!source || target_fd < 0 || target_fd >= CRT_FILE_MAX) {
        *crt_errno() = CRT_EBADF;
        return -1;
    }
    if (source_fd == target_fd) return 0;

    HANDLE duplicate = source->nt_handle;
    BOOL owns_handle = FALSE;
    if (source->owns_handle) {
        if (!DuplicateHandle(GetCurrentProcess(), source->nt_handle,
                             GetCurrentProcess(), &duplicate, 0, FALSE,
                             CRT_DUPLICATE_SAME_ACCESS)) {
            *crt_errno() = crt_errno_from_last_error();
            return -1;
        }
        owns_handle = TRUE;
    }

    CRT_FILE *target = &crt_files[target_fd];
    if (target->nt_handle && target->owns_handle)
        CloseHandle(target->nt_handle);
    *target = *source;
    target->nt_handle = duplicate;
    target->owns_handle = owns_handle;
    target->flags &= ~(4 | 8 | 16);
    target->ungetc_ch = -1;
    crt_file_proxy_sync(target_fd);
    *crt_errno() = 0;
    return 0;
}

int WINAPI crt_commit(int fd)
{
    CRT_FILE *file = crt_file_from_fd(fd);
    if (!file) {
        *crt_errno() = CRT_EBADF;
        return -1;
    }
    if (!FlushFileBuffers(file->nt_handle)) {
        *crt_errno() = crt_errno_from_last_error();
        return -1;
    }
    *crt_errno() = 0;
    return 0;
}

int WINAPI crt_isatty(int fd)
{
    CRT_FILE *file = crt_file_from_fd(fd);
    if (!file) {
        *crt_errno() = CRT_EBADF;
        return 0;
    }
    *crt_errno() = 0;
    return file->nt_handle == (HANDLE)(ULONG_PTR)4 ||
           file->nt_handle == (HANDLE)(ULONG_PTR)8 ||
           file->nt_handle == (HANDLE)(ULONG_PTR)12;
}

int WINAPI crt_setmode(int fd, int mode)
{
    CRT_FILE *file = crt_file_from_fd(fd);
    const int mode_mask = CRT_O_TEXT | CRT_O_BINARY | 0x10000 | 0x20000 |
                          0x40000;
    if (!file) {
        *crt_errno() = CRT_EBADF;
        return -1;
    }
    if (mode != CRT_O_TEXT && mode != CRT_O_BINARY && mode != 0x10000 &&
        mode != 0x20000 && mode != 0x40000) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }

    int previous = file->open_flags & mode_mask;
    if (!previous) previous = CRT_O_TEXT;
    file->open_flags = (file->open_flags & ~mode_mask) | mode;
    *crt_errno() = 0;
    return previous;
}

int WINAPI crt_chsize_s(int fd, ULONGLONG size)
{
    CRT_FILE *file = crt_file_from_fd(fd);
    IO_STATUS_BLOCK iosb;
    FILE_POSITION_INFORMATION original;
    LARGE_INTEGER end;
    if (!file) {
        *crt_errno() = CRT_EBADF;
        return CRT_EBADF;
    }

    NTSTATUS status = NtQueryInformationFile(file->nt_handle, &iosb, &original,
                                              sizeof(original),
                                              FilePositionInformation);
    if (!NT_SUCCESS(status)) {
        *crt_errno() = CRT_EBADF;
        return CRT_EBADF;
    }

    end.QuadPart = (LONGLONG)size;
    status = NtSetInformationFile(file->nt_handle, &iosb, &end, sizeof(end),
                                  FileEndOfFileInformation);
    (void)NtSetInformationFile(file->nt_handle, &iosb, &original,
                               sizeof(original), FilePositionInformation);
    if (!NT_SUCCESS(status)) {
        *crt_errno() = CRT_EINVAL;
        return CRT_EINVAL;
    }
    *crt_errno() = 0;
    return 0;
}

int WINAPI crt_atoi(const char *s)
{
    return (int)crt_strtol(s, NULL, 10);
}

long WINAPI crt_atol(const char *s)
{
    return crt_strtol(s, NULL, 10);
}

double WINAPI crt_atof(const char *s)
{
    if (!s) return 0.0;

    /* Skip leading whitespace */
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;

    /* Sign */
    double sign = 1.0;
    if (*s == '-') { sign = -1.0; s++; }
    else if (*s == '+') s++;

    /* Integer part */
    double result = 0.0;
    while (*s >= '0' && *s <= '9') {
        result = result * 10.0 + (*s - '0');
        s++;
    }

    /* Fractional part */
    if (*s == '.') {
        s++;
        double frac = 0.0;
        double divisor = 10.0;
        while (*s >= '0' && *s <= '9') {
            frac += (*s - '0') / divisor;
            divisor *= 10.0;
            s++;
        }
        result += frac;
    }

    return sign * result;
}

int WINAPI crt_abs(int value)
{
    return value < 0 ? -value : value;
}

long WINAPI crt_strtol(const char *s, char **endptr, int base)
{
    long result = 0;
    int neg = 0;

    while (*s == ' ' || *s == '\t') s++;

    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;

    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (s[0] == '0') { base = 8; s++; }
        else base = 10;
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    while (*s) {
        int digit;
        if (*s >= '0' && *s <= '9') digit = *s - '0';
        else if (*s >= 'a' && *s <= 'f') digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') digit = *s - 'A' + 10;
        else break;
        if (digit >= base) break;
        result = result * base + digit;
        s++;
    }

    if (endptr) *endptr = (char *)s;
    return neg ? -result : result;
}

unsigned long WINAPI crt_strtoul(const char *s, char **endptr, int base)
{
    /* Same logic, unsigned */
    return (unsigned long)crt_strtol(s, endptr, base);
}

/* ── Process ───────────────────────────────────────────────── */

#define ATEXIT_MAX 32
static void (*atexit_funcs[ATEXIT_MAX])(void);
static int atexit_count = 0;

void WINAPI crt_exit(int code)
{

    /* In compat32 mode, skip atexit handlers — PE32 cleanup code
     * tends to crash (NULL vtable calls, uninitialized subsystems).
     * Just terminate cleanly. */
    if (!g_compat32_mode) {
        for (int i = atexit_count - 1; i >= 0; i--)
            atexit_funcs[i]();
    }
    ExitProcess((DWORD)code);
}

void WINAPI crt_abort(void)
{
    serial_puts("[MSVCRT] abort() called\n");
    ExitProcess(3); /* SIGABRT-like */
}

void WINAPI crt__exit(int code)
{
    ExitProcess((DWORD)code);
}

int WINAPI crt_getpid(void)
{
    return (int)GetCurrentProcessId();
}

ULONG_PTR WINAPI crt_beginthreadex(PVOID security, unsigned stack_size,
                                    PVOID start_address, PVOID argument,
                                    unsigned init_flags, unsigned *thread_id)
{
    HANDLE thread = CreateThread(
        security, (SIZE_T)stack_size,
        (LPTHREAD_START_ROUTINE)(ULONG_PTR)start_address, argument,
        (DWORD)init_flags, (DWORD *)thread_id);
    return (ULONG_PTR)thread;
}

void WINAPI __attribute__((noreturn)) crt_endthreadex(unsigned exit_code)
{
    ExitThread((DWORD)exit_code);
    __builtin_unreachable();
}

int WINAPI crt_atexit(void (*func)(void))
{
    if (atexit_count >= ATEXIT_MAX) return -1;
    atexit_funcs[atexit_count++] = func;
    return 0;
}

/* Universal CRT startup uses a caller-owned descriptor containing three
 * pointers: first registered callback, next free slot, and allocation end.
 * Keep the descriptor layout dependent on the importing PE instead of the
 * kernel's native pointer width. */
typedef struct {
    uint32_t first;
    uint32_t last;
    uint32_t end;
} UCRT_ONEXIT_TABLE32;

typedef struct {
    ULONG_PTR first;
    ULONG_PTR last;
    ULONG_PTR end;
} UCRT_ONEXIT_TABLE64;

#define UCRT_ONEXIT_INITIAL_CAPACITY 32U
#define UCRT_ONEXIT_MAX_CAPACITY     (1U << 20)
#define UCRT_INVALID_HANDLER_SLOTS   512U
#define UCRT_PROCESS_MODE_SLOTS      512U

typedef struct {
    DWORD owner_pid;
    DWORD owner_tid;
    PVOID handler;
    BOOL used;
} UCRT_INVALID_HANDLER_SLOT;

struct crt_tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

typedef struct {
    int commode;
    int fmode;
    int new_mode;
    PVOID new_handler;
    struct crt_tm time_buffer;
} UCRT_PROCESS_MODE_VALUES;

typedef struct {
    DWORD owner_pid;
    UCRT_PROCESS_MODE_VALUES *values;
} UCRT_PROCESS_MODE_SLOT;

typedef struct {
    int errno_value;
    ULONG doserrno_value;
} UCRT_THREAD_VALUES;

typedef struct _UCRT_THREAD_STATE {
    struct _UCRT_THREAD_STATE *next;
    DWORD owner_pid;
    DWORD owner_tid;
    UCRT_THREAD_VALUES *values;
} UCRT_THREAD_STATE;

static UCRT_INVALID_HANDLER_SLOT ucrt_invalid_handlers[
    UCRT_INVALID_HANDLER_SLOTS];
static UCRT_PROCESS_MODE_SLOT ucrt_process_modes[UCRT_PROCESS_MODE_SLOTS];
static UCRT_THREAD_STATE *ucrt_thread_states;
static volatile uint32_t ucrt_state_lock;

static void crt_env_release_process(DWORD process_id);
static void crt_locale_release_process(DWORD process_id);

static void ucrt_state_lock_acquire(void)
{
    while (__sync_lock_test_and_set(&ucrt_state_lock, 1)) {
        for (int spin = 0; spin < 100; spin++)
            __asm__ volatile ("pause" ::: "memory");
        sched_yield();
    }
}

static void ucrt_state_lock_release(void)
{
    __sync_lock_release(&ucrt_state_lock);
}

static UCRT_THREAD_VALUES *ucrt_thread_state(BOOL create)
{
    DWORD owner_pid = win32_current_process_id();
    DWORD owner_tid = GetCurrentThreadId();
    if (!owner_pid) owner_pid = 1;

    UCRT_THREAD_VALUES *values = NULL;
    ucrt_state_lock_acquire();
    for (UCRT_THREAD_STATE *state = ucrt_thread_states; state;
         state = state->next) {
        if (state->owner_pid == owner_pid && state->owner_tid == owner_tid) {
            values = state->values;
            break;
        }
    }

    if (!values && create) {
        UCRT_THREAD_STATE *state =
            (UCRT_THREAD_STATE *)kmalloc(sizeof(*state));
        values = (UCRT_THREAD_VALUES *)VirtualAlloc(
            NULL, sizeof(*values), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!state || !values ||
            (g_compat32_mode &&
             (ULONG_PTR)values > (ULONG_PTR)UINT32_MAX)) {
            if (values) VirtualFree(values, 0, MEM_RELEASE);
            if (state) kfree(state);
            values = NULL;
        } else {
            values->errno_value = 0;
            values->doserrno_value = 0;
            state->owner_pid = owner_pid;
            state->owner_tid = owner_tid;
            state->values = values;
            state->next = ucrt_thread_states;
            ucrt_thread_states = state;

            serial_puts("[CRT-PTD] pid=");
            serial_putdec(owner_pid);
            serial_puts(" tid=");
            serial_putdec(owner_tid);
            serial_puts(" va=0x");
            serial_puthex((ULONG_PTR)values,
                          g_compat32_mode ? 8 : 16);
            serial_puts("\n");
        }
    }
    ucrt_state_lock_release();
    return values;
}

static UCRT_PROCESS_MODE_VALUES *ucrt_process_mode_state(BOOL create)
{
    DWORD owner_pid = win32_current_process_id();
    if (!owner_pid) owner_pid = 1;

    UCRT_PROCESS_MODE_SLOT *free_slot = NULL;
    UCRT_PROCESS_MODE_VALUES *values = NULL;
    ucrt_state_lock_acquire();
    for (uint32_t i = 0; i < UCRT_PROCESS_MODE_SLOTS; i++) {
        UCRT_PROCESS_MODE_SLOT *slot = &ucrt_process_modes[i];
        if (slot->owner_pid == owner_pid) {
            values = slot->values;
            break;
        }
        if (!slot->owner_pid && !free_slot)
            free_slot = slot;
    }

    if (!values && create && free_slot) {
        values = (UCRT_PROCESS_MODE_VALUES *)VirtualAlloc(
            NULL, sizeof(*values), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (values && (!g_compat32_mode ||
                       (ULONG_PTR)values <= (ULONG_PTR)UINT32_MAX)) {
            values->commode = 0;
            values->fmode = 0x4000; /* _O_TEXT */
            values->new_mode = 0;
            values->new_handler = NULL;
            free_slot->owner_pid = owner_pid;
            free_slot->values = values;
            serial_puts("[CRT] process mode state pid=");
            serial_putdec(owner_pid);
            serial_puts(" va=0x");
            serial_puthex((ULONG_PTR)values, g_compat32_mode ? 8 : 16);
            serial_puts("\n");
        } else {
            values = NULL;
        }
    }
    ucrt_state_lock_release();
    return values;
}

static int crt_exchange_new_mode(int mode)
{
    UCRT_PROCESS_MODE_VALUES *values = ucrt_process_mode_state(TRUE);
    if (!values) return 0;

    ucrt_state_lock_acquire();
    int previous = values->new_mode;
    values->new_mode = mode;
    ucrt_state_lock_release();
    return previous;
}

static PVOID crt_exchange_new_handler(PVOID handler)
{
    UCRT_PROCESS_MODE_VALUES *values = ucrt_process_mode_state(TRUE);
    if (!values) return NULL;

    ucrt_state_lock_acquire();
    PVOID previous = values->new_handler;
    values->new_handler = handler;
    ucrt_state_lock_release();
    return previous;
}

void msvcrt_release_process(DWORD process_id)
{
    if (!process_id) return;

    UCRT_THREAD_STATE *released_thread_states = NULL;
    ucrt_state_lock_acquire();
    for (uint32_t i = 0; i < UCRT_PROCESS_MODE_SLOTS; i++) {
        if (ucrt_process_modes[i].owner_pid == process_id) {
            ucrt_process_modes[i].owner_pid = 0;
            ucrt_process_modes[i].values = NULL;
        }
    }
    for (uint32_t i = 0; i < UCRT_INVALID_HANDLER_SLOTS; i++) {
        if (ucrt_invalid_handlers[i].used &&
            ucrt_invalid_handlers[i].owner_pid == process_id) {
            ucrt_invalid_handlers[i].used = FALSE;
            ucrt_invalid_handlers[i].owner_pid = 0;
            ucrt_invalid_handlers[i].owner_tid = 0;
            ucrt_invalid_handlers[i].handler = NULL;
        }
    }
    UCRT_THREAD_STATE **link = &ucrt_thread_states;
    while (*link) {
        UCRT_THREAD_STATE *state = *link;
        if (state->owner_pid == process_id) {
            *link = state->next;
            state->next = released_thread_states;
            released_thread_states = state;
        } else {
            link = &state->next;
        }
    }
    ucrt_state_lock_release();

    while (released_thread_states) {
        UCRT_THREAD_STATE *next = released_thread_states->next;
        kfree(released_thread_states);
        released_thread_states = next;
    }

    crt_locale_release_process(process_id);
    crt_env_release_process(process_id);
    crt_file_proxy_release_process(process_id);
}

static void ucrt_onexit_read(PVOID opaque, BOOL is_32bit,
                             ULONG_PTR *first, ULONG_PTR *last,
                             ULONG_PTR *end)
{
    if (is_32bit) {
        UCRT_ONEXIT_TABLE32 *table = (UCRT_ONEXIT_TABLE32 *)opaque;
        *first = table->first;
        *last = table->last;
        *end = table->end;
    } else {
        UCRT_ONEXIT_TABLE64 *table = (UCRT_ONEXIT_TABLE64 *)opaque;
        *first = table->first;
        *last = table->last;
        *end = table->end;
    }
}

static void ucrt_onexit_write(PVOID opaque, BOOL is_32bit,
                              ULONG_PTR first, ULONG_PTR last,
                              ULONG_PTR end)
{
    if (is_32bit) {
        UCRT_ONEXIT_TABLE32 *table = (UCRT_ONEXIT_TABLE32 *)opaque;
        table->first = (uint32_t)first;
        table->last = (uint32_t)last;
        table->end = (uint32_t)end;
    } else {
        UCRT_ONEXIT_TABLE64 *table = (UCRT_ONEXIT_TABLE64 *)opaque;
        table->first = first;
        table->last = last;
        table->end = end;
    }
}

static BOOL ucrt_onexit_valid(ULONG_PTR first, ULONG_PTR last,
                              ULONG_PTR end, SIZE_T slot_size)
{
    if (!first && !last && !end) return TRUE;
    if (!first || !last || !end || last < first || end < last)
        return FALSE;
    if ((last - first) % slot_size || (end - first) % slot_size)
        return FALSE;
    return (end - first) / slot_size <= UCRT_ONEXIT_MAX_CAPACITY;
}

int WINAPI crt_initialize_onexit_table(PVOID opaque)
{
    if (!opaque) return -1;

    ucrt_state_lock_acquire();
    ucrt_onexit_write(opaque, g_compat32_mode ? TRUE : FALSE, 0, 0, 0);
    ucrt_state_lock_release();
    return 0;
}

int WINAPI crt_register_onexit_function(PVOID opaque, PVOID function)
{
    if (!opaque || !function) return -1;

    BOOL is_32bit = g_compat32_mode ? TRUE : FALSE;
    SIZE_T slot_size = is_32bit ? sizeof(uint32_t) : sizeof(ULONG_PTR);
    ULONG_PTR first, last, end;

    ucrt_state_lock_acquire();
    ucrt_onexit_read(opaque, is_32bit, &first, &last, &end);
    if (!ucrt_onexit_valid(first, last, end, slot_size)) {
        ucrt_state_lock_release();
        return -1;
    }

    SIZE_T count = first ? (last - first) / slot_size : 0;
    SIZE_T capacity = first ? (end - first) / slot_size : 0;
    if (count == capacity) {
        SIZE_T new_capacity = capacity ? capacity * 2 :
                              UCRT_ONEXIT_INITIAL_CAPACITY;
        if (new_capacity > UCRT_ONEXIT_MAX_CAPACITY ||
            new_capacity < capacity) {
            ucrt_state_lock_release();
            return -1;
        }

        SIZE_T bytes = new_capacity * slot_size;
        PVOID storage = first
            ? HeapReAlloc(GetProcessHeap(), 0, (PVOID)first, bytes)
            : HeapAlloc(GetProcessHeap(), 0, bytes);
        if (!storage || (is_32bit && (ULONG_PTR)storage > 0xFFFFFFFFULL)) {
            if (storage && !first)
                HeapFree(GetProcessHeap(), 0, storage);
            ucrt_state_lock_release();
            return -1;
        }
        first = (ULONG_PTR)storage;
        last = first + count * slot_size;
        end = first + new_capacity * slot_size;
    }

    if (is_32bit)
        *(uint32_t *)last = (uint32_t)(ULONG_PTR)function;
    else
        *(ULONG_PTR *)last = (ULONG_PTR)function;
    last += slot_size;
    ucrt_onexit_write(opaque, is_32bit, first, last, end);
    ucrt_state_lock_release();
    return 0;
}

int WINAPI crt_execute_onexit_table(PVOID opaque)
{
    if (!opaque) return -1;

    BOOL is_32bit = g_compat32_mode ? TRUE : FALSE;
    SIZE_T slot_size = is_32bit ? sizeof(uint32_t) : sizeof(ULONG_PTR);
    ULONG_PTR first, last, end;

    ucrt_state_lock_acquire();
    ucrt_onexit_read(opaque, is_32bit, &first, &last, &end);
    if (!ucrt_onexit_valid(first, last, end, slot_size)) {
        ucrt_state_lock_release();
        return -1;
    }
    ucrt_onexit_write(opaque, is_32bit, 0, 0, 0);
    ucrt_state_lock_release();

    while (last > first) {
        last -= slot_size;
        ULONG_PTR callback = is_32bit
            ? *(uint32_t *)last : *(ULONG_PTR *)last;
        if (!callback) continue;
        if (is_32bit)
            compat32_callback((uint32_t)callback);
        else
            ((void (WINAPI *)(void))callback)();
    }

    if (first)
        HeapFree(GetProcessHeap(), 0, (PVOID)first);
    return 0;
}

int WINAPI crt_configure_narrow_argv(int mode)
{
    /* _crt_argv_mode has exactly three public values. OsitoK already
     * supplies normalized argv storage; wildcard expansion is not needed by
     * the Win32 loader itself. */
    return mode >= 0 && mode <= 2 ? 0 : -1;
}

int WINAPI crt_initialize_narrow_environment(void)
{
    /* __getmainargs exposes the process environment initialized by winexec. */
    return 0;
}

char *WINAPI crt_get_narrow_winmain_command_line(void)
{
    char *command = (char *)(ULONG_PTR)GetCommandLineA();
    if (!command)
        return NULL;

    char *cursor = command;
    if (*cursor == '"') {
        cursor++;
        while (*cursor && *cursor != '"')
            cursor++;
        if (*cursor == '"')
            cursor++;
    } else {
        while (*cursor && *cursor != ' ' && *cursor != '\t')
            cursor++;
    }
    while (*cursor == ' ' || *cursor == '\t')
        cursor++;
    return cursor;
}

PVOID WINAPI crt_set_thread_local_invalid_parameter_handler(PVOID handler)
{
    DWORD owner_pid = GetCurrentProcessId();
    DWORD owner_tid = GetCurrentThreadId();
    uint32_t start = (owner_pid * 2654435761U ^ owner_tid) &
                     (UCRT_INVALID_HANDLER_SLOTS - 1);
    UCRT_INVALID_HANDLER_SLOT *free_slot = NULL;
    PVOID previous = NULL;

    ucrt_state_lock_acquire();
    for (uint32_t probe = 0; probe < UCRT_INVALID_HANDLER_SLOTS; probe++) {
        UCRT_INVALID_HANDLER_SLOT *slot =
            &ucrt_invalid_handlers[(start + probe) &
                                   (UCRT_INVALID_HANDLER_SLOTS - 1)];
        if (slot->used && slot->owner_pid == owner_pid &&
            slot->owner_tid == owner_tid) {
            previous = slot->handler;
            if (handler) {
                slot->handler = handler;
            } else {
                slot->used = FALSE;
                slot->owner_pid = 0;
                slot->owner_tid = 0;
                slot->handler = NULL;
            }
            ucrt_state_lock_release();
            return previous;
        }
        if (!slot->used && !free_slot) free_slot = slot;
    }

    if (handler && free_slot) {
        free_slot->owner_pid = owner_pid;
        free_slot->owner_tid = owner_tid;
        free_slot->handler = handler;
        free_slot->used = TRUE;
    }
    ucrt_state_lock_release();
    return previous;
}

/* Locale. The CRT starts in C and OsitoK currently has no configurable
 * user-locale backend, so an empty locale name resolves to C as well. */
#define CRT_LC_ALL       0
#define CRT_LC_COLLATE   1
#define CRT_LC_CTYPE     2
#define CRT_LC_MONETARY  3
#define CRT_LC_NUMERIC   4
#define CRT_LC_TIME      5
#define CRT_CHAR_MAX     127

typedef struct {
    char *decimal_point;
    char *thousands_sep;
    char *grouping;
    char *int_curr_symbol;
    char *currency_symbol;
    char *mon_decimal_point;
    char *mon_thousands_sep;
    char *mon_grouping;
    char *positive_sign;
    char *negative_sign;
    char int_frac_digits;
    char frac_digits;
    char p_cs_precedes;
    char p_sep_by_space;
    char n_cs_precedes;
    char n_sep_by_space;
    char p_sign_posn;
    char n_sign_posn;
    WCHAR *_W_decimal_point;
    WCHAR *_W_thousands_sep;
    WCHAR *_W_int_curr_symbol;
    WCHAR *_W_currency_symbol;
    WCHAR *_W_mon_decimal_point;
    WCHAR *_W_mon_thousands_sep;
    WCHAR *_W_positive_sign;
    WCHAR *_W_negative_sign;
} CRT_LCONV;

/* PE32 sees 32-bit pointers inside struct lconv even though the shim itself
 * is compiled as x86-64. Keep a separate layout for compat-mode callers. */
typedef struct {
    uint32_t decimal_point;
    uint32_t thousands_sep;
    uint32_t grouping;
    uint32_t int_curr_symbol;
    uint32_t currency_symbol;
    uint32_t mon_decimal_point;
    uint32_t mon_thousands_sep;
    uint32_t mon_grouping;
    uint32_t positive_sign;
    uint32_t negative_sign;
    char int_frac_digits;
    char frac_digits;
    char p_cs_precedes;
    char p_sep_by_space;
    char n_cs_precedes;
    char n_sep_by_space;
    char p_sign_posn;
    char n_sign_posn;
    uint32_t _W_decimal_point;
    uint32_t _W_thousands_sep;
    uint32_t _W_int_curr_symbol;
    uint32_t _W_currency_symbol;
    uint32_t _W_mon_decimal_point;
    uint32_t _W_mon_thousands_sep;
    uint32_t _W_positive_sign;
    uint32_t _W_negative_sign;
} CRT_LCONV32;

static char crt_locale_c[] = "C";
static char crt_locale_dot[] = ".";
static char crt_locale_empty[] = "";
static WCHAR crt_wlocale_c[] = { 'C', 0 };
static WCHAR crt_wlocale_dot[] = { '.', 0 };
static WCHAR crt_wlocale_empty[] = { 0 };

static CRT_LCONV crt_c_lconv = {
    .decimal_point = crt_locale_dot,
    .thousands_sep = crt_locale_empty,
    .grouping = crt_locale_empty,
    .int_curr_symbol = crt_locale_empty,
    .currency_symbol = crt_locale_empty,
    .mon_decimal_point = crt_locale_empty,
    .mon_thousands_sep = crt_locale_empty,
    .mon_grouping = crt_locale_empty,
    .positive_sign = crt_locale_empty,
    .negative_sign = crt_locale_empty,
    .int_frac_digits = CRT_CHAR_MAX,
    .frac_digits = CRT_CHAR_MAX,
    .p_cs_precedes = CRT_CHAR_MAX,
    .p_sep_by_space = CRT_CHAR_MAX,
    .n_cs_precedes = CRT_CHAR_MAX,
    .n_sep_by_space = CRT_CHAR_MAX,
    .p_sign_posn = CRT_CHAR_MAX,
    .n_sign_posn = CRT_CHAR_MAX,
    ._W_decimal_point = crt_wlocale_dot,
    ._W_thousands_sep = crt_wlocale_empty,
    ._W_int_curr_symbol = crt_wlocale_empty,
    ._W_currency_symbol = crt_wlocale_empty,
    ._W_mon_decimal_point = crt_wlocale_empty,
    ._W_mon_thousands_sep = crt_wlocale_empty,
    ._W_positive_sign = crt_wlocale_empty,
    ._W_negative_sign = crt_wlocale_empty,
};

typedef struct {
    CRT_LCONV32 lconv;
    char locale_c[2];
    char locale_dot[2];
    char locale_empty[1];
    WCHAR wlocale_c[2];
    WCHAR wlocale_dot[2];
    WCHAR wlocale_empty[1];
} CRT_LOCALE32_BLOCK;

typedef struct {
    DWORD owner_pid;
    CRT_LOCALE32_BLOCK *block;
} CRT_LOCALE32_SLOT;

static CRT_LOCALE32_SLOT crt_locale32_slots[UCRT_PROCESS_MODE_SLOTS];

static void crt_locale32_block_init(CRT_LOCALE32_BLOCK *block)
{
    memset(block, 0, sizeof(*block));
    block->locale_c[0] = 'C';
    block->locale_dot[0] = '.';
    block->wlocale_c[0] = 'C';
    block->wlocale_dot[0] = '.';

    uint32_t dot = (uint32_t)(ULONG_PTR)block->locale_dot;
    uint32_t empty = (uint32_t)(ULONG_PTR)block->locale_empty;
    uint32_t wdot = (uint32_t)(ULONG_PTR)block->wlocale_dot;
    uint32_t wempty = (uint32_t)(ULONG_PTR)block->wlocale_empty;

    block->lconv.decimal_point = dot;
    block->lconv.thousands_sep = empty;
    block->lconv.grouping = empty;
    block->lconv.int_curr_symbol = empty;
    block->lconv.currency_symbol = empty;
    block->lconv.mon_decimal_point = empty;
    block->lconv.mon_thousands_sep = empty;
    block->lconv.mon_grouping = empty;
    block->lconv.positive_sign = empty;
    block->lconv.negative_sign = empty;
    block->lconv.int_frac_digits = CRT_CHAR_MAX;
    block->lconv.frac_digits = CRT_CHAR_MAX;
    block->lconv.p_cs_precedes = CRT_CHAR_MAX;
    block->lconv.p_sep_by_space = CRT_CHAR_MAX;
    block->lconv.n_cs_precedes = CRT_CHAR_MAX;
    block->lconv.n_sep_by_space = CRT_CHAR_MAX;
    block->lconv.p_sign_posn = CRT_CHAR_MAX;
    block->lconv.n_sign_posn = CRT_CHAR_MAX;
    block->lconv._W_decimal_point = wdot;
    block->lconv._W_thousands_sep = wempty;
    block->lconv._W_int_curr_symbol = wempty;
    block->lconv._W_currency_symbol = wempty;
    block->lconv._W_mon_decimal_point = wempty;
    block->lconv._W_mon_thousands_sep = wempty;
    block->lconv._W_positive_sign = wempty;
    block->lconv._W_negative_sign = wempty;
}

static CRT_LOCALE32_BLOCK *crt_locale32_state(BOOL create)
{
    DWORD owner_pid = win32_current_process_id();
    if (!owner_pid) owner_pid = 1;

    CRT_LOCALE32_BLOCK *block = NULL;
    BOOL have_free_slot = FALSE;
    ucrt_state_lock_acquire();
    for (uint32_t i = 0; i < UCRT_PROCESS_MODE_SLOTS; i++) {
        CRT_LOCALE32_SLOT *slot = &crt_locale32_slots[i];
        if (slot->owner_pid == owner_pid) {
            block = slot->block;
            break;
        }
        if (!slot->owner_pid)
            have_free_slot = TRUE;
    }
    ucrt_state_lock_release();

    if (block || !create || !have_free_slot)
        return block;

    CRT_LOCALE32_BLOCK *candidate = (CRT_LOCALE32_BLOCK *)VirtualAlloc(
        NULL, sizeof(*candidate), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!candidate ||
        (ULONG_PTR)candidate > (ULONG_PTR)UINT32_MAX - sizeof(*candidate) + 1) {
        if (candidate)
            VirtualFree(candidate, 0, MEM_RELEASE);
        return NULL;
    }
    crt_locale32_block_init(candidate);

    CRT_LOCALE32_BLOCK *unused = candidate;
    ucrt_state_lock_acquire();
    CRT_LOCALE32_SLOT *free_slot = NULL;
    for (uint32_t i = 0; i < UCRT_PROCESS_MODE_SLOTS; i++) {
        CRT_LOCALE32_SLOT *slot = &crt_locale32_slots[i];
        if (slot->owner_pid == owner_pid) {
            block = slot->block;
            break;
        }
        if (!slot->owner_pid && !free_slot)
            free_slot = slot;
    }
    if (!block && free_slot) {
        free_slot->owner_pid = owner_pid;
        free_slot->block = candidate;
        block = candidate;
        unused = NULL;
    }
    ucrt_state_lock_release();

    if (unused)
        VirtualFree(unused, 0, MEM_RELEASE);
    if (block == candidate) {
        serial_puts("[CRT] PE32 locale state pid=");
        serial_putdec(owner_pid);
        serial_puts(" va=0x");
        serial_puthex((ULONG_PTR)block, 8);
        serial_puts("\n");
    }
    return block;
}

static void crt_locale_release_process(DWORD process_id)
{
    ucrt_state_lock_acquire();
    for (uint32_t i = 0; i < UCRT_PROCESS_MODE_SLOTS; i++) {
        CRT_LOCALE32_SLOT *slot = &crt_locale32_slots[i];
        if (slot->owner_pid == process_id) {
            slot->owner_pid = 0;
            slot->block = NULL;
        }
    }
    ucrt_state_lock_release();
}

static BOOL crt_locale_category_valid(int category)
{
    return category >= CRT_LC_ALL && category <= CRT_LC_TIME;
}

static BOOL crt_locale_name_is_c(const char *locale)
{
    if (!locale[0]) return TRUE;
    if (locale[0] == 'C' && !locale[1]) return TRUE;
    return locale[0] == 'P' && locale[1] == 'O' && locale[2] == 'S' &&
           locale[3] == 'I' && locale[4] == 'X' && !locale[5];
}

static BOOL crt_wlocale_name_is_c(const WCHAR *locale)
{
    if (!locale[0]) return TRUE;
    if (locale[0] == 'C' && !locale[1]) return TRUE;
    return locale[0] == 'P' && locale[1] == 'O' && locale[2] == 'S' &&
           locale[3] == 'I' && locale[4] == 'X' && !locale[5];
}

char* WINAPI crt_setlocale(int category, const char *locale)
{
    if (!crt_locale_category_valid(category)) {
        *crt_errno() = 22; /* EINVAL */
        return NULL;
    }
    if (!locale || crt_locale_name_is_c(locale)) {
        if (g_compat32_mode) {
            CRT_LOCALE32_BLOCK *block = crt_locale32_state(TRUE);
            return block ? block->locale_c : NULL;
        }
        return crt_locale_c;
    }
    return NULL;
}

WCHAR* WINAPI crt_wsetlocale(int category, const WCHAR *locale)
{
    if (!crt_locale_category_valid(category)) {
        *crt_errno() = 22; /* EINVAL */
        return NULL;
    }
    if (!locale || crt_wlocale_name_is_c(locale)) {
        if (g_compat32_mode) {
            CRT_LOCALE32_BLOCK *block = crt_locale32_state(TRUE);
            return block ? block->wlocale_c : NULL;
        }
        return crt_wlocale_c;
    }
    return NULL;
}

PVOID WINAPI crt_localeconv(void)
{
    if (g_compat32_mode) {
        CRT_LOCALE32_BLOCK *block = crt_locale32_state(TRUE);
        return block ? &block->lconv : NULL;
    }
    return &crt_c_lconv;
}

/* ── ctype ─────────────────────────────────────────────────── */

int WINAPI crt_isalpha(int c)  { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
int WINAPI crt_isdigit(int c)  { return c >= '0' && c <= '9'; }
int WINAPI crt_isalnum(int c)  { return crt_isalpha(c) || crt_isdigit(c); }
int WINAPI crt_isspace(int c)  { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
int WINAPI crt_isupper(int c)  { return c >= 'A' && c <= 'Z'; }
int WINAPI crt_islower(int c)  { return c >= 'a' && c <= 'z'; }
int WINAPI crt_isprint(int c)  { return c >= 0x20 && c <= 0x7e; }
int WINAPI crt_toupper(int c)  { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int WINAPI crt_tolower(int c)  { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static BOOL crt_is_wide_upper(int c)
{
    return (c >= 'A' && c <= 'Z') ||
           (c >= 0x00C0 && c <= 0x00D6) ||
           (c >= 0x00D8 && c <= 0x00DE) || c == 0x0178;
}

static BOOL crt_is_wide_lower(int c)
{
    return (c >= 'a' && c <= 'z') ||
           (c >= 0x00E0 && c <= 0x00F6) ||
           (c >= 0x00F8 && c <= 0x00FF);
}

int WINAPI crt_towupper(int c)
{
    if ((c >= 'a' && c <= 'z') ||
        (c >= 0x00E0 && c <= 0x00F6) ||
        (c >= 0x00F8 && c <= 0x00FE))
        return c - 0x20;
    if (c == 0x00FF) return 0x0178;
    return c;
}

int WINAPI crt_towlower(int c)
{
    if ((c >= 'A' && c <= 'Z') ||
        (c >= 0x00C0 && c <= 0x00D6) ||
        (c >= 0x00D8 && c <= 0x00DE))
        return c + 0x20;
    if (c == 0x0178) return 0x00FF;
    return c;
}

static int crt_wide_ctype_mask(int c)
{
    const int upper = 0x0001;
    const int lower = 0x0002;
    const int digit = 0x0004;
    const int space = 0x0008;
    const int punct = 0x0010;
    const int control = 0x0020;
    const int blank = 0x0040;
    const int hex = 0x0080;
    const int alpha = 0x0100;
    int result = 0;

    if (crt_is_wide_upper(c)) result |= upper | alpha;
    if (crt_is_wide_lower(c)) result |= lower | alpha;
    if (c >= '0' && c <= '9') result |= digit;
    if (c == ' ' || c == '\t') result |= blank;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
        c == '\f' || c == '\v')
        result |= space;
    if ((c >= 0 && c < 0x20) || c == 0x7F) result |= control;
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
        (c >= 'a' && c <= 'f'))
        result |= hex;
    if (c >= 0x20 && c <= 0x7E &&
        !(result & (upper | lower | digit | space)))
        result |= punct;
    return result;
}

int WINAPI crt_iswctype(int c, int mask)
{
    return crt_wide_ctype_mask(c) & mask;
}

int WINAPI crt_iswalpha(int c)
{
    return (crt_wide_ctype_mask(c) & 0x0100) != 0;
}

int WINAPI crt_iswalnum(int c)
{
    return (crt_wide_ctype_mask(c) & (0x0100 | 0x0004)) != 0;
}

int WINAPI crt_iswdigit(int c)
{
    return (crt_wide_ctype_mask(c) & 0x0004) != 0;
}

int WINAPI crt_iswspace(int c)
{
    return (crt_wide_ctype_mask(c) & 0x0008) != 0;
}

int WINAPI crt_iswupper(int c)
{
    return (crt_wide_ctype_mask(c) & 0x0001) != 0;
}

int WINAPI crt_iswlower(int c)
{
    return (crt_wide_ctype_mask(c) & 0x0002) != 0;
}

int WINAPI crt_iswprint(int c)
{
    return c >= 0x20 && c != 0x7F && c <= 0xFFFF;
}

/* ── Algorithm ─────────────────────────────────────────────── */

/* Simple quicksort */
static void qs_swap(BYTE *a, BYTE *b, SIZE_T size)
{
    for (SIZE_T i = 0; i < size; i++) {
        BYTE tmp = a[i]; a[i] = b[i]; b[i] = tmp;
    }
}

void WINAPI crt_qsort(PVOID base, SIZE_T nmemb, SIZE_T size,
                       int (WINAPI *compar)(PCVOID, PCVOID))
{
    if (nmemb < 2) return;

    BYTE *arr = (BYTE *)base;
    BYTE *pivot = arr + (nmemb - 1) * size;
    SIZE_T i = 0;

    for (SIZE_T j = 0; j < nmemb - 1; j++) {
        if (compar(arr + j * size, pivot) <= 0) {
            qs_swap(arr + i * size, arr + j * size, size);
            i++;
        }
    }
    qs_swap(arr + i * size, pivot, size);

    crt_qsort(arr, i, size, compar);
    crt_qsort(arr + (i + 1) * size, nmemb - i - 1, size, compar);
}

PVOID WINAPI crt_bsearch(PCVOID key, PCVOID base, SIZE_T nmemb,
                          SIZE_T size,
                          int (WINAPI *compar)(PCVOID, PCVOID))
{
    const BYTE *arr = (const BYTE *)base;
    SIZE_T lo = 0, hi = nmemb;
    while (lo < hi) {
        SIZE_T mid = lo + (hi - lo) / 2;
        int cmp = compar(key, arr + mid * size);
        if (cmp == 0) return (PVOID)(arr + mid * size);
        if (cmp < 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
}

/* ── Error ─────────────────────────────────────────────────── */

static int crt_errno_fallback;
static ULONG crt_doserrno_fallback;

static char *crt_error_messages[] = {
    "No error",
    "Operation not permitted",
    "No such file or directory",
    "No such process",
    "Interrupted function call",
    "Input/output error",
    "No such device or address",
    "Arg list too long",
    "Exec format error",
    "Bad file descriptor",
    "No child processes",
    "Resource temporarily unavailable",
    "Not enough space",
    "Permission denied",
    "Bad address",
    "Unknown error",
    "Resource device",
    "File exists",
    "Improper link",
    "No such device",
    "Not a directory",
    "Is a directory",
    "Invalid argument",
    "Too many open files in system",
    "Too many open files",
    "Inappropriate I/O control operation",
    "Unknown error",
    "File too large",
    "No space left on device",
    "Invalid seek",
    "Read-only file system",
    "Too many links",
    "Broken pipe",
    "Domain error",
    "Result too large",
    "Unknown error",
    "Resource deadlock avoided",
    "Unknown error",
    "Filename too long",
    "No locks available",
    "Function not implemented",
    "Directory not empty",
    "Illegal byte sequence",
};

static int crt_sys_nerr_val =
    (int)(sizeof(crt_error_messages) / sizeof(crt_error_messages[0]));

int* WINAPI crt_errno(void)
{
    UCRT_THREAD_VALUES *values = ucrt_thread_state(TRUE);
    return values ? &values->errno_value : &crt_errno_fallback;
}

ULONG* WINAPI crt_doserrno(void)
{
    UCRT_THREAD_VALUES *values = ucrt_thread_state(TRUE);
    return values ? &values->doserrno_value : &crt_doserrno_fallback;
}

char** WINAPI crt_sys_errlist(void) { return crt_error_messages; }

int* WINAPI crt_sys_nerr(void) { return &crt_sys_nerr_val; }

char* WINAPI crt_strerror(int error)
{
    if (error >= 0 && error < crt_sys_nerr_val)
        return crt_error_messages[error];
    return "Unknown error";
}

int WINAPI crt_fpe_flt_rounds(void)
{
    unsigned int mxcsr;
    __asm__ volatile ("stmxcsr %0" : "=m"(mxcsr));

    switch ((mxcsr >> 13) & 3U) {
    case 0: return 1; /* nearest */
    case 1: return 3; /* toward negative infinity */
    case 2: return 2; /* toward positive infinity */
    default: return 0; /* toward zero */
    }
}

/* ── Time ──────────────────────────────────────────────────── */

extern uint64_t idt_get_ticks(void);

/* The Win32 time-zone APIs expose UTC until Osito gains configurable zones. */
static LONG crt_timezone_val;
static int crt_daylight_val;

void WINAPI crt_tzset(void)
{
    crt_timezone_val = 0;
    crt_daylight_val = 0;
}

LONG* WINAPI crt_timezone(void) { return &crt_timezone_val; }

int* WINAPI crt_daylight(void) { return &crt_daylight_val; }

crt_time_t WINAPI crt_time(crt_time_t *timer)
{
    int64_t unix_seconds = wintime_now_unix_seconds();
    crt_time_t t = unix_seconds >= 0 ? (crt_time_t)unix_seconds
                                     : (crt_time_t)-1;
    if (timer) *timer = t;
    return t;
}

crt_clock_t WINAPI crt_clock(void)
{
    /* MSVCRT CLOCKS_PER_SEC is 1000; the APIC clock advances at 100 Hz. */
    return (crt_clock_t)(idt_get_ticks() * 10ULL);
}

/* ── SEH (Structured Exception Handling) ───────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);

/*
 * _except_handler3 — MSVC SEH frame-based exception handler.
 *
 * Called by the OS exception dispatcher when an exception occurs.
 * Walks the scopetable for this frame, calls filter expressions,
 * and if a filter returns EXCEPTION_EXECUTE_HANDLER, transfers
 * control to the __except handler block.
 *
 * EstablisherFrame points to the EH3_EXCEPTION_REGISTRATION on stack,
 * which contains: { registration, ScopeTable, TryLevel }
 */
EXCEPTION_DISPOSITION WINAPI crt_except_handler3(
    PEXCEPTION_RECORD ExceptionRecord,
    PEH3_EXCEPTION_REGISTRATION EstablisherFrame,
    PCONTEXT ContextRecord,
    PVOID DispatcherContext)
{
    (void)DispatcherContext;
    (void)ContextRecord;

    /*
     * Read ExceptionCode + ExceptionFlags.
     * In compat32 mode, the ExceptionRecord pointer is a 32-bit address
     * pointing to our static seh32_exception_record (EXCEPTION_RECORD32).
     * ExceptionCode (+0) and ExceptionFlags (+4) are at the same offsets
     * in both 32-bit and 64-bit layouts, so direct access is safe.
     */
    DWORD code  = ExceptionRecord->ExceptionCode;
    DWORD flags = ExceptionRecord->ExceptionFlags;

    /* During unwind, just return — _except_handler3 doesn't do unwind cleanup */
    if (flags & EXCEPTION_UNWIND) {
        return ExceptionContinueSearch;
    }

    serial_puts("[SEH] _except_handler3: code=0x");
    serial_puthex(code, 8);
    serial_puts("\n");


    /*
     * Walk the scopetable from current TryLevel upward.
     *
     * In compat32 mode, EstablisherFrame points to a 32-bit struct:
     *   +0:  uint32_t Next
     *   +4:  uint32_t Handler
     *   +8:  uint32_t ScopeTable   ← 32-bit pointer
     *   +12: uint32_t TryLevel
     *
     * The 64-bit EH3_EXCEPTION_REGISTRATION has 8-byte pointers, so
     * ScopeTable is at +16 and TryLevel at +24.  We must read manually.
     *
     * Scopetable entries are also 32-bit (12 bytes each):
     *   +0: uint32_t EnclosingLevel
     *   +4: uint32_t FilterFunc    ← 32-bit code pointer
     *   +8: uint32_t HandlerFunc   ← 32-bit code pointer
     */
    if (g_compat32_mode) {
        uint32_t *frame32 = (uint32_t *)(ULONG_PTR)EstablisherFrame;
        uint32_t scope32  = frame32[2];   /* offset +8 */
        uint32_t level    = frame32[3];   /* offset +12 */

        serial_puts("[SEH] scope32=0x");
        serial_puthex(scope32, 8);
        serial_puts(" level=");
        serial_putdec(level);
        serial_puts("\n");

        while (level != (uint32_t)-1 && scope32 != 0) {
            /* 32-bit SCOPETABLE_ENTRY: 12 bytes each */
            uint32_t *se = (uint32_t *)(ULONG_PTR)(scope32 + level * 12);
            uint32_t enclosing = se[0];
            uint32_t filter32  = se[1];
            uint32_t handler32 = se[2];

            if (filter32) {
                serial_puts("[SEH] filter32 @0x");
                serial_puthex(filter32, 8);
                serial_puts("\n");

                /*
                 * Call 32-bit filter: int __cdecl filter(EXCEPTION_POINTERS32 *)
                 * We already have seh32_exception_pointers set up by the caller.
                 */
                extern PVOID seh32_ep_addr_for_filter(void);
                uint32_t ep_addr = (uint32_t)(ULONG_PTR)seh32_ep_addr_for_filter();
                uint32_t fargs[1] = { ep_addr };
                uint32_t result = compat32_callback_args(filter32, 1, fargs);

                serial_puts("[SEH] filter returned ");
                serial_putdec(result);
                serial_puts("\n");

                if ((int32_t)result == 1 /* EXCEPTION_EXECUTE_HANDLER */) {
                    serial_puts("[SEH] EXECUTE_HANDLER @0x");
                    serial_puthex(handler32, 8);
                    serial_puts("\n");

                    /* Update TryLevel to enclosing scope */
                    frame32[3] = enclosing;

                    /* Call the 32-bit handler (longjmp-style, may not return) */
                    compat32_callback(handler32);

                    serial_puts("[SEH] handler returned\n");
                    return ExceptionContinueSearch;
                }
                else if ((int32_t)result == -1 /* EXCEPTION_CONTINUE_EXECUTION */) {
                    return ExceptionContinueExecution;
                }
                /* EXCEPTION_CONTINUE_SEARCH → try enclosing scope */
            }

            level = enclosing;
        }

        return ExceptionContinueSearch;
    }

    /* ── 64-bit path (PE64 or test harness) ── */
    PSCOPETABLE_ENTRY scope_table = EstablisherFrame->ScopeTable;
    DWORD try_level = EstablisherFrame->TryLevel;

    while (try_level != (DWORD)-1) {
        PSCOPETABLE_ENTRY entry = &scope_table[try_level];

        if (entry->FilterFunc) {
            EXCEPTION_POINTERS ep;
            ep.ExceptionRecord = ExceptionRecord;
            ep.ContextRecord   = ContextRecord;

            typedef int (WINAPI *filter_fn)(PEXCEPTION_POINTERS);
            filter_fn filter = (filter_fn)entry->FilterFunc;
            int result = filter(&ep);

            serial_puts("[SEH] filter returned ");
            serial_puthex((uint64_t)(uint32_t)result, 1);
            serial_puts("\n");

            if (result == EXCEPTION_EXECUTE_HANDLER) {
                serial_puts("[SEH] executing handler\n");
                EstablisherFrame->TryLevel = entry->EnclosingLevel;

                typedef void (WINAPI *handler_fn)(void);
                handler_fn handler = (handler_fn)entry->HandlerFunc;
                handler();

                return ExceptionContinueSearch;
            }
            else if (result == EXCEPTION_CONTINUE_EXECUTION) {
                return ExceptionContinueExecution;
            }
        }

        try_level = entry->EnclosingLevel;
    }

    return ExceptionContinueSearch;
}

/* 32-bit EH4 scope-table support used by _except_handler4_common. */
typedef struct __attribute__((packed)) {
    int32_t previous_try_level;
    uint32_t filter;
    uint32_t handler;
} CRT_EH4_SCOPE_ENTRY32;

static int crt_eh4_read_entry32(uint32_t scope_table, int32_t level,
                                CRT_EH4_SCOPE_ENTRY32 *entry)
{
    if (!entry || level < 0 || level > 4095)
        return 0;

    uint64_t address = (uint64_t)scope_table + 16ULL +
                       (uint64_t)(uint32_t)level * 12ULL;
    if (address > 0xFFFFFFFFULL ||
        !compat32_range_readable((uint32_t)address, sizeof(*entry)))
        return 0;

    *entry = *(const CRT_EH4_SCOPE_ENTRY32 *)(ULONG_PTR)(uint32_t)address;
    return 1;
}

static int crt_eh4_local_unwind32(uint32_t scope_table,
                                  uint32_t frame_address,
                                  int32_t stop_level)
{
    uint32_t *frame = (uint32_t *)(ULONG_PTR)frame_address;
    int32_t level = (int32_t)frame[3];
    uint32_t frame_ebp = frame_address + 16U;

    for (int guard = 0; level != stop_level && level != -2; guard++) {
        CRT_EH4_SCOPE_ENTRY32 entry;
        if (guard >= 1024 ||
            !crt_eh4_read_entry32(scope_table, level, &entry) ||
            entry.previous_try_level == level)
            return 0;

        frame[3] = (uint32_t)entry.previous_try_level;
        if (!entry.filter && entry.handler) {
            if (!compat32_range_readable(entry.handler, 1))
                return 0;
            (void)compat32_callback_args_with_ebp(entry.handler, 0, NULL,
                                                   frame_ebp);
        }
        level = entry.previous_try_level;
    }
    return level == stop_level;
}

EXCEPTION_DISPOSITION WINAPI crt_except_handler4_common(
    ULONG *cookie,
    PVOID check_cookie,
    PEXCEPTION_RECORD ExceptionRecord,
    PVOID EstablisherFrame,
    PCONTEXT ContextRecord,
    PVOID DispatcherContext)
{
    (void)check_cookie;
    (void)ContextRecord;
    (void)DispatcherContext;

    if (!g_compat32_mode)
        return ExceptionContinueSearch;

    uint32_t cookie_address = (uint32_t)(ULONG_PTR)cookie;
    uint32_t frame_address = (uint32_t)(ULONG_PTR)EstablisherFrame;
    uint32_t record_address = (uint32_t)(ULONG_PTR)ExceptionRecord;
    if (!compat32_range_readable(cookie_address, sizeof(uint32_t)) ||
        !compat32_range_readable(frame_address, 6U * sizeof(uint32_t)) ||
        !compat32_range_readable(record_address, 2U * sizeof(uint32_t))) {
        serial_puts("[SEH4] invalid handler arguments\n");
        return ExceptionContinueSearch;
    }

    uint32_t *frame = (uint32_t *)(ULONG_PTR)frame_address;
    uint32_t scope_table = frame[2] ^
                           *(const uint32_t *)(ULONG_PTR)cookie_address;
    if (!compat32_range_readable(scope_table, 16)) {
        serial_puts("[SEH4] invalid decoded scope table 0x");
        serial_puthex(scope_table, 8);
        serial_puts("\n");
        return ExceptionContinueSearch;
    }

    DWORD flags = *(const uint32_t *)(ULONG_PTR)(record_address + 4U);
    if (flags & EXCEPTION_UNWIND) {
        if (!crt_eh4_local_unwind32(scope_table, frame_address, -2))
            serial_puts("[SEH4] malformed local unwind metadata\n");
        return ExceptionContinueSearch;
    }

    extern PVOID seh32_ep_addr_for_filter(void);
    uint32_t ep_address = (uint32_t)(ULONG_PTR)seh32_ep_addr_for_filter();
    if (frame_address >= sizeof(uint32_t) &&
        compat32_range_readable(frame_address - sizeof(uint32_t),
                                sizeof(uint32_t)))
        *(uint32_t *)(ULONG_PTR)(frame_address - sizeof(uint32_t)) = ep_address;

    int32_t level = (int32_t)frame[3];
    uint32_t frame_ebp = frame_address + 16U;
    for (int guard = 0; level != -2; guard++) {
        CRT_EH4_SCOPE_ENTRY32 entry;
        if (guard >= 1024 ||
            !crt_eh4_read_entry32(scope_table, level, &entry) ||
            entry.previous_try_level == level) {
            serial_puts("[SEH4] malformed scope chain\n");
            return ExceptionContinueSearch;
        }

        if (entry.filter) {
            if (!compat32_range_readable(entry.filter, 1)) {
                serial_puts("[SEH4] invalid filter address\n");
                return ExceptionContinueSearch;
            }

            uint32_t args[1] = { ep_address };
            int32_t result = (int32_t)compat32_callback_args_with_ebp(
                entry.filter, 1, args, frame_ebp);
            if (result == EXCEPTION_CONTINUE_EXECUTION)
                return ExceptionContinueExecution;

            if (result == EXCEPTION_EXECUTE_HANDLER) {
                if (!entry.handler ||
                    !compat32_range_readable(entry.handler, 1) ||
                    !crt_eh4_local_unwind32(scope_table, frame_address,
                                             level)) {
                    serial_puts("[SEH4] invalid execute-handler metadata\n");
                    return ExceptionContinueSearch;
                }

                frame[3] = (uint32_t)entry.previous_try_level;
                TEB32 *teb = compat32_current_teb();
                if (teb) teb->ExceptionList = frame_address;
                (void)compat32_callback_args_with_ebp(entry.handler, 0, NULL,
                                                       frame_ebp);
                return ExceptionContinueSearch;
            }
        }

        level = entry.previous_try_level;
    }
    return ExceptionContinueSearch;
}

EXCEPTION_DISPOSITION WINAPI crt_except_handler4(
    PEXCEPTION_RECORD ExceptionRecord,
    PEH3_EXCEPTION_REGISTRATION EstablisherFrame,
    PCONTEXT ContextRecord,
    PVOID DispatcherContext)
{
    /* In real Windows, handler4 XORs scopetable pointer with security cookie.
     * We skip cookie validation and forward to handler3 logic. */
    return crt_except_handler3(ExceptionRecord, EstablisherFrame,
                                ContextRecord, DispatcherContext);
}

int WINAPI crt_XcptFilter(int code, PVOID pointers)
{
    (void)code;
    (void)pointers;
    /* Default: continue search (let next handler try) */
    return EXCEPTION_CONTINUE_SEARCH;
}

/* compat32_dispatch intercepts these targets before the generic ABI bridge.
 * Keeping distinct functions makes import resolution explicit while avoiding
 * an incorrect 64-bit C approximation of the i386 register capture. */
__attribute__((noinline))
int WINAPI crt_compat32_setjmp_marker(PVOID environment)
{
    (void)environment;
    return 0;
}

__attribute__((noinline))
int WINAPI crt_compat32_setjmp3_marker(PVOID environment, int unwind_count)
{
    (void)environment;
    (void)unwind_count;
    return 0;
}

__attribute__((noinline))
void WINAPI crt_compat32_longjmp_marker(PVOID environment, int value)
{
    (void)environment;
    (void)value;
}

/* ── Misc CRT internal ────────────────────────────────────── */

int  WINAPI crt_controlfp_s(unsigned int *old, unsigned int newval, unsigned int mask)
    { if (old) *old = 0; (void)newval; (void)mask; return 0; }
int  WINAPI crt_configthreadlocale(int type) { (void)type; return 0; }
void WINAPI crt_lock(int locknum) { (void)locknum; }
void WINAPI crt_unlock(int locknum) { (void)locknum; }
int  WINAPI crt_crt_debugger_hook(int reserved) { (void)reserved; return 0; }
void* WINAPI crt_encoded_null(void) { return NULL; }
PVOID WINAPI crt_amsg_exit(int errnum) { (void)errnum; crt_abort(); return NULL; }

/* ── C++ EH / UT99 required stubs ─────────────────────────── */

/* ??1type_info@@UAE@XZ — type_info destructor (no-op) */
void WINAPI crt_type_info_dtor(PVOID _this)
{
    (void)_this;
}

typedef struct _CRT_TYPE_INFO_ENTRY {
    struct _CRT_TYPE_INFO_ENTRY *next;
} CRT_TYPE_INFO_ENTRY;

void WINAPI crt_std_type_info_destroy_list(PVOID list_head)
{
    CRT_TYPE_INFO_ENTRY *entry =
        (CRT_TYPE_INFO_ENTRY *)InterlockedFlushSList(list_head);
    while (entry) {
        CRT_TYPE_INFO_ENTRY *next = entry->next;
        crt_free(entry);
        entry = next;
    }
}

/*
 * _CxxThrowException — C++ throw.
 *
 * Builds an EXCEPTION_RECORD with MSVC C++ exception code (0xE06D7363)
 * and dispatches through the 32-bit SEH chain. This allows __try/__except
 * catch-all handlers (common in Unreal Engine) to intercept the exception.
 *
 * If no handler catches it, terminates the process.
 */
extern int compat32_seh_dispatch(PEXCEPTION_RECORD ExceptionRecord);

/*
 * Track the current in-flight C++ exception for re-throw support.
 * When catch(...) calls throw; → _CxxThrowException(NULL, NULL),
 * we re-use the saved exception record instead of creating a fresh one.
 */
static EXCEPTION_RECORD cxx_current_exception;
static int cxx_exception_active = 0;
uint32_t crt_get_base_seh_thunk(void); /* forward decl */

void WINAPI crt_CxxThrowException(PVOID pExceptionObject, PVOID pThrowInfo)
{
    TEB32 *teb = compat32_current_teb();
    extern uint32_t compat32_get_last_caller_eip(void);
    uint32_t throw_eip = compat32_get_last_caller_eip();
    serial_puts("[MSVCRT] _CxxThrowException: obj=0x");
    serial_puthex((uint64_t)(ULONG_PTR)pExceptionObject, 8);
    serial_puts(" throwInfo=0x");
    serial_puthex((uint64_t)(ULONG_PTR)pThrowInfo, 8);
    serial_puts(" thrown_from=0x");
    serial_puthex(throw_eip, 8);
    serial_puts("\n");

    /* OpenJDK AWT keeps JNIEnv* in ESI while creating a frame.  If its
     * AwtFrame::Create path throws because FindClass returned NULL, describe
     * the already-pending Java exception before the C++ unwind hides it on
     * the toolkit thread.  ExceptionDescribe clears the pending exception,
     * so restore the same throwable immediately afterwards. */
    if (throw_eip == 0x6D0894C4U &&
        (uint32_t)(ULONG_PTR)pThrowInfo == 0x6D0D93A8U) {
        extern uint32_t compat32_get_last_user_esi(void);
        uint32_t env32 = compat32_get_last_user_esi();

        serial_puts("[AWT-JNI] env=0x");
        serial_puthex(env32, 8);
        if (env32 >= 0x10000U && env32 < 0x80000000U) {
            uint32_t functions = *(volatile uint32_t *)(uintptr_t)env32;
            serial_puts(" functions=0x");
            serial_puthex(functions, 8);
            serial_puts("\n");

            if (functions >= 0x10000U && functions < 0x80000000U) {
                uint32_t exception_occurred =
                    *(volatile uint32_t *)(uintptr_t)(functions + 15U * 4U);
                uint32_t throw_exception =
                    *(volatile uint32_t *)(uintptr_t)(functions + 13U * 4U);
                uint32_t exception_clear =
                    *(volatile uint32_t *)(uintptr_t)(functions + 17U * 4U);
                uint32_t find_class =
                    *(volatile uint32_t *)(uintptr_t)(functions + 6U * 4U);
                uint32_t delete_local_ref =
                    *(volatile uint32_t *)(uintptr_t)(functions + 23U * 4U);
                uint32_t is_instance_of =
                    *(volatile uint32_t *)(uintptr_t)(functions + 32U * 4U);
                uint32_t env_args[1] = { env32 };
                uint32_t pending = compat32_callback_args(
                    exception_occurred, 1, env_args);

                serial_puts("[AWT-JNI] pending=0x");
                serial_puthex(pending, 8);
                serial_puts("\n");

                if (pending != 0 && exception_clear != 0 &&
                    find_class != 0 && delete_local_ref != 0 &&
                    is_instance_of != 0 && throw_exception != 0) {
                    static const struct {
                        uint32_t name32;
                        const char *label;
                    } exception_types[] = {
                        { 0x6D427C48U, "ClassNotFoundException" },
                        { 0x6D427CF2U, "IllegalMonitorStateException" },
                        { 0x6D427DDFU, "LinkageError" },
                        { 0x6D427E5AU, "NullPointerException" },
                        { 0x6D427F2BU, "RuntimeException" },
                        { 0x6D428012U, "ExceptionInInitializerError" },
                        { 0x6D42807CU, "InternalError" },
                        { 0x6D428094U, "NoClassDefFoundError" },
                        { 0x6D4280EAU, "OutOfMemoryError" },
                    };
                    uint32_t throw_args[2] = { env32, pending };

                    compat32_callback_args(exception_clear, 1, env_args);
                    serial_puts("[AWT-JNI] exception=");
                    int identified = 0;
                    for (uint32_t i = 0;
                         i < sizeof(exception_types) / sizeof(exception_types[0]);
                         i++) {
                        uint32_t find_args[2] = {
                            env32, exception_types[i].name32
                        };
                        uint32_t cls = compat32_callback_args(
                            find_class, 2, find_args);
                        if (cls != 0) {
                            uint32_t instance_args[3] = {
                                env32, pending, cls
                            };
                            uint32_t match = compat32_callback_args(
                                is_instance_of, 3, instance_args);
                            uint32_t delete_args[2] = { env32, cls };
                            compat32_callback_args(
                                delete_local_ref, 2, delete_args);
                            if (match) {
                                serial_puts(exception_types[i].label);
                                identified = 1;
                                break;
                            }
                        }
                    }
                    if (!identified)
                        serial_puts("<other>");
                    serial_puts("\n");

                    compat32_callback_args(throw_exception, 2, throw_args);
                }
            }
        } else {
            serial_puts(" (invalid)\n");
        }
    }

    /* [THROWMSG] diagnostic: UT99's New-Game crash is preceded by a recoverable
     * `throw (TCHAR*)errmsg` (throwInfo 0x1017D4B0, type wchar_t*) from a failed
     * map load. The thrown object is the TCHAR* pointer; dump the message it
     * points to (first few) to learn WHY the load fails (the crash trigger). */
    {
        static int throwmsg_n = 0;
        if (pThrowInfo == (PVOID)(uintptr_t)0x1017D4B0ULL && throwmsg_n < 6 && pExceptionObject) {
            throwmsg_n++;
            uint32_t pstr = *(volatile uint32_t *)pExceptionObject;  /* TCHAR* */
            serial_puts("[THROWMSG] \"");
            if (pstr >= 0x10000 && pstr < 0x80000000) {
                const uint16_t *w = (const uint16_t *)(uintptr_t)pstr;
                for (int k = 0; k < 160 && w[k]; k++) {
                    char c = (w[k] >= 0x20 && w[k] < 0x7F) ? (char)w[k] : '?';
                    char s[2] = { c, 0 }; serial_puts(s);
                }
            }
            serial_puts("\"\n");
        }
    }

    /* [GERRHIST DIAGNOSTIC — uncommitted] For appError `throw 1` (funclet rethrow
     * @0x10903EE4), the message is in GErrorHist (Core.dll buffer @0x101E3474, UTF-16),
     * not the throw object. Dump it once-per-cascade to learn the real fatal reason
     * (e.g. render/audio device init failure) behind the render-frontier exit. */
    {
        static int gerr_n = 0;
        const volatile uint16_t *gh = (const volatile uint16_t *)(uintptr_t)0x101E3474ULL;
        if (gerr_n < 4 && gh[0] != 0) {
            gerr_n++;
            serial_puts("[GERRHIST] \"");
            for (int k = 0; k < 240 && gh[k]; k++) {
                char c = (gh[k] >= 0x20 && gh[k] < 0x7F) ? (char)gh[k] : '?';
                char s[2] = { c, 0 }; serial_puts(s);
            }
            serial_puts("\"\n");
        }
    }

    /* Dump thrown object to identify the error message.
     * Try reading the first few fields and interpret as string pointers. */
    if (pExceptionObject) {
        uint32_t obj32 = (uint32_t)(ULONG_PTR)pExceptionObject;
        if (obj32 > 0x10000 && obj32 < 0x7FFFFFFF) {
            uint32_t *f = (uint32_t *)(ULONG_PTR)obj32;
            serial_puts("[CXX-OBJ] ");
            for (int i = 0; i < 4; i++) {
                serial_puthex(f[i], 8);
                serial_puts(" ");
            }
            serial_puts("\n");
            /* Try ALL fields as wide and narrow strings */
            for (int fi = 0; fi < 4; fi++) {
                if (f[fi] > 0x10000 && f[fi] < 0x7FFFFFFF) {
                    uint16_t *ws = (uint16_t *)(ULONG_PTR)f[fi];
                    char *ns = (char *)(ULONG_PTR)f[fi];
                    if (ws[0] > 0x20 && ws[0] < 0x7F) {
                        serial_puts("[CXX-F");
                        serial_putdec(fi);
                        serial_puts("] W\"");
                        for (int i = 0; i < 200 && ws[i] > 0 && ws[i] < 0x7F; i++)
                            serial_putchar((char)ws[i]);
                        serial_puts("\"\n");
                    } else if (ns[0] > 0x20 && ns[0] < 0x7F) {
                        serial_puts("[CXX-F");
                        serial_putdec(fi);
                        serial_puts("] A\"");
                        for (int i = 0; i < 200 && ns[i] >= 0x20 && ns[i] < 0x7F; i++)
                            serial_putchar(ns[i]);
                        serial_puts("\"\n");
                    }
                }
            }
        }
    }

    /*
     * WORKAROUND: Full C++ EH (SEH unwind + __CxxFrameHandler dispatch)
     * is not yet implemented. Calling RaiseException without proper CONTEXT
     * and DispatcherContext causes all handlers to return ContinueSearch,
     * leaving the C++ runtime corrupted (_CxxThrowException returns when
     * it should never return → undefined behavior → vtable=0 crashes).
     *
     * For now: suppress real throws and rethrows. Return immediately.
     * The engine code after throw is technically unreachable, but MSVC
     * often generates fall-through code that works as error cleanup.
     * This is NOT correct C++ semantics but lets the engine survive
     * past localization failures and similar non-fatal errors.
     */
    /* Rethrow (throw;) — re-dispatch the current exception.
     * This happens inside catch handlers that do: throw;
     * Re-use the saved exception record from the original throw. */
    if (!pExceptionObject && !pThrowInfo) {
        serial_puts("[CXX] rethrow → re-dispatching current exception\n");
        if (cxx_exception_active) {
            /* Dispatch the saved exception to the next handler.
             * Return 1 = handled (compat32 longjmped, this RET path
             * is the post-handler unwind).  Return 0 = UNHANDLED, in
             * which case _CxxThrowException MUST NOT RETURN — the
             * engine compiler emitted padding bytes after the throw
             * call assuming it never comes back.  Returning normally
             * lands the engine in 0xCC INT3 padding.  Force exit. */
            int handled = compat32_seh_dispatch(&cxx_current_exception);
            if (!handled) {
                extern void proc_exit(int32_t code);
                serial_puts("[CXX] rethrow UNHANDLED — proc_exit\n");
                proc_exit(0xE06D7363);
                /* unreachable */
            }
            return;
        }
        /* No active exception — just suppress */
        serial_puts("[CXX] WARNING: rethrow without active exception\n");
        return;
    }

    /*
     * ORIGINAL CODE (disabled):
     * _CxxThrowException MUST NOT RETURN. The MSVC implementation calls
     * RaiseException(0xE06D7363, EXCEPTION_NONCONTINUABLE, 3, args)
     * which triggers SEH dispatch → __CxxFrameHandler → catch block.
     *
     * We implement a minimal SEH dispatch: walk the chain from
     * the current TEB's ExceptionList, call each handler via compat32_callback,
     * looking for EXCEPTION_EXECUTE_HANDLER. If found, restore the
     * handler's stack frame and longjmp to the catch block.
     *
     * For now: call RaiseException which walks the SEH chain from
     * that TEB and dispatches to registered handlers.
     */
    {
        uint32_t seh_head = teb->ExceptionList;
        serial_puts("[CXX] SEH chain head: 0x");
        serial_puthex(seh_head, 8);
        serial_puts("\n");

        if (seh_head != 0xFFFFFFFF && seh_head != 0) {
            /* Walk the SEH chain and dump handlers */
            uint32_t *frame = (uint32_t *)(ULONG_PTR)seh_head;
            for (int i = 0; i < 5 && frame && (uint32_t)(ULONG_PTR)frame != 0xFFFFFFFF; i++) {
                uint32_t next = frame[0];
                uint32_t handler = frame[1];
                serial_puts("[CXX]  frame[");
                serial_putdec(i);
                serial_puts("] at 0x");
                serial_puthex((uint64_t)(ULONG_PTR)frame, 8);
                serial_puts(" handler=0x");
                serial_puthex(handler, 8);
                serial_puts(" next=0x");
                serial_puthex(next, 8);
                serial_puts("\n");
                frame = (next == 0xFFFFFFFF) ? NULL : (uint32_t *)(ULONG_PTR)next;
            }
        }
    }

    /* Build and save the exception record for re-throw support */
    {
        BYTE *p = (BYTE *)&cxx_current_exception;
        for (SIZE_T i = 0; i < sizeof(cxx_current_exception); i++) p[i] = 0;
        cxx_current_exception.ExceptionCode = 0xE06D7363;
        cxx_current_exception.ExceptionFlags = 1; /* NONCONTINUABLE */
        cxx_current_exception.NumberParameters = 3;
        cxx_current_exception.ExceptionInformation[0] = 0x19930520;
        cxx_current_exception.ExceptionInformation[1] = (ULONG_PTR)pExceptionObject;
        cxx_current_exception.ExceptionInformation[2] = (ULONG_PTR)pThrowInfo;
        cxx_exception_active = 1;
    }

    /* Pre-check: if SEH chain head is in PE-image .text range
     * (DLL CODE, not stack), try to repair. UT99's PE32 stack is
     * around 0x13E0xxxx-0x13F0xxxx — NOT corrupt, just user stack.
     * Engine.dll/Core.dll text is 0x10000000-0x10A00000 typically;
     * the data/import area extends past that. Tighten the range to
     * only DLL code regions where SEH frames CAN'T live. */
    {
        uint32_t head = teb->ExceptionList;
        /* PE image .text range: 0x10000000-0x12000000 (Engine + Core
         * + Render + a few smaller DLLs). UT99 stack is 0x13xxxxxx,
         * so 0x12000000 is a safe upper bound — anything above is
         * either stack (valid SEH frame) or NULL/end-sentinel. */
        if (head >= 0x10000000 && head < 0x12000000) {
            /* Try to repair: follow Next pointers past corrupt entries */
            uint32_t *corrupt = (uint32_t *)(uintptr_t)head;
            uint32_t next = corrupt[0];
            serial_puts("[CXX] SEH chain head corrupt (0x");
            serial_puthex(head, 8);
            serial_puts("), next=0x");
            serial_puthex(next, 8);
            serial_puts("\n");

            if (next != 0 && next != 0xFFFFFFFF &&
                (next < 0x10000000 || next >= 0x12000000)) {
                /* Next is a valid non-PE address — repair chain.
                 * Also insert our base SEH handler so there's at least
                 * one handler to dispatch to (the original chain may only
                 * have end sentinels after the corrupt entry). */
                static uint32_t emergency_frame[3];
                uint32_t base_handler = crt_get_base_seh_thunk();
                if (base_handler) {
                    emergency_frame[0] = next; /* chain to remaining frames */
                    emergency_frame[1] = base_handler;
                    emergency_frame[2] = 0;
                    teb->ExceptionList =
                        (uint32_t)(uintptr_t)emergency_frame;
                    serial_puts("[CXX] Repaired: emergency frame at 0x");
                    serial_puthex((uint32_t)(uintptr_t)emergency_frame, 8);
                    serial_puts(" -> 0x");
                    serial_puthex(next, 8);
                    serial_puts("\n");
                } else {
                    teb->ExceptionList = next;
                    serial_puts("[CXX] Repaired: chain head -> 0x");
                    serial_puthex(next, 8);
                    serial_puts("\n");
                }
                /* Fall through to RaiseException with repaired chain */
            } else {
                /* Can't repair — suppress throw */
                serial_puts("[CXX] Cannot repair chain — suppressing throw\n");
                cxx_exception_active = 0;
                return;
            }
        }
    }

    /* Call RaiseException with the C++ exception code.
     * This will dispatch through the SEH chain. */
    {
        ULONG_PTR args[3];
        args[0] = 0x19930520;  /* EH_MAGIC_NUMBER1 */
        args[1] = (ULONG_PTR)pExceptionObject;
        args[2] = (ULONG_PTR)pThrowInfo;
        RaiseException(0xE06D7363, 1 /* EXCEPTION_NONCONTINUABLE */, 3, args);
    }

    /* RaiseException returned — check if the dispatch handled it.
     * If unwind globals are set, the INT2E handler will redirect to the
     * catch handler. The exception IS handled — keep cxx_exception_active
     * so re-throws from the catch handler can propagate. */
    {
        extern uint32_t g_compat32_unwind_eip;
        if (g_compat32_unwind_eip != 0) {
            /* Dispatch handled it — INT2E will redirect to catch.
             * Keep cxx_exception_active for re-throw support. */
            return;
        }
    }
    serial_puts("[CXX] WARNING: _CxxThrowException unhandled — suppressing\n");
    cxx_exception_active = 0;

    /* Clear GErrorHist + GIsCriticalError so Browse() doesn't see stale
     * error state from suppressed exceptions during init. */
    {
        volatile uint16_t *gerr = (volatile uint16_t *)(uintptr_t)0x101E3474;
        volatile uint32_t *gcrit = (volatile uint32_t *)(uintptr_t)0x101E568C;
        *gerr = 0;
        *gcrit = 0;
        serial_puts("[CXX] cleared GErrorHist+GIsCriticalError\n");
    }

    /* Instead of proc_exit(1), just return and let the 32-bit code continue.
     * _CxxThrowException "should never return" but the engine's code after
     * throw often has fall-through error cleanup that's reachable.
     * With NULL-REDIRECT and page 0 cleanup, post-throw crashes are handled.
     * The engine may enter its game loop in error state — better than exiting. */
    return;

    /* ── Diagnostic: dump GObjRegistrants state ────────────── */
    {
        /* GObjRegistrants@UObject is a TArray<UObject*> at Core.dll export RVA 0x1A0360
         * Core.dll base = 0x10100000, so VA = 0x102A0360
         * TArray layout: { T* Data (+0), INT Num (+4), INT Max (+8) } */
        uint32_t *gobjreg = (uint32_t *)(ULONG_PTR)0x102A0360;
        uint32_t data_ptr = gobjreg[0];
        int32_t  num      = (int32_t)gobjreg[1];
        int32_t  max      = (int32_t)gobjreg[2];

        serial_puts("[CXX-DIAG] GObjRegistrants: Data=0x");
        serial_puthex(data_ptr, 8);
        serial_puts(" Num=");
        serial_putdec(num);
        serial_puts(" Max=");
        serial_putdec(max);
        serial_puts("\n");

        /* UObject::PrivateStaticClass at VA 0x102A1768 */
        uint32_t *uobj = (uint32_t *)(ULONG_PTR)0x102A1768;
        serial_puts("[CXX-DIAG] UObject.Index=0x");
        serial_puthex(uobj[1], 8);  /* +0x04 = Index */
        serial_puts("\n");

        /* Dump first 16 bytes of UObject to check if registration changed anything */
        serial_puts("[CXX-DIAG] UObject @0x102A1768 raw: ");
        for (int i = 0; i < 16; i++) {
            serial_puthex(uobj[i], 8);
            serial_puts(" ");
        }
        serial_puts("\n");

        /* Scan GObjRegistrants array: count zeros vs non-zero */
        if (data_ptr && num > 0 && num < 10000) {
            uint32_t *arr = (uint32_t *)(ULONG_PTR)data_ptr;
            int zeros = 0, nonzeros = 0;
            int first_nz = -1, last_nz = -1;
            int uobj_idx = -1;
            for (int i = 0; i < num && i < 300; i++) {
                if (arr[i] == 0) {
                    zeros++;
                } else {
                    nonzeros++;
                    if (first_nz < 0) first_nz = i;
                    last_nz = i;
                }
                if (arr[i] == 0x102A1768) uobj_idx = i;
            }
            serial_puts("[CXX-DIAG] zeros=");
            serial_putdec(zeros);
            serial_puts(" nonzeros=");
            serial_putdec(nonzeros);
            serial_puts(" first_nz=");
            serial_putdec(first_nz >= 0 ? first_nz : -1);
            serial_puts(" last_nz=");
            serial_putdec(last_nz >= 0 ? last_nz : -1);
            serial_puts(" UObject_idx=");
            serial_putdec(uobj_idx >= 0 ? uobj_idx : -1);
            serial_puts("\n");

            /* Dump ALL non-zero entries (max 20) */
            int shown = 0;
            for (int i = 0; i < num && i < 300 && shown < 20; i++) {
                if (arr[i] == 0) continue;
                uint32_t ea = arr[i];
                uint32_t *e = (uint32_t *)(ULONG_PTR)ea;
                serial_puts("[CXX-DIAG] nz[");
                serial_putdec(i);
                serial_puts("] @0x");
                serial_puthex(ea, 8);
                serial_puts(": idx=");
                serial_puthex(e[1], 8);
                serial_puts(" flags=");
                serial_puthex(e[7], 8);
                serial_puts(" super=");
                serial_puthex(e[10], 8);
                serial_puts(" propSz=");
                serial_puthex(*(uint32_t *)((uint8_t *)(ULONG_PTR)ea + 0x3C), 8);
                serial_puts("\n");
                shown++;
            }

            /* Also dump raw 32 bytes around the Data pointer to check alignment */
            serial_puts("[CXX-DIAG] raw @Data+0x000:");
            for (int i = 0; i < 8; i++) {
                serial_puts(" ");
                serial_puthex(arr[i], 8);
            }
            serial_puts("\n");
            /* And at the end */
            serial_puts("[CXX-DIAG] raw @Data+");
            serial_puthex((num - 4) * 4, 4);
            serial_puts(":");
            for (int i = num - 4; i < num; i++) {
                serial_puts(" ");
                serial_puthex(arr[i < 0 ? 0 : i], 8);
            }
            serial_puts("\n");

            /* Check the physical memory at the GObjRegistrants.Data address */
            /* Read GObjNoRegister (at Core.dll RVA 0x1A21A0 → VA 0x102A21A0) */
            uint32_t *noregister = (uint32_t *)(ULONG_PTR)0x102A21A0;
            serial_puts("[CXX-DIAG] GObjNoRegister = ");
            serial_putdec(*noregister);
            serial_puts("\n");
        }

        /* Dump FName table: FName::Names is a TArray at 0x10295D30 (IAT resolved) */
        /* Actually read the pointer from 0x10295D30 which is the Names TArray address */
        uint32_t *fname_names = (uint32_t *)(ULONG_PTR)0x10295D30;
        serial_puts("[CXX-DIAG] FName::Names: Data=0x");
        serial_puthex(fname_names[0], 8);
        serial_puts(" Num=");
        serial_putdec((int32_t)fname_names[1]);
        serial_puts(" Max=");
        serial_putdec((int32_t)fname_names[2]);
        serial_puts("\n");

        /* Follow GetSuperClass JMP thunk to get actual implementation */
        /* GetSuperClass VA=0x10103341, starts with E9 xx xx xx xx (JMP rel32) */
        uint8_t *gsc = (uint8_t *)(ULONG_PTR)0x10103341;
        if (gsc[0] == 0xE9) {
            int32_t rel = *(int32_t *)(gsc + 1);
            uint32_t target = 0x10103341 + 5 + rel;
            uint8_t *impl = (uint8_t *)(ULONG_PTR)target;
            serial_puts("[CXX-DIAG] GetSuperClass @0x");
            serial_puthex(target, 8);
            serial_puts(" bytes: ");
            for (int i = 0; i < 8; i++) {
                serial_puthex(impl[i], 2);
                serial_puts(" ");
            }
            serial_puts("\n");
            /* If mov eax,[ecx+XX]; ret → 8B 41 XX C3 */
            if (impl[0] == 0x8B && impl[1] == 0x41) {
                serial_puts("[CXX-DIAG] SuperField offset = +0x");
                serial_puthex(impl[2], 2);
                serial_puts("\n");
            } else if (impl[0] == 0x8B && impl[1] == 0x81) {
                int32_t off = *(int32_t *)(impl + 2);
                serial_puts("[CXX-DIAG] SuperField offset = +0x");
                serial_puthex(off, 8);
                serial_puts("\n");
            }
        }
    }
    /* ── End diagnostic ────────────────────────────────────── */

    EXCEPTION_RECORD rec;
    BYTE *p = (BYTE *)&rec;

    if (pExceptionObject == NULL && pThrowInfo == NULL && cxx_exception_active) {
        /* Re-throw (C++ "throw;") — reuse the saved exception */
        serial_puts("[MSVCRT] re-throw — using saved exception\n");
        for (SIZE_T i = 0; i < sizeof(rec); i++) p[i] = ((BYTE *)&cxx_current_exception)[i];
    } else {
        /* New throw — build MSVC C++ exception record */
        for (SIZE_T i = 0; i < sizeof(rec); i++) p[i] = 0;
        rec.ExceptionCode  = 0xE06D7363;  /* MSVC C++ exception 'msc' */
        rec.ExceptionFlags = 0;           /* continuable */
        rec.NumberParameters = 3;
        rec.ExceptionInformation[0] = 0x19930520;  /* MSVC EH magic */
        rec.ExceptionInformation[1] = (ULONG_PTR)pExceptionObject;
        rec.ExceptionInformation[2] = (ULONG_PTR)pThrowInfo;

        /* Save for potential re-throw */
        for (SIZE_T i = 0; i < sizeof(rec); i++) ((BYTE *)&cxx_current_exception)[i] = p[i];
        cxx_exception_active = 1;
    }

    /* Dispatch through the 32-bit SEH chain */
    int handled = compat32_seh_dispatch(&rec);

    if (handled) {
        serial_puts("[MSVCRT] _CxxThrowException: handled by SEH\n");
        cxx_exception_active = 0;
        return;
    }

    /* Unhandled — terminate */
    serial_puts("[MSVCRT] _CxxThrowException: UNHANDLED — aborting\n");
    cxx_exception_active = 0;
    crt_abort();
}

/*
 * __CxxFrameHandler — MSVC 6 C++ exception frame handler.
 *
 * Called from handler stubs in PE32 DLLs:
 *   mov eax, offset FuncInfo
 *   jmp __CxxFrameHandler
 *
 * In compat32 mode, the EstablisherFrame is a 32-bit EH3 registration:
 *   +0:  Next
 *   +4:  Handler (stub address)
 *   +8:  FuncInfo* (32-bit pointer to MSVC FuncInfo structure)
 *   +12: TryLevel (current exception state, -1 = no try block active)
 *
 * FuncInfo layout (MSVC 6, 32-bit):
 *   +0:  magic     (0x19930520 = VC5/6, 0x19930522 = VC7)
 *   +4:  maxState
 *   +8:  pUnwindMap
 *   +12: nTryBlocks
 *   +16: pTryBlockMap
 *
 * TryBlockMapEntry (20 bytes):
 *   +0:  tryLow
 *   +4:  tryHigh
 *   +8:  catchHigh
 *   +12: nCatches
 *   +16: pHandlerArray
 *
 * HandlerType (16 bytes):
 *   +0:  adjectives
 *   +4:  pType (type_info*, 0 = catch(...))
 *   +8:  dispCatchObj
 *   +12: addressOfHandler
 */
uint32_t crt_find_cxx_func_info(uint32_t handler_addr)
{
    if (handler_addr < 0x10000 || handler_addr >= 0x80000000)
        return 0;

    const uint8_t *stub = (const uint8_t *)(ULONG_PTR)handler_addr;
    for (uint32_t i = 0; i + 10 <= 64; i++) {
        if (stub[i] != 0xB8 || stub[i + 5] != 0xE9)
            continue;

        uint32_t candidate = *(const uint32_t *)(stub + i + 1);
        if (candidate < 0x10000 || candidate >= 0x80000000)
            continue;

        uint32_t magic = *(const uint32_t *)(ULONG_PTR)candidate;
        if (magic == 0x19930520 || magic == 0x19930522)
            return candidate;
    }
    return 0;
}

EXCEPTION_DISPOSITION WINAPI crt_CxxFrameHandler(
    PEXCEPTION_RECORD ExceptionRecord,
    PVOID EstablisherFrame,
    PCONTEXT ContextRecord,
    PVOID DispatcherContext)
{
    (void)ContextRecord;
    (void)DispatcherContext;

    DWORD code  = ExceptionRecord->ExceptionCode;
    DWORD flags = ExceptionRecord->ExceptionFlags;

    if (flags & EXCEPTION_UNWIND)
        return ExceptionContinueSearch;

    if (!g_compat32_mode || code != 0xE06D7363)
        return ExceptionContinueSearch;

    uint32_t *frame32 = (uint32_t *)(ULONG_PTR)EstablisherFrame;

    /*
     * MSVC 6 C++ EH frame layout (different from _except_handler3!):
     *   frame+0: Next
     *   frame+4: Handler (stub address with MOV EAX, FuncInfo; JMP __CxxFrameHandler)
     *   frame+8: TryLevel (at EBP-4 of establishing function)
     *
     * FuncInfo is NOT in the frame — it's encoded in the handler stub's
     * MOV EAX, imm32 instruction (opcode 0xB8).
     */
    uint32_t handler_addr = frame32[1];
    int32_t  cur_state    = (int32_t)frame32[2];  /* TryLevel */

    /* /GS wrappers perform cookie checks before MOV EAX, FuncInfo. */
    uint32_t func_info_addr = crt_find_cxx_func_info(handler_addr);

    serial_puts("[CxxEH] handler=0x");
    serial_puthex(handler_addr, 8);
    serial_puts(" FuncInfo=0x");
    serial_puthex(func_info_addr, 8);
    serial_puts(" state=");
    serial_putdec((uint32_t)cur_state);
    serial_puts("\n");

    if (!func_info_addr || cur_state == -1)
        return ExceptionContinueSearch;

    /* Validate FuncInfo magic */
    uint32_t *fi = (uint32_t *)(ULONG_PTR)func_info_addr;
    uint32_t magic = fi[0];
    if (magic != 0x19930520 && magic != 0x19930522) {
        serial_puts("[CxxEH] bad magic 0x");
        serial_puthex(magic, 8);
        serial_puts("\n");
        return ExceptionContinueSearch;
    }

    uint32_t nTryBlocks   = fi[3];
    uint32_t pTryBlockMap = fi[4];

    serial_puts("[CxxEH] nTryBlocks=");
    serial_putdec(nTryBlocks);
    serial_puts("\n");

    /* Walk try blocks looking for one that covers current state */
    for (uint32_t i = 0; i < nTryBlocks && i < 64; i++) {
        uint32_t *tb = (uint32_t *)(ULONG_PTR)(pTryBlockMap + i * 20);
        int32_t tryLow    = (int32_t)tb[0];
        int32_t tryHigh   = (int32_t)tb[1];
        int32_t catchHigh = (int32_t)tb[2];
        uint32_t nCatches  = tb[3];
        uint32_t pHandlers = tb[4];

        if (cur_state < tryLow || cur_state > tryHigh)
            continue;

        serial_puts("[CxxEH] try[");
        serial_putdec(i);
        serial_puts("] low=");
        serial_putdec((uint32_t)tryLow);
        serial_puts(" high=");
        serial_putdec((uint32_t)tryHigh);
        serial_puts(" nCatches=");
        serial_putdec(nCatches);
        serial_puts("\n");

        /* Walk catch handlers */
        for (uint32_t j = 0; j < nCatches && j < 16; j++) {
            uint32_t *ht = (uint32_t *)(ULONG_PTR)(pHandlers + j * 16);
            uint32_t adjectives  = ht[0];
            uint32_t pType       = ht[1];  /* type_info*, 0 = catch(...) */
            int32_t  dispCatchObj = (int32_t)ht[2];
            uint32_t handlerAddr = ht[3];

            serial_puts("[CxxEH] catch adj=0x");
            serial_puthex(adjectives, 8);
            serial_puts(" type=0x");
            serial_puthex(pType, 8);
            serial_puts(" handler=0x");
            serial_puthex(handlerAddr, 8);
            serial_puts("\n");

            /*
             * Match: catch(...) has pType==0.
             * For typed catches, we'd need to compare type_info names
             * between the thrown type and the catch type. For now, also
             * match if adjectives has 0x40 (HT_IsComplusEh / catch-all).
             */
            if (pType == 0 || (adjectives & 0x40)) {
                serial_puts("[CxxEH] MATCH catch(...) — executing handler @0x");
                serial_puthex(handlerAddr, 8);
                serial_puts("\n");

                /* Update TryLevel to catchHigh+1 (entered catch block).
                 * C++ EH frame: Next(+0), Handler(+4), TryLevel(+8) = frame32[2].
                 * NOT frame32[3] — that's the _except_handler3 layout. */
                frame32[2] = (uint32_t)(catchHigh + 1);

                /*
                 * Copy exception object if needed.
                 * dispCatchObj is the offset from EBP where the catch
                 * object should be stored. For catch(...), it's typically 0.
                 */
                (void)dispCatchObj;

                /*
                 * Call the 32-bit catch handler.
                 * In MSVC, this does a longjmp-style transfer to the
                 * catch block. It may not return.
                 */
                compat32_callback(handlerAddr);

                /* If handler returned, exception was handled */
                serial_puts("[CxxEH] catch handler returned\n");
                return ExceptionContinueExecution;
            }
        }
    }

    return ExceptionContinueSearch;
}

/* __dllonexit — register DLL exit callback in caller's table */
_PVFV_DLL WINAPI crt_dllonexit(_PVFV_DLL func, _PVFV_DLL **pbegin, _PVFV_DLL **pend)
{
    /* Real impl grows *pbegin..*pend table. We just use global atexit for simplicity. */
    if (func) crt_atexit((void (*)(void))func);
    return func;
}

/* __p__commode — pointer to _commode variable */
int* WINAPI crt_p_commode(void)
{
    UCRT_PROCESS_MODE_VALUES *values = ucrt_process_mode_state(TRUE);
    return values ? &values->commode : NULL;
}

/* __p__fmode — pointer to _fmode variable */
int* WINAPI crt_p_fmode(void)
{
    UCRT_PROCESS_MODE_VALUES *values = ucrt_process_mode_state(TRUE);
    return values ? &values->fmode : NULL;
}

int WINAPI crt_set_fmode(int mode)
{
    switch (mode) {
        case 0x4000:  /* _O_TEXT */
        case 0x8000:  /* _O_BINARY */
        case 0x10000: /* _O_WTEXT */
        case 0x20000: /* _O_U16TEXT */
        case 0x40000: /* _O_U8TEXT */
            break;
        default:
            return 22; /* EINVAL */
    }

    UCRT_PROCESS_MODE_VALUES *values = ucrt_process_mode_state(TRUE);
    if (!values) return 12; /* ENOMEM */
    values->fmode = mode;
    return 0;
}

int WINAPI crt_get_fmode(int *mode)
{
    if (!mode)
        return 22; /* EINVAL */

    UCRT_PROCESS_MODE_VALUES *values = ucrt_process_mode_state(TRUE);
    if (!values) return 12; /* ENOMEM */
    *mode = values->fmode;
    return 0;
}

/* __C_specific_handler — x86-64 SEH handler (stub, no-op) */
EXCEPTION_DISPOSITION WINAPI crt_C_specific_handler(
    PEXCEPTION_RECORD ExceptionRecord,
    PVOID EstablisherFrame,
    PCONTEXT ContextRecord,
    PVOID DispatcherContext)
{
    (void)ExceptionRecord; (void)EstablisherFrame;
    (void)ContextRecord; (void)DispatcherContext;
    return 1; /* ExceptionContinueSearch */
}

/* __initenv — pointer to initial environment (char **) */
static char *crt_initenv_data[] = { NULL };
static char **crt_initenv_val = crt_initenv_data;

/* signal — install signal handler (stub, returns SIG_DFL) */
typedef void (*crt_sighandler_t)(int);
#define CRT_SIG_DFL ((crt_sighandler_t)0)
crt_sighandler_t WINAPI crt_signal(int sig, crt_sighandler_t handler)
{
    (void)sig; (void)handler;
    return CRT_SIG_DFL;
}

/* __setusermatherr — set math error handler (store, ignore) */
static _UserMathErrFunc crt_usermatherr_handler = NULL;
void WINAPI crt_setusermatherr(_UserMathErrFunc handler)
{
    crt_usermatherr_handler = handler;
}

/* _acmdln — pointer to command line string */
static char *crt_acmdln_val = "";
char* WINAPI crt_acmdln(void)
{
    return crt_acmdln_val;
}

/* _adjust_fdiv — FDIV adjustment flag (always 0, no bug) */
static int crt_adjust_fdiv_val = 0;
int* WINAPI crt_adjust_fdiv(void)
{
    return &crt_adjust_fdiv_val;
}

/* _controlfp — control floating point
 * Default x87 control word: 0x027F (round nearest, double precision, all exceptions masked)
 * We store and return a state but don't actually modify FPU — safe for single-threaded compat */
static unsigned int crt_fpcontrol = 0x0009001F; /* MCW_EM=0x1F | MCW_RC=0 | MCW_PC=0x20000 */
unsigned int WINAPI crt_controlfp(unsigned int newval, unsigned int mask)
{
    if (mask) {
        crt_fpcontrol = (crt_fpcontrol & ~mask) | (newval & mask);
    }
    return crt_fpcontrol;
}

/* _ftol — float to long conversion */
long WINAPI crt_ftol(double val)
{
    return (long)val;
}

/* _onexit — register exit callback */
_onexit_t WINAPI crt_onexit(_onexit_t func)
{
    if (func) crt_atexit((void (*)(void))func);
    return func;
}

/* _purecall — pure virtual call handler */
void WINAPI crt_purecall(void)
{
    serial_puts("[MSVCRT] _purecall — pure virtual function call — aborting\n");
    crt_abort();
}

/* ── vprintf family (ms_abi va_list wrappers) ─────────────── */

/*
 * v*printf family — the va_list parameter is a 32-bit pointer to the
 * 32-bit caller's stack (4-byte arg slots). Cast to uint32_t* and use
 * do_vformat32 which walks 4-byte slots correctly.
 */
static int WINAPI crt_vprintf(const char *fmt, ms_va_list ap)
{
    FMT_CTX ctx = { NULL, 0, 0 };
    return do_vformat32(&ctx, fmt, (uint32_t *)(void *)ap);
}

static int WINAPI crt_vsprintf(char *buf, const char *fmt, ms_va_list ap)
{
    FMT_CTX ctx = { buf, (SIZE_T)-1, 0 };
    return do_vformat32(&ctx, fmt, (uint32_t *)(void *)ap);
}

static int WINAPI crt_vsnprintf(char *buf, SIZE_T size, const char *fmt, ms_va_list ap)
{
    FMT_CTX ctx = { buf, size, 0 };
    return do_vformat32(&ctx, fmt, (uint32_t *)(void *)ap);
}

static BOOL crt_secure_truncate_count(SIZE_T count)
{
    /* PE32 passes _TRUNCATE as a zero-extended 0xffffffff. */
    return count == (SIZE_T)-1 || count == (SIZE_T)0xFFFFFFFFU;
}

static SIZE_T crt_snprintf_s_capacity(SIZE_T size, SIZE_T count)
{
    if (crt_secure_truncate_count(count) || count >= size)
        return size;
    return count + 1;
}

static int crt_snprintf_s_result(char *buf, SIZE_T size, SIZE_T count,
                                 int required)
{
    SIZE_T max_chars = crt_secure_truncate_count(count)
        ? size - 1
        : (count < size ? count : size - 1);

    if (required >= 0 && (SIZE_T)required <= max_chars)
        return required;

    /* Explicit count and _TRUNCATE both leave a terminated prefix. When the
     * caller claimed the whole destination was available, MSVCRT treats an
     * overrun as a range error and clears the destination. */
    if (crt_secure_truncate_count(count) || count < size)
        return -1;

    buf[0] = 0;
    *crt_errno() = CRT_ERANGE;
    return -1;
}

static int WINAPI crt_snprintf_s_compat32(char *buf, SIZE_T size,
                                           SIZE_T count, const char *fmt,
                                           uint32_t *args)
{
    if (!buf || !size || !fmt) {
        if (buf && size) buf[0] = 0;
        *crt_errno() = CRT_EINVAL;
        return -1;
    }

    FMT_CTX ctx = { buf, crt_snprintf_s_capacity(size, count), 0 };
    int required = do_vformat32(&ctx, fmt, args);
    return crt_snprintf_s_result(buf, size, count, required);
}

int WINAPI crt_snprintf_s(char *buf, SIZE_T size, SIZE_T count,
                          const char *fmt, ...)
{
    if (!buf || !size || !fmt) {
        if (buf && size) buf[0] = 0;
        *crt_errno() = CRT_EINVAL;
        return -1;
    }

    ms_va_list ap;
    ms_va_start(ap, fmt);
    FMT_CTX ctx = { buf, crt_snprintf_s_capacity(size, count), 0 };
    int required = do_vformat(&ctx, fmt, ap);
    ms_va_end(ap);
    return crt_snprintf_s_result(buf, size, count, required);
}

static int WINAPI crt_vfprintf(PVOID stream, const char *fmt, ms_va_list ap)
{
    (void)stream;
    FMT_CTX ctx = { NULL, 0, 0 };
    return do_vformat32(&ctx, fmt, (uint32_t *)(void *)ap);
}

#define CRT_PRINTF_STANDARD_SNPRINTF (1ULL << 1)

int WINAPI crt_stdio_common_vsprintf(uint64_t options, char *buffer,
                                     SIZE_T buffer_count,
                                     const char *format, PVOID locale,
                                     PVOID arg_list)
{
    (void)locale;
    if (!format || (!buffer && buffer_count != 0) || !arg_list) {
        if (buffer && buffer_count) buffer[0] = 0;
        *crt_errno() = 22; /* EINVAL */
        return -1;
    }

    FMT_CTX ctx = { buffer, buffer_count, 0 };
    int required = do_vformat(&ctx, format, (ms_va_list)arg_list);
    int truncated = buffer_count == 0 || (SIZE_T)required >= buffer_count;

    static int trace_count;
    if (trace_count < 8) {
        trace_count++;
        serial_puts("[UCRT-VS] options=0x");
        serial_puthex(options, 8);
        serial_puts(" count=");
        serial_putdec(buffer_count);
        serial_puts(" required=");
        serial_putdec((uint64_t)required);
        serial_puts(" fmt=\"");
        for (int i = 0; i < 64 && format[i]; i++)
            serial_putchar((unsigned char)format[i] < 0x80 ? format[i] : '?');
        serial_puts("\"\n");
    }

    if (truncated && !(options & CRT_PRINTF_STANDARD_SNPRINTF))
        return -1;
    return required;
}

/* Universal CRT wide formatting for native PE32+ callers. A Microsoft x64
 * va_list walks 8-byte argument homes; it must not share the PE32 formatter
 * below, whose va_list is a packed array of 4-byte stack slots. */
typedef struct {
    WCHAR *buf;
    SIZE_T size;
    SIZE_T pos;
} WFMT_CTX;

static void wfmt_putc(WFMT_CTX *ctx, WCHAR c)
{
    if (ctx->buf && ctx->size > 0 && ctx->pos < ctx->size - 1)
        ctx->buf[ctx->pos] = c;
    ctx->pos++;
}

static void wfmt_put_ascii(WFMT_CTX *ctx, const char *s, SIZE_T len)
{
    for (SIZE_T i = 0; i < len; i++)
        wfmt_putc(ctx, (WCHAR)(unsigned char)s[i]);
}

static void wfmt_put_wide(WFMT_CTX *ctx, const WCHAR *s, SIZE_T len)
{
    for (SIZE_T i = 0; i < len; i++)
        wfmt_putc(ctx, s[i]);
}

static void wfmt_pad(WFMT_CTX *ctx, int count, WCHAR c)
{
    while (count-- > 0)
        wfmt_putc(ctx, c);
}

static SIZE_T wfmt_wcsnlen(const WCHAR *s, int precision)
{
    SIZE_T n = 0;
    if (!s) return 0;
    while (s[n] && (precision < 0 || n < (SIZE_T)precision)) n++;
    return n;
}

enum {
    WLEN_DEFAULT,
    WLEN_HH,
    WLEN_H,
    WLEN_L,
    WLEN_LL,
    WLEN_J,
    WLEN_Z,
    WLEN_T,
    WLEN_I32,
    WLEN_I64
};

static void wfmt_integer(WFMT_CTX *ctx, unsigned long long value, int negative,
                         int base, int upper, int width, int precision,
                         int left, int zero, int plus, int space, int alternate)
{
    char digits[24];
    SIZE_T digits_len = uint_to_str(digits, value, base, upper);
    if (precision == 0 && value == 0) digits_len = 0;

    char prefix[2];
    int prefix_len = 0;
    if (negative) prefix[prefix_len++] = '-';
    else if (plus) prefix[prefix_len++] = '+';
    else if (space) prefix[prefix_len++] = ' ';

    char radix_prefix[2];
    int radix_len = 0;
    if (alternate && base == 16 && value != 0) {
        radix_prefix[radix_len++] = '0';
        radix_prefix[radix_len++] = upper ? 'X' : 'x';
    } else if (alternate && base == 8 &&
               (digits_len == 0 || digits[0] != '0')) {
        radix_prefix[radix_len++] = '0';
    }

    int precision_zeroes = precision > (int)digits_len
                         ? precision - (int)digits_len : 0;
    int content = prefix_len + radix_len + precision_zeroes + (int)digits_len;
    int width_pad = width > content ? width - content : 0;

    if (!left && (!zero || precision >= 0)) wfmt_pad(ctx, width_pad, ' ');
    wfmt_put_ascii(ctx, prefix, (SIZE_T)prefix_len);
    wfmt_put_ascii(ctx, radix_prefix, (SIZE_T)radix_len);
    if (!left && zero && precision < 0) wfmt_pad(ctx, width_pad, '0');
    wfmt_pad(ctx, precision_zeroes, '0');
    wfmt_put_ascii(ctx, digits, digits_len);
    if (left) wfmt_pad(ctx, width_pad, ' ');
}

static void wfmt_float(WFMT_CTX *ctx, double value, int width, int precision,
                       int left, int zero, int plus, int space)
{
    char out[96];
    int pos = 0;
    int negative = value < 0.0;
    if (negative) value = -value;
    if (precision < 0) precision = 6;
    if (precision > 48) precision = 48;

    unsigned long long integer = (unsigned long long)value;
    double fraction = value - (double)integer;
    pos = (int)uint_to_str(out, integer, 10, 0);
    if (precision > 0) {
        out[pos++] = '.';
        for (int i = 0; i < precision; i++) {
            fraction *= 10.0;
            int digit = (int)fraction;
            if (digit < 0) digit = 0;
            if (digit > 9) digit = 9;
            out[pos++] = (char)('0' + digit);
            fraction -= digit;
        }
        if (fraction >= 0.5) {
            int i = pos - 1;
            while (i >= 0) {
                if (out[i] == '.') { i--; continue; }
                if (out[i] != '9') { out[i]++; break; }
                out[i--] = '0';
            }
            if (i < 0 && pos < (int)sizeof(out) - 1) {
                for (int j = pos; j > 0; j--) out[j] = out[j - 1];
                out[0] = '1';
                pos++;
            }
        }
    } else if (fraction >= 0.5) {
        integer++;
        pos = (int)uint_to_str(out, integer, 10, 0);
    }

    char sign = negative ? '-' : plus ? '+' : space ? ' ' : 0;
    int content = pos + (sign != 0);
    int pad = width > content ? width - content : 0;
    if (!left && !zero) wfmt_pad(ctx, pad, ' ');
    if (sign) wfmt_putc(ctx, (WCHAR)sign);
    if (!left && zero) wfmt_pad(ctx, pad, '0');
    wfmt_put_ascii(ctx, out, (SIZE_T)pos);
    if (left) wfmt_pad(ctx, pad, ' ');
}

static int do_vformat_wide64(WFMT_CTX *ctx, const WCHAR *fmt, ms_va_list ap)
{
    static const WCHAR null_wide[] = {'(','n','u','l','l',')',0};
    static const char null_narrow[] = "(null)";

    while (*fmt) {
        if (*fmt != '%') {
            wfmt_putc(ctx, *fmt++);
            continue;
        }
        fmt++;

        int left = 0, zero = 0, plus = 0, space = 0, alternate = 0;
        for (;;) {
            if (*fmt == '-') { left = 1; fmt++; }
            else if (*fmt == '0') { zero = 1; fmt++; }
            else if (*fmt == '+') { plus = 1; fmt++; }
            else if (*fmt == ' ') { space = 1; fmt++; }
            else if (*fmt == '#') { alternate = 1; fmt++; }
            else break;
        }

        int width = 0;
        if (*fmt == '*') {
            width = ms_va_arg(ap, int);
            fmt++;
            if (width < 0) { left = 1; width = -width; }
        } else {
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + (*fmt++ - '0');
        }

        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            if (*fmt == '*') {
                precision = ms_va_arg(ap, int);
                fmt++;
                if (precision < 0) precision = -1;
            } else {
                while (*fmt >= '0' && *fmt <= '9')
                    precision = precision * 10 + (*fmt++ - '0');
            }
        }

        int length = WLEN_DEFAULT;
        if (*fmt == 'h') {
            fmt++;
            length = WLEN_H;
            if (*fmt == 'h') { fmt++; length = WLEN_HH; }
        } else if (*fmt == 'l') {
            fmt++;
            length = WLEN_L;
            if (*fmt == 'l') { fmt++; length = WLEN_LL; }
        } else if (*fmt == 'j') { fmt++; length = WLEN_J; }
        else if (*fmt == 'z') { fmt++; length = WLEN_Z; }
        else if (*fmt == 't') { fmt++; length = WLEN_T; }
        else if (*fmt == 'I') {
            if (fmt[1] == '6' && fmt[2] == '4') { fmt += 3; length = WLEN_I64; }
            else if (fmt[1] == '3' && fmt[2] == '2') { fmt += 3; length = WLEN_I32; }
            else { fmt++; length = WLEN_Z; }
        }

        WCHAR conversion = *fmt;
        if (!conversion) break;
        fmt++;

        switch (conversion) {
        case 'd': case 'i': {
            long long signed_value;
            if (length == WLEN_LL || length == WLEN_I64 ||
                length == WLEN_J || length == WLEN_Z || length == WLEN_T)
                signed_value = ms_va_arg(ap, long long);
            else
                signed_value = (long long)ms_va_arg(ap, int);
            int negative = signed_value < 0;
            unsigned long long magnitude = negative
                ? 0ULL - (unsigned long long)signed_value
                : (unsigned long long)signed_value;
            wfmt_integer(ctx, magnitude, negative, 10, 0, width, precision,
                         left, zero, plus, space, 0);
            break;
        }
        case 'u': case 'o': case 'x': case 'X': {
            unsigned long long value;
            if (length == WLEN_LL || length == WLEN_I64 ||
                length == WLEN_J || length == WLEN_Z || length == WLEN_T)
                value = ms_va_arg(ap, unsigned long long);
            else
                value = (unsigned long long)ms_va_arg(ap, unsigned int);
            int base = conversion == 'o' ? 8 :
                       (conversion == 'x' || conversion == 'X') ? 16 : 10;
            wfmt_integer(ctx, value, 0, base, conversion == 'X', width,
                         precision, left, zero, 0, 0, alternate);
            break;
        }
        case 'p': {
            unsigned long long value =
                (unsigned long long)(ULONG_PTR)ms_va_arg(ap, PVOID);
            wfmt_integer(ctx, value, 0, 16, 0, width, precision,
                         left, zero, 0, 0, 1);
            break;
        }
        case 's': {
            if (length == WLEN_H || length == WLEN_HH) {
                const char *s = ms_va_arg(ap, const char *);
                if (!s) s = null_narrow;
                SIZE_T len = 0;
                while (s[len] && (precision < 0 || len < (SIZE_T)precision)) len++;
                if (!left) wfmt_pad(ctx, width - (int)len, ' ');
                wfmt_put_ascii(ctx, s, len);
                if (left) wfmt_pad(ctx, width - (int)len, ' ');
            } else {
                const WCHAR *s = ms_va_arg(ap, const WCHAR *);
                if (!s) s = null_wide;
                SIZE_T len = wfmt_wcsnlen(s, precision);
                if (!left) wfmt_pad(ctx, width - (int)len, ' ');
                wfmt_put_wide(ctx, s, len);
                if (left) wfmt_pad(ctx, width - (int)len, ' ');
            }
            break;
        }
        case 'S': {
            const char *s = ms_va_arg(ap, const char *);
            if (!s) s = null_narrow;
            SIZE_T len = 0;
            while (s[len] && (precision < 0 || len < (SIZE_T)precision)) len++;
            if (!left) wfmt_pad(ctx, width - (int)len, ' ');
            wfmt_put_ascii(ctx, s, len);
            if (left) wfmt_pad(ctx, width - (int)len, ' ');
            break;
        }
        case 'c': {
            WCHAR c = (WCHAR)ms_va_arg(ap, int);
            if (!left) wfmt_pad(ctx, width - 1, ' ');
            wfmt_putc(ctx, c);
            if (left) wfmt_pad(ctx, width - 1, ' ');
            break;
        }
        case 'C': {
            WCHAR c = (WCHAR)(unsigned char)ms_va_arg(ap, int);
            if (!left) wfmt_pad(ctx, width - 1, ' ');
            wfmt_putc(ctx, c);
            if (left) wfmt_pad(ctx, width - 1, ' ');
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            wfmt_float(ctx, ms_va_arg(ap, double), width, precision,
                       left, zero, plus, space);
            break;
        case 'n': {
            PVOID out = ms_va_arg(ap, PVOID);
            if (!out) break;
            if (length == WLEN_HH) *(signed char *)out = (signed char)ctx->pos;
            else if (length == WLEN_H) *(short *)out = (short)ctx->pos;
            else if (length == WLEN_LL || length == WLEN_I64 ||
                     length == WLEN_J || length == WLEN_Z || length == WLEN_T)
                *(long long *)out = (long long)ctx->pos;
            else *(int *)out = (int)ctx->pos;
            break;
        }
        case '%':
            wfmt_putc(ctx, '%');
            break;
        default:
            wfmt_putc(ctx, '%');
            wfmt_putc(ctx, conversion);
            break;
        }
    }

    if (ctx->buf && ctx->size > 0) {
        SIZE_T end = ctx->pos < ctx->size - 1 ? ctx->pos : ctx->size - 1;
        ctx->buf[end] = 0;
    }
    return (int)ctx->pos;
}

int WINAPI crt_stdio_common_vswprintf(uint64_t options, WCHAR *buffer,
                                      SIZE_T buffer_count,
                                      const WCHAR *format, PVOID locale,
                                      PVOID arg_list)
{
    (void)locale; /* The locale shim currently exposes the invariant C locale. */
    if (!format || (!buffer && buffer_count != 0) || !arg_list) {
        if (buffer && buffer_count) buffer[0] = 0;
        *crt_errno() = 22; /* EINVAL */
        return -1;
    }

    WFMT_CTX ctx = { buffer, buffer_count, 0 };
    int required = do_vformat_wide64(&ctx, format, (ms_va_list)arg_list);
    int truncated = buffer_count == 0 || (SIZE_T)required >= buffer_count;

    static int trace_count;
    if (trace_count < 8) {
        trace_count++;
        serial_puts("[UCRT-VSW] options=0x");
        serial_puthex(options, 8);
        serial_puts(" count=");
        serial_putdec(buffer_count);
        serial_puts(" required=");
        serial_putdec((uint64_t)required);
        serial_puts(" fmt=\"");
        for (int i = 0; i < 48 && format[i]; i++)
            serial_putchar((char)(format[i] < 0x80 ? format[i] : '?'));
        serial_puts("\"\n");
    }

    if (truncated && !(options & CRT_PRINTF_STANDARD_SNPRINTF))
        return -1;
    return required;
}

/* ── UT99 Core.dll / Engine.dll missing exports ────────────── */

/* ?terminate@@YAXXZ — C++ terminate() handler */
void WINAPI crt_terminate(void)
{
    serial_puts("[MSVCRT] terminate() called — ExitProcess(3)\n");
    ExitProcess(3);
}

/* _CIacos — compiler intrinsic wrapper for acos */
double WINAPI crt_CIacos(double x)
{
#ifdef TEST_HARNESS
    return acos(x);
#else
    /* Stub: Bhaskara I approximation for acos(x) */
    /* acos(x) ≈ pi/2 - asin(x), asin(x) ≈ x for small x */
    /* For UT99, a rough approximation is acceptable */
    if (x >= 1.0)  return 0.0;
    if (x <= -1.0) return 3.14159265358979323846;
    /* Use identity: acos(x) = pi/2 - x - x^3/6 - 3*x^5/40 */
    double x2 = x * x;
    double x3 = x2 * x;
    double x5 = x3 * x2;
    return 1.5707963267948966 - x - x3 / 6.0 - 3.0 * x5 / 40.0;
#endif
}

/* _CIfmod — compiler intrinsic wrapper for fmod */
double WINAPI crt_CIfmod(double x, double y)
{
    if (y == 0.0) return 0.0;
    /* fmod(x, y) = x - trunc(x/y) * y */
    double quotient = x / y;
    long trunc_q = (long)quotient;
    return x - (double)trunc_q * y;
}

/* ── Fast 32-bit x87 math (no INT 0x2E overhead) ──────────────
 *
 * _CIpow, _CIfmod, _CIacos use the MSVC _CI calling convention:
 * arguments on the x87 FPU stack, result in ST(0).
 * These run as native 32-bit code in compat mode — no mode switch. */

uint32_t g_fast_CIpow_addr = 0;
uint32_t g_fast_CIfmod_addr = 0;
uint32_t g_fast_CIacos_addr = 0;
uint32_t g_fast_fabs_addr = 0;
uint32_t g_fast_sqrt_addr = 0;

void compat32_init_fast_math(uint8_t *page, uint32_t user_base)
{
    if (!page || !user_base) {
        serial_puts("[FAST-MATH] runtime page missing\n");
        return;
    }

    /* Zero and make executable (page from mem_alloc_pages is identity-mapped) */
    for (int i = 0; i < 4096; i++) page[i] = 0xCC;  /* INT3 fill */

    int p = 0;

    /* _CIpow: ST(1)=base, ST(0)=exp → result in ST(0)
     * Algorithm: pow(x,y) = 2^(y * log2(x))
     * Using x87: fyl2x → f2xm1 → fscale */
    g_fast_CIpow_addr = user_base + (uint32_t)p;
    /* fxch st(1) */          page[p++] = 0xD9; page[p++] = 0xC9;
    /* fyl2x — ST(0) = ST(1) * log2(ST(0)), pop */
                               page[p++] = 0xD9; page[p++] = 0xF1;
    /* fld st(0) — dup */     page[p++] = 0xD9; page[p++] = 0xC0;
    /* frndint — ST(0) = round(val) */ page[p++] = 0xD9; page[p++] = 0xFC;
    /* fxch st(1) */          page[p++] = 0xD9; page[p++] = 0xC9;
    /* fsub st(0), st(1) — frac = val - int_part */
                               page[p++] = 0xD8; page[p++] = 0xE1;
    /* f2xm1 — ST(0) = 2^frac - 1 */
                               page[p++] = 0xD9; page[p++] = 0xF0;
    /* fld1 */                page[p++] = 0xD9; page[p++] = 0xE8;
    /* faddp st(1), st(0) — ST(0) = 2^frac */
                               page[p++] = 0xDE; page[p++] = 0xC1;
    /* fscale — ST(0) *= 2^ST(1) = 2^int_part */
                               page[p++] = 0xD9; page[p++] = 0xFD;
    /* fstp st(1) — pop int_part, result in ST(0) */
                               page[p++] = 0xDD; page[p++] = 0xD9;
    /* ret */                 page[p++] = 0xC3;

    /* Align next function */
    p = (p + 15) & ~15;

    /* _CIfmod: ST(1)=x, ST(0)=y → result in ST(0) = x mod y */
    g_fast_CIfmod_addr = user_base + (uint32_t)p;
    /* fxch st(1) */          page[p++] = 0xD9; page[p++] = 0xC9;
    /* fprem */               page[p++] = 0xD9; page[p++] = 0xF8;
    /* fstp st(1) */          page[p++] = 0xDD; page[p++] = 0xD9;
    /* ret */                 page[p++] = 0xC3;

    p = (p + 15) & ~15;

    /* _CIacos: ST(0)=x → result in ST(0) = acos(x)
     * acos(x) = atan2(sqrt(1-x^2), x) */
    g_fast_CIacos_addr = user_base + (uint32_t)p;
    /* fld st(0) — dup x */   page[p++] = 0xD9; page[p++] = 0xC0;
    /* fmul st(0), st(0) */   page[p++] = 0xD8; page[p++] = 0xC8;
    /* fld1 */                page[p++] = 0xD9; page[p++] = 0xE8;
    /* fsubrp st(1) */        page[p++] = 0xDE; page[p++] = 0xE9;
    /* fsqrt */               page[p++] = 0xD9; page[p++] = 0xFA;
    /* fxch st(1) */          page[p++] = 0xD9; page[p++] = 0xC9;
    /* fpatan — atan2(ST(1), ST(0)) */
                               page[p++] = 0xD9; page[p++] = 0xF3;
    /* ret */                 page[p++] = 0xC3;

    p = (p + 15) & ~15;

    /* fabs(double): argument at [esp+4], cdecl result in ST(0). */
    g_fast_fabs_addr = user_base + (uint32_t)p;
    /* fld qword [esp+4] */   page[p++] = 0xDD; page[p++] = 0x44;
                               page[p++] = 0x24; page[p++] = 0x04;
    /* fabs */                page[p++] = 0xD9; page[p++] = 0xE1;
    /* ret */                 page[p++] = 0xC3;

    p = (p + 15) & ~15;

    /* sqrt(double): argument at [esp+4], cdecl result in ST(0). */
    g_fast_sqrt_addr = user_base + (uint32_t)p;
    /* fld qword [esp+4] */   page[p++] = 0xDD; page[p++] = 0x44;
                               page[p++] = 0x24; page[p++] = 0x04;
    /* fsqrt */               page[p++] = 0xD9; page[p++] = 0xFA;
    /* ret */                 page[p++] = 0xC3;

    serial_puts("[FAST-MATH] pow=0x");
    serial_puthex(g_fast_CIpow_addr, 8);
    serial_puts(" fmod=0x");
    serial_puthex(g_fast_CIfmod_addr, 8);
    serial_puts(" acos=0x");
    serial_puthex(g_fast_CIacos_addr, 8);
    serial_puts(" fabs=0x");
    serial_puthex(g_fast_fabs_addr, 8);
    serial_puts(" sqrt=0x");
    serial_puthex(g_fast_sqrt_addr, 8);
    serial_puts("\n");
}

/* _CIpow — compiler intrinsic wrapper for pow (fallback via INT 0x2E) */
double WINAPI crt_CIpow(double base, double exp)
{
#ifdef TEST_HARNESS
    return pow(base, exp);
#else
    /* Simple integer-exponent pow for common UT99 cases */
    if (exp == 0.0) return 1.0;
    if (base == 0.0) return 0.0;
    if (base == 1.0) return 1.0;

    /* Handle integer exponents exactly */
    int iexp = (int)exp;
    if ((double)iexp == exp && iexp >= 0) {
        double result = 1.0;
        double b = base;
        int e = iexp;
        while (e > 0) {
            if (e & 1) result *= b;
            b *= b;
            e >>= 1;
        }
        return result;
    }

    /* For non-integer exponents, use repeated squaring approximation */
    /* This is a rough fallback — UT99 mostly uses integer powers */
    if (exp < 0.0) return 1.0 / crt_CIpow(base, -exp);
    return base; /* fallback for fractional exponents */
#endif
}

/* _isnan — check for NaN (IEEE 754: exponent all 1s, mantissa non-zero) */
int WINAPI crt_isnan(double x)
{
    uint64_t bits;
    __builtin_memcpy(&bits, &x, 8);
    return ((bits >> 52) & 0x7FF) == 0x7FF && (bits & 0x000FFFFFFFFFFFFFULL) != 0;
}

static short crt_fpclass_from_parts(uint64_t exponent, uint64_t fraction,
                                    uint64_t exponent_mask)
{
    if (exponent == exponent_mask)
        return fraction ? 2 : 1;   /* FP_NAN : FP_INFINITE */
    if (exponent == 0)
        return fraction ? -2 : 0;  /* FP_SUBNORMAL : FP_ZERO */
    return -1;                     /* FP_NORMAL */
}

short WINAPI crt_dclass(double x)
{
    uint64_t bits;
    __builtin_memcpy(&bits, &x, sizeof(bits));
    return crt_fpclass_from_parts((bits >> 52) & 0x7FF,
                                  bits & 0x000FFFFFFFFFFFFFULL, 0x7FF);
}

short WINAPI crt_fdclass(float x)
{
    uint32_t bits;
    __builtin_memcpy(&bits, &x, sizeof(bits));
    return crt_fpclass_from_parts((bits >> 23) & 0xFF,
                                  bits & 0x007FFFFF, 0xFF);
}

/* PE32 MSVC stat layouts. Keep these independent from the 64-bit kernel ABI. */
typedef struct __attribute__((packed)) {
    uint32_t st_dev;
    uint16_t st_ino;
    uint16_t st_mode;
    int16_t st_nlink;
    int16_t st_uid;
    int16_t st_gid;
    uint16_t reserved0;
    uint32_t st_rdev;
    int32_t st_size;
    int32_t st_atime;
    int32_t st_mtime;
    int32_t st_ctime;
} CRT_STAT32;

typedef struct __attribute__((packed)) {
    uint32_t st_dev;
    uint16_t st_ino;
    uint16_t st_mode;
    int16_t st_nlink;
    int16_t st_uid;
    int16_t st_gid;
    uint16_t reserved0;
    uint32_t st_rdev;
    uint32_t reserved1;
    int64_t st_size;
    int32_t st_atime;
    int32_t st_mtime;
    int32_t st_ctime;
    uint32_t reserved2;
} CRT_STAT32I64;

typedef struct __attribute__((packed)) {
    uint32_t st_dev;
    uint16_t st_ino;
    uint16_t st_mode;
    int16_t st_nlink;
    int16_t st_uid;
    int16_t st_gid;
    uint16_t reserved0;
    uint32_t st_rdev;
    int32_t st_size;
    int64_t st_atime;
    int64_t st_mtime;
    int64_t st_ctime;
} CRT_STAT64I32;

typedef struct __attribute__((packed)) {
    uint32_t st_dev;
    uint16_t st_ino;
    uint16_t st_mode;
    int16_t st_nlink;
    int16_t st_uid;
    int16_t st_gid;
    uint16_t reserved0;
    uint32_t st_rdev;
    uint32_t reserved1;
    int64_t st_size;
    int64_t st_atime;
    int64_t st_mtime;
    int64_t st_ctime;
} CRT_STAT64;

_Static_assert(sizeof(CRT_STAT32) == 36, "PE32 _stat32 layout changed");
_Static_assert(sizeof(CRT_STAT32I64) == 48,
               "PE32 _stat32i64 layout changed");
_Static_assert(sizeof(CRT_STAT64I32) == 48,
               "PE32 _stat64i32 layout changed");
_Static_assert(sizeof(CRT_STAT64) == 56, "PE32 _stat64 layout changed");

typedef struct {
    uint16_t mode;
    uint64_t size;
    int64_t atime;
    int64_t mtime;
    int64_t ctime;
} CRT_STAT_META;

#define CRT_S_IFDIR  0x4000
#define CRT_S_IFREG  0x8000
#define CRT_S_IEXEC  0x0040
#define CRT_S_IWRITE 0x0080
#define CRT_S_IREAD  0x0100

extern void *osfs2_find_exact_ci(const char *name);
extern void *osfs2_find(const char *name);
extern uint64_t osfs2_file_size(void *file);
extern uint32_t osfs2_file_ctime(void *file);
extern uint32_t osfs2_file_mtime(void *file);

static int crt_path_has_executable_suffix(const char *path)
{
    const char *suffix = path;
    for (const char *p = path; *p; p++) {
        if (*p == '\\' || *p == '/' || *p == '.') suffix = p;
    }
    if (*suffix != '.') return 0;

    char ext[5] = {0};
    int i = 0;
    while (suffix[i] && i < 4) {
        char c = suffix[i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        ext[i++] = c;
    }
    if (suffix[i]) return 0;
    return (ext[1] == 'e' && ext[2] == 'x' && ext[3] == 'e') ||
           (ext[1] == 'c' && ext[2] == 'o' && ext[3] == 'm') ||
           (ext[1] == 'b' && ext[2] == 'a' && ext[3] == 't') ||
           (ext[1] == 'c' && ext[2] == 'm' && ext[3] == 'd');
}

static int crt_stat_query(const char *path, CRT_STAT_META *meta)
{
    if (!path || !path[0] || !meta) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }

    char normalized[260];
    if (!win32_normalize_path(path, normalized)) {
        *crt_errno() = CRT_ENAMETOOLONG;
        return -1;
    }

    void *file = osfs2_find_exact_ci(normalized);
    if (file) {
        meta->mode = CRT_S_IFREG | CRT_S_IREAD | CRT_S_IWRITE;
        if (crt_path_has_executable_suffix(normalized))
            meta->mode |= CRT_S_IEXEC;
        meta->size = osfs2_file_size(file);
        meta->ctime = (int64_t)osfs2_file_ctime(file);
        meta->mtime = (int64_t)osfs2_file_mtime(file);
        meta->atime = meta->mtime;
        *crt_errno() = 0;
        return 0;
    }

    if (win32_directory_exists_normalized(normalized)) {
        meta->mode = CRT_S_IFDIR | CRT_S_IREAD | CRT_S_IWRITE | CRT_S_IEXEC;
        meta->size = 0;
        meta->atime = 0;
        meta->mtime = 0;
        meta->ctime = 0;
        *crt_errno() = 0;
        return 0;
    }

    *crt_errno() = CRT_ENOENT;
    return -1;
}

#define CRT_STAT_FILL_COMMON(stat, meta) do { \
    (stat)->st_dev = 2; /* C: */ \
    (stat)->st_ino = 0; \
    (stat)->st_mode = (meta)->mode; \
    (stat)->st_nlink = 1; \
    (stat)->st_uid = 0; \
    (stat)->st_gid = 0; \
    (stat)->st_rdev = 2; \
} while (0)

static int crt_stat32_impl(const char *path, PVOID buf)
{
    CRT_STAT_META meta;
    if (!buf) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }
    if (crt_stat_query(path, &meta) < 0) return -1;
    if (meta.size > 0x7FFFFFFFULL) {
        *crt_errno() = CRT_EOVERFLOW;
        return -1;
    }

    CRT_STAT32 *stat = (CRT_STAT32 *)buf;
    crt_memset(stat, 0, sizeof(*stat));
    CRT_STAT_FILL_COMMON(stat, &meta);
    stat->st_size = (int32_t)meta.size;
    stat->st_atime = (int32_t)meta.atime;
    stat->st_mtime = (int32_t)meta.mtime;
    stat->st_ctime = (int32_t)meta.ctime;
    return 0;
}

static int crt_stat32i64_impl(const char *path, PVOID buf)
{
    CRT_STAT_META meta;
    if (!buf) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }
    if (crt_stat_query(path, &meta) < 0) return -1;

    CRT_STAT32I64 *stat = (CRT_STAT32I64 *)buf;
    crt_memset(stat, 0, sizeof(*stat));
    CRT_STAT_FILL_COMMON(stat, &meta);
    stat->st_size = (int64_t)meta.size;
    stat->st_atime = (int32_t)meta.atime;
    stat->st_mtime = (int32_t)meta.mtime;
    stat->st_ctime = (int32_t)meta.ctime;
    return 0;
}

static int crt_stat64i32_impl(const char *path, PVOID buf)
{
    CRT_STAT_META meta;
    if (!buf) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }
    if (crt_stat_query(path, &meta) < 0) return -1;
    if (meta.size > 0x7FFFFFFFULL) {
        *crt_errno() = CRT_EOVERFLOW;
        return -1;
    }

    CRT_STAT64I32 *stat = (CRT_STAT64I32 *)buf;
    crt_memset(stat, 0, sizeof(*stat));
    CRT_STAT_FILL_COMMON(stat, &meta);
    stat->st_size = (int32_t)meta.size;
    stat->st_atime = meta.atime;
    stat->st_mtime = meta.mtime;
    stat->st_ctime = meta.ctime;
    if (crt_io_trace_take(&crt_stat_trace_count, 64)) {
        serial_puts("[CRT-STAT64I32] size=");
        serial_putdec(meta.size);
        serial_puts(" mode=0x");
        serial_puthex(meta.mode, 4);
        serial_puts(" path='");
        serial_puts(path);
        serial_puts("'\n");
    }
    return 0;
}

static int crt_stat64_impl(const char *path, PVOID buf)
{
    CRT_STAT_META meta;
    if (!buf) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }
    if (crt_stat_query(path, &meta) < 0) return -1;

    CRT_STAT64 *stat = (CRT_STAT64 *)buf;
    crt_memset(stat, 0, sizeof(*stat));
    CRT_STAT_FILL_COMMON(stat, &meta);
    stat->st_size = (int64_t)meta.size;
    stat->st_atime = meta.atime;
    stat->st_mtime = meta.mtime;
    stat->st_ctime = meta.ctime;
    return 0;
}

int WINAPI crt_stat(const char *path, PVOID buf)
{
    return crt_stat64i32_impl(path, buf);
}

int WINAPI crt_stat32(const char *path, PVOID buf)
{
    return crt_stat32_impl(path, buf);
}

int WINAPI crt_stat32i64(const char *path, PVOID buf)
{
    return crt_stat32i64_impl(path, buf);
}

int WINAPI crt_stat64i32(const char *path, PVOID buf)
{
    return crt_stat64i32_impl(path, buf);
}

int WINAPI crt_stat64(const char *path, PVOID buf)
{
    return crt_stat64_impl(path, buf);
}

typedef int (*CRT_STAT_IMPL)(const char *, PVOID);

static int crt_wstat_impl(const WCHAR *path, PVOID buf, CRT_STAT_IMPL impl)
{
    if (!path || !buf) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }
    char narrow[260];
    if (!WideCharToMultiByte(0 /* CP_ACP */, 0, path, -1, narrow,
                             sizeof(narrow), NULL, NULL)) {
        *crt_errno() = CRT_ENAMETOOLONG;
        return -1;
    }
    return impl(narrow, buf);
}

int WINAPI crt_wstat(const WCHAR *path, PVOID buf)
{
    return crt_wstat_impl(path, buf, crt_stat64i32_impl);
}

int WINAPI crt_wstat32(const WCHAR *path, PVOID buf)
{
    return crt_wstat_impl(path, buf, crt_stat32_impl);
}

int WINAPI crt_wstat32i64(const WCHAR *path, PVOID buf)
{
    return crt_wstat_impl(path, buf, crt_stat32i64_impl);
}

int WINAPI crt_wstat64i32(const WCHAR *path, PVOID buf)
{
    return crt_wstat_impl(path, buf, crt_stat64i32_impl);
}

int WINAPI crt_wstat64(const WCHAR *path, PVOID buf)
{
    return crt_wstat_impl(path, buf, crt_stat64_impl);
}

/* _strdate / _strtime — date/time strings (stub values) */
char* WINAPI crt_strdate(char *buf)
{
    /* MM/DD/YY format */
    static const char date[] = "01/01/26";
    if (buf) {
        for (int i = 0; i < 9; i++) buf[i] = date[i];
    }
    return buf;
}

char* WINAPI crt_strtime(char *buf)
{
    /* HH:MM:SS format */
    static const char time_str[] = "00:00:00";
    if (buf) {
        for (int i = 0; i < 9; i++) buf[i] = time_str[i];
    }
    return buf;
}

/* _wstrdate / _wstrtime — wide date/time strings */
WCHAR* WINAPI crt_wstrdate(WCHAR *buf)
{
    static const WCHAR date[] = {'0','1','/','0','1','/','2','6',0};
    if (buf) {
        for (int i = 0; i < 9; i++) buf[i] = date[i];
    }
    return buf;
}

WCHAR* WINAPI crt_wstrtime(WCHAR *buf)
{
    static const WCHAR time_str[] = {'0','0',':','0','0',':','0','0',0};
    if (buf) {
        for (int i = 0; i < 9; i++) buf[i] = time_str[i];
    }
    return buf;
}

/* _vsnwprintf — wide vsnprintf with format processing
 *
 * CRITICAL: This is called from 32-bit compat mode via INT 0x2E thunk.
 * The va_list (ap) points to the 32-bit caller's stack where each vararg
 * occupies 4 bytes. ms_va_arg() reads 8-byte slots (64-bit mode) which
 * is WRONG — it combines two 4-byte args into one garbage value.
 * We must manually walk 4-byte slots using a uint32_t pointer.
 */
static int vsnw_trace_count = 0;

int WINAPI crt_vsnwprintf(WCHAR *buf, SIZE_T count, const WCHAR *fmt, ms_va_list ap)
{
    if (!buf || count == 0) return 0;
    if (!fmt) { buf[0] = 0; return 0; }

    /* Walk the 32-bit va_list manually — 4 bytes per arg */
    uint32_t *vp = (uint32_t *)(void *)ap;

    /* Diagnostic: print caller_eip + first 4 args when fmt starts with
     * "Failed to load" — this is the appSprintf that builds the cascade
     * we're hunting. */
    if (fmt[0] == L'F' && fmt[1] == L'a' && fmt[2] == L'i' && fmt[3] == L'l') {
        extern uint32_t compat32_get_last_caller_eip(void);
        extern void serial_puthex(uint64_t v, int d);
        uint32_t ceip = compat32_get_last_caller_eip();
        serial_puts("[FAIL-FMT] caller_eip=0x");
        serial_puthex((uint64_t)ceip, 8);
        serial_puts(" fmt=\"");
        for (int k = 0; k < 50 && fmt[k]; k++)
            serial_putchar((char)(fmt[k] & 0x7F));
        serial_puts("\" args=");
        for (int k = 0; k < 6; k++) {
            serial_puts(" [");
            serial_putdec((uint64_t)k);
            serial_puts("]=0x");
            serial_puthex((uint64_t)vp[k], 8);
        }
        serial_puts("\n");
        for (int k = 0; k < 6; k++) {
            uint32_t a = vp[k];
            if (a < 0x10000 || a >= 0x80000000u) continue;
            const WCHAR *p = (const WCHAR *)(uintptr_t)a;
            uint16_t w0 = *(volatile uint16_t *)p;
            uint8_t lo = (uint8_t)(w0 & 0xFF), hi = (uint8_t)(w0 >> 8);
            if (lo < 0x20 || lo >= 0x7F || hi != 0) continue;
            serial_puts("  arg[");
            serial_putdec((uint64_t)k);
            serial_puts("]=L\"");
            for (int j = 0; j < 60 && p[j]; j++)
                serial_putchar((char)(p[j] & 0x7F));
            serial_puts("\"\n");
        }
    }

#ifndef OK_QUIET
    /* Debug: trace first 30 calls to see what's going on */
    if (vsnw_trace_count < 30) {
        vsnw_trace_count++;
        /* Dump narrow version of the wide format string */
        serial_puts("[VSNW#");
        serial_putdec(vsnw_trace_count);
        serial_puts("] fmt=\"");
        for (int k = 0; k < 40 && fmt[k]; k++)
            serial_putchar((char)(fmt[k] & 0x7F));
        serial_puts("\" ap=0x");
        serial_puthex((uint64_t)(uintptr_t)vp, 8);
        serial_puts(" vp[0..11]=");
        for (int k = 0; k < 12; k++) {
            serial_puts(" 0x");
            serial_puthex((uint64_t)vp[k], 8);
        }
        serial_puts("\n");
        /* If vp[1] looks like a small number (FString.Num?) check vp[3] as WCHAR* */
        if (vp[1] < 0x100 && vp[3] >= 0x10000 && vp[3] < 0x20000000) {
            const WCHAR *probe = (const WCHAR *)(uintptr_t)vp[3];
            serial_puts("  [PROBE vp[3] as WCHAR*] -> \"");
            for (int k = 0; k < 30 && probe[k]; k++)
                serial_putchar((char)(probe[k] & 0x7F));
            serial_puts("\"\n");
        }
    }

    /* ── FName::Names diagnostic ── */
    {
        extern uint32_t g_fname_names_addr;
        extern uint32_t g_gmalloc_addr;
        static int fname_diag_count = 0;
        int has_0c = 0;
        for (int k = 0; k < 8; k++) {
            if (vp[k] == 0x0000000C) { has_0c = 1; break; }
        }
        /* Also dump when "Name subsystem" message appears */
        int is_namesys = 0;
        {
            const WCHAR *f = fmt;
            if (f[0]=='N' && f[1]=='a' && f[2]=='m' && f[3]=='e' && f[4]==' ') is_namesys = 1;
        }
        if ((has_0c || is_namesys) && fname_diag_count < 3 && g_fname_names_addr) {
            fname_diag_count++;
            /*
             * FName::Names is TArray<FNameEntry*>:
             *   +0x00: FNameEntry** Data  (4 bytes)
             *   +0x04: INT Num            (4 bytes)
             *   +0x08: INT Max            (4 bytes)
             */
            uint32_t *tarray = (uint32_t *)(uintptr_t)g_fname_names_addr;
            uint32_t data_ptr = tarray[0];
            uint32_t num      = tarray[1];
            uint32_t max      = tarray[2];
            serial_puts("[FNAME-DIAG] FName::Names @ 0x");
            serial_puthex(g_fname_names_addr, 8);
            serial_puts(" Data=0x");
            serial_puthex(data_ptr, 8);
            serial_puts(" Num=");
            serial_putdec(num);
            serial_puts(" Max=");
            serial_putdec(num > 0x10000 ? 0xBAD : max);
            serial_puts("\n");
            /* Dump entries 0-7 and scan for first 8 non-null entries */
            if (data_ptr >= 0x10000 && data_ptr < 0x20000000 && num > 0 && num < 0x10000) {
                uint32_t *entries = (uint32_t *)(uintptr_t)data_ptr;
                /* First dump indices 0-7 */
                int show = num < 8 ? (int)num : 8;
                for (int k = 0; k < show; k++) {
                    serial_puts("  Names[");
                    serial_putdec(k);
                    serial_puts("]=0x");
                    serial_puthex(entries[k], 8);
                    if (entries[k] >= 0x10000 && entries[k] < 0x20000000) {
                        /* FNameEntry: +0x00 Index(4), +0x04 Flags(4), +0x08 HashNext(4), +0x0C Name[] */
                        uint8_t *entry = (uint8_t *)(uintptr_t)entries[k];
                        serial_puts(" W=\"");
                        /* Try WCHAR: read 2 bytes at a time from +0x0C */
                        const uint16_t *wn = (const uint16_t *)(entry + 0x0C);
                        for (int j = 0; j < 16 && wn[j] && wn[j] < 128; j++)
                            serial_putchar((char)wn[j]);
                        serial_puts("\"");
                    } else if (entries[k] == 0) {
                        serial_puts(" (NULL!)");
                    }
                    serial_puts("\n");
                }
                /* Scan for non-null entries beyond index 7 */
                int found_nonnull = 0;
                int limit = (int)(num < 838 ? num : 838);
                for (int k = 8; k < limit && found_nonnull < 4; k++) {
                    if (entries[k] != 0) {
                        serial_puts("  Names[");
                        serial_putdec(k);
                        serial_puts("]=0x");
                        serial_puthex(entries[k], 8);
                        if (entries[k] >= 0x10000 && entries[k] < 0x20000000) {
                            uint8_t *entry = (uint8_t *)(uintptr_t)entries[k];
                            serial_puts(" W=\"");
                            const uint16_t *wn = (const uint16_t *)(entry + 0x0C);
                            for (int j = 0; j < 16 && wn[j] && wn[j] < 128; j++)
                                serial_putchar((char)wn[j]);
                            serial_puts("\"");
                        }
                        serial_puts("\n");
                        found_nonnull++;
                    }
                }
                if (found_nonnull == 0)
                    serial_puts("  (all entries 8..838 are NULL!)\n");
                /* Count total non-null */
                int total_nonnull = 0;
                for (int k = 0; k < limit; k++)
                    if (entries[k] != 0) total_nonnull++;
                serial_puts("  total non-null: ");
                serial_putdec(total_nonnull);
                serial_puts(" / ");
                serial_putdec(limit);
                serial_puts("\n");
            }
            /* Also dump GMalloc */
            if (g_gmalloc_addr) {
                uint32_t *gm = (uint32_t *)(uintptr_t)g_gmalloc_addr;
                serial_puts("[FNAME-DIAG] GMalloc @ 0x");
                serial_puthex(g_gmalloc_addr, 8);
                serial_puts(" -> 0x");
                serial_puthex(*gm, 8);
                serial_puts("\n");
            }
        }
    }
#endif

    SIZE_T pos = 0;
    SIZE_T max = count - 1;

    while (*fmt && pos < max) {
        if (*fmt != '%') {
            buf[pos++] = *fmt++;
            continue;
        }
        fmt++; /* skip '%' */

        /* Parse flags */
        int left_align = 0, zero_pad = 0;
        while (*fmt == '-' || *fmt == '0') {
            if (*fmt == '-') left_align = 1;
            if (*fmt == '0') zero_pad = 1;
            fmt++;
        }
        (void)left_align;

        /* Parse width */
        int width = 0;
        if (*fmt == '*') {
            width = (int)(*vp++);
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + (*fmt++ - '0');
        }

        /* Parse precision */
        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            if (*fmt == '*') {
                precision = (int)(*vp++);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9')
                    precision = precision * 10 + (*fmt++ - '0');
            }
        }

        /* Parse length modifier */
        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; }
        if (*fmt == 'l') { fmt++; } /* ll */

        /* Conversion */
        switch (*fmt) {
        case 's': {
            /* In MSVC _vsnwprintf: %s = WCHAR* (wide string).
             * %ls is also wide string. We treat both the same. */
            uint32_t raw_ptr = *vp++;
            const WCHAR *ws = (const WCHAR *)(uintptr_t)raw_ptr;
#ifndef OK_QUIET
            if (vsnw_trace_count <= 30) {
                serial_puts("  %s ptr=0x");
                serial_puthex((uint64_t)raw_ptr, 8);
                if (ws && raw_ptr >= 0x1000) {
                    serial_puts(" -> \"");
                    for (int k = 0; k < 80 && ws[k]; k++)
                        serial_putchar((char)(ws[k] & 0x7F));
                    serial_puts("\"");
                }
                serial_puts("\n");
            }
#endif
            if (!ws) ws = (const WCHAR[]){'(','n','u','l','l',')',0};
            int n = 0;
            while (ws[n]) n++;
            if (precision >= 0 && n > precision) n = precision;
            for (int i = 0; i < n && pos < max; i++)
                buf[pos++] = ws[i];
            fmt++;
            break;
        }
        case 'S': {
            /* %S = narrow string in wide printf (MSVC: %S or %hs = char*) */
            const char *ns = (const char *)(uintptr_t)(*vp++);
            if (!ns) ns = "(null)";
            int n = 0;
            while (ns[n]) n++;
            if (precision >= 0 && n > precision) n = precision;
            for (int i = 0; i < n && pos < max; i++)
                buf[pos++] = (WCHAR)(unsigned char)ns[i];
            fmt++;
            break;
        }
        case 'c': {
            WCHAR c = (WCHAR)(*vp++);
            if (pos < max) buf[pos++] = c;
            fmt++;
            break;
        }
        case 'd': case 'i': {
            int32_t val32 = (int32_t)(*vp++);
            long long val = (long long)val32;
            int neg = 0;
            if (val < 0) { neg = 1; val = -val; }
            WCHAR tmp[24];
            int len = 0;
            if (val == 0) tmp[len++] = '0';
            else while (val > 0) { tmp[len++] = '0' + (int)(val % 10); val /= 10; }
            if (neg) tmp[len++] = '-';
            int pad = width - len;
            if (pad > 0 && zero_pad) for (int i = 0; i < pad && pos < max; i++) buf[pos++] = '0';
            else if (pad > 0) for (int i = 0; i < pad && pos < max; i++) buf[pos++] = ' ';
            for (int i = len - 1; i >= 0 && pos < max; i--) buf[pos++] = tmp[i];
            fmt++;
            break;
        }
        case 'u': {
            uint32_t val = *vp++;
            WCHAR tmp[24];
            int len = 0;
            if (val == 0) tmp[len++] = '0';
            else while (val > 0) { tmp[len++] = '0' + (int)(val % 10); val /= 10; }
            int pad = width - len;
            if (pad > 0 && zero_pad) for (int i = 0; i < pad && pos < max; i++) buf[pos++] = '0';
            else if (pad > 0) for (int i = 0; i < pad && pos < max; i++) buf[pos++] = ' ';
            for (int i = len - 1; i >= 0 && pos < max; i--) buf[pos++] = tmp[i];
            fmt++;
            break;
        }
        case 'x': case 'X': {
            int upper = (*fmt == 'X');
            uint32_t val = *vp++;
            const char *hex = upper ? "0123456789ABCDEF" : "0123456789abcdef";
            WCHAR tmp[20];
            int len = 0;
            if (val == 0) tmp[len++] = '0';
            else while (val > 0) { tmp[len++] = hex[val & 0xF]; val >>= 4; }
            int pad = width - len;
            if (pad > 0 && zero_pad) for (int i = 0; i < pad && pos < max; i++) buf[pos++] = '0';
            else if (pad > 0) for (int i = 0; i < pad && pos < max; i++) buf[pos++] = ' ';
            for (int i = len - 1; i >= 0 && pos < max; i--) buf[pos++] = tmp[i];
            fmt++;
            break;
        }
        case 'p': {
            uint32_t val = *vp++;
            const char *hex = "0123456789abcdef";
            WCHAR tmp[20];
            int len = 0;
            if (val == 0) tmp[len++] = '0';
            else while (val > 0) { tmp[len++] = hex[val & 0xF]; val >>= 4; }
            for (int i = len - 1; i >= 0 && pos < max; i--) buf[pos++] = tmp[i];
            fmt++;
            break;
        }
        case '%':
            if (pos < max) buf[pos++] = '%';
            fmt++;
            break;
        case 0:
            break;
        default:
            /* Unknown format — output literal */
            if (pos < max) buf[pos++] = '%';
            if (pos < max) buf[pos++] = *fmt;
            fmt++;
            break;
        }
    }

    buf[pos] = 0;
    return (int)pos;
}

/* _wcsicmp — case-insensitive wide string compare */
static WCHAR wchar_to_lower(WCHAR c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int wcsicmp_trace_count = 0;

int WINAPI crt_wcsicmp(const WCHAR *a, const WCHAR *b)
{
    /* Detect infinite loop: if called with same (a,b) pair 1000+ times,
     * force a match (return 0) to break the loop. This happens when the
     * engine searches FName hash table for an empty string "" — the hash
     * bucket is circular and the search never terminates. */
    {
        static uintptr_t prev_a, prev_b;
        static int repeat_count;
        if ((uintptr_t)a == prev_a && (uintptr_t)b == prev_b) {
            if (++repeat_count > 1000) return 0;
        } else {
            prev_a = (uintptr_t)a;
            prev_b = (uintptr_t)b;
            repeat_count = 0;
        }
    }

    wcsicmp_trace_count++;
    if (wcsicmp_trace_count <= 20 || (wcsicmp_trace_count % 500000) == 0) {
        serial_puts("[WCSICMP#");
        serial_putdec(wcsicmp_trace_count);
        serial_puts("] a=0x");
        serial_puthex((uint64_t)(uintptr_t)a, 8);
        serial_puts(" b=0x");
        serial_puthex((uint64_t)(uintptr_t)b, 8);
        if (a && (uintptr_t)a >= 0x1000) {
            serial_puts(" a=\"");
            for (int k = 0; k < 20 && a[k]; k++)
                serial_putchar((char)(a[k] & 0x7F));
            serial_puts("\"");
        }
        if (b && (uintptr_t)b >= 0x1000) {
            serial_puts(" b=\"");
            for (int k = 0; k < 20 && b[k]; k++)
                serial_putchar((char)(b[k] & 0x7F));
            serial_puts("\"");
        }
        serial_puts("\n");
    }
    while (*a && *b) {
        WCHAR ca = wchar_to_lower(*a), cb = wchar_to_lower(*b);
        if (ca != cb) return (int)ca - (int)cb;
        a++; b++;
    }
    return (int)wchar_to_lower(*a) - (int)wchar_to_lower(*b);
}

/* _wcsnicmp — case-insensitive wide string compare (n chars) */
int WINAPI crt_wcsnicmp(const WCHAR *a, const WCHAR *b, SIZE_T n)
{
    for (SIZE_T i = 0; i < n; i++) {
        WCHAR ca = wchar_to_lower(a[i]), cb = wchar_to_lower(b[i]);
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == 0) break;
    }
    return 0;
}

/* _wcsupr — uppercase wide string in-place */
WCHAR* WINAPI crt_wcsupr(WCHAR *s)
{
    WCHAR *p = s;
    while (*p) {
        if (*p >= 'a' && *p <= 'z') *p -= 32;
        p++;
    }
    return s;
}

/* _wtoi — wide string to int */
int WINAPI crt_wtoi(const WCHAR *s)
{
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    int val = 0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    return neg ? -val : val;
}

/* ceil / floor — math functions */
double WINAPI crt_ceil(double x)
{
    long i = (long)x;
    if (x > 0.0 && (double)i != x) return (double)(i + 1);
    return (double)i;
}

double WINAPI crt_floor(double x)
{
    long i = (long)x;
    if (x < 0.0 && (double)i != x) return (double)(i - 1);
    return (double)i;
}

double WINAPI crt_fabs(double x)
{
    union { double value; uint64_t bits; } v = { x };
    v.bits &= ~(1ULL << 63);
    return v.value;
}

double WINAPI crt_sqrt(double x)
{
    union { double value; uint64_t bits; } v = { x };
    if (x == 0.0 || (v.bits & 0x7FF0000000000000ULL) ==
                    0x7FF0000000000000ULL)
        return x;
    if (x < 0.0) {
        v.bits = 0x7FF8000000000000ULL;
        return v.value;
    }

    union { double value; uint64_t bits; } guess;
    guess.bits = (v.bits >> 1) + 0x1FF8000000000000ULL;
    for (int i = 0; i < 8; i++)
        guess.value = 0.5 * (guess.value + x / guess.value);
    return guess.value;
}

/* difftime — difference between two time_t values */
double WINAPI crt_difftime(crt_time_t t1, crt_time_t t0)
{
    return (double)(t1 - t0);
}

/* gmtime — convert time_t to struct tm (stub) */
static int crt_tm_from_unix(int64_t unix_seconds, struct crt_tm *result)
{
    WINTIME_CALENDAR calendar;
    if (!result || wintime_unix_to_calendar(unix_seconds, &calendar) < 0)
        return CRT_EINVAL;

    result->tm_sec = calendar.second;
    result->tm_min = calendar.minute;
    result->tm_hour = calendar.hour;
    result->tm_mday = calendar.day;
    result->tm_mon = calendar.month - 1;
    result->tm_year = calendar.year - 1900;
    result->tm_wday = calendar.day_of_week;
    result->tm_yday = calendar.day_of_year;
    result->tm_isdst = 0;
    return 0;
}

PVOID WINAPI crt_gmtime(const crt_time_t *timer)
{
    if (!timer) {
        *crt_errno() = CRT_EINVAL;
        return NULL;
    }
    UCRT_PROCESS_MODE_VALUES *state = ucrt_process_mode_state(TRUE);
    if (!state ||
        crt_tm_from_unix((int64_t)*timer, &state->time_buffer) != 0) {
        *crt_errno() = CRT_EINVAL;
        return NULL;
    }
    *crt_errno() = 0;
    return (PVOID)&state->time_buffer;
}

int64_t WINAPI crt_time64(int64_t *timer)
{
    int64_t result = wintime_now_unix_seconds();
    if (timer) *timer = result;
    return result;
}

int WINAPI crt_gmtime64_s(PVOID result, const int64_t *timer)
{
    if (!result || !timer) {
        *crt_errno() = CRT_EINVAL;
        return CRT_EINVAL;
    }
    int error = crt_tm_from_unix(*timer, (struct crt_tm *)result);
    *crt_errno() = error;
    return error;
}

PVOID WINAPI crt_localtime64(const int64_t *timer)
{
    if (!timer) {
        *crt_errno() = CRT_EINVAL;
        return NULL;
    }

    UCRT_PROCESS_MODE_VALUES *state = ucrt_process_mode_state(TRUE);
    if (!state || crt_tm_from_unix(*timer, &state->time_buffer) != 0) {
        *crt_errno() = CRT_EINVAL;
        return NULL;
    }
    *crt_errno() = 0;
    return (PVOID)&state->time_buffer;
}

int WINAPI crt_localtime64_s(PVOID result, const int64_t *timer)
{
    /* Local time is UTC while the system time-zone bias is zero. */
    return crt_gmtime64_s(result, timer);
}

/* mktime — convert struct tm to time_t (stub) */
typedef struct {
    char *data;
    SIZE_T capacity;
    SIZE_T length;
    BOOL overflow;
} CRT_STRFTIME_OUTPUT;

static void crt_strftime_putc(CRT_STRFTIME_OUTPUT *output, char c)
{
    if (output->overflow) return;
    if (output->length + 1 >= output->capacity) {
        output->overflow = TRUE;
        return;
    }
    output->data[output->length++] = c;
    output->data[output->length] = 0;
}

static void crt_strftime_puts(CRT_STRFTIME_OUTPUT *output, const char *text)
{
    while (*text) crt_strftime_putc(output, *text++);
}

static void crt_strftime_number(CRT_STRFTIME_OUTPUT *output,
                                unsigned int value, int width, char padding,
                                BOOL alternate)
{
    char digits[16];
    int count = 0;
    do {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value && count < (int)sizeof(digits));

    if (!alternate)
        while (count < width) {
            crt_strftime_putc(output, padding);
            width--;
        }
    while (count > 0) crt_strftime_putc(output, digits[--count]);
}

static BOOL crt_tm_is_leap_year(int year)
{
    return (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0);
}

static int crt_tm_jan1_wday(const struct crt_tm *tm)
{
    int wday = tm->tm_wday - (tm->tm_yday % 7);
    if (wday < 0) wday += 7;
    return wday;
}

static int crt_tm_iso_weeks(int year, int jan1_wday)
{
    return jan1_wday == 4 ||
           (jan1_wday == 3 && crt_tm_is_leap_year(year)) ? 53 : 52;
}

static void crt_tm_iso_week(const struct crt_tm *tm, int *iso_year,
                            int *iso_week)
{
    int year = tm->tm_year + 1900;
    int iso_wday = tm->tm_wday == 0 ? 7 : tm->tm_wday;
    int week = (tm->tm_yday + 10 - iso_wday) / 7;
    int jan1_wday = crt_tm_jan1_wday(tm);

    if (week < 1) {
        int previous_year = year - 1;
        int previous_days = crt_tm_is_leap_year(previous_year) ? 366 : 365;
        int previous_jan1 = (jan1_wday - previous_days % 7 + 7) % 7;
        year = previous_year;
        week = crt_tm_iso_weeks(year, previous_jan1);
    } else if (week > crt_tm_iso_weeks(year, jan1_wday)) {
        year++;
        week = 1;
    }

    *iso_year = year;
    *iso_week = week;
}

static void crt_strftime_format(CRT_STRFTIME_OUTPUT *output,
                                const char *format,
                                const struct crt_tm *tm)
{
    static const char *const weekdays_short[] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };
    static const char *const weekdays_long[] = {
        "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday",
        "Friday", "Saturday"
    };
    static const char *const months_short[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    static const char *const months_long[] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
    };

    while (*format && !output->overflow) {
        if (*format != '%') {
            crt_strftime_putc(output, *format++);
            continue;
        }
        format++;
        BOOL alternate = FALSE;
        if (*format == '#') {
            alternate = TRUE;
            format++;
        }
        if (*format == 'E' || *format == 'O') format++;
        char specifier = *format ? *format++ : 0;
        int year = tm->tm_year + 1900;
        int hour12 = tm->tm_hour % 12;
        if (!hour12) hour12 = 12;

        switch (specifier) {
        case '%': crt_strftime_putc(output, '%'); break;
        case 'a':
            crt_strftime_puts(output,
                tm->tm_wday >= 0 && tm->tm_wday < 7
                    ? weekdays_short[tm->tm_wday] : "???");
            break;
        case 'A':
            crt_strftime_puts(output,
                tm->tm_wday >= 0 && tm->tm_wday < 7
                    ? weekdays_long[tm->tm_wday] : "???");
            break;
        case 'b': case 'h':
            crt_strftime_puts(output,
                tm->tm_mon >= 0 && tm->tm_mon < 12
                    ? months_short[tm->tm_mon] : "???");
            break;
        case 'B':
            crt_strftime_puts(output,
                tm->tm_mon >= 0 && tm->tm_mon < 12
                    ? months_long[tm->tm_mon] : "???");
            break;
        case 'c':
            crt_strftime_format(output, "%a %b %e %H:%M:%S %Y", tm);
            break;
        case 'C':
            crt_strftime_number(output, (unsigned int)(year / 100), 2, '0',
                                alternate);
            break;
        case 'd':
            crt_strftime_number(output, (unsigned int)tm->tm_mday, 2, '0',
                                alternate);
            break;
        case 'D': crt_strftime_format(output, "%m/%d/%y", tm); break;
        case 'e':
            crt_strftime_number(output, (unsigned int)tm->tm_mday, 2, ' ',
                                alternate);
            break;
        case 'F': crt_strftime_format(output, "%Y-%m-%d", tm); break;
        case 'g': case 'G': case 'V': {
            int iso_year, iso_week;
            crt_tm_iso_week(tm, &iso_year, &iso_week);
            if (specifier == 'V')
                crt_strftime_number(output, (unsigned int)iso_week, 2, '0',
                                    alternate);
            else if (specifier == 'g')
                crt_strftime_number(output,
                    (unsigned int)((iso_year % 100 + 100) % 100), 2, '0',
                    alternate);
            else
                crt_strftime_number(output, (unsigned int)iso_year, 4, '0',
                                    alternate);
            break;
        }
        case 'H':
            crt_strftime_number(output, (unsigned int)tm->tm_hour, 2, '0',
                                alternate);
            break;
        case 'I':
            crt_strftime_number(output, (unsigned int)hour12, 2, '0',
                                alternate);
            break;
        case 'j':
            crt_strftime_number(output, (unsigned int)(tm->tm_yday + 1),
                                3, '0', alternate);
            break;
        case 'm':
            crt_strftime_number(output, (unsigned int)(tm->tm_mon + 1),
                                2, '0', alternate);
            break;
        case 'M':
            crt_strftime_number(output, (unsigned int)tm->tm_min, 2, '0',
                                alternate);
            break;
        case 'n': crt_strftime_putc(output, '\n'); break;
        case 'p': crt_strftime_puts(output, tm->tm_hour < 12 ? "AM" : "PM"); break;
        case 'r': crt_strftime_format(output, "%I:%M:%S %p", tm); break;
        case 'R': crt_strftime_format(output, "%H:%M", tm); break;
        case 'S':
            crt_strftime_number(output, (unsigned int)tm->tm_sec, 2, '0',
                                alternate);
            break;
        case 't': crt_strftime_putc(output, '\t'); break;
        case 'T': crt_strftime_format(output, "%H:%M:%S", tm); break;
        case 'u':
            crt_strftime_number(output,
                (unsigned int)(tm->tm_wday == 0 ? 7 : tm->tm_wday),
                1, '0', alternate);
            break;
        case 'U':
            crt_strftime_number(output,
                (unsigned int)((tm->tm_yday + 7 - tm->tm_wday) / 7),
                2, '0', alternate);
            break;
        case 'w':
            crt_strftime_number(output, (unsigned int)tm->tm_wday, 1, '0',
                                alternate);
            break;
        case 'W': {
            int monday_wday = (tm->tm_wday + 6) % 7;
            crt_strftime_number(output,
                (unsigned int)((tm->tm_yday + 7 - monday_wday) / 7),
                2, '0', alternate);
            break;
        }
        case 'x': crt_strftime_format(output, "%m/%d/%y", tm); break;
        case 'X': crt_strftime_format(output, "%H:%M:%S", tm); break;
        case 'y':
            crt_strftime_number(output,
                (unsigned int)((year % 100 + 100) % 100), 2, '0', alternate);
            break;
        case 'Y':
            crt_strftime_number(output, (unsigned int)year, 4, '0', alternate);
            break;
        case 'z': crt_strftime_puts(output, "+0000"); break;
        case 'Z': crt_strftime_puts(output, "UTC"); break;
        case 0:
            crt_strftime_putc(output, '%');
            break;
        default:
            crt_strftime_putc(output, '%');
            crt_strftime_putc(output, specifier);
            break;
        }
    }
}

SIZE_T WINAPI crt_strftime(char *buffer, SIZE_T max_size,
                           const char *format, PCVOID tm_ptr)
{
    if (!buffer || !max_size || !format || !tm_ptr) {
        if (buffer && max_size) buffer[0] = 0;
        *crt_errno() = CRT_EINVAL;
        return 0;
    }

    CRT_STRFTIME_OUTPUT output = { buffer, max_size, 0, FALSE };
    buffer[0] = 0;
    crt_strftime_format(&output, format, (const struct crt_tm *)tm_ptr);
    if (output.overflow) {
        buffer[0] = 0;
        *crt_errno() = CRT_ERANGE;
        return 0;
    }
    *crt_errno() = 0;
    return output.length;
}

void WINAPI crt_sleep(unsigned long milliseconds)
{
    Sleep((DWORD)milliseconds);
}

crt_time_t WINAPI crt_mktime(PVOID tm_ptr)
{
    struct crt_tm *tm = (struct crt_tm *)tm_ptr;
    if (!tm) {
        *crt_errno() = 22;
        return (crt_time_t)-1;
    }
    WINTIME_CALENDAR calendar;
    calendar.year = (uint16_t)(tm->tm_year + 1900);
    calendar.month = (uint16_t)(tm->tm_mon + 1);
    calendar.day_of_week = 0;
    calendar.day = (uint16_t)tm->tm_mday;
    calendar.hour = (uint16_t)tm->tm_hour;
    calendar.minute = (uint16_t)tm->tm_min;
    calendar.second = (uint16_t)tm->tm_sec;
    calendar.millisecond = 0;
    calendar.day_of_year = 0;
    int64_t unix_seconds;
    if (wintime_calendar_to_unix(&calendar, &unix_seconds) < 0) {
        *crt_errno() = 22;
        return (crt_time_t)-1;
    }

    WINTIME_CALENDAR normalized;
    if (wintime_unix_to_calendar(unix_seconds, &normalized) == 0) {
        tm->tm_wday = normalized.day_of_week;
        tm->tm_yday = normalized.day_of_year;
        tm->tm_isdst = 0;
    }
    *crt_errno() = 0;
    return (crt_time_t)unix_seconds;
}

int64_t WINAPI crt_mktime64(PVOID tm_ptr)
{
    return (int64_t)crt_mktime(tm_ptr);
}

/* rand / srand — simple LCG PRNG */
static unsigned int crt_rand_seed = 1;

int WINAPI crt_rand(void)
{
    crt_rand_seed = crt_rand_seed * 214013 + 2531011;
    return (int)((crt_rand_seed >> 16) & 0x7fff);
}

void WINAPI crt_srand(unsigned int seed)
{
    crt_rand_seed = seed;
}

/* strncat — string concatenation with length limit */
char* WINAPI crt_strncat(char *dst, const char *src, SIZE_T n)
{
    char *d = dst;
    while (*d) d++;
    for (SIZE_T i = 0; i < n && src[i]; i++)
        *d++ = src[i];
    *d = 0;
    return dst;
}

/* ── Wide string functions ─────────────────────────────────── */

char* WINAPI crt_strdup(const char *s)
{
    static volatile uint32_t strdup_trace_count;
    if (!s) {
        *crt_errno() = CRT_EINVAL;
        return NULL;
    }
    SIZE_T chars = crt_strlen(s) + 1;
    char *copy = (char *)crt_malloc(chars);
    if (!copy) {
        *crt_errno() = CRT_ENOMEM;
        return NULL;
    }
    for (SIZE_T i = 0; i < chars; i++) copy[i] = s[i];
    if (__sync_fetch_and_add(&strdup_trace_count, 1) < 4) {
        serial_puts("[CRT-STRDUP] -> 0x");
        serial_puthex((ULONG_PTR)copy, 16);
        serial_puts(" value='");
        serial_puts(s);
        serial_puts("'\n");
    }
    return copy;
}

SIZE_T WINAPI crt_strcspn(const char *s, const char *reject)
{
    SIZE_T length = 0;
    while (s[length]) {
        for (const char *r = reject; *r; r++)
            if (s[length] == *r) return length;
        length++;
    }
    return length;
}

char* WINAPI crt_strpbrk(const char *s, const char *accept)
{
    for (; *s; s++)
        for (const char *a = accept; *a; a++)
            if (*s == *a) return (char *)s;
    return NULL;
}

SIZE_T WINAPI crt_wcslen(const WCHAR *s)
{
    SIZE_T len = 0;
    while (s[len]) len++;
    return len;
}

SIZE_T WINAPI crt_wcsnlen(const WCHAR *s, SIZE_T max_chars)
{
    if (!s) {
        *crt_errno() = CRT_EINVAL;
        return 0;
    }
    SIZE_T len = 0;
    while (len < max_chars && s[len]) len++;
    return len;
}

WCHAR* WINAPI crt_wcsdup(const WCHAR *s)
{
    if (!s) return NULL;
    SIZE_T chars = crt_wcslen(s) + 1;
    WCHAR *copy = (WCHAR *)crt_malloc(chars * sizeof(WCHAR));
    if (!copy) return NULL;
    for (SIZE_T i = 0; i < chars; i++)
        copy[i] = s[i];
    return copy;
}

WCHAR* WINAPI crt_wcscpy(WCHAR *dst, const WCHAR *src)
{
    /* Diagnostic: catch the caller that copies the bad "0" package name.
     * Renders src as ASCII when it looks like a wstring, then logs caller
     * EIP via compat32's saved per-thunk return address. */
    if (src && ((uintptr_t)src >= 0x10000) && ((uintptr_t)src < 0x80000000ULL)) {
        const WCHAR *s = src;
        static uint32_t wcs0_log_n = 0;
        if (s[0] == L'0' && s[1] == 0 && wcs0_log_n++ < 8) {
            extern uint32_t compat32_get_last_caller_eip(void);
            extern uint32_t g_last_stack_args;
            extern void serial_puts(const char *s);
            extern void serial_puthex(uint64_t v, int d);
            uint32_t ceip = compat32_get_last_caller_eip();
            uint32_t outer = 0;
            /* stack_args[2] for a jmp-thunk wcscpy is the outer caller's
             * return address (the real user code that wanted to copy "0"). */
            if (g_last_stack_args) {
                uint32_t *sa = (uint32_t *)(uintptr_t)g_last_stack_args;
                outer = sa[2];
            }
            serial_puts("[wcscpy] src=L\"0\" dst=0x");
            serial_puthex((uint64_t)(uintptr_t)dst, 8);
            serial_puts(" inner_eip=0x");
            serial_puthex((uint64_t)ceip, 8);
            serial_puts(" outer_eip=0x");
            serial_puthex((uint64_t)outer, 8);
            serial_puts("\n");
            /* [BT-0 DIAGNOSTIC — uncommitted] First few times only, walk the
             * guest stack and print PE-code return addresses so we can identify
             * the iterator that keeps appending the bad "0" name. */
            {
                static int bt0_n = 0;
                if (bt0_n < 4 && g_last_stack_args) {
                    bt0_n++;
                    uint32_t *sp = (uint32_t *)(uintptr_t)g_last_stack_args;
                    serial_puts("[BT-0]");
                    int printed = 0;
                    for (int k = 0; k < 64 && printed < 12; k++) {
                        uint32_t v = sp[k];
                        if (v >= 0x10100000 && v < 0x11000000) {
                            serial_puts(" 0x");
                            serial_puthex((uint64_t)v, 8);
                            printed++;
                        }
                    }
                    serial_puts("\n");
                }
            }
        }
    }
    WCHAR *d = dst;
    while ((*d++ = *src++));
    return dst;
}

WCHAR* WINAPI crt_wcsncpy(WCHAR *dst, const WCHAR *src, SIZE_T n)
{
    SIZE_T i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

int WINAPI crt_wcscpy_s(WCHAR *dst, SIZE_T dst_chars, const WCHAR *src)
{
    if (!dst) {
        *crt_errno() = CRT_EINVAL;
        return CRT_EINVAL;
    }
    if (!src) {
        if (dst_chars) dst[0] = 0;
        *crt_errno() = CRT_EINVAL;
        return CRT_EINVAL;
    }
    if (!dst_chars) {
        *crt_errno() = CRT_ERANGE;
        return CRT_ERANGE;
    }

    SIZE_T src_chars = crt_wcslen(src);
    if (src_chars >= dst_chars) {
        dst[0] = 0;
        *crt_errno() = CRT_ERANGE;
        return CRT_ERANGE;
    }
    for (SIZE_T i = 0; i <= src_chars; i++) dst[i] = src[i];
    return 0;
}

int WINAPI crt_strncpy_s(char *dst, SIZE_T dst_chars,
                         const char *src, SIZE_T count)
{
    if (!dst || !dst_chars) {
        *crt_errno() = CRT_EINVAL;
        return CRT_EINVAL;
    }
    if (count == 0) {
        dst[0] = 0;
        return 0;
    }
    if (!src) {
        dst[0] = 0;
        *crt_errno() = CRT_EINVAL;
        return CRT_EINVAL;
    }

    BOOL truncate = crt_secure_truncate_count(count);
    SIZE_T src_chars = crt_strlen(src);
    SIZE_T copy_chars = truncate || src_chars < count ? src_chars : count;

    if (truncate && src_chars >= dst_chars) {
        copy_chars = dst_chars - 1;
        for (SIZE_T i = 0; i < copy_chars; i++) dst[i] = src[i];
        dst[copy_chars] = 0;
        return CRT_STRUNCATE;
    }
    if (copy_chars >= dst_chars) {
        dst[0] = 0;
        *crt_errno() = CRT_ERANGE;
        return CRT_ERANGE;
    }

    ULONG_PTR dst_begin = (ULONG_PTR)dst;
    ULONG_PTR src_begin = (ULONG_PTR)src;
    SIZE_T span = copy_chars + 1;
    if (dst_begin < src_begin + span && src_begin < dst_begin + span) {
        dst[0] = 0;
        *crt_errno() = CRT_EINVAL;
        return CRT_EINVAL;
    }

    for (SIZE_T i = 0; i < copy_chars; i++) dst[i] = src[i];
    dst[copy_chars] = 0;
    return 0;
}

int WINAPI crt_wcsncpy_s(WCHAR *dst, SIZE_T dst_chars,
                         const WCHAR *src, SIZE_T count)
{
    if (!dst || !dst_chars) {
        *crt_errno() = CRT_EINVAL;
        return CRT_EINVAL;
    }
    if (!src) {
        dst[0] = 0;
        *crt_errno() = CRT_EINVAL;
        return CRT_EINVAL;
    }

    BOOL truncate = count == (SIZE_T)-1 || count == 0xFFFFFFFFU;
    SIZE_T src_chars = crt_wcslen(src);
    SIZE_T copy_chars = src_chars < count ? src_chars : count;
    if (truncate && src_chars >= dst_chars) {
        copy_chars = dst_chars - 1;
        for (SIZE_T i = 0; i < copy_chars; i++) dst[i] = src[i];
        dst[copy_chars] = 0;
        return CRT_STRUNCATE;
    }
    if (copy_chars >= dst_chars) {
        dst[0] = 0;
        *crt_errno() = CRT_ERANGE;
        return CRT_ERANGE;
    }
    for (SIZE_T i = 0; i < copy_chars; i++) dst[i] = src[i];
    dst[copy_chars] = 0;
    return 0;
}

WCHAR* WINAPI crt_wcscat(WCHAR *dst, const WCHAR *src)
{
    WCHAR *d = dst;
    while (*d) d++;
    while ((*d++ = *src++));
    return dst;
}

int WINAPI crt_wcscat_s(WCHAR *dst, SIZE_T dst_chars, const WCHAR *src)
{
    if (!dst) {
        *crt_errno() = CRT_EINVAL;
        return CRT_EINVAL;
    }
    if (!dst_chars) {
        *crt_errno() = CRT_ERANGE;
        return CRT_ERANGE;
    }

    SIZE_T dst_len = crt_wcsnlen(dst, dst_chars);
    if (dst_len == dst_chars) {
        *crt_errno() = CRT_EINVAL;
        return CRT_EINVAL;
    }
    if (!src) {
        dst[0] = 0;
        *crt_errno() = CRT_EINVAL;
        return CRT_EINVAL;
    }

    SIZE_T src_len = crt_wcslen(src);
    if (src_len >= dst_chars - dst_len) {
        dst[0] = 0;
        *crt_errno() = CRT_ERANGE;
        return CRT_ERANGE;
    }
    for (SIZE_T i = 0; i <= src_len; i++) dst[dst_len + i] = src[i];
    return 0;
}

int WINAPI crt_wcscmp(const WCHAR *a, const WCHAR *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)*a - (int)*b;
}

int WINAPI crt_wcsncmp(const WCHAR *a, const WCHAR *b, SIZE_T n)
{
    for (SIZE_T i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)a[i] - (int)b[i];
        if (a[i] == 0) break;
    }
    return 0;
}

int WINAPI crt_wcscoll(const WCHAR *a, const WCHAR *b)
{
    /* OsitoK currently exposes the invariant C locale. */
    return crt_wcscmp(a, b);
}

SIZE_T WINAPI crt_wcsxfrm(WCHAR *dst, const WCHAR *src, SIZE_T dst_chars)
{
    if (!src) {
        *crt_errno() = CRT_EINVAL;
        return (SIZE_T)-1;
    }
    SIZE_T src_chars = crt_wcslen(src);
    if (dst && dst_chars) {
        SIZE_T copy_chars = src_chars < dst_chars ? src_chars : dst_chars;
        for (SIZE_T i = 0; i < copy_chars; i++) dst[i] = src[i];
        if (src_chars < dst_chars) dst[src_chars] = 0;
    }
    return src_chars;
}

WCHAR* WINAPI crt_wcschr(const WCHAR *s, WCHAR c)
{
    for (; *s; s++)
        if (*s == c) return (WCHAR *)s;
    return (c == 0) ? (WCHAR *)s : NULL;
}

WCHAR* WINAPI crt_wcsrchr(const WCHAR *s, WCHAR c)
{
    const WCHAR *last = NULL;
    do {
        if (*s == c)
            last = s;
    } while (*s++);
    return (WCHAR *)last;
}

WCHAR* WINAPI crt_wcsstr(const WCHAR *haystack, const WCHAR *needle)
{
    if (!*needle) return (WCHAR *)haystack;
    for (; *haystack; haystack++) {
        const WCHAR *h = haystack, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (WCHAR *)haystack;
    }
    return NULL;
}

static BOOL crt_wchar_is_delimiter(WCHAR c, const WCHAR *delimiters)
{
    for (const WCHAR *d = delimiters; *d; d++)
        if (*d == c) return TRUE;
    return FALSE;
}

WCHAR* WINAPI crt_wcstok_s(WCHAR *str, const WCHAR *delimiters,
                           WCHAR **context)
{
    if (!delimiters || !context || (!str && !*context)) {
        *crt_errno() = CRT_EINVAL;
        return NULL;
    }

    WCHAR *cursor = str ? str : *context;
    while (*cursor && crt_wchar_is_delimiter(*cursor, delimiters)) cursor++;
    if (!*cursor) {
        *context = cursor;
        return NULL;
    }

    WCHAR *token = cursor;
    while (*cursor && !crt_wchar_is_delimiter(*cursor, delimiters)) cursor++;
    if (*cursor) *cursor++ = 0;
    *context = cursor;
    return token;
}

unsigned long WINAPI crt_wcstoul(const WCHAR *s, WCHAR **endptr, int base)
{
    unsigned long result = 0;

    while (*s == ' ' || *s == '\t') s++;

    if (*s == '+') s++;

    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (s[0] == '0') { base = 8; s++; }
        else base = 10;
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    while (*s) {
        int digit;
        if (*s >= '0' && *s <= '9') digit = *s - '0';
        else if (*s >= 'a' && *s <= 'f') digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') digit = *s - 'A' + 10;
        else break;
        if (digit >= base) break;
        result = result * base + digit;
        s++;
    }

    if (endptr) *endptr = (WCHAR *)s;
    return result;
}

/* _access — check file accessibility (0=exist, 2=write, 4=read, 6=r+w) */
int WINAPI crt_access(const char *path, int mode)
{
    (void)mode;
    if (!path) return -1;
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '\\' || *p == '/') base = p + 1;
    }
    void *f = osfs2_find(base);
    if (!f && base != path) f = osfs2_find(path);
    return f ? 0 : -1;
}

int WINAPI crt_waccess(const WCHAR *path, int mode)
{
    if (!path) return -1;
    char narrow[260];
    int i = 0;
    for (; path[i] && i < 259; i++)
        narrow[i] = (char)(path[i] & 0xFF);
    narrow[i] = 0;
    return crt_access(narrow, mode);
}

/* _fltused — compiler marker for floating point usage */
static int crt_fltused_val = 0x9875;
int* WINAPI crt_fltused(void) { return &crt_fltused_val; }

/* CRT version globals (Windows NT 5.0 = Windows 2000) */
static unsigned int crt_osver_val = 2195;
static unsigned int crt_winver_val = 0x0500;
static unsigned int crt_winmajor_val = 5;
static unsigned int crt_winminor_val = 0;
unsigned int* WINAPI crt_p_osver(void) { return &crt_osver_val; }
unsigned int* WINAPI crt_p_winver(void) { return &crt_winver_val; }
unsigned int* WINAPI crt_p_winmajor(void) { return &crt_winmajor_val; }
unsigned int* WINAPI crt_p_winminor(void) { return &crt_winminor_val; }

/* ── Stubs for bundled MSVCRT.dll ────────────────────────────── */

static int crt_getch_stub(void)  { return -1; /* EOF */ }
static int crt_kbhit_stub(void)  { return 0;  /* no key pressed */ }

typedef struct _CRT_ENV_VALUE_CACHE {
    struct _CRT_ENV_VALUE_CACHE *next;
    DWORD pid;
    DWORD tid;
    char *narrow;
    SIZE_T narrow_capacity;
    WCHAR *wide;
    SIZE_T wide_capacity;
} CRT_ENV_VALUE_CACHE;

typedef struct _CRT_WENV_CACHE {
    struct _CRT_WENV_CACHE *next;
    DWORD pid;
    BOOL is_32bit;
    WCHAR *block;
    SIZE_T block_capacity;
    PVOID entries;
    SIZE_T entry_capacity;
    PVOID view_cell;
} CRT_WENV_CACHE;

static CRT_ENV_VALUE_CACHE *crt_env_values;
static CRT_WENV_CACHE *crt_wenv_cache;
static volatile uint32_t crt_env_lock;

static void crt_env_lock_acquire(void)
{
    while (__sync_lock_test_and_set(&crt_env_lock, 1)) {
        for (int spin = 0; spin < 100; spin++)
            __asm__ volatile ("pause" ::: "memory");
        sched_yield();
    }
}

static void crt_env_lock_release(void)
{
    __sync_lock_release(&crt_env_lock);
}

static CRT_ENV_VALUE_CACHE *crt_env_value_cache(void)
{
    DWORD pid = GetCurrentProcessId();
    DWORD tid = GetCurrentThreadId();

    crt_env_lock_acquire();
    for (CRT_ENV_VALUE_CACHE *cache = crt_env_values; cache;
         cache = cache->next) {
        if (cache->pid == pid && cache->tid == tid) {
            crt_env_lock_release();
            return cache;
        }
    }

    CRT_ENV_VALUE_CACHE *cache = (CRT_ENV_VALUE_CACHE *)kmalloc(sizeof(*cache));
    if (cache) {
        cache->pid = pid;
        cache->tid = tid;
        cache->narrow = NULL;
        cache->narrow_capacity = 0;
        cache->wide = NULL;
        cache->wide_capacity = 0;
        cache->next = crt_env_values;
        crt_env_values = cache;
    }
    crt_env_lock_release();
    return cache;
}

static BOOL crt_env_user_buffer(PVOID buffer, SIZE_T bytes)
{
    if (!buffer) return FALSE;
    if (!g_compat32_mode) return TRUE;

    ULONG_PTR address = (ULONG_PTR)buffer;
    return address <= UINT32_MAX && bytes - 1 <= UINT32_MAX - address;
}

char* WINAPI crt_getenv(const char *name)
{
    if (!name || !*name) {
        *crt_errno() = CRT_EINVAL;
        return NULL;
    }
    DWORD required = GetEnvironmentVariableA(name, NULL, 0);
    if (!required) return NULL;

    CRT_ENV_VALUE_CACHE *cache = crt_env_value_cache();
    if (!cache) {
        *crt_errno() = CRT_ENOMEM;
        return NULL;
    }
    if (required > cache->narrow_capacity) {
        char *buffer = (char *)crt_realloc(cache->narrow, required);
        if (!crt_env_user_buffer(buffer, required)) {
            if (buffer) crt_free(buffer);
            cache->narrow = NULL;
            cache->narrow_capacity = 0;
            *crt_errno() = CRT_ENOMEM;
            return NULL;
        }
        cache->narrow = buffer;
        cache->narrow_capacity = required;
    }
    GetEnvironmentVariableA(name, cache->narrow,
                            (DWORD)cache->narrow_capacity);
    *crt_errno() = 0;
    return cache->narrow;
}

WCHAR* WINAPI crt_wgetenv(const WCHAR *name)
{
    if (!name || !*name) {
        *crt_errno() = CRT_EINVAL;
        return NULL;
    }
    DWORD required = GetEnvironmentVariableW(name, NULL, 0);
    if (!required) return NULL;

    CRT_ENV_VALUE_CACHE *cache = crt_env_value_cache();
    if (!cache) {
        *crt_errno() = CRT_ENOMEM;
        return NULL;
    }
    SIZE_T bytes = (SIZE_T)required * sizeof(WCHAR);
    if (required > cache->wide_capacity) {
        WCHAR *buffer = (WCHAR *)crt_realloc(cache->wide, bytes);
        if (!crt_env_user_buffer(buffer, bytes)) {
            if (buffer) crt_free(buffer);
            cache->wide = NULL;
            cache->wide_capacity = 0;
            *crt_errno() = CRT_ENOMEM;
            return NULL;
        }
        cache->wide = buffer;
        cache->wide_capacity = required;
    }
    GetEnvironmentVariableW(name, cache->wide,
                            (DWORD)cache->wide_capacity);
    *crt_errno() = 0;
    return cache->wide;
}

int WINAPI crt_putenv_s(const char *name, const char *value)
{
    if (!name || !*name || !value) {
        *crt_errno() = CRT_EINVAL;
        return CRT_EINVAL;
    }
    if (!SetEnvironmentVariableA(name, *value ? value : NULL)) {
        int error = crt_errno_from_last_error();
        *crt_errno() = error;
        return error;
    }
    *crt_errno() = 0;
    return 0;
}

int WINAPI crt_wputenv_s(const WCHAR *name, const WCHAR *value)
{
    if (!name || !*name || !value) {
        *crt_errno() = CRT_EINVAL;
        return CRT_EINVAL;
    }
    if (!SetEnvironmentVariableW(name, *value ? value : NULL)) {
        int error = crt_errno_from_last_error();
        *crt_errno() = error;
        return error;
    }
    *crt_errno() = 0;
    return 0;
}

int WINAPI crt_putenv(const char *assignment)
{
    if (!assignment) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }
    SIZE_T separator = assignment[0] == '=' ? 1 : 0;
    while (assignment[separator] && assignment[separator] != '=') separator++;
    if (!separator || !assignment[separator] || separator >= 64) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }
    char name[64];
    for (SIZE_T i = 0; i < separator; i++) name[i] = assignment[i];
    name[separator] = 0;
    return crt_putenv_s(name, assignment + separator + 1) ? -1 : 0;
}

int WINAPI crt_wputenv(const WCHAR *assignment)
{
    if (!assignment) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }
    SIZE_T separator = assignment[0] == '=' ? 1 : 0;
    while (assignment[separator] && assignment[separator] != '=') separator++;
    if (!separator || !assignment[separator] || separator >= 64) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }
    WCHAR name[64];
    for (SIZE_T i = 0; i < separator; i++) name[i] = assignment[i];
    name[separator] = 0;
    return crt_wputenv_s(name, assignment + separator + 1) ? -1 : 0;
}

WCHAR*** WINAPI crt_p_wenviron(void)
{
    DWORD pid = GetCurrentProcessId();
    CRT_WENV_CACHE *cache = NULL;
    BOOL is_32bit = g_compat32_mode;

    crt_env_lock_acquire();
    for (CRT_WENV_CACHE *candidate = crt_wenv_cache; candidate;
         candidate = candidate->next) {
        if (candidate->pid == pid) {
            cache = candidate;
            break;
        }
    }
    if (!cache) {
        cache = (CRT_WENV_CACHE *)kmalloc(sizeof(*cache));
        if (!cache) {
            crt_env_lock_release();
            *crt_errno() = CRT_ENOMEM;
            return NULL;
        }
        cache->pid = pid;
        cache->is_32bit = is_32bit;
        cache->block = NULL;
        cache->block_capacity = 0;
        cache->entries = NULL;
        cache->entry_capacity = 0;
        cache->view_cell = NULL;
        cache->next = crt_wenv_cache;
        crt_wenv_cache = cache;
    }

    SIZE_T chars = kernel32_build_environment_block_w(pid, NULL, 0);
    if (chars < 2) chars = 2;
    SIZE_T entries_capacity = chars + 1;
    if (chars > cache->block_capacity) {
        WCHAR *block = (WCHAR *)crt_realloc(cache->block,
                                             chars * sizeof(WCHAR));
        if (!crt_env_user_buffer(block, chars * sizeof(WCHAR))) {
            if (block) crt_free(block);
            cache->block = NULL;
            cache->block_capacity = 0;
            crt_env_lock_release();
            *crt_errno() = CRT_ENOMEM;
            return NULL;
        }
        cache->block = block;
        cache->block_capacity = chars;
    }
    if (entries_capacity > cache->entry_capacity) {
        SIZE_T entry_size = is_32bit ? sizeof(uint32_t) : sizeof(WCHAR *);
        SIZE_T entries_bytes = entries_capacity * entry_size;
        PVOID entries = crt_realloc(cache->entries, entries_bytes);
        if (!crt_env_user_buffer(entries, entries_bytes)) {
            if (entries) crt_free(entries);
            cache->entries = NULL;
            cache->entry_capacity = 0;
            crt_env_lock_release();
            *crt_errno() = CRT_ENOMEM;
            return NULL;
        }
        cache->entries = entries;
        cache->entry_capacity = entries_capacity;
    }
    if (!cache->view_cell) {
        SIZE_T cell_size = is_32bit ? sizeof(uint32_t) : sizeof(WCHAR **);
        cache->view_cell = crt_malloc(cell_size);
        if (!crt_env_user_buffer(cache->view_cell, cell_size)) {
            if (cache->view_cell) crt_free(cache->view_cell);
            cache->view_cell = NULL;
            crt_env_lock_release();
            *crt_errno() = CRT_ENOMEM;
            return NULL;
        }
    }

    kernel32_build_environment_block_w(pid, cache->block,
                                        cache->block_capacity);
    SIZE_T offset = 0;
    SIZE_T count = 0;
    while (offset < chars && cache->block[offset]) {
        if (is_32bit)
            ((uint32_t *)cache->entries)[count++] =
                (uint32_t)(ULONG_PTR)(cache->block + offset);
        else
            ((WCHAR **)cache->entries)[count++] = cache->block + offset;
        while (offset < chars && cache->block[offset]) offset++;
        offset++;
    }
    if (is_32bit) {
        ((uint32_t *)cache->entries)[count] = 0;
        *(uint32_t *)cache->view_cell = (uint32_t)(ULONG_PTR)cache->entries;
    } else {
        ((WCHAR **)cache->entries)[count] = NULL;
        *(WCHAR ***)cache->view_cell = (WCHAR **)cache->entries;
    }
    WCHAR ***result = (WCHAR ***)cache->view_cell;
    crt_env_lock_release();
    *crt_errno() = 0;
    return result;
}

static void crt_env_release_process(DWORD process_id)
{
    CRT_ENV_VALUE_CACHE *released_values = NULL;
    CRT_WENV_CACHE *released_wenv = NULL;

    crt_env_lock_acquire();
    CRT_ENV_VALUE_CACHE **value_link = &crt_env_values;
    while (*value_link) {
        CRT_ENV_VALUE_CACHE *cache = *value_link;
        if (cache->pid != process_id) {
            value_link = &cache->next;
            continue;
        }
        *value_link = cache->next;
        cache->next = released_values;
        released_values = cache;
    }

    CRT_WENV_CACHE **wenv_link = &crt_wenv_cache;
    while (*wenv_link) {
        CRT_WENV_CACHE *cache = *wenv_link;
        if (cache->pid != process_id) {
            wenv_link = &cache->next;
            continue;
        }
        *wenv_link = cache->next;
        cache->next = released_wenv;
        released_wenv = cache;
    }
    crt_env_lock_release();

    /* User buffers belong to the process VM and are released with it. Only
     * the kernel-side cache metadata needs explicit reclamation here. */
    while (released_values) {
        CRT_ENV_VALUE_CACHE *next = released_values->next;
        kfree(released_values);
        released_values = next;
    }
    while (released_wenv) {
        CRT_WENV_CACHE *next = released_wenv->next;
        kfree(released_wenv);
        released_wenv = next;
    }
}

char* WINAPI crt_getcwd(char *buffer, int max_length)
{
    if (max_length <= 0) {
        *crt_errno() = CRT_EINVAL;
        return NULL;
    }
    BOOL allocated = buffer == NULL;
    if (allocated) buffer = (char *)crt_malloc((SIZE_T)max_length);
    if (!buffer) {
        *crt_errno() = CRT_ENOMEM;
        return NULL;
    }
    DWORD length = GetCurrentDirectoryA((DWORD)max_length, buffer);
    if (!length || length >= (DWORD)max_length) {
        if (allocated) crt_free(buffer);
        *crt_errno() = CRT_ERANGE;
        return NULL;
    }
    *crt_errno() = 0;
    return buffer;
}

WCHAR* WINAPI crt_wgetcwd(WCHAR *buffer, int max_length)
{
    if (max_length <= 0) {
        *crt_errno() = CRT_EINVAL;
        return NULL;
    }
    BOOL allocated = buffer == NULL;
    if (allocated)
        buffer = (WCHAR *)crt_malloc((SIZE_T)max_length * sizeof(WCHAR));
    if (!buffer) {
        *crt_errno() = CRT_ENOMEM;
        return NULL;
    }
    DWORD length = GetCurrentDirectoryW((DWORD)max_length, buffer);
    if (!length || length >= (DWORD)max_length) {
        if (allocated) crt_free(buffer);
        *crt_errno() = CRT_ERANGE;
        return NULL;
    }
    *crt_errno() = 0;
    return buffer;
}

/* Path resolution */

char* WINAPI crt_fullpath(char *absolute, const char *relative,
                          SIZE_T max_length)
{
    char resolved[260];
    if (!relative) {
        *crt_errno() = CRT_EINVAL;
        return NULL;
    }

    DWORD length = GetFullPathNameA(relative, sizeof(resolved), resolved, NULL);
    if (!length || length >= sizeof(resolved)) {
        *crt_errno() = length ? CRT_ENAMETOOLONG : CRT_EINVAL;
        return NULL;
    }

    BOOL allocated = absolute == NULL;
    if (!allocated && (max_length == 0 || (SIZE_T)length >= max_length)) {
        *crt_errno() = max_length ? CRT_ERANGE : CRT_EINVAL;
        return NULL;
    }
    if (allocated) {
        absolute = (char *)crt_malloc((SIZE_T)length + 1);
        if (!absolute) {
            *crt_errno() = CRT_ENOMEM;
            return NULL;
        }
    }

    memcpy(absolute, resolved, (SIZE_T)length + 1);
    *crt_errno() = 0;
    return absolute;
}

WCHAR* WINAPI crt_wfullpath(WCHAR *absolute, const WCHAR *relative,
                            SIZE_T max_length)
{
    WCHAR resolved[260];
    if (!relative) {
        *crt_errno() = CRT_EINVAL;
        return NULL;
    }

    DWORD length = GetFullPathNameW(relative, 260, resolved, NULL);
    if (!length || length >= 260) {
        *crt_errno() = length ? CRT_ENAMETOOLONG : CRT_EINVAL;
        return NULL;
    }

    BOOL allocated = absolute == NULL;
    if (!allocated && (max_length == 0 || (SIZE_T)length >= max_length)) {
        *crt_errno() = max_length ? CRT_ERANGE : CRT_EINVAL;
        return NULL;
    }
    if (allocated) {
        absolute = (WCHAR *)crt_malloc(((SIZE_T)length + 1) * sizeof(WCHAR));
        if (!absolute) {
            *crt_errno() = CRT_ENOMEM;
            return NULL;
        }
    }

    memcpy(absolute, resolved, ((SIZE_T)length + 1) * sizeof(WCHAR));
    *crt_errno() = 0;
    return absolute;
}

/* ── Base SEH handler (catch-all for unhandled exceptions) ── */

/*
 * Installed as the bottom-most SEH frame before WinMain.
 * When all inner handlers have been corrupted/popped, this
 * handler catches the exception and returns EXECUTE_HANDLER
 * so the SEH dispatcher does global unwind + handler execution.
 *
 * For C++ exceptions (0xE06D7363): return CONTINUE_SEARCH (0)
 * because we can't properly unwind C++ catch blocks.
 * For access violations: return CONTINUE_SEARCH so the kernel
 * NULL-CALL handler catches it.
 *
 * The key benefit: this frame's PRESENCE in the chain ensures
 * the chain always has a valid stack-based entry, even when
 * WinDrv.dll's stack corruption overwrites inner frames.
 */
static uint64_t WINAPI crt_base_seh_handler(
    uint64_t pExceptionRecord, uint64_t pEstablisherFrame,
    uint64_t pContextRecord, uint64_t pDispatcherContext)
{
    (void)pEstablisherFrame;
    (void)pContextRecord;
    (void)pDispatcherContext;
    uint32_t code = 0;
    if (pExceptionRecord)
        code = *(uint32_t *)(uintptr_t)pExceptionRecord;
    serial_puts("[SEH-BASE] handler called, code=0x");
    serial_puthex(code, 8);
    serial_puts("\n");
    /* Do not absorb access violations. Returning ContinueExecution here
     * retries the same faulting EIP, which turns NULL calls into #PF loops. */
    return 1; /* ExceptionContinueSearch */
}

static uint32_t g_base_seh_thunk = 0;

void crt_install_base_seh_thunk(void)
{
    extern uint32_t compat32_make_thunk(uint64_t target, const char *name,
                                         uint8_t num_args);
    g_base_seh_thunk = compat32_make_thunk(
        (uint64_t)(uintptr_t)crt_base_seh_handler,
        "__base_seh_handler", 4);
    if (g_base_seh_thunk) {
        serial_puts("[CRT] Base SEH handler thunk at 0x");
        serial_puthex(g_base_seh_thunk, 8);
        serial_puts("\n");
    }
}

uint32_t crt_get_base_seh_thunk(void) { return g_base_seh_thunk; }

/* ── Export resolution table ───────────────────────────────── */

typedef struct {
    const char *name;
    PVOID       func;
    uint8_t     argc;
    uint8_t     cc;
} MSVCRT_EXPORT;

static const MSVCRT_EXPORT msvcrt_exports[] = {
    /* CRT init */
    { "_initterm",           (PVOID)_initterm,        2, CC_CDECL },
    { "_initterm_e",         (PVOID)_initterm_e,      2, CC_CDECL },
    { "__getmainargs",       (PVOID)__getmainargs,    5, CC_CDECL },
    { "__wgetmainargs",      (PVOID)__wgetmainargs,   5, CC_CDECL },
    { "__crtGetShowWindowMode", (PVOID)__crtGetShowWindowMode, 0, CC_CDECL },
    { "__crtSetUnhandledExceptionFilter",
                            (PVOID)SetUnhandledExceptionFilter, 1, CC_CDECL },
    { "__set_app_type",      (PVOID)__set_app_type,   1, CC_CDECL },
    { "_set_app_type",       (PVOID)__set_app_type,   1, CC_CDECL },
    { "_set_fmode",          (PVOID)crt_set_fmode,    1, CC_CDECL },
    { "_get_fmode",          (PVOID)crt_get_fmode,    1, CC_CDECL },
    { "_set_new_mode",       (PVOID)_set_new_mode,    1, CC_CDECL },
    { "?_set_new_mode@@YAHH@Z", (PVOID)_set_new_mode, 1, CC_CDECL },
    { "?_set_new_handler@@YAP6AHI@ZP6AHI@Z@Z",
                              (PVOID)crt_set_new_handler, 1, CC_CDECL },

    /* Memory */
    { "malloc",              (PVOID)crt_malloc,       1, CC_CDECL },
    { "_malloc_crt",         (PVOID)crt_malloc,       1, CC_CDECL },
    { "calloc",              (PVOID)crt_calloc,       2, CC_CDECL },
    { "realloc",             (PVOID)crt_realloc,      2, CC_CDECL },
    { "free",                (PVOID)crt_free,         1, CC_CDECL },
    { "_msize",              (PVOID)crt_msize,        1, CC_CDECL },
    { "?malloc@@YAPEAX_K@Z", (PVOID)crt_malloc,       1, CC_CDECL },  /* C++ mangled (64-bit) */
    { "?free@@YAXPEAX@Z",   (PVOID)crt_free,          1, CC_CDECL },
    /* MSVC 32-bit operator new/delete — used by C++ code via MSVCRT */
    { "??2@YAPAXI@Z",       (PVOID)crt_malloc,        1, CC_CDECL },  /* operator new(unsigned int) */
    { "??3@YAXPAX@Z",       (PVOID)crt_free,          1, CC_CDECL },  /* operator delete(void*) */
    { "??_U@YAPAXI@Z",      (PVOID)crt_malloc,        1, CC_CDECL },  /* operator new[](unsigned int) */
    { "??_V@YAXPAX@Z",      (PVOID)crt_free,          1, CC_CDECL },  /* operator delete[](void*) */

    /* String */
    { "strlen",              (PVOID)crt_strlen,       1, CC_CDECL },
    { "strcmp",              (PVOID)crt_strcmp,       2, CC_CDECL },
    { "strncmp",             (PVOID)crt_strncmp,      3, CC_CDECL },
    { "_stricmp",            (PVOID)crt_stricmp,      2, CC_CDECL },
    { "_strnicmp",           (PVOID)crt_strnicmp,     3, CC_CDECL },
    { "_strcmpi",            (PVOID)crt_stricmp,      2, CC_CDECL },
    { "strcpy",              (PVOID)crt_strcpy,       2, CC_CDECL },
    { "strncpy",             (PVOID)crt_strncpy,      3, CC_CDECL },
    { "strncpy_s",           (PVOID)crt_strncpy_s,    4, CC_CDECL },
    { "strcat",              (PVOID)crt_strcat,       2, CC_CDECL },
    { "strstr",              (PVOID)crt_strstr,       2, CC_CDECL },
    { "strchr",              (PVOID)crt_strchr,       2, CC_CDECL },
    { "strrchr",             (PVOID)crt_strrchr,      2, CC_CDECL },
    { "_strdup",             (PVOID)crt_strdup,       1, CC_CDECL },
    { "strcspn",             (PVOID)crt_strcspn,      2, CC_CDECL },
    { "strpbrk",             (PVOID)crt_strpbrk,      2, CC_CDECL },
    { "strtok_s",            (PVOID)crt_strtok_s,     3, CC_CDECL },

    /* Memory ops */
    { "memcpy",              (PVOID)crt_memcpy,       3, CC_CDECL },
    { "memset",              (PVOID)crt_memset,       3, CC_CDECL },
    { "memmove",             (PVOID)crt_memmove,      3, CC_CDECL },
    { "memcmp",              (PVOID)crt_memcmp,       3, CC_CDECL },
    { "memchr",              (PVOID)crt_memchr,       3, CC_CDECL },

    /* Format I/O (variadic → fixed-param count) */
    { "printf",              (PVOID)crt_printf,       1, CC_CDECL | CC_VARIADIC },
    { "sprintf",             (PVOID)crt_sprintf,      2, CC_CDECL | CC_VARIADIC },
    { "_snprintf",           (PVOID)crt_snprintf,     3, CC_CDECL | CC_VARIADIC },
    { "_snprintf_s",         (PVOID)crt_snprintf_s,   4, CC_CDECL | CC_VARIADIC },
    { "_vsnprintf",          (PVOID)crt_vsnprintf,    4, CC_CDECL },
    { "fprintf",             (PVOID)crt_fprintf,      2, CC_CDECL | CC_VARIADIC },
    { "vprintf",             (PVOID)crt_vprintf,      2, CC_CDECL },
    { "vsprintf",            (PVOID)crt_vsprintf,     3, CC_CDECL },
    { "vfprintf",            (PVOID)crt_vfprintf,     3, CC_CDECL },
    { "__stdio_common_vsprintf",
                            (PVOID)crt_stdio_common_vsprintf, 6, CC_CDECL },
    { "__stdio_common_vswprintf",
                            (PVOID)crt_stdio_common_vswprintf, 6, CC_CDECL },
    { "sscanf",              (PVOID)crt_sscanf,       2, CC_CDECL | CC_VARIADIC },
    { "sscanf_s",            (PVOID)crt_sscanf,       2, CC_CDECL | CC_VARIADIC },
    { "puts",                (PVOID)crt_puts,         1, CC_CDECL },
    { "putchar",             (PVOID)crt_putchar,      1, CC_CDECL },

    /* stdio FILE* */
    { "fopen",               (PVOID)crt_fopen,        2, CC_CDECL },
    { "_wfopen",             (PVOID)crt_wfopen,       2, CC_CDECL },
    { "fread",               (PVOID)crt_fread,        4, CC_CDECL },
    { "fwrite",              (PVOID)crt_fwrite,       4, CC_CDECL },
    { "fclose",              (PVOID)crt_fclose,       1, CC_CDECL },
    { "fseek",               (PVOID)crt_fseek,        3, CC_CDECL },
    { "ftell",               (PVOID)crt_ftell,        1, CC_CDECL },
    { "fflush",              (PVOID)crt_fflush,       1, CC_CDECL },
    { "feof",                (PVOID)crt_feof,         1, CC_CDECL },
    { "ferror",              (PVOID)crt_ferror,       1, CC_CDECL },
    { "fgetc",               (PVOID)crt_fgetc,        1, CC_CDECL },
    { "getc",                (PVOID)crt_fgetc,        1, CC_CDECL },
    { "fputc",               (PVOID)crt_fputc,        2, CC_CDECL },
    { "fgets",               (PVOID)crt_fgets,        3, CC_CDECL },
    { "fputs",               (PVOID)crt_fputs,        2, CC_CDECL },
    { "ungetc",              (PVOID)crt_ungetc,       2, CC_CDECL },
    { "clearerr",            (PVOID)crt_clearerr,     1, CC_CDECL },
    { "rewind",              (PVOID)crt_rewind,       1, CC_CDECL },
    { "setvbuf",             (PVOID)crt_setvbuf,      4, CC_CDECL },
    { "_fileno",             (PVOID)crt_fileno,       1, CC_CDECL },
    { "fileno",              (PVOID)crt_fileno,       1, CC_CDECL },
    { "__iob_func",          (PVOID)crt_iob_func,     0, CC_CDECL },
    { "__acrt_iob_func",     (PVOID)crt_acrt_iob_func, 1, CC_CDECL },

    /* Low-level UCRT descriptors. */
    { "_get_osfhandle",      (PVOID)crt_get_osfhandle, 1, CC_CDECL },
    { "_open_osfhandle",     (PVOID)crt_open_osfhandle, 2, CC_CDECL },
    { "_open",               (PVOID)crt_open,          3, CC_CDECL },
    { "?_open@@YAHPBDHH@Z",  (PVOID)crt_open,          3, CC_CDECL },
    { "_wopen",              (PVOID)crt_wopen,         3, CC_CDECL },
    { "_close",              (PVOID)crt_close,         1, CC_CDECL },
    { "_read",               (PVOID)crt_read,          3, CC_CDECL },
    { "_write",              (PVOID)crt_write,         3, CC_CDECL },
    { "_lseek",              (PVOID)crt_lseek,         3, CC_CDECL },
    { "_lseeki64",           (PVOID)crt_lseeki64,      4, CC_CDECL },
    { "_dup",                (PVOID)crt_dup,           1, CC_CDECL },
    { "_dup2",               (PVOID)crt_dup2,          2, CC_CDECL },
    { "_commit",             (PVOID)crt_commit,        1, CC_CDECL },
    { "_isatty",             (PVOID)crt_isatty,        1, CC_CDECL },
    { "_setmode",            (PVOID)crt_setmode,       2, CC_CDECL },
    { "_chsize_s",           (PVOID)crt_chsize_s,      2, CC_CDECL },

    /* Conversion */
    { "atoi",                (PVOID)crt_atoi,         1, CC_CDECL },
    { "atol",                (PVOID)crt_atol,         1, CC_CDECL },
    { "atof",                (PVOID)crt_atof,         1, CC_CDECL },
    { "abs",                 (PVOID)crt_abs,          1, CC_CDECL },
    { "strtol",              (PVOID)crt_strtol,       3, CC_CDECL },
    { "strtoul",             (PVOID)crt_strtoul,      3, CC_CDECL },

    /* Process */
    { "exit",                (PVOID)crt_exit,         1, CC_CDECL },
    { "abort",               (PVOID)crt_abort,        0, CC_CDECL },
    { "_exit",               (PVOID)crt__exit,        1, CC_CDECL },
    { "_cexit",              (PVOID)crt_exit,         1, CC_CDECL },
    { "_c_exit",             (PVOID)crt__exit,        1, CC_CDECL },
    { "_getpid",             (PVOID)crt_getpid,       0, CC_CDECL },
    { "_beginthreadex",      (PVOID)crt_beginthreadex, 6, CC_CDECL },
    { "_endthreadex",        (PVOID)crt_endthreadex,   1, CC_CDECL },
    { "atexit",              (PVOID)crt_atexit,       1, CC_CDECL },
    { "_crt_atexit",         (PVOID)crt_atexit,       1, CC_CDECL },
    { "_configure_narrow_argv",
                            (PVOID)crt_configure_narrow_argv, 1, CC_CDECL },
    { "_initialize_narrow_environment",
                            (PVOID)crt_initialize_narrow_environment, 0, CC_CDECL },
    { "_get_narrow_winmain_command_line",
                            (PVOID)crt_get_narrow_winmain_command_line,
                            0, CC_CDECL },
    { "getenv",              (PVOID)crt_getenv,       1, CC_CDECL },
    { "_wgetenv",            (PVOID)crt_wgetenv,      1, CC_CDECL },
    { "_putenv",             (PVOID)crt_putenv,       1, CC_CDECL },
    { "_wputenv",            (PVOID)crt_wputenv,      1, CC_CDECL },
    { "_putenv_s",           (PVOID)crt_putenv_s,     2, CC_CDECL },
    { "_wputenv_s",          (PVOID)crt_wputenv_s,    2, CC_CDECL },
    { "__p__wenviron",       (PVOID)crt_p_wenviron,   0, CC_CDECL },
    { "_initialize_onexit_table",
                            (PVOID)crt_initialize_onexit_table, 1, CC_CDECL },
    { "_register_onexit_function",
                            (PVOID)crt_register_onexit_function, 2, CC_CDECL },
    { "_execute_onexit_table",
                            (PVOID)crt_execute_onexit_table, 1, CC_CDECL },
    { "_set_thread_local_invalid_parameter_handler",
                            (PVOID)crt_set_thread_local_invalid_parameter_handler,
                            1, CC_CDECL },

    /* Locale */
    { "setlocale",           (PVOID)crt_setlocale,    2, CC_CDECL },
    { "_wsetlocale",         (PVOID)crt_wsetlocale,   2, CC_CDECL },
    { "localeconv",          (PVOID)crt_localeconv,   0, CC_CDECL },

    /* ctype */
    { "isalpha",             (PVOID)crt_isalpha,      1, CC_CDECL },
    { "isdigit",             (PVOID)crt_isdigit,      1, CC_CDECL },
    { "isalnum",             (PVOID)crt_isalnum,      1, CC_CDECL },
    { "isspace",             (PVOID)crt_isspace,      1, CC_CDECL },
    { "isupper",             (PVOID)crt_isupper,      1, CC_CDECL },
    { "islower",             (PVOID)crt_islower,      1, CC_CDECL },
    { "isprint",             (PVOID)crt_isprint,      1, CC_CDECL },
    { "toupper",             (PVOID)crt_toupper,      1, CC_CDECL },
    { "tolower",             (PVOID)crt_tolower,      1, CC_CDECL },
    { "iswctype",            (PVOID)crt_iswctype,     2, CC_CDECL },
    { "_iswctype",           (PVOID)crt_iswctype,     2, CC_CDECL },
    { "iswalpha",            (PVOID)crt_iswalpha,     1, CC_CDECL },
    { "iswalnum",            (PVOID)crt_iswalnum,     1, CC_CDECL },
    { "iswdigit",            (PVOID)crt_iswdigit,     1, CC_CDECL },
    { "iswspace",            (PVOID)crt_iswspace,     1, CC_CDECL },
    { "iswupper",            (PVOID)crt_iswupper,     1, CC_CDECL },
    { "iswlower",            (PVOID)crt_iswlower,     1, CC_CDECL },
    { "iswprint",            (PVOID)crt_iswprint,     1, CC_CDECL },
    { "towupper",            (PVOID)crt_towupper,     1, CC_CDECL },
    { "towlower",            (PVOID)crt_towlower,     1, CC_CDECL },

    /* Algorithm */
    { "qsort",               (PVOID)crt_qsort,        4, CC_CDECL },
    { "bsearch",             (PVOID)crt_bsearch,      5, CC_CDECL },

    /* Error */
    { "_errno",              (PVOID)crt_errno,        0, CC_CDECL },
    { "__doserrno",          (PVOID)crt_doserrno,     0, CC_CDECL },
    { "__sys_errlist",       (PVOID)crt_sys_errlist,  0, CC_CDECL },
    { "__sys_nerr",          (PVOID)crt_sys_nerr,     0, CC_CDECL },
    { "strerror",            (PVOID)crt_strerror,     1, CC_CDECL },
    { "__fpe_flt_rounds",    (PVOID)crt_fpe_flt_rounds, 0, CC_CDECL },

    /* Time */
    { "time",                (PVOID)crt_time,         1, CC_CDECL },
    { "_time64",             (PVOID)crt_time64,       1, CC_CDECL },
    { "clock",               (PVOID)crt_clock,        0, CC_CDECL },
    { "_tzset",              (PVOID)crt_tzset,        0, CC_CDECL },
    { "__timezone",          (PVOID)crt_timezone,     0, CC_CDECL },
    { "__daylight",          (PVOID)crt_daylight,     0, CC_CDECL },
    { "_gmtime64_s",         (PVOID)crt_gmtime64_s,   2, CC_CDECL },
    { "_localtime64",        (PVOID)crt_localtime64,  1, CC_CDECL },
    { "_localtime64_s",      (PVOID)crt_localtime64_s, 2, CC_CDECL },
    { "_mktime64",           (PVOID)crt_mktime64,     1, CC_CDECL },
    { "strftime",            (PVOID)crt_strftime,     4, CC_CDECL },
    { "_sleep",              (PVOID)crt_sleep,        1, CC_CDECL },

    /* SEH */
    { "_except_handler3",    (PVOID)crt_except_handler3, 4, CC_CDECL },
    { "_except_handler4",    (PVOID)crt_except_handler4, 4, CC_CDECL },
    { "_except_handler4_common", (PVOID)crt_except_handler4_common,
                                                        6, CC_CDECL },
    { "_XcptFilter",         (PVOID)crt_XcptFilter,   2, CC_CDECL },
    { "_setjmp",             (PVOID)crt_compat32_setjmp_marker,
                                                        1, CC_CDECL },
    { "_setjmp3",            (PVOID)crt_compat32_setjmp3_marker,
                                                        2, CC_CDECL },
    { "longjmp",             (PVOID)crt_compat32_longjmp_marker,
                                                        2, CC_CDECL },
    { "_longjmpex",          (PVOID)crt_compat32_longjmp_marker,
                                                        2, CC_CDECL },

    /* Misc CRT internal */
    { "_controlfp_s",        (PVOID)crt_controlfp_s,  3, CC_CDECL },
    { "_configthreadlocale", (PVOID)crt_configthreadlocale, 1, CC_CDECL },
    { "_lock",               (PVOID)crt_lock,         1, CC_CDECL },
    { "_unlock",             (PVOID)crt_unlock,       1, CC_CDECL },
    { "__CxxFrameHandler3",  (PVOID)crt_CxxFrameHandler, 4, CC_CDECL },
    { "__CxxFrameHandler4",  (PVOID)crt_except_handler4, 4, CC_CDECL },
    { "_CRT_DEBUGGER_HOOK",  (PVOID)crt_crt_debugger_hook, 1, CC_CDECL },
    { "_crt_debugger_hook",  (PVOID)crt_crt_debugger_hook, 1, CC_CDECL },
    { "_encoded_null",       (PVOID)crt_encoded_null, 0, CC_CDECL },
    { "_amsg_exit",          (PVOID)crt_amsg_exit,    1, CC_CDECL },

    /* C++ EH / UT99 required stubs */
    { "??1type_info@@UAE@XZ", (PVOID)crt_type_info_dtor, 0, CC_THISCALL },
    { "__std_type_info_destroy_list", (PVOID)crt_std_type_info_destroy_list, 1, CC_CDECL },
    { "_CxxThrowException",  (PVOID)crt_CxxThrowException, 2, CC_CDECL },
    { "__CxxFrameHandler",   (PVOID)crt_CxxFrameHandler, 4, CC_CDECL },
    { "__dllonexit",         (PVOID)crt_dllonexit,    3, CC_CDECL },
    { "__p__commode",        (PVOID)crt_p_commode,    0, CC_CDECL },
    { "__p__fmode",          (PVOID)crt_p_fmode,      0, CC_CDECL },
    { "__C_specific_handler",(PVOID)crt_C_specific_handler, 4, CC_CDECL },
    WX_DATA("__initenv",      &crt_initenv_val),
    { "signal",              (PVOID)crt_signal,       2, CC_CDECL },
    { "__setusermatherr",    (PVOID)crt_setusermatherr, 1, CC_CDECL },
    WX_DATA("_acmdln",        &crt_acmdln_val),
    { "_adjust_fdiv",        (PVOID)crt_adjust_fdiv,  0, CC_CDECL },
    { "_controlfp",          (PVOID)crt_controlfp,    2, CC_CDECL },
    { "_ftol",               (PVOID)crt_ftol,         2, CC_CDECL },  /* double = 2 DWORDs */
    { "_onexit",             (PVOID)crt_onexit,       1, CC_CDECL },
    { "_purecall",           (PVOID)crt_purecall,     0, CC_CDECL },

    /* UT99 Core.dll / Engine.dll required exports */
    { "?terminate@@YAXXZ",   (PVOID)crt_terminate,    0, CC_CDECL },
    { "_CIacos",             (PVOID)crt_CIacos,       2, CC_CDECL },  /* double = 2 DWORDs */
    { "_CIfmod",             (PVOID)crt_CIfmod,       4, CC_CDECL },  /* 2 doubles = 4 DWORDs */
    { "_CIpow",              (PVOID)crt_CIpow,        4, CC_CDECL },  /* 2 doubles = 4 DWORDs */
    { "_isnan",              (PVOID)crt_isnan,        2, CC_CDECL },  /* double = 2 DWORDs */
    { "_dclass",             (PVOID)crt_dclass,       2, CC_CDECL },  /* double = 2 DWORDs */
    { "_fdclass",            (PVOID)crt_fdclass,      1, CC_CDECL },
    { "_stat",               (PVOID)crt_stat,         2, CC_CDECL },
    { "_stat32",             (PVOID)crt_stat32,       2, CC_CDECL },
    { "_stat32i64",          (PVOID)crt_stat32i64,    2, CC_CDECL },
    { "_stat64i32",          (PVOID)crt_stat64i32,    2, CC_CDECL },
    { "_stat64",             (PVOID)crt_stat64,       2, CC_CDECL },
    { "_wstat",              (PVOID)crt_wstat,        2, CC_CDECL },
    { "_wstat32",            (PVOID)crt_wstat32,      2, CC_CDECL },
    { "_wstat32i64",         (PVOID)crt_wstat32i64,   2, CC_CDECL },
    { "_wstat64i32",         (PVOID)crt_wstat64i32,   2, CC_CDECL },
    { "_wstat64",            (PVOID)crt_wstat64,      2, CC_CDECL },
    { "_strdate",            (PVOID)crt_strdate,      1, CC_CDECL },
    { "_strtime",            (PVOID)crt_strtime,      1, CC_CDECL },
    { "_wstrdate",           (PVOID)crt_wstrdate,     1, CC_CDECL },
    { "_wstrtime",           (PVOID)crt_wstrtime,     1, CC_CDECL },
    { "_vsnwprintf",         (PVOID)crt_vsnwprintf,   4, CC_CDECL },
    { "_wcsicmp",            (PVOID)crt_wcsicmp,      2, CC_CDECL },
    { "_wcsnicmp",           (PVOID)crt_wcsnicmp,     3, CC_CDECL },
    { "_wcsupr",             (PVOID)crt_wcsupr,       1, CC_CDECL },
    { "_wtoi",               (PVOID)crt_wtoi,         1, CC_CDECL },
    { "ceil",                (PVOID)crt_ceil,         2, CC_CDECL },  /* double = 2 DWORDs */
    { "floor",               (PVOID)crt_floor,        2, CC_CDECL },  /* double = 2 DWORDs */
    { "fabs",                (PVOID)crt_fabs,         2, CC_CDECL },  /* double = 2 DWORDs */
    { "sqrt",                (PVOID)crt_sqrt,         2, CC_CDECL },  /* double = 2 DWORDs */
    { "difftime",            (PVOID)crt_difftime,     2, CC_CDECL },  /* 2× long (time_t) */
    { "gmtime",              (PVOID)crt_gmtime,       1, CC_CDECL },
    { "mktime",              (PVOID)crt_mktime,       1, CC_CDECL },
    { "rand",                (PVOID)crt_rand,         0, CC_CDECL },
    { "srand",               (PVOID)crt_srand,        1, CC_CDECL },
    { "strncat",             (PVOID)crt_strncat,      3, CC_CDECL },
    { "wcscat",              (PVOID)crt_wcscat,       2, CC_CDECL },
    { "wcscat_s",            (PVOID)crt_wcscat_s,     3, CC_CDECL },
    { "wcschr",              (PVOID)crt_wcschr,       2, CC_CDECL },
    { "wcsrchr",             (PVOID)crt_wcsrchr,      2, CC_CDECL },
    { "wcscoll",             (PVOID)crt_wcscoll,      2, CC_CDECL },
    { "wcscmp",              (PVOID)crt_wcscmp,       2, CC_CDECL },
    { "wcscpy",              (PVOID)crt_wcscpy,       2, CC_CDECL },
    { "wcscpy_s",            (PVOID)crt_wcscpy_s,     3, CC_CDECL },
    { "_wcsdup",             (PVOID)crt_wcsdup,       1, CC_CDECL },
    { "wcslen",              (PVOID)crt_wcslen,       1, CC_CDECL },
    { "wcsnlen",             (PVOID)crt_wcsnlen,      2, CC_CDECL },
    { "wcsncmp",             (PVOID)crt_wcsncmp,      3, CC_CDECL },
    { "wcsncpy",             (PVOID)crt_wcsncpy,      3, CC_CDECL },
    { "wcsncpy_s",           (PVOID)crt_wcsncpy_s,    4, CC_CDECL },
    { "wcsstr",              (PVOID)crt_wcsstr,       2, CC_CDECL },
    { "wcstok_s",            (PVOID)crt_wcstok_s,     3, CC_CDECL },
    { "wcsxfrm",             (PVOID)crt_wcsxfrm,      3, CC_CDECL },
    { "wcstoul",             (PVOID)crt_wcstoul,      3, CC_CDECL },
    /* File access */
    { "_access",             (PVOID)crt_access,       2, CC_CDECL },
    { "_waccess",            (PVOID)crt_waccess,      2, CC_CDECL },
    { "_getcwd",             (PVOID)crt_getcwd,       2, CC_CDECL },
    { "_wgetcwd",            (PVOID)crt_wgetcwd,      2, CC_CDECL },
    { "_fullpath",           (PVOID)crt_fullpath,     3, CC_CDECL },
    { "_wfullpath",          (PVOID)crt_wfullpath,    3, CC_CDECL },

    /* CRT globals (as accessor functions through INT 0x2E) */
    { "_fltused",            (PVOID)crt_fltused,      0, CC_CDECL },
    { "__p__osver",          (PVOID)crt_p_osver,      0, CC_CDECL },
    { "__p__winver",         (PVOID)crt_p_winver,     0, CC_CDECL },
    { "__p__winmajor",       (PVOID)crt_p_winmajor,   0, CC_CDECL },
    { "__p__winminor",       (PVOID)crt_p_winminor,   0, CC_CDECL },

    /* Stubs for bundled MSVCRT.dll imports */
    { "_getch",              (PVOID)crt_getch_stub,   0, CC_CDECL },
    { "_kbhit",              (PVOID)crt_kbhit_stub,   0, CC_CDECL },

    { NULL, NULL, 0, CC_CDECL }
};

const WIN32_EXPORT *msvcrt_abi_table(int *count) {
    *count = (int)(sizeof(msvcrt_exports)/sizeof(msvcrt_exports[0]));
    return (const WIN32_EXPORT *)msvcrt_exports;
}

static int msvcrt_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

PVOID msvcrt_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    if (by_ordinal) return NULL;

    /* These CRT data exports are process-local writable variables. Resolve
     * them when each image is loaded instead of exposing kernel .data. */
    if (msvcrt_strcmp(func_name, "_commode") == 0)
        return (PVOID)crt_p_commode();
    if (msvcrt_strcmp(func_name, "_fmode") == 0)
        return (PVOID)crt_p_fmode();

    for (int i = 0; msvcrt_exports[i].name; i++) {
        if (msvcrt_strcmp(func_name, msvcrt_exports[i].name) == 0)
            return msvcrt_exports[i].func;
    }

    return NULL;
}

PVOID msvcrt_shim_init(void)
{
    ensure_stdio_init();
    win32_abi_register_compat32_bridge((PVOID)crt_printf,
                                        (PVOID)crt_printf_compat32);
    win32_abi_register_compat32_bridge((PVOID)crt_sprintf,
                                        (PVOID)crt_sprintf_compat32);
    win32_abi_register_compat32_bridge((PVOID)crt_snprintf,
                                        (PVOID)crt_snprintf_compat32);
    win32_abi_register_compat32_bridge((PVOID)crt_snprintf_s,
                                        (PVOID)crt_snprintf_s_compat32);
    win32_abi_register_compat32_bridge((PVOID)crt_fprintf,
                                        (PVOID)crt_fprintf_compat32);
    win32_abi_register_compat32_bridge((PVOID)crt_sscanf,
                                        (PVOID)crt_sscanf_compat32);
    win32_abi_register_compat32_bridge((PVOID)crt_lseeki64,
                                        (PVOID)crt_lseeki64_compat32);
    /* Re-exec resets. stub_gmalloc_installed is a one-shot whose stub vtable
     * holds thunks into the PREVIOUS run's thunk pool (compat32_init re-allocates
     * it) — must rebuild. The FMW Free router likewise re-installs on the freshly
     * reloaded Core.dll/UT.exe vtables; drop the old router pool (page leaks,
     * bounded by relaunch count). */
    stub_gmalloc_installed = 0;
    fmw_router_pool = NULL;
    fmw_router_used = 0;
    return (PVOID)msvcrt_exports;
}
