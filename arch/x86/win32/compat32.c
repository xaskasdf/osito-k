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
#include "kernel32_shim.h"
#include "msvcrt_shim.h"
#include "wdbg.h"
#include "win32_abi.h"
#include "../include/paging.h"
#include "../kernel/smp.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void *mem_alloc_pages(uint64_t count);
extern void mem_free_pages(void *addr, uint64_t count);
extern void *kcalloc(uint64_t count, uint64_t size);
extern void kfree(void *ptr);
extern int  kern_setjmp(uint64_t *buf) __attribute__((returns_twice));
extern void kern_longjmp(uint64_t *buf, int val);

static int compat32_running_ut99(void)
{
    extern char win32_exe_name[64];
    const char *base = win32_exe_name;
    const char *want = "UnrealTournament.exe";

    for (const char *p = base; *p; p++)
        if (*p == '\\' || *p == '/') base = p + 1;
    while (*base && *want) {
        char a = *base++, b = *want++;
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return 0;
    }
    return *base == 0 && *want == 0;
}

/* ── Global compat32 mode flag ────────────────────────────────── */

#ifndef __KERNEL_X86__
int g_compat32_mode = 0;
#endif
int g_compat32_ut99 = 0;
uint32_t g_int2e_rsp_depth = 0; /* shared with int2e_stub.S */

/* ── C++ EH unwind state (set by _CxxThrowException) ────────── */
uint32_t g_compat32_unwind_eip = 0;
uint32_t g_compat32_last_stack_arg13 = 0;  /* for CreateWindowExW lpParam workaround */
uint32_t g_compat32_unwind_esp = 0;
uint32_t g_compat32_unwind_ebp = 0;
uint32_t g_compat32_unwind_ebx = 0;
uint32_t g_compat32_unwind_esi = 0;
uint32_t g_compat32_unwind_edi = 0;
uint32_t g_compat32_unwind_restore_nonvolatile = 0;

/* ── Thunk state ─────────────────────────────────────────────── */

#define THUNK_STUB_SIZE  64      /* bytes per thunk stub */
#define THUNKS_PER_PAGE  (4096 / THUNK_STUB_SIZE)  /* 64 */
#define THUNK_CODE_PAGES ((COMPAT32_MAX_THUNKS + THUNKS_PER_PAGE - 1) / \
                          THUNKS_PER_PAGE)
/* Keep callback/data stubs out of the final four regular thunk slots. */
#define THUNK_POOL_PAGES (THUNK_CODE_PAGES + 1)
#define THUNK_POOL_BYTES (THUNK_POOL_PAGES * 4096U)
#define RUNTIME_QSORT_PAGE THUNK_POOL_PAGES
#define RUNTIME_ATOF_PAGE  (RUNTIME_QSORT_PAGE + 1)
#define RUNTIME_MATH_PAGE  (RUNTIME_ATOF_PAGE + 1)
#define COMPAT32_RUNTIME_PAGES (RUNTIME_MATH_PAGE + 1)
#define COMPAT32_RUNTIME_BYTES (COMPAT32_RUNTIME_PAGES * 4096U)
#define THUNK_NAME_POOL_SIZE (COMPAT32_MAX_THUNKS * 256U)
#define THUNK_NAME_MAX 512U

/* The runtime is allocated once below the Win32 VirtualAlloc window. PE32
 * code uses its physical address as a stable 32-bit VA; kernel code writes
 * through the shared upper-half alias, which is valid in every CR3. */
static uint8_t *thunk_pool = NULL;       /* upper-half writable alias */
static uint64_t compat32_runtime_phys;
static uint32_t compat32_runtime_addr;
static volatile uint32_t compat32_runtime_state; /* 0=empty, 1=init, 2=ready */
static char thunk_name_pool[THUNK_NAME_POOL_SIZE];
static uint32_t thunk_name_pool_used;
static spinlock_t thunk_table_lock = SPINLOCK_INIT;

/* B3 fix: native 32-bit qsort/bsearch blob (clang -m32, position-independent,
 * no relocations; qsort@0, bsearch@0x1a0). Installed in PE-executable low memory
 * and used as the MSVCRT qsort/bsearch import so the guest calls them NATIVELY in
 * 32-bit mode and the comparator runs 32->32 native — NO INT 0x2E, NO compat32
 * callback round-trip, NO IST1 drift (which corrupted state and triple-faulted
 * New Game when crt_qsort called compat32_callback_args per comparison). Source:
 * arch/x86/scripts/qsort32.c. */
#define QSORT32_BSEARCH_OFF 0x1a0
static uint32_t qsort32_blob_addr = 0;
static const unsigned char qsort32_blob[498] = {
    0x55,0x89,0xe5,0x53,0x57,0x56,0x83,0xec,0x2c,0x83,0x7d,0x0c,
    0x02,0x0f,0x82,0x77,0x01,0x00,0x00,0x8b,0x75,0x10,0x89,0xf0,
    0x83,0xe0,0xe0,0x89,0x45,0xf0,0x89,0xf0,0x83,0xe0,0xfc,0x89,
    0x45,0xcc,0x89,0xf3,0xf7,0xdb,0x8b,0x7d,0x08,0x8d,0x0c,0x37,
    0x83,0xc1,0x10,0x8d,0x57,0x10,0x01,0xfe,0xb8,0x01,0x00,0x00,
    0x00,0x89,0x5d,0xec,0xeb,0x32,0x66,0x66,0x66,0x66,0x66,0x2e,
    0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00,0x8b,0x45,0xdc,0x40,
    0x8b,0x4d,0xd0,0x01,0xd1,0x89,0xd7,0x8b,0x55,0xd4,0x01,0xfa,
    0x8b,0x75,0xd8,0x01,0xfe,0x8b,0x7d,0xe0,0x03,0x7d,0x10,0x3b,
    0x45,0x0c,0x0f,0x84,0x16,0x01,0x00,0x00,0x89,0x7d,0xe0,0x89,
    0x75,0xd8,0x89,0x55,0xd4,0x89,0x55,0xe4,0x89,0x4d,0xd0,0x89,
    0x4d,0xe8,0x89,0x45,0xdc,0x8b,0x55,0x10,0xeb,0x16,0x66,0x90,
    0x01,0x5d,0xe8,0x01,0x5d,0xe4,0x01,0xde,0x01,0xdf,0x8b,0x45,
    0xc8,0x85,0xc0,0x8b,0x55,0x10,0x74,0xac,0x8d,0x48,0xff,0x89,
    0x4d,0xc8,0x8d,0x48,0xff,0x0f,0xaf,0xca,0x8b,0x55,0x08,0x01,
    0xd1,0x0f,0xaf,0x45,0x10,0x01,0xd0,0x50,0x51,0xff,0x55,0x14,
    0x8b,0x55,0x10,0x83,0xc4,0x08,0x85,0xc0,0x7e,0x86,0x85,0xd2,
    0x74,0xc2,0x31,0xc0,0x83,0x7d,0x10,0x04,0x0f,0x82,0x96,0x00,
    0x00,0x00,0x31,0xc9,0x83,0x7d,0x10,0x20,0x72,0x52,0x31,0xc0,
    0x8b,0x4d,0xf0,0x8b,0x55,0xe4,0x8b,0x5d,0xe8,0x0f,0x1f,0x00,
    0x0f,0x10,0x44,0x02,0xf0,0x0f,0x10,0x0c,0x02,0x0f,0x10,0x54,
    0x03,0xf0,0x0f,0x10,0x1c,0x03,0x0f,0x11,0x54,0x02,0xf0,0x0f,
    0x11,0x1c,0x02,0x0f,0x11,0x44,0x03,0xf0,0x0f,0x11,0x0c,0x03,
    0x83,0xc0,0x20,0x39,0xc1,0x75,0xd5,0x39,0x4d,0x10,0x8b,0x5d,
    0xec,0x0f,0x84,0x69,0xff,0xff,0xff,0x8b,0x45,0xf0,0x89,0xc1,
    0x8b,0x55,0x10,0xf6,0xc2,0x1c,0x74,0x3c,0x8b,0x5d,0xcc,0x66,
    0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00,0x8b,0x04,0x0f,0x8b,
    0x14,0x0e,0x89,0x14,0x0f,0x89,0x04,0x0e,0x83,0xc1,0x04,0x39,
    0xcb,0x75,0xed,0x89,0xd8,0x39,0x5d,0x10,0x8b,0x5d,0xec,0x0f,
    0x84,0x2f,0xff,0xff,0xff,0x66,0x66,0x66,0x66,0x66,0x66,0x2e,
    0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00,0x0f,0xb6,0x0c,0x07,
    0x0f,0xb6,0x14,0x06,0x88,0x14,0x07,0x88,0x0c,0x06,0x40,0x89,
    0xd9,0x01,0xc1,0x75,0xeb,0xe9,0x06,0xff,0xff,0xff,0x83,0xc4,
    0x2c,0x5e,0x5f,0x5b,0x5d,0xc3,0x66,0x66,0x66,0x66,0x66,0x2e,
    0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00,0x55,0x89,0xe5,0x53,
    0x57,0x56,0x83,0xec,0x08,0x31,0xff,0x8b,0x4d,0x10,0x66,0x90,
    0x89,0xcb,0x29,0xfb,0x76,0x30,0xd1,0xeb,0x8d,0x34,0x3b,0x89,
    0x75,0xf0,0x0f,0xaf,0x75,0x14,0x03,0x75,0x0c,0x56,0xff,0x75,
    0x08,0x89,0x4d,0xec,0xff,0x55,0x18,0x8b,0x4d,0xec,0x83,0xc4,
    0x08,0x85,0xc0,0x8d,0x44,0x3b,0x01,0x0f,0x48,0x4d,0xf0,0x0f,
    0x49,0xf8,0x75,0xcc,0xeb,0x02,0x31,0xf6,0x89,0xf0,0x83,0xc4,
    0x08,0x5e,0x5f,0x5b,0x5d,0xc3,
};
static uint32_t atof32_blob_addr = 0;
static void emit_atof32_blob(uint8_t *code);
static uint32_t thunk_count = 0;

static compat32_thunk_t thunk_table[COMPAT32_MAX_THUNKS];

static const char *thunk_copy_name(const char *name)
{
    if (!name)
        return NULL;

    uint32_t len = 0;
    while (name[len]) {
        if (len + 1 >= THUNK_NAME_POOL_SIZE)
            return NULL;
        len++;
    }

    uint32_t bytes = len + 1;
    uint32_t aligned = (bytes + 7U) & ~7U;
    if (aligned > THUNK_NAME_POOL_SIZE - thunk_name_pool_used)
        return NULL;

    char *copy = &thunk_name_pool[thunk_name_pool_used];
    for (uint32_t i = 0; i < bytes; i++)
        copy[i] = name[i];
    thunk_name_pool_used += aligned;
    return copy;
}

BOOL compat32_runtime_range_conflicts(ULONGLONG base, ULONGLONG size)
{
    if (__atomic_load_n(&compat32_runtime_state, __ATOMIC_ACQUIRE) != 2 ||
        !size || base + size < base)
        return FALSE;

    uint64_t runtime_base = compat32_runtime_addr;
    uint64_t runtime_end = runtime_base + COMPAT32_RUNTIME_BYTES;
    uint64_t end = base + size;
    return base < runtime_end && end > runtime_base;
}

static int compat32_map_runtime_current(void)
{
    if (__atomic_load_n(&compat32_runtime_state, __ATOMIC_ACQUIRE) != 2)
        return -1;

#ifdef TEST_HARNESS
    return 0;
#else
    extern uint64_t proc_current_cr3(void);
    uint64_t cr3 = proc_current_cr3();
    if (!cr3) cr3 = paging_get_kernel_cr3();
    if (!cr3) return -1;

    uint64_t last_offset = (COMPAT32_RUNTIME_PAGES - 1U) * 4096ULL;
    uint64_t first = paging_translate_in_cr3(cr3, compat32_runtime_addr);
    uint64_t last = paging_translate_in_cr3(
        cr3, (uint64_t)compat32_runtime_addr + last_offset);
    if ((first & ~0xFFFULL) == compat32_runtime_phys &&
        (last & ~0xFFFULL) == compat32_runtime_phys + last_offset)
        return 0;

    int installed = 0;
    for (uint64_t i = 0; i < COMPAT32_RUNTIME_PAGES; i++) {
        uint64_t offset = i * 4096ULL;
        uint64_t va = (uint64_t)compat32_runtime_addr + offset;
        uint64_t phys = compat32_runtime_phys + offset;
        uint64_t mapped = paging_translate_in_cr3(cr3, va);

        if (mapped != UINT64_MAX && (mapped & ~0xFFFULL) != phys) {
            serial_puts("[COMPAT32] runtime VA conflict at 0x");
            serial_puthex(va, 8);
            serial_puts(" mapped=0x");
            serial_puthex(mapped, 16);
            serial_puts("\n");
            return -1;
        }
        if (mapped != UINT64_MAX)
            continue;
        if (paging_map_page_in_cr3(cr3, va, phys,
                                   (1ULL << 0) | (1ULL << 1)) != 0)
            return -1;
        installed = 1;
    }

    if (installed) {
        serial_puts("[COMPAT32] runtime mapped cr3=0x");
        serial_puthex(cr3, 16);
        serial_puts(" va=0x");
        serial_puthex(compat32_runtime_addr, 8);
        serial_puts("\n");
    }
    return 0;
#endif
}

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

/* Fallback for unresolved imports whose ABI cannot be determined. */
static uint32_t unresolved_stub_addr = 0;

static uint64_t WINAPI unresolved_import_zero(void)
{
    return 0;
}
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
#define MAX_CALLBACK_DEPTH    128
#define SEH32_MAX_DISPATCH_DEPTH 4

/* Win32 i386 exception ABI.  These objects must live at an address visible
 * below 4GB because native PE32 handlers dereference them directly. */
typedef struct __attribute__((packed, aligned(4))) {
    uint32_t ExceptionCode;
    uint32_t ExceptionFlags;
    uint32_t ExceptionRecord;
    uint32_t ExceptionAddress;
    uint32_t NumberParameters;
    uint32_t ExceptionInformation[15];
} EXCEPTION_RECORD32;

typedef struct __attribute__((packed, aligned(4))) {
    uint32_t ControlWord;
    uint32_t StatusWord;
    uint32_t TagWord;
    uint32_t ErrorOffset;
    uint32_t ErrorSelector;
    uint32_t DataOffset;
    uint32_t DataSelector;
    uint8_t  RegisterArea[80];
    uint32_t Cr0NpxState;
} FLOATING_SAVE_AREA32;

typedef struct __attribute__((packed, aligned(4))) {
    uint32_t ContextFlags;
    uint32_t Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
    FLOATING_SAVE_AREA32 FloatSave;
    uint32_t SegGs, SegFs, SegEs, SegDs;
    uint32_t Edi, Esi, Ebx, Edx, Ecx, Eax;
    uint32_t Ebp, Eip, SegCs, EFlags, Esp, SegSs;
    uint8_t  ExtendedRegisters[512];
} CONTEXT32;

typedef struct __attribute__((packed, aligned(4))) {
    uint32_t ExceptionRecord;
    uint32_t ContextRecord;
} EXCEPTION_POINTERS32;

typedef struct __attribute__((packed, aligned(4))) {
    EXCEPTION_RECORD32 exception_record;
    EXCEPTION_POINTERS32 exception_pointers;
    CONTEXT32 context;
} seh32_dispatch_slot_t;

#define CONTEXT32_FULL 0x00010007U

_Static_assert(sizeof(FLOATING_SAVE_AREA32) == 112,
               "Win32 FLOATING_SAVE_AREA ABI changed");
_Static_assert(sizeof(CONTEXT32) == 716, "Win32 CONTEXT32 ABI changed");
_Static_assert(__builtin_offsetof(CONTEXT32, Ebp) == 180,
               "Win32 CONTEXT32 Ebp offset changed");
_Static_assert(__builtin_offsetof(CONTEXT32, Eip) == 184,
               "Win32 CONTEXT32 Eip offset changed");
_Static_assert(__builtin_offsetof(CONTEXT32, Esp) == 196,
               "Win32 CONTEXT32 Esp offset changed");
_Static_assert(sizeof(seh32_dispatch_slot_t) * SEH32_MAX_DISPATCH_DEPTH <=
                   4096,
               "PE32 SEH scratch no longer fits one page");

/* Callback ownership is indexed by the scheduler's process-table slot. The
 * owner table follows the scheduler's RAM-derived capacity so concurrent
 * callbacks cannot longjmp into another task after the old fixed limit. */
#define CALLBACK_STACK_SIZE   65536

/* kern_setjmp / kern_longjmp use a 9-quad jmp_buf:
 *   [0..5] callee-saved GPRs (rbx, rbp, r12-r15)
 *   [6]    rsp     [7] rip     [8] cr3
 * Sizing this array as [...][8] truncated each slot to 64 bytes and the
 * setjmp at depth N+1 wrote its rbx (slot[0]) on top of slot N's cr3 —
 * causing the depth=0 longjmp after _initterm to triple-fault on a
 * bogus 0x40 CR3. Must be at least 9. */
typedef struct {
    int depth;
    int seh_dispatch_depth;
    uint64_t jmpbufs[MAX_CALLBACK_DEPTH][9];
    uint64_t saved_ist1[MAX_CALLBACK_DEPTH];
    uint32_t saved_stack_args[MAX_CALLBACK_DEPTH];
    uint8_t *stacks[MAX_CALLBACK_DEPTH];
    DWORD stack_process_ids[MAX_CALLBACK_DEPTH];
    uint32_t retvals[MAX_CALLBACK_DEPTH];
    uint32_t current_stack_args;
    seh32_dispatch_slot_t *seh32_slots;
    DWORD seh32_process_id;
    int in_catch_dispatch;
    uint32_t saved_next_frame;
} callback_owner_state_t;

/* One pointer per scheduler slot; owner state and callback stacks are lazy. */
static callback_owner_state_t **callback_owners;
static uint32_t callback_owner_capacity;
static volatile uint32_t callback_owner_table_lock;

static void callback_table_lock(void)
{
    while (__sync_lock_test_and_set(&callback_owner_table_lock, 1))
        __asm__ volatile ("pause" ::: "memory");
}

static void callback_table_unlock(void)
{
    __sync_lock_release(&callback_owner_table_lock);
}

static int callback_owner_table_ensure(void)
{
    if (__atomic_load_n(&callback_owners, __ATOMIC_ACQUIRE))
        return 1;

    callback_table_lock();
    if (!callback_owners) {
        extern uint32_t sched_capacity_get(void);
        uint32_t capacity = sched_capacity_get();
        callback_owner_state_t **owners =
            (callback_owner_state_t **)kcalloc(capacity, sizeof(*owners));
        if (!capacity || !owners) {
            callback_table_unlock();
            return 0;
        }
        callback_owner_capacity = capacity;
        __atomic_store_n(&callback_owners, owners, __ATOMIC_RELEASE);
    }
    callback_table_unlock();
    return 1;
}

static int callback_owner(void)
{
#ifndef TEST_HARNESS
    extern int sched_current_get(void);
    int owner = sched_current_get();
    if (owner >= 0 && (uint32_t)owner < callback_owner_capacity)
        return owner;
#else
    return 0;
#endif
    return -1;
}

static callback_owner_state_t *callback_state_get(int create, int *owner_out)
{
    if (!callback_owner_table_ensure())
        return NULL;
    int owner = callback_owner();
    if (owner < 0)
        return NULL;
    if (owner_out)
        *owner_out = owner;

    callback_owner_state_t *state = __atomic_load_n(
        &callback_owners[owner], __ATOMIC_ACQUIRE);
    if (!state && create) {
        callback_owner_state_t *fresh = (callback_owner_state_t *)kcalloc(
            1, sizeof(*fresh));
        if (!fresh)
            return NULL;
        callback_owner_state_t *expected = NULL;
        if (!__atomic_compare_exchange_n(&callback_owners[owner], &expected,
                                         fresh, FALSE, __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE)) {
            kfree(fresh);
            state = expected;
        } else {
            state = fresh;
        }
    }
    return state;
}

static int seh32_scratch_prepare(callback_owner_state_t *state)
{
    if (!state) return 0;

    extern DWORD win32_current_process_id(void);
    DWORD process_id = win32_current_process_id();
    if (!process_id) process_id = 1;

    /* Scheduler slots are reusable and the old VMA disappears with its
     * process address space.  Never carry its numeric pointer into a new CR3. */
    if (state->seh32_slots && state->seh32_process_id != process_id) {
        state->seh32_slots = NULL;
        state->seh32_process_id = 0;
    }
    if (!state->seh32_slots) {
        PVOID page = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT,
                                  PAGE_READWRITE);
        if (!page || (uint64_t)(ULONG_PTR)page + 4096ULL > UINT32_MAX) {
            if (page) VirtualFree(page, 0, MEM_RELEASE);
            return 0;
        }
        memset(page, 0, 4096);
        state->seh32_slots = (seh32_dispatch_slot_t *)page;
        state->seh32_process_id = process_id;
    }
    return 1;
}

static seh32_dispatch_slot_t *seh32_current_slot(
    callback_owner_state_t *state)
{
    if (!state || state->seh_dispatch_depth <= 0 ||
        state->seh_dispatch_depth > SEH32_MAX_DISPATCH_DEPTH ||
        !seh32_scratch_prepare(state))
        return NULL;
    return &state->seh32_slots[state->seh_dispatch_depth - 1];
}

uint32_t compat32_current_user_stack_top(void)
{
#ifndef TEST_HARNESS
    callback_owner_state_t *state = callback_state_get(0, NULL);
    uint32_t stack_args = state ? state->current_stack_args : 0;
    if (stack_args < sizeof(uint32_t))
        return 0;

    uint32_t stack_top = stack_args - sizeof(uint32_t);
    TEB32 *teb = compat32_current_teb();
    if (!teb || !teb->StackBase || !teb->StackLimit ||
        stack_top < teb->StackLimit || stack_top > teb->StackBase ||
        stack_top - teb->StackLimit < 64U)
        return 0;
    return stack_top;
#else
    return 0;
#endif
}

static uint8_t *callback_stack_get(callback_owner_state_t *state, int depth)
{
    if (!state || depth < 0 || depth >= MAX_CALLBACK_DEPTH)
        return NULL;
    extern DWORD win32_current_process_id(void);
    DWORD process_id = win32_current_process_id();
    if (!process_id) process_id = 1;

    /* Scheduler slots are reusable. A stack VMA from the previous process is
     * not mapped in the new process even if the numeric slot is identical. */
    if (state->stacks[depth] &&
        state->stack_process_ids[depth] != process_id) {
        state->stacks[depth] = NULL;
        state->stack_process_ids[depth] = 0;
    }
    if (!state->stacks[depth]) {
        uint8_t *stack = (uint8_t *)VirtualAlloc(
            NULL, CALLBACK_STACK_SIZE, MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE);
        if (!stack || (uint64_t)(ULONG_PTR)stack + CALLBACK_STACK_SIZE >
                          UINT32_MAX) {
            if (stack) VirtualFree(stack, 0, MEM_RELEASE);
            return NULL;
        }
        state->stacks[depth] = stack;
        state->stack_process_ids[depth] = process_id;
    }
    return state->stacks[depth];
}

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

/* Recent-native-call ring buffer (diagnostic): records every INT 0x2E shim
 * dispatch (no serial I/O) so the #PF/NULL-CALL handler can dump the last ~24
 * native calls before a crash — to find a shim whose wrong arg-count/return
 * corrupted the caller's registers/stack (the New-Game LocalMapURL NULL-vtable
 * crash). Safe to add now that the IST1 stack-overflow is fixed. */
const char *g_rcall_name[64];
uint32_t    g_rcall_args[64][4];
uint32_t    g_rcall_ret[64];
uint8_t     g_rcall_nargs[64];
uint32_t    g_rcall_caller[64];
uint32_t    g_rcall_repeat[64];
uint32_t    g_rcall_ebp[64];
uint32_t    g_rcall_esp[64];
uint32_t    g_rcall_idx = 0;

void compat32_dump_recent_calls(void)
{
    serial_puts("[RCALL] last native calls before fault (oldest->newest):\n");
    uint32_t end = g_rcall_idx;
    uint32_t start = (end >= 64) ? end - 64 : 0;
    for (uint32_t k = start; k < end; k++) {
        uint32_t ri = k & 63;
        serial_puts("  ");
        serial_putdec(k);
        serial_puts(": ");
        serial_puts(g_rcall_name[ri] ? g_rcall_name[ri] : "?");
        serial_puts(" (");
        serial_putdec(g_rcall_nargs[ri]);
        serial_puts(" args) caller=0x");
        serial_puthex(g_rcall_caller[ri], 8);
        serial_puts(" ebp=0x");
        serial_puthex(g_rcall_ebp[ri], 8);
        serial_puts(" esp=0x");
        serial_puthex(g_rcall_esp[ri], 8);
        if (g_rcall_repeat[ri] > 1) {
            serial_puts(" repeat=");
            serial_putdec(g_rcall_repeat[ri]);
        }
        serial_puts(" a=[0x");
        serial_puthex(g_rcall_args[ri][0], 8);
        serial_puts(" 0x"); serial_puthex(g_rcall_args[ri][1], 8);
        serial_puts(" 0x"); serial_puthex(g_rcall_args[ri][2], 8);
        serial_puts(" 0x"); serial_puthex(g_rcall_args[ri][3], 8);
        serial_puts("] ret=0x");
        serial_puthex(g_rcall_ret[ri], 8);
        serial_puts("\n");
    }
}

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
    if ((callconv & CC_CONVENTION_MASK) == CC_CDECL || num_args == 0) {
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
    g_compat32_ut99 = compat32_running_ut99();

    /* Re-exec resets only this scheduler slot. Its prior callback-stack VMAs
     * were released with the process address space. */
    int owner = -1;
    callback_owner_state_t *callback_state = callback_state_get(1, &owner);
    if (!callback_state) {
        serial_puts("[COMPAT32] Failed to allocate callback owner state\n");
        return;
    }
    (void)owner;
    memset(callback_state, 0, sizeof(*callback_state));

    if (__atomic_load_n(&compat32_runtime_state, __ATOMIC_ACQUIRE) == 2) {
        if (compat32_map_runtime_current() != 0)
            serial_puts("[COMPAT32] Failed to map shared runtime\n");
        return;
    }

    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&compat32_runtime_state, &expected, 1,
                                     FALSE, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&compat32_runtime_state, __ATOMIC_ACQUIRE) == 1)
            __asm__ volatile ("pause");
        if (__atomic_load_n(&compat32_runtime_state, __ATOMIC_ACQUIRE) == 2 &&
            compat32_map_runtime_current() != 0)
            serial_puts("[COMPAT32] Failed to map shared runtime\n");
        return;
    }

    thunk_count = 0;
    thunk_name_pool_used = 0;
    void *runtime_block = mem_alloc_pages(COMPAT32_RUNTIME_PAGES);
    if (!runtime_block) {
        serial_puts("[COMPAT32] Failed to allocate shared runtime!\n");
        __atomic_store_n(&compat32_runtime_state, 0, __ATOMIC_RELEASE);
        return;
    }

    compat32_runtime_phys = (uint64_t)(ULONG_PTR)runtime_block;
#ifndef TEST_HARNESS
    /* Keep the identity VA outside the Win32 VirtualAlloc window. */
    if (compat32_runtime_phys + COMPAT32_RUNTIME_BYTES <
            compat32_runtime_phys ||
        compat32_runtime_phys + COMPAT32_RUNTIME_BYTES > 0x40000000ULL) {
        serial_puts("[COMPAT32] No shared runtime space below 1GB\n");
        mem_free_pages(runtime_block, COMPAT32_RUNTIME_PAGES);
        compat32_runtime_phys = 0;
        __atomic_store_n(&compat32_runtime_state, 0, __ATOMIC_RELEASE);
        return;
    }
    thunk_pool = (uint8_t *)PHYS_TO_VIRT(compat32_runtime_phys);
#else
    thunk_pool = (uint8_t *)runtime_block;
#endif
    compat32_runtime_addr = (uint32_t)(ULONG_PTR)runtime_block;

    for (uint32_t i = 0; i < COMPAT32_RUNTIME_BYTES; i++)
        thunk_pool[i] = 0xCC;

    serial_puts("[COMPAT32] Shared runtime at 0x");
    serial_puthex(compat32_runtime_addr, 8);
    serial_puts(" (");
    serial_putdec(COMPAT32_MAX_THUNKS);
    serial_puts(" thunk slots)\n");

    /* Native PE32 qsort/bsearch and atof code uses the same persistent
     * physical backing as the regular thunk pool. */
    {
        uint8_t *blob = thunk_pool + RUNTIME_QSORT_PAGE * 4096U;
        for (uint32_t i = 0; i < (uint32_t)sizeof(qsort32_blob); i++)
            blob[i] = qsort32_blob[i];
        qsort32_blob_addr = compat32_runtime_addr +
                            RUNTIME_QSORT_PAGE * 4096U;
        serial_puts("[COMPAT32] qsort32 blob at 0x");
        serial_puthex(qsort32_blob_addr, 8);
        serial_puts(" (bsearch +0x");
        serial_puthex(QSORT32_BSEARCH_OFF, 4);
        serial_puts(")\n");
    }

    {
        uint8_t *blob = thunk_pool + RUNTIME_ATOF_PAGE * 4096U;
        emit_atof32_blob(blob);
        atof32_blob_addr = compat32_runtime_addr +
                           RUNTIME_ATOF_PAGE * 4096U;
        serial_puts("[COMPAT32] atof32 blob at 0x");
        serial_puthex(atof32_blob_addr, 8);
        serial_puts("\n");
    }

#ifdef TEST_HARNESS
    /* On Linux test harness, make thunk pool executable */
    #include <sys/mman.h>
    mprotect((void *)(ULONG_PTR)compat32_runtime_addr,
             COMPAT32_RUNTIME_BYTES,
             PROT_READ | PROT_WRITE | PROT_EXEC);
#endif

#ifndef TEST_HARNESS
    /*
     * Install a "callback return stub" at the END of the thunk pool.
     * This is a 32-bit code snippet that a compat-mode function RETs to.
     * It copies EAX (function return value) to EDX, then does INT 0x2E
     * with a magic index to signal "callback complete".
     *
     * Code (13 bytes):
     *   89 C2                MOV EDX, EAX
     *   B8 FE FF FF FF       MOV EAX, 0xFFFFFFFE  (THUNK_CALLBACK_RETURN)
     *   B9 00 00 00 00       MOV ECX, 0
     *   CD 2E                INT 0x2E
     *   F4                   HLT  (should never reach here)
     */
    {
        uint32_t offset = THUNK_POOL_BYTES - 24;
        uint8_t *stub = thunk_pool + offset;
        int p = 0;
        stub[p++] = 0x89; stub[p++] = 0xC2;  /* MOV EDX, EAX */
        stub[p++] = 0xB8;  /* MOV EAX, imm32 */
        stub[p++] = 0xFE; stub[p++] = 0xFF; stub[p++] = 0xFF; stub[p++] = 0xFF;
        stub[p++] = 0xB9;  /* MOV ECX, 0 */
        stub[p++] = 0x00; stub[p++] = 0x00; stub[p++] = 0x00; stub[p++] = 0x00;
        stub[p++] = 0xCD; stub[p++] = 0x2E;  /* INT 0x2E */
        stub[p++] = 0xF4;  /* HLT */
        callback_return_stub_addr = compat32_runtime_addr + offset;
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
        uint32_t offset = THUNK_POOL_BYTES - 32;
        uint8_t *stub = thunk_pool + offset;
        stub[0] = 0x31; stub[1] = 0xC0;  /* XOR EAX, EAX */
        stub[2] = 0xC3;                   /* RET */
        unresolved_stub_addr = compat32_runtime_addr + offset;
    }

    /* Catch continuation stub: JMP EAX
     * MSVC catch funclets return with EAX = continuation address.
     * This stub is used as the return address for catch funclets —
     * when the funclet does RET, it pops this stub's address, and
     * JMP EAX continues execution at the establishing function's
     * code after the try/catch block. */
    {
        uint32_t offset = THUNK_POOL_BYTES - 40;
        uint8_t *stub = thunk_pool + offset;
        stub[0] = 0xFF; stub[1] = 0xE0;  /* JMP EAX */
        catch_continue_stub_addr = compat32_runtime_addr + offset;
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
        uint32_t offset = THUNK_POOL_BYTES - 256;
        uint8_t *data = thunk_pool + offset;
        memset(data, 0, 128);
        /* _adjust_fdiv at +0 */
        *(int32_t *)(data + 0) = 0;
        /* _acmdln at +4: points to cmdline string at +20 */
        *(uint32_t *)(data + 4) = compat32_runtime_addr + offset + 20;
        /* _commode at +8 */
        *(int32_t *)(data + 8) = 0;
        /* _fmode at +12 */
        *(int32_t *)(data + 12) = 0;
        /* __mb_cur_max at +16 */
        *(int32_t *)(data + 16) = 1;
        /* cmdline at +20 */
        extern char win32_command_line[4096];
        const char *cmd = win32_command_line[0] ? win32_command_line : "UnrealTournament.exe";
        int ci = 0;
        while (cmd[ci] && ci < 60) { data[20 + ci] = cmd[ci]; ci++; }
        data[20 + ci] = 0;

        compat32_data_area = compat32_runtime_addr + offset;
        serial_puts("[COMPAT32] Data exports at 0x");
        serial_puthex(compat32_data_area, 8);
        serial_puts("\n");
    }

    /* Initialize fast 32-bit x87 math functions (pow, fmod, acos).
     * These run natively in compat mode without INT 0x2E overhead. */
    extern void compat32_init_fast_math(uint8_t *, uint32_t);
    compat32_init_fast_math(thunk_pool + RUNTIME_MATH_PAGE * 4096U,
                            compat32_runtime_addr +
                            RUNTIME_MATH_PAGE * 4096U);
#endif

    __atomic_store_n(&compat32_runtime_state, 2, __ATOMIC_RELEASE);
    if (compat32_map_runtime_current() != 0)
        serial_puts("[COMPAT32] Failed to map shared runtime\n");
}

BOOL compat32_is_initialized(void)
{
    return __atomic_load_n(&compat32_runtime_state, __ATOMIC_ACQUIRE) == 2;
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
        if (name[1] == 'c' && name[2] == 'o' && name[3] == 'm' &&
            name[4] == 'm' && name[5] == 'o' && name[6] == 'd' &&
            name[7] == 'e' && name[8] == 0) { /* _commode */
            int *commode = crt_p_commode();
            return (uint32_t)(ULONG_PTR)commode;
        }
        if (name[1] == 'f' && name[2] == 'm' && name[3] == 'o' &&
            name[4] == 'd' && name[5] == 'e' && name[6] == 0) { /* _fmode */
            int *fmode = crt_p_fmode();
            return (uint32_t)(ULONG_PTR)fmode;
        }
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

/* Native 32-bit _ftol stub. The MS CRT _ftol helper takes its argument in the
 * x87 ST(0) register — the compiler emits `fld X; call _ftol`, NOT a stack push
 * — and returns the truncated int64 in EDX:EAX. Routing it through an INT 0x2E
 * shim is wrong twice: (a) the 64-bit shim reads two garbage DWORDs off the
 * 32-bit stack instead of ST(0), and (b) the x87 state isn't preserved across
 * the 32->64 transition (int2e_stub does no fxsave). So emit a real 32-bit
 * fistp stub that runs entirely in compat mode where ST(0) is valid. This is
 * the root of UT99's black screen: the fullscreen mode pick does
 * `fld <matched 640.0>; call _ftol` and was getting 0 back -> ddraw
 * SetDisplayMode(0,0) -> 0x0 surface. (bpp survived because it's integer
 * `lea eax,[..*8]`, never _ftol'd — hence "bpp right, WxH zero".) */
static void emit_ftol_stub(uint8_t *code)
{
    static const uint8_t blob[] = {
        0x83,0xEC,0x0C,             /* sub   esp,12               */
        0xD9,0x7C,0x24,0x08,        /* fnstcw [esp+8]  (save CW)  */
        0x0F,0xB7,0x44,0x24,0x08,   /* movzx eax,word [esp+8]     */
        0x0D,0x00,0x0C,0x00,0x00,   /* or    eax,0x0C00 (RC=trunc)*/
        0x66,0x89,0x44,0x24,0x0A,   /* mov   [esp+10],ax          */
        0xD9,0x6C,0x24,0x0A,        /* fldcw [esp+10] (truncate)  */
        0xDF,0x3C,0x24,             /* fistp qword [esp]          */
        0xD9,0x6C,0x24,0x08,        /* fldcw [esp+8]  (restore)   */
        0x8B,0x04,0x24,             /* mov   eax,[esp]            */
        0x8B,0x54,0x24,0x04,        /* mov   edx,[esp+4]          */
        0x83,0xC4,0x0C,             /* add   esp,12               */
        0xC3,                       /* ret   (cdecl, no stack arg)*/
    };
    int p = 0;
    for (unsigned i = 0; i < sizeof(blob); i++) code[p++] = blob[i];
    while (p < THUNK_STUB_SIZE) code[p++] = 0xCC;
}

static void emit_atof32_blob(uint8_t *code)
{
    int p = 0;
    int j_null, j_ep0, j_ws_done, j_not_minus, j_sign_done1, j_sign_done2;
    int j_int_done_1, j_int_done_2, j_no_frac, j_frac_done_1, j_frac_done_2;
    int j_no_frac_part, j_no_neg;
    int l_parse, l_ws, l_ws_inc, l_ws_done, l_not_minus, l_sign_done;
    int l_int, l_int_done, l_frac, l_build, l_sign, l_epilogue;

#define E8(v) do { code[p++] = (uint8_t)(v); } while (0)
#define PATCH8(pos, target) do { code[(pos)] = (uint8_t)((target) - ((pos) + 1)); } while (0)

    E8(0x53);                                           /* push ebx */
    E8(0x56);                                           /* push esi */
    E8(0x57);                                           /* push edi */
    E8(0x83); E8(0xEC); E8(0x10);                       /* sub esp,16 */
    E8(0x8B); E8(0x74); E8(0x24); E8(0x20);             /* mov esi,[esp+32] */
    E8(0x85); E8(0xF6);                                 /* test esi,esi */
    E8(0x75); j_null = p++;                             /* jnz parse */
    E8(0xD9); E8(0xEE);                                 /* fldz */
    E8(0xEB); j_ep0 = p++;                              /* jmp epilogue */

    l_parse = p;
    PATCH8(j_null, l_parse);
    l_ws = p;
    E8(0x8A); E8(0x06);                                 /* mov al,[esi] */
    E8(0x3C); E8(0x20); E8(0x74); j_ws_done = p++;      /* cmp al,' '; je ws_inc */
    E8(0x3C); E8(0x09); E8(0x74); int j_ws_tab = p++;
    E8(0x3C); E8(0x0A); E8(0x74); int j_ws_lf = p++;
    E8(0x3C); E8(0x0D); E8(0x74); int j_ws_cr = p++;
    E8(0xEB); int j_ws_out = p++;                       /* jmp ws_done */
    l_ws_inc = p;
    PATCH8(j_ws_done, l_ws_inc);
    PATCH8(j_ws_tab, l_ws_inc);
    PATCH8(j_ws_lf, l_ws_inc);
    PATCH8(j_ws_cr, l_ws_inc);
    E8(0x46);                                           /* inc esi */
    E8(0xEB); PATCH8(p, l_ws); p++;                     /* jmp ws */

    l_ws_done = p;
    PATCH8(j_ws_out, l_ws_done);
    E8(0x31); E8(0xDB);                                 /* xor ebx,ebx */
    E8(0x8A); E8(0x06);                                 /* mov al,[esi] */
    E8(0x3C); E8(0x2D); E8(0x75); j_not_minus = p++;    /* cmp '-'; jne */
    E8(0xB3); E8(0x01);                                 /* mov bl,1 */
    E8(0x46);                                           /* inc esi */
    E8(0xEB); j_sign_done1 = p++;                       /* jmp sign_done */
    l_not_minus = p;
    PATCH8(j_not_minus, l_not_minus);
    E8(0x3C); E8(0x2B); E8(0x75); j_sign_done2 = p++;   /* cmp '+'; jne */
    E8(0x46);                                           /* inc esi */
    l_sign_done = p;
    PATCH8(j_sign_done1, l_sign_done);
    PATCH8(j_sign_done2, l_sign_done);

    E8(0x31); E8(0xC9);                                 /* xor ecx,ecx */
    E8(0x31); E8(0xD2);                                 /* xor edx,edx */
    E8(0xBF); E8(0x01); E8(0x00); E8(0x00); E8(0x00);   /* mov edi,1 */
    l_int = p;
    E8(0x0F); E8(0xB6); E8(0x06);                       /* movzx eax,byte [esi] */
    E8(0x3C); E8(0x30); E8(0x72); j_int_done_1 = p++;   /* jb int_done */
    E8(0x3C); E8(0x39); E8(0x77); j_int_done_2 = p++;   /* ja int_done */
    E8(0x83); E8(0xE8); E8(0x30);                       /* sub eax,'0' */
    E8(0x6B); E8(0xC9); E8(0x0A);                       /* imul ecx,ecx,10 */
    E8(0x01); E8(0xC1);                                 /* add ecx,eax */
    E8(0x46);                                           /* inc esi */
    E8(0xEB); PATCH8(p, l_int); p++;                    /* jmp int */

    l_int_done = p;
    PATCH8(j_int_done_1, l_int_done);
    PATCH8(j_int_done_2, l_int_done);
    E8(0x80); E8(0x3E); E8(0x2E); E8(0x75); j_no_frac = p++; /* cmp byte [esi],'.'; jne */
    E8(0x46);                                           /* inc esi */
    l_frac = p;
    E8(0x0F); E8(0xB6); E8(0x06);                       /* movzx eax,byte [esi] */
    E8(0x3C); E8(0x30); E8(0x72); j_frac_done_1 = p++;  /* jb build */
    E8(0x3C); E8(0x39); E8(0x77); j_frac_done_2 = p++;  /* ja build */
    E8(0x83); E8(0xE8); E8(0x30);                       /* sub eax,'0' */
    E8(0x6B); E8(0xD2); E8(0x0A);                       /* imul edx,edx,10 */
    E8(0x01); E8(0xC2);                                 /* add edx,eax */
    E8(0x6B); E8(0xFF); E8(0x0A);                       /* imul edi,edi,10 */
    E8(0x46);                                           /* inc esi */
    E8(0xEB); PATCH8(p, l_frac); p++;                   /* jmp frac */

    l_build = p;
    PATCH8(j_no_frac, l_build);
    PATCH8(j_frac_done_1, l_build);
    PATCH8(j_frac_done_2, l_build);
    E8(0x89); E8(0x0C); E8(0x24);                       /* mov [esp],ecx */
    E8(0x89); E8(0x54); E8(0x24); E8(0x04);             /* mov [esp+4],edx */
    E8(0x89); E8(0x7C); E8(0x24); E8(0x08);             /* mov [esp+8],edi */
    E8(0xDB); E8(0x04); E8(0x24);                       /* fild dword [esp] */
    E8(0x85); E8(0xD2);                                 /* test edx,edx */
    E8(0x74); j_no_frac_part = p++;                     /* jz sign */
    E8(0xDB); E8(0x44); E8(0x24); E8(0x04);             /* fild dword [esp+4] */
    E8(0xDB); E8(0x44); E8(0x24); E8(0x08);             /* fild dword [esp+8] */
    E8(0xDE); E8(0xF9);                                 /* fdivp st(1),st */
    E8(0xDE); E8(0xC1);                                 /* faddp st(1),st */
    l_sign = p;
    PATCH8(j_no_frac_part, l_sign);
    E8(0x84); E8(0xDB);                                 /* test bl,bl */
    E8(0x74); j_no_neg = p++;                           /* jz epilogue */
    E8(0xD9); E8(0xE0);                                 /* fchs */
    l_epilogue = p;
    PATCH8(j_ep0, l_epilogue);
    PATCH8(j_no_neg, l_epilogue);
    E8(0x83); E8(0xC4); E8(0x10);                       /* add esp,16 */
    E8(0x5F);                                           /* pop edi */
    E8(0x5E);                                           /* pop esi */
    E8(0x5B);                                           /* pop ebx */
    E8(0xC3);                                           /* ret */

    for (; p < 4096; p++) code[p] = 0xCC;

#undef PATCH8
#undef E8
}

/* True for the st0-based CRT float->int helpers that must run native. */
static int is_ftol_helper(const char *n)
{
    if (!n) return 0;
    const char *cands[] = { "_ftol", "_ftol2", "__ftol", 0 };
    for (int i = 0; cands[i]; i++) {
        const char *a = n, *b = cands[i];
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == 0 && *b == 0) return 1;
    }
    return 0;
}

static int is_atof_helper(const char *n)
{
    if (!n) return 0;
    return n[0] == 'a' && n[1] == 't' && n[2] == 'o' &&
           n[3] == 'f' && n[4] == 0;
}

static int thunk_name_equal(const char *a, const char *b)
{
    if (a == b) return 1;
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

uint32_t compat32_make_thunk_ex(uint64_t target, const char *name,
                                 uint8_t num_args, uint8_t callconv)
{
    if (!compat32_is_initialized()) return 0;

    /* Import names normally point into a process-owned PE mapping. Copy the
     * name before taking the global table lock so a malformed/stale mapping
     * cannot leave every Win32 process spinning on a leaked lock. */
    char local_name[THUNK_NAME_MAX];
    const char *safe_name = NULL;
    if (name) {
        uint32_t i = 0;
        while (i + 1 < THUNK_NAME_MAX && name[i]) {
            local_name[i] = name[i];
            i++;
        }
        if (i + 1 == THUNK_NAME_MAX && name[i]) {
            serial_puts("[COMPAT32] Import name exceeds limit\n");
            return 0;
        }
        local_name[i] = 0;
        safe_name = local_name;
    }

    /* B3: resolve qsort/bsearch to the native 32-bit blob (runs in-mode, calls
     * the comparator 32->32 native) instead of an INT 0x2E thunk into the 64-bit
     * crt_qsort, whose per-comparison compat32_callback_args round-trip corrupts
     * IST1 state and triple-faults New Game. */
    if (qsort32_blob_addr && safe_name) {
        const char *q = "qsort", *b = "bsearch";
        int mq = 1, mb = 1;
        for (int i = 0; i < 6; i++) if (safe_name[i] != q[i]) { mq = 0; break; }
        for (int i = 0; i < 8; i++) if (safe_name[i] != b[i]) { mb = 0; break; }
        if (mq) return qsort32_blob_addr;
        if (mb) return qsort32_blob_addr + QSORT32_BSEARCH_OFF;
    }
    if (atof32_blob_addr && is_atof_helper(safe_name))
        return atof32_blob_addr;

    spin_lock(&thunk_table_lock);
    for (uint32_t i = 0; i < thunk_count; i++) {
        compat32_thunk_t *t = &thunk_table[i];
        if (t->target_addr == target && t->num_args == num_args &&
            t->callconv == callconv && thunk_name_equal(t->name, safe_name)) {
            spin_unlock(&thunk_table_lock);
            return t->thunk_addr;
        }
    }
    if (thunk_count >= COMPAT32_MAX_THUNKS) {
        spin_unlock(&thunk_table_lock);
        serial_puts("[COMPAT32] Thunk table full!\n");
        return 0;
    }

    uint32_t idx = thunk_count;
    uint8_t *stub = thunk_pool + (idx * THUNK_STUB_SIZE);
    uint32_t stub_addr = compat32_runtime_addr + idx * THUNK_STUB_SIZE;
    const char *owned_name = thunk_copy_name(safe_name);
    if (safe_name && !owned_name) {
        spin_unlock(&thunk_table_lock);
        serial_puts("[COMPAT32] Thunk name pool full!\n");
        return 0;
    }

    /* Generate thunk code — _ftol family runs as a native x87 stub (see above);
     * everything else goes through the INT 0x2E gateway. */
    if (is_ftol_helper(safe_name))
        emit_ftol_stub(stub);
    else
        emit_thunk(stub, target, num_args, callconv);

    /* Record in table */
    thunk_table[idx].thunk_addr  = stub_addr;
    thunk_table[idx].target_addr = target;
    thunk_table[idx].num_args    = num_args;
    thunk_table[idx].callconv    = callconv;
    /* Import names live inside PE mappings and disappear on module unload. */
    thunk_table[idx].name        = owned_name;

    thunk_count++;
    spin_unlock(&thunk_table_lock);

    return stub_addr;
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
        { "SetHandleInformation",  3 },
        { "MulDiv",                3 },
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
        { "QueryUnbiasedInterruptTimePrecise", 1 },
        { "AcquireSRWLockShared",      1 },
        { "TryAcquireSRWLockShared",   1 },
        { "ReleaseSRWLockShared",      1 },
        { "EnumResourceNamesW",        4 },
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
        /* Atom APIs take 1 arg (3 for GetAtomName). Without these, the default
         * of 4 args makes the stdcall thunk RET 16 and over-clean the stack by
         * 12 bytes per call → ESP imbalance → caller's callee-saved EDI/EBX
         * corrupt. UT99's UWindowsClient ctor calls GlobalAddAtomW 4x for key
         * names ("UnrealAltEsc"…), which was crashing the render-device load. */
        { "GlobalAddAtomW",       1 }, { "GlobalAddAtomA",       1 },
        { "GlobalFindAtomW",      1 }, { "GlobalFindAtomA",      1 },
        { "GlobalDeleteAtom",     1 }, { "AddAtomW",             1 },
        { "AddAtomA",             1 }, { "FindAtomW",            1 },
        { "FindAtomA",            1 }, { "DeleteAtom",           1 },
        { "GlobalGetAtomNameW",   3 }, { "GlobalGetAtomNameA",   3 },
        /* ── ARGCOUNT systematic fix (Jun 3): stdcall Win32 APIs whose real
         * arg count != the default of 4. Wrong counts over/under-clean the
         * stack → caller callee-saved register / pool corruption (same class
         * as the GlobalAddAtomW bug). cdecl/CRT funcs are unaffected (caller
         * cleans) so they're omitted. */
        /* kernel32 */
        { "GetVersion",           0 }, { "GetSystemTime",        1 },
        { "SetLocalTime",         1 }, { "GlobalMemoryStatus",   1 },
        { "InterlockedIncrement", 1 }, { "InterlockedDecrement", 1 },
        { "SetErrorMode",         1 }, { "HeapDestroy",          1 },
        { "HeapCompact",          2 }, { "HeapValidate",         3 },
        { "HeapWalk",             2 }, { "GetFileInformationByHandle", 2 },
        { "DuplicateHandle",      7 }, { "GetExitCodeProcess",   2 },
        { "TerminateProcess",     2 }, { "SetEnvironmentVariableA", 2 },
        { "SetEnvironmentVariableW", 2 }, { "FreeEnvironmentStringsA", 1 },
        { "GetEnvironmentStrings", 0 }, { "GetProcessWorkingSetSize", 3 },
        { "QueryWorkingSetEx",     3 }, { "K32QueryWorkingSetEx", 3 },
        { "RemoveDirectoryA",     1 }, { "RemoveDirectoryW",     1 },
        { "FileTimeToSystemTime", 2 }, { "FileTimeToLocalFileTime", 2 },
        { "LocalFileTimeToFileTime", 2 }, { "SystemTimeToFileTime", 2 },
        { "SystemTimeToTzSpecificLocalTime", 3 },
        { "TzSpecificLocalTimeToSystemTime", 3 },
        { "SetConsoleCtrlHandler", 2 }, { "GetNumberOfConsoleInputEvents", 2 },
        { "PeekNamedPipe",        6 }, { "PeekConsoleInputA",    5 },
        { "ReadConsoleA",         5 }, { "ReadConsoleInputA",    5 },
        { "GetStringTypeA",       5 }, { "LCMapStringA",         6 },
        { "IsValidLocale",        3 }, { "EnumSystemLocalesA",   2 },
        { "Beep",                 2 }, { "CreateProcessA",       10 },
        { "CreateProcessW",       10 },
        /* ── ABI inventory (Jun 4): kernel32 stdcall fns verified absent from
         * this table → were silently getting default-4. Group (A) = GT<4
         * OVER-cleaners (RET 16 over-pops the caller stack → corrupts callee-
         * saved EDI/ESI/EBX = the exact GlobalAddAtomW crash class). Group (B) =
         * GT>4 under-cleaners (stale args left on stack). Ground truth from the
         * shim prototypes; see docs/win32-layer-correctness.md §3.9. */
        /* (A) over-cleaners */
        { "lstrlenA",             1 }, { "lstrlenW",             1 },
        { "GetSystemTimeAsFileTime", 1 }, { "IsDebuggerPresent", 0 },
        { "GetTickCount64",       0 }, { "GetEnvironmentStringsA", 0 },
        { "PulseEvent",           1 }, { "TryEnterCriticalSection", 1 },
        { "UnmapViewOfFile",      1 },
        /* (B) under-cleaners */
        { "CreateFileMappingA",   6 }, { "CreateFileMappingW",   6 },
        { "MapViewOfFile",        5 }, { "VirtualProtect",       4 },
        { "InterlockedExchange",  2 }, { "InterlockedCompareExchange", 3 },
        { "LoadLibraryExA",       3 }, { "LoadLibraryExW",       3 },
        { "InitializeCriticalSectionAndSpinCount", 2 },
        /* user32 */
        { "CheckMenuItem",        3 }, { "CloseClipboard",       0 },
        { "CreateDialogParamA",   5 }, { "CreateDialogParamW",   5 },
        { "DrawFocusRect",        2 }, { "DrawTextA",            5 },
        { "EmptyClipboard",       0 }, { "EndDialog",            2 },
        { "GetClipboardData",     1 }, { "GetDlgItem",           2 },
        { "GetMenu",              1 }, { "GetMenuItemCount",     1 },
        { "GetMenuState",         2 }, { "GetMessageTime",       0 },
        { "GetCaretBlinkTime",    0 }, { "GetGuiResources",       2 },
        { "GetKeyboardLayoutList", 2 },
        { "ImmGetIMEFileNameW",   3 },
        { "ImmGetIMEFileNameA",   3 }, { "ImmGetContext",         1 },
        { "ImmReleaseContext",    2 }, { "ImmAssociateContext",   2 },
        { "ImmGetCompositionStringW", 4 },
        { "ImmSetCompositionStringW", 6 },
        { "ImmGetCandidateListW", 4 }, { "ImmGetCompositionFontW", 2 },
        { "ImmNotifyIME",         4 }, { "ImmSetCompositionWindow", 2 },
        { "ImmSetCandidateWindow", 2 },
        { "GetSysColor",          1 }, { "GetWindowLongA",       2 },
        { "GetWindowThreadProcessId", 2 }, { "IsWindowVisible",  1 },
        { "LoadImageA",           6 }, { "LoadImageW",           6 },
        { "LoadMenuA",            2 },
        { "LoadMenuW",            2 }, { "OpenClipboard",        1 },
        { "SetClipboardData",     2 }, { "SetMenu",              2 },
        { "SetParent",            2 }, { "SetWindowLongA",       3 },
        { "TrackPopupMenu",       7 }, { "UnregisterHotKey",     2 },
        { "ValidateRect",         2 }, { "GetSubMenu",           2 },
        { "ChooseColorA",         1 }, { "DrawTextExA",          5 },
        { "DrawTextExW",          5 },
        /* gdi32 */
        { "CreateFontA",          14 }, { "CreateFontW",         14 },
        { "CreateFontIndirectW",   1 },
        { "CreatePen",            3 }, { "ExtTextOutA",          8 },
        { "GetPixel",             3 }, { "LineTo",               2 },
        { "PatBlt",               6 }, { "SetBkColor",           2 },
        { "SetBkMode",            2 }, { "SetTextColor",         2 },
        { "TextOutW",             5 },
        /* winmm / mci / joy / aux / mixer / wave */
        { "auxGetDevCapsA",       3 }, { "auxGetNumDevs",        0 },
        { "auxSetVolume",         2 }, { "joyGetDevCapsA",       3 },
        { "joyGetNumDevs",        0 }, { "joyGetPosEx",          2 },
        { "mixerGetDevCapsA",     3 }, { "mixerGetNumDevs",      0 },
        { "mixerGetLineInfoA",    3 }, { "mixerGetControlDetailsA", 3 },
        { "mixerSetControlDetails", 3 }, { "waveOutClose",       1 },
        { "waveOutGetDevCapsA",   3 }, { "waveOutGetPosition",   3 },
        { "waveOutOpen",          7 }, { "waveOutPrepareHeader", 3 },
        { "waveOutReset",         1 }, { "waveOutUnprepareHeader", 3 },
        { "waveOutWrite",         3 }, { "mciSendCommandA",      4 },
        /* ole32 / shell32 */
        { "CoCreateGuid",         1 }, { "CoCreateInstance",     5 },
        { "CoInitialize",         1 }, { "CoUninitialize",       0 },
        { "RegisterDragDrop",     2 }, { "RevokeDragDrop",       1 },
        { "ShellExecuteA",        6 }, { "ShellExecuteW",        6 },
        { "CommandLineToArgvW",   2 },
        { "Shell_NotifyIconA",    2 },
        { "HeapReAlloc",          4 }, { "HeapSize",             3 },
        { "RtlUnwind",            4 },
        { "RtlAddFunctionTable",  3 }, { "RtlDeleteFunctionTable", 1 },
        { "RtlLookupFunctionEntry", 3 },
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
        { "GetModuleHandleExA",   3 }, { "GetModuleHandleExW",   3 },
        { "IsProcessorFeaturePresent", 1 },
        { "GetTimeZoneInformation",    1 },
        { "GetDynamicTimeZoneInformation", 1 },
        { "FormatMessageA",       7 }, { "FormatMessageW",        7 },
        { "CompareStringA",       6 }, { "CompareStringW",        6 },
        { "GetDiskFreeSpaceA",    5 }, { "GetVolumeInformationA", 8 },
        { "GetTempPathA",         2 }, { "GetTempFileNameA",      4 },
        { "MoveFileA",            2 }, { "CopyFileA",             3 },
        { "GetFileAttributesA",   1 }, { "SetFileAttributesA",    2 },
        { "GetPrivateProfileSectionNamesA", 3 },
        { "GetComputerNameA",     2 }, { "GetComputerNameW",     2 },
        { "GetComputerNameExA",   3 }, { "GetComputerNameExW",   3 },
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
        { "MapVirtualKeyW",       2 }, { "ToUnicode",             6 },
        { "MessageBoxW",          4 },
        { "GetObjectW",           3 },
        /* MSVCRT — critical: _initterm with wrong args crashes! */
        { "_initterm",            2 }, { "_initterm_e",           2 },
        { "__dllonexit",          3 }, { "_onexit",               1 },
        { "_atexit",              1 }, { "atexit",                1 },
        { "_controlfp",           2 }, { "__set_app_type",        1 },
        { "_set_app_type",        1 },
        { "_set_fmode",           1 }, { "_get_fmode",            1 },
        { "_get_narrow_winmain_command_line", 0 },
        { "__p__fmode",           0 }, { "__p__commode",          0 },
        { "_adjust_fdiv",         0 }, { "__setusermatherr",      1 },
        { "_except_handler3",     4 }, { "_except_handler4",      4 },
        { "_setjmp",              1 }, { "_setjmp3",               2 },
        { "longjmp",              2 }, { "_longjmpex",             2 },
        { "InitializeSecurityDescriptor", 2 },
        { "RegOpenKeyExW",        5 }, { "RegQueryValueExW",      6 },
        { "RegSetValueExW",       6 }, { "RegCreateKeyExW",       9 },
        { "RegDeleteKeyW",        2 }, { "RegDeleteKeyExW",       4 },
        /* Completely missing functions */
        { "GetLogicalDrives",     0 },
        { "GetLogicalDriveStringsA", 2 }, { "GetLogicalDriveStringsW", 2 },
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
        { "MonitorFromRect",      2 },
        { "MoveWindow",           6 }, { "SetWindowPos",          7 },
        { "MessageBoxA",          4 }, { "GetAsyncKeyState",      1 },
        { "GetKeyState",          1 }, { "MapVirtualKeyA",        2 },
        { "SendInput",            3 },
        { "SetTimer",             4 }, { "KillTimer",             2 },
        { "ClipCursor",           1 }, { "GetClipCursor",         1 },
        { "SetCursorPos",         2 },
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
        { "SetClassLongA",        3 }, { "SetClassLongW",         3 },
        { "SetClassLongPtrA",     3 }, { "SetClassLongPtrW",      3 },
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
        { "GetDeviceGammaRamp",   2 }, { "SetDeviceGammaRamp",    2 },
        { "DeleteDC",             1 }, { "SelectObject",          2 },
        { "GetCurrentObject",     2 },
        { "CreateRectRgn",        4 }, { "SetRectRgn",            5 },
        { "CreateRectRgnIndirect", 1 }, { "CombineRgn",            4 },
        { "EqualRgn",             2 }, { "PtInRegion",            3 },
        { "RectInRegion",         2 }, { "GetRgnBox",             2 },
        { "OffsetRgn",            3 },
        { "SelectClipRgn",        2 }, { "SetTextAlign",          2 },
        { "GetObjectA",           3 }, { "DeleteObject",          1 },
        { "ChoosePixelFormat",    2 }, { "SetPixelFormat",        3 },
        { "CreateDIBitmap",       6 }, { "CreateBitmap",          5 },
        { "CreatePatternBrush",   1 }, { "CreateSolidBrush",      1 },
        { "GetStockObject",       1 }, { "GetObjectW",            3 },
        { "GetTextMetricsA",      2 }, { "GetTextMetricsW",       2 },
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
        { "RegDeleteKeyA",        2 }, { "RegDeleteKeyExA",       4 },

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
        { "_except_handler4_common", 6 },
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
    /* ARGCOUNT-AUDIT: log functions falling back to the default of 4 so we can
     * spot the next GlobalAddAtomW-style stack-imbalance bug (a stdcall API
     * whose real arg count != 4 over/under-cleans the stack → caller
     * callee-saved register / pool corruption). Logged once per import patch. */
#ifndef OK_QUIET
    serial_puts("[ARGCOUNT-DEFAULT4] ");
    serial_puts(name);
    serial_puts("\n");
#endif
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
    return dll_is_shim(dll_name);
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

    if (!compat32_is_initialized()) {
        serial_puts("[COMPAT32] Thunk pool not initialized\n");
        return STATUS_UNSUCCESSFUL;
    }
    if (compat32_map_runtime_current() != 0) {
        serial_puts("[COMPAT32] Thunk runtime map failed\n");
        return STATUS_CONFLICTING_ADDRESSES;
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

    int saved_compat_mode = g_compat32_mode;
    g_compat32_mode = 1;

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
                 * Preserve the imported function's 32-bit stack contract even
                 * when its implementation is optional. A plain RET corrupts
                 * ESP for stdcall imports because their arguments remain.
                 */
                uint32_t thunk_addr = 0;
                if (func_name) {
                    uint8_t nargs, abi_cc;
                    if (!win32_abi_lookup(dll_name, func_name, &nargs, &abi_cc)) {
                        nargs = guess_num_args(func_name);
                        abi_cc = cc;
                    }
                    thunk_addr = compat32_make_thunk_ex(
                        (uint64_t)(ULONG_PTR)unresolved_import_zero,
                        func_name, nargs, abi_cc);
                }
                iat_entry->u1.Function = thunk_addr ? thunk_addr
                                                   : unresolved_stub_addr;
                continue;
            }

            /* A compatibility fallback may resolve an unregistered provider
             * (for example MSVCR100.dll) through one of our kernel shims.
             * Classify the resolved target as well as the import DLL name:
             * writing a truncated FFFF8000... kernel pointer into a PE32 IAT
             * produces an unmapped low-address jump. */
            BOOL kernel_target =
                (uint64_t)(ULONG_PTR)resolved >= KERNEL_VBASE;
            if (shim || kernel_target) {
                /*
                 * Import from a shim DLL (64-bit kernel code).
                 * Check for fast-math native 32-bit implementations first.
                 * These run directly in compat mode (no INT 0x2E overhead).
                 */
                extern uint32_t g_fast_CIpow_addr, g_fast_CIfmod_addr,
                                g_fast_CIacos_addr, g_fast_fabs_addr,
                                g_fast_sqrt_addr;
                if (func_name && g_fast_CIpow_addr) {
                    uint32_t fast = 0;
                    if (func_name[0]=='f' && func_name[1]=='a' &&
                        func_name[2]=='b' && func_name[3]=='s' && !func_name[4])
                        fast = g_fast_fabs_addr;
                    else if (func_name[0]=='s' && func_name[1]=='q' &&
                             func_name[2]=='r' && func_name[3]=='t' && !func_name[4])
                        fast = g_fast_sqrt_addr;
                    else if (func_name[0]=='_' && func_name[1]=='C' && func_name[2]=='I') {
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
                    /* Function import — create INT 0x2E thunk.
                     * Phase 1: prefer the co-located ABI descriptor (argc + cc
                     * from the shim's own export table / MSVC demangle). The
                     * name-keyed guess_num_args (with its default-4) is only a
                     * fallback until every shim table is migrated. */
                    uint64_t target64 = (uint64_t)(ULONG_PTR)resolved;
                    const char *thunk_name = func_name;
                    uint8_t nargs, abi_cc;
                    if (func_name &&
                        win32_abi_lookup(dll_name, func_name, &nargs, &abi_cc)) {
                        cc = abi_cc;  /* co-located convention wins */
                    } else if (!func_name && win32_abi_lookup_target(
                                   dll_name, resolved, &thunk_name,
                                   &nargs, &abi_cc)) {
                        cc = abi_cc;
                    } else {
                        nargs = func_name ? guess_num_args(func_name) : 4;
                    }
                    uint32_t thunk_addr = compat32_make_thunk_ex(
                        target64, thunk_name, nargs, cc);
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
                volatile uint32_t *iat_ptr =
                    (volatile uint32_t *)&iat_entry->u1.Function;
                LOADED_MODULE *target_module =
                    dll_find_module_by_address(resolved);
                if (!target_module || !target_module->image.Is32Bit) {
                    serial_puts("[IAT-WARN] non-PE32 direct target: ");
                    if (func_name) serial_puts(func_name);
                    serial_puts(" from ");
                    serial_puts(dll_name);
                    serial_puts(" -> 0x");
                    serial_puthex((uint32_t)(ULONG_PTR)resolved, 8);
                    serial_puts("\n");
                    *iat_ptr = unresolved_stub_addr;
                    continue;
                }
                *iat_ptr = (uint32_t)(ULONG_PTR)resolved;
                direct++;

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

    g_compat32_mode = saved_compat_mode;
    return STATUS_SUCCESS;
}

/* Initialize one PE32 module's static TLS before its entry point runs. */
NTSTATUS compat32_attach_tls(PE_IMAGE_INFO *info)
{
    if (!info || !info->ImageBase || !info->Is32Bit)
        return STATUS_INVALID_PARAMETER;

    int process_bits = dll_current_process_bitness();
    if (process_bits && process_bits != 32) {
        serial_puts("[TLS32] refusing PE32 TLS in PE64 process\n");
        return STATUS_INVALID_IMAGE_FORMAT;
    }

    BYTE *base = (BYTE *)info->ImageBase;
    uint64_t image_start = (uint64_t)(ULONG_PTR)base;
    uint64_t image_end = image_start + info->SizeOfImage;
    if (info->SizeOfImage < sizeof(IMAGE_DOS_HEADER))
        return STATUS_INVALID_PARAMETER;

    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        (ULONG)dos->e_lfanew > info->SizeOfImage - sizeof(IMAGE_NT_HEADERS32))
        return STATUS_INVALID_PARAMETER;

    PIMAGE_NT_HEADERS32 nt =
        (PIMAGE_NT_HEADERS32)(base + (ULONG)dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        return STATUS_INVALID_PARAMETER;

    if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_TLS)
        return STATUS_SUCCESS;

    IMAGE_DATA_DIRECTORY *dir =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (!dir->VirtualAddress || !dir->Size)
        return STATUS_SUCCESS;
    if (dir->Size < sizeof(IMAGE_TLS_DIRECTORY32) ||
        dir->VirtualAddress > info->SizeOfImage - sizeof(IMAGE_TLS_DIRECTORY32))
        return STATUS_INVALID_PARAMETER;

    PIMAGE_TLS_DIRECTORY32 tls =
        (PIMAGE_TLS_DIRECTORY32)(base + dir->VirtualAddress);
    uint64_t raw_start = tls->StartAddressOfRawData;
    uint64_t raw_end = tls->EndAddressOfRawData;
    uint64_t index_addr = tls->AddressOfIndex;
    uint64_t callbacks_addr = tls->AddressOfCallBacks;

    if (raw_end < raw_start ||
        (raw_end != raw_start &&
         (raw_start < image_start || raw_end > image_end)) ||
        index_addr < image_start || index_addr > image_end - sizeof(uint32_t))
        return STATUS_INVALID_PARAMETER;

    uint64_t raw_size = raw_end - raw_start;
    uint64_t total_size = raw_size + tls->SizeOfZeroFill;
    if (total_size < raw_size || total_size > 16ULL * 1024 * 1024)
        return STATUS_NO_MEMORY;

    uint32_t callback_count = 0;
    if (callbacks_addr) {
        while (callback_count < 64) {
            uint64_t entry_addr = callbacks_addr +
                                  callback_count * sizeof(uint32_t);
            if (entry_addr < image_start ||
                entry_addr > image_end - sizeof(uint32_t))
                return STATUS_INVALID_PARAMETER;
            uint32_t callback = *(uint32_t *)(ULONG_PTR)entry_addr;
            if (!callback) break;
            if ((uint64_t)callback < image_start ||
                (uint64_t)callback >= image_end)
                return STATUS_INVALID_PARAMETER;
            callback_count++;
        }
        if (callback_count == 64)
            return STATUS_INVALID_PARAMETER;
    }

    uint64_t pages = (total_size + 4095) / 4096;
    if (!pages) pages = 1;

    int saved_compat_mode = g_compat32_mode;
    g_compat32_mode = 1;
    BYTE *block = (BYTE *)VirtualAlloc(NULL, pages * 4096,
                                       MEM_RESERVE | MEM_COMMIT,
                                       PAGE_READWRITE);
    if (!block || (uint64_t)(ULONG_PTR)block > UINT32_MAX) {
        if (block) VirtualFree(block, 0, MEM_RELEASE);
        g_compat32_mode = saved_compat_mode;
        return STATUS_NO_MEMORY;
    }

    for (uint64_t i = 0; i < pages * 4096; i++) block[i] = 0;
    for (uint64_t i = 0; i < raw_size; i++)
        block[i] = *(BYTE *)(ULONG_PTR)(raw_start + i);

    DWORD index = TlsAlloc();
    if (index == (DWORD)-1) {
        VirtualFree(block, 0, MEM_RELEASE);
        g_compat32_mode = saved_compat_mode;
        return STATUS_NO_MEMORY;
    }
    if (!TlsSetValue(index, block)) {
        TlsFree(index);
        VirtualFree(block, 0, MEM_RELEASE);
        g_compat32_mode = saved_compat_mode;
        return STATUS_NO_MEMORY;
    }
    *(uint32_t *)(ULONG_PTR)index_addr = index;

    extern BOOL win32_tls_register_static(DWORD, uint32_t, uint32_t,
                                           uint32_t, uint32_t, uint32_t,
                                           uint32_t);
    if (!win32_tls_register_static(index,
            (uint32_t)(ULONG_PTR)info->ImageBase,
            (uint32_t)raw_start, (uint32_t)raw_size,
            (uint32_t)total_size, (uint32_t)callbacks_addr,
            callback_count)) {
        TlsFree(index);
        VirtualFree(block, 0, MEM_RELEASE);
        g_compat32_mode = saved_compat_mode;
        return STATUS_NO_MEMORY;
    }
    g_compat32_mode = saved_compat_mode;

    serial_puts("[TLS32] index@0x");
    serial_puthex(index_addr, 8);
    serial_puts("=0x");
    serial_puthex(*(uint32_t *)(ULONG_PTR)index_addr, 8);
    serial_puts("\n");

    serial_puts("[TLS32] slot=");
    serial_putdec(index);
    serial_puts(" template=0x");
    serial_puthex(raw_size, 8);
    serial_puts(" block=0x");
    serial_puthex((uint64_t)(ULONG_PTR)block, 8);
    serial_puts("\n");

    for (uint32_t i = 0; i < callback_count; i++) {
        uint32_t callback =
            *(uint32_t *)(ULONG_PTR)(callbacks_addr + i * sizeof(uint32_t));
        uint32_t args[3] = {
            (uint32_t)(ULONG_PTR)info->ImageBase,
            DLL_PROCESS_ATTACH,
            0
        };
        compat32_callback_args(callback, 3, args);
    }

    serial_puts("[TLS32] post-callback index=0x");
    serial_puthex(*(uint32_t *)(ULONG_PTR)index_addr, 8);
    serial_puts("\n");
#ifndef TEST_HARNESS
    if (index_addr == 0x008571A8ULL) {
        extern int hwbp_set(int, uint64_t, int, int, const char *);
        hwbp_set(2, index_addr, 1 /* write */, 3 /* 4 bytes */, "tlsidx");
    }
#endif

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
    extern void proc_set_fs_base(uint64_t addr);
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
    proc_set_fs_base(addr);
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

extern TEB32 g_teb32;

TEB32 *compat32_current_teb(void)
{
#ifndef TEST_HARNESS
    extern uint64_t proc_get_fs_base(void);
    uint64_t addr = proc_get_fs_base();
    if (addr) return (TEB32 *)(ULONG_PTR)addr;
#endif
    return &g_teb32;
}

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

    /* Mask APIC timer for UT99's compat32 execution. UT99
     * runs almost entirely in 32-bit user code on a low-half
     * stack; if the timer ISR fires there, it saves the GP frame on
     * that low-half stack and the scheduler stores frame_ptr (a low-
     * half address) in PID 1->kernel_rsp. Subsequent user-mode writes
     * to that same memory overwrite the saved frame, and the next
     * dispatch reads garbage as CS/RIP/SS/RSP — `[SCHED] CORRUPT PID
     * 1 CS=0x1F10` style triple-fault.
     *
     * Other PE32 programs need the timer for waits and scheduling, so
     * explicitly leave it unmasked for them. */
    {
        extern volatile uint32_t *idt_get_apic_base(void);
        volatile uint32_t *apic = idt_get_apic_base();
        if (apic) {
            if (g_compat32_ut99)
                apic[0x320/4] |= 0x10000;   /* LVT_TIMER |= MASKED */
            else
                apic[0x320/4] &= ~0x10000;  /* keep scheduler/timers alive */
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
    callback_owner_state_t *callback_state = callback_state_get(1, NULL);
    if (!callback_state) return;

    /* Do not recycle live callback slots. A deep WndProc/message-pump nest can
     * legitimately approach the old limit; reusing slot 0 corrupts jmpbufs and
     * later returns through a stale callback_return_stub. */
    if (callback_state->depth >= MAX_CALLBACK_DEPTH) {
        serial_puts("[CB32] FATAL: callback depth overflow\n");
        return;
    }

    int depth = callback_state->depth++;
    callback_state->saved_stack_args[depth] =
        callback_state->current_stack_args;

    if (depth >= 16) {
        serial_puts("[CB32] depth=");
        serial_putdec(depth);
        serial_puts(" calling 0x");
        serial_puthex(func_addr, 8);
        serial_puts("\n");
    }

    /* Save IST1 before callback — longjmp bypasses int2e_stub's restore */
    {
        extern uint64_t *tss_ist1_ptr;
        if (tss_ist1_ptr) callback_state->saved_ist1[depth] = *tss_ist1_ptr;
    }

    /* Save SEH ExceptionList — 32-bit code may push SEH frames on the
     * callback stack. Restore on return so the main chain stays valid. */
    TEB32 *teb = compat32_current_teb();
    uint32_t saved_seh = teb->ExceptionList;

    /* PE32 callbacks run on a low callback stack. Keep timer preemption out
     * until that stack has returned through the callback gate. */
    extern volatile uint32_t *idt_get_apic_base(void);
    volatile uint32_t *callback_apic = idt_get_apic_base();
    uint32_t saved_callback_timer = callback_apic ? callback_apic[0x320/4] : 0;
    if (callback_apic)
        callback_apic[0x320/4] = saved_callback_timer | 0x10000;

    if (kern_setjmp(callback_state->jmpbufs[depth]) == 0) {
        /*
         * First return from setjmp — switch to compat mode.
         * Set up a small stack with the return stub as return address,
         * then LRETQ to the 32-bit function.
         */
        uint8_t *stack = callback_stack_get(callback_state, depth);
        if (!stack) {
            if (!g_compat32_ut99 && callback_apic)
                callback_apic[0x320/4] = saved_callback_timer;
            teb->ExceptionList = saved_seh;
            callback_state->current_stack_args =
                callback_state->saved_stack_args[depth];
            callback_state->depth--;
            return;
        }
        uint32_t *sp = (uint32_t *)(stack + CALLBACK_STACK_SIZE);
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

    /* UT99 owns a lifetime mask; other callers get their prior state back. */
    if (!g_compat32_ut99 && callback_apic)
        callback_apic[0x320/4] = saved_callback_timer;

    teb->ExceptionList = saved_seh;  /* Restore SEH chain */
    callback_state->current_stack_args =
        callback_state->saved_stack_args[depth];
    callback_state->depth--;
    if (depth >= 16) {
        serial_puts("[CB32] depth=");
        serial_putdec(depth);
        serial_puts(" returned\n");
    }
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
static uint32_t compat32_callback_args_impl(uint32_t func_addr, int nargs,
                                             const uint32_t *args,
                                             uint32_t stack_top,
                                             uint32_t frame_ebp,
                                             bool mask_timer)
{
#ifndef TEST_HARNESS
    if (!callback_return_stub_addr) return 0;
    callback_owner_state_t *callback_state = callback_state_get(1, NULL);
    if (!callback_state) return 0;

    if (callback_state->depth >= MAX_CALLBACK_DEPTH) {
        serial_puts("[CB32] FATAL: callback depth overflow\n");
        return 0;
    }

    int depth = callback_state->depth++;
    callback_state->retvals[depth] = 0;
    callback_state->saved_stack_args[depth] =
        callback_state->current_stack_args;

    /* Save IST1 before callback — longjmp bypasses int2e_stub's restore */
    {
        extern uint64_t *tss_ist1_ptr;
        if (tss_ist1_ptr) callback_state->saved_ist1[depth] = *tss_ist1_ptr;
    }

    /* Save SEH ExceptionList — 32-bit code may push SEH frames on the
     * callback stack. Restore on return so the main chain stays valid. */
    TEB32 *teb = compat32_current_teb();
    uint32_t saved_seh = teb->ExceptionList;

    extern volatile uint32_t *idt_get_apic_base(void);
    volatile uint32_t *callback_apic = idt_get_apic_base();
    uint32_t saved_callback_timer = callback_apic ? callback_apic[0x320/4] : 0;
    if (mask_timer && callback_apic)
        callback_apic[0x320/4] = saved_callback_timer | 0x10000;

    if (kern_setjmp(callback_state->jmpbufs[depth]) == 0) {
        uint8_t *stack = stack_top ? NULL :
                         callback_stack_get(callback_state, depth);
        if (!stack_top && !stack) {
            if (mask_timer && !g_compat32_ut99 && callback_apic)
                callback_apic[0x320/4] = saved_callback_timer;
            teb->ExceptionList = saved_seh;
            callback_state->current_stack_args =
                callback_state->saved_stack_args[depth];
            callback_state->depth--;
            return 0;
        }
        uint32_t *sp = stack_top
                     ? (uint32_t *)(uintptr_t)stack_top
                     : (uint32_t *)(stack + CALLBACK_STACK_SIZE);

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
        uint64_t bp64 = frame_ebp;

        __asm__ volatile (
            "movw $0x48, %%ax\n"    /* GDT_SEL_DATA32 */
            "mov %%ax, %%ds\n"
            "mov %%ax, %%es\n"
            "mov %%ax, %%ss\n"
            "test %[bp], %[bp]\n"
            "jz 1f\n"
            "mov %[bp], %%rbp\n"
            "1:\n"
            "mov %[sp], %%rsp\n"
            "sti\n"
            "push %[cs]\n"
            "push %[ip]\n"
            "lretq\n"
            :
            : [cs] "r"(cs64),
              [ip] "r"(ip64),
              [sp] "r"(sp64),
              [bp] "r"(bp64)
            : "memory", "cc", "rax"
        );
        /* never reached */
    }

    if (mask_timer && !g_compat32_ut99 && callback_apic)
        callback_apic[0x320/4] = saved_callback_timer;

    teb->ExceptionList = saved_seh;  /* Restore SEH chain */
    callback_state->current_stack_args =
        callback_state->saved_stack_args[depth];
    callback_state->depth--;
    return callback_state->retvals[depth];
#else
    /* Test harness: call directly */
    (void)frame_ebp;
    (void)mask_timer;
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

uint32_t compat32_callback_args(uint32_t func_addr, int nargs,
                                const uint32_t *args)
{
    return compat32_callback_args_impl(func_addr, nargs, args, 0, 0, true);
}

uint32_t compat32_callback_args_with_ebp(uint32_t func_addr, int nargs,
                                         const uint32_t *args,
                                         uint32_t frame_ebp)
{
    return compat32_callback_args_impl(func_addr, nargs, args, 0, frame_ebp,
                                       true);
}

uint32_t compat32_callback_args_on_stack(uint32_t func_addr, int nargs,
                                         const uint32_t *args,
                                         uint32_t stack_top)
{
    return compat32_callback_args_impl(func_addr, nargs, args,
                                       stack_top & ~0xFULL, 0, true);
}

uint32_t compat32_thread_entry_on_stack(uint32_t func_addr, int nargs,
                                         const uint32_t *args,
                                         uint32_t stack_top)
{
    /* A Win32 thread entry may run for the process lifetime. Its dedicated
     * scheduler-owned stack is safe to preempt, unlike a nested callback
     * stack. Masking the local APIC here would starve every kernel task until
     * the PE thread exits. */
    return compat32_callback_args_impl(func_addr, nargs, args,
                                       stack_top & ~0xFULL, 0, false);
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

#define CXX32_MAX_TYPES           64
#define CXX32_MAX_UNWIND_ACTIONS  32
#define CXX32_MAX_TYPE_NAME       256

typedef struct __attribute__((packed)) {
    uint32_t attributes;
    uint32_t unwind;
    uint32_t forward_compat;
    uint32_t catchable_types;
} CXX_THROW_INFO32;

typedef struct __attribute__((packed)) {
    uint32_t properties;
    uint32_t type;
    int32_t mdisp;
    int32_t pdisp;
    int32_t vdisp;
    int32_t size_or_offset;
    uint32_t copy_function;
} CXX_CATCHABLE_TYPE32;

typedef struct __attribute__((packed)) {
    int32_t to_state;
    uint32_t action;
} CXX_UNWIND_MAP_ENTRY32;

static int seh32_range_readable(uint32_t address, uint32_t size)
{
    if (address < 0x10000U || size == 0)
        return 0;

    uint32_t end = address + size - 1U;
    if (end < address || end >= 0x80000000U)
        return 0;

#ifndef TEST_HARNESS
    extern uint64_t proc_current_cr3(void);
    uint64_t cr3 = proc_current_cr3();
    if (!cr3)
        return 0;

    uint64_t page = (uint64_t)address & ~0xFFFULL;
    uint64_t last = (uint64_t)end & ~0xFFFULL;
    for (;;) {
        if (paging_translate_in_cr3(cr3, page) == UINT64_MAX)
            return 0;
        if (page == last)
            break;
        page += 0x1000ULL;
    }
#endif
    return 1;
}

int compat32_range_readable(uint32_t address, uint32_t size)
{
    return seh32_range_readable(address, size);
}

#define MSVC_JMPBUF_COOKIE 0x56433230U

/* _setjmp/_setjmp3 cannot be implemented by an ordinary 64-bit shim: they
 * capture the i386 caller's register file and return address. Likewise,
 * longjmp must replace the pending iret frame instead of returning through
 * the thunk. The marker targets exported by msvcrt_shim route those calls
 * here while the original PE32 state is still available. */
static int compat32_dispatch_nonlocal_jump(uint64_t target,
                                           uint32_t *stack_args,
                                           uint64_t *result)
{
    const uint64_t setjmp_target =
        (uint64_t)(ULONG_PTR)crt_compat32_setjmp_marker;
    const uint64_t setjmp3_target =
        (uint64_t)(ULONG_PTR)crt_compat32_setjmp3_marker;
    const uint64_t longjmp_target =
        (uint64_t)(ULONG_PTR)crt_compat32_longjmp_marker;

    if (target != setjmp_target && target != setjmp3_target &&
        target != longjmp_target)
        return 0;

    if (!stack_args || !result)
        return 0;

    uint32_t env_addr = stack_args[0];
    uint32_t required_dwords = target == setjmp3_target ? 10U : 8U;

    if (target == setjmp3_target) {
        uint32_t count = stack_args[1];
        uint32_t copied = count > 2U ? count - 2U : 0U;
        if (copied > 6U) copied = 6U;
        required_dwords += copied;

        uint32_t input_count = count > 8U ? 8U : count;
        uint32_t input_addr = (uint32_t)(uintptr_t)stack_args;
        if (!seh32_range_readable(input_addr,
                                  (2U + input_count) * sizeof(uint32_t))) {
            serial_puts("[MSVCRT-JMP] invalid _setjmp3 argument range\n");
            *result = 0;
            return 1;
        }
    }

    if (!seh32_range_readable(env_addr,
                              required_dwords * sizeof(uint32_t))) {
        serial_puts("[MSVCRT-JMP] invalid jump buffer 0x");
        serial_puthex(env_addr, 8);
        serial_puts("\n");
        *result = 0;
        return 1;
    }

    uint32_t *env = (uint32_t *)(uintptr_t)env_addr;
    TEB32 *teb = compat32_current_teb();

    if (target == setjmp_target || target == setjmp3_target) {
        uint32_t registration = teb ? teb->ExceptionList : 0xFFFFFFFFU;
        uint32_t try_level = 0xFFFFFFFFU;

        if (registration != 0xFFFFFFFFU &&
            seh32_range_readable(registration, 4U * sizeof(uint32_t)))
            try_level = ((uint32_t *)(uintptr_t)registration)[3];

        env[0] = (uint32_t)g_int2e_user_rbp;
        env[1] = (uint32_t)g_int2e_user_rbx;
        env[2] = (uint32_t)g_int2e_user_rdi;
        env[3] = (uint32_t)g_int2e_user_rsi;
        env[4] = (uint32_t)(uintptr_t)(stack_args - 1);
        env[5] = stack_args[-1];
        env[6] = registration;
        env[7] = try_level;

        if (target == setjmp3_target) {
            uint32_t count = stack_args[1];
            env[8] = MSVC_JMPBUF_COOKIE;
            env[9] = 0;

            if (registration != 0xFFFFFFFFU && count != 0) {
                env[9] = stack_args[2];
                count--;
                if (count != 0) {
                    env[7] = stack_args[3];
                    count--;
                    if (count > 6U) count = 6U;
                    for (uint32_t i = 0; i < count; i++)
                        env[10 + i] = stack_args[4 + i];
                }
            }
        }

        *result = 0;
        return 1;
    }

    uint32_t return_eip = env[5];
    uint32_t saved_esp = env[4];
    if (!seh32_range_readable(return_eip, 1) ||
        !seh32_range_readable(saved_esp, sizeof(uint32_t))) {
        serial_puts("[MSVCRT-JMP] invalid longjmp target eip=0x");
        serial_puthex(return_eip, 8);
        serial_puts(" esp=0x");
        serial_puthex(saved_esp, 8);
        serial_puts("\n");
        *result = 0;
        return 1;
    }

    uint32_t registration = env[6];
    uint32_t unwind_function = 0;
    if (seh32_range_readable(env_addr, 10U * sizeof(uint32_t)) &&
        env[8] == MSVC_JMPBUF_COOKIE)
        unwind_function = env[9];

    /* _setjmp3 may provide the compiler's local-unwind helper. Run it on the
     * active PE32 stack before discarding that stack frame. */
    if (unwind_function && seh32_range_readable(unwind_function, 1)) {
        uint32_t unwind_arg = env_addr;
        uint32_t stack_top = (uint32_t)(uintptr_t)(stack_args - 1);
        (void)compat32_callback_args_on_stack(unwind_function, 1,
                                              &unwind_arg, stack_top);
    }

    if (teb) {
        if (registration == 0xFFFFFFFFU || registration == 0 ||
            seh32_range_readable(registration, 4U * sizeof(uint32_t))) {
            teb->ExceptionList = registration;

            /* The plain _setjmp path relies on _local_unwind2. There is no
             * custom helper in that buffer, so retain the saved EH3 state
             * when the target registration has the standard frame layout. */
            if (!unwind_function && registration != 0xFFFFFFFFU &&
                registration != 0)
                ((uint32_t *)(uintptr_t)registration)[3] = env[7];
        }
    }

    g_compat32_unwind_ebp = env[0];
    g_compat32_unwind_ebx = env[1];
    g_compat32_unwind_edi = env[2];
    g_compat32_unwind_esi = env[3];
    g_compat32_unwind_esp = saved_esp + sizeof(uint32_t);
    g_compat32_unwind_restore_nonvolatile = 1;
    g_compat32_unwind_eip = return_eip;

    *result = stack_args[1] ? stack_args[1] : 1U;
    return 1;
}

static int cxx32_type_name_equal(uint32_t left_type, uint32_t right_type)
{
    /* TypeDescriptor is { vftable, spare, name[] }. Keep the fallback
     * bounded; identical descriptors normally match by pointer first. */
    const uint32_t bytes = 8U + CXX32_MAX_TYPE_NAME;
    if (!seh32_range_readable(left_type, bytes) ||
        !seh32_range_readable(right_type, bytes))
        return 0;

    const char *left = (const char *)(uintptr_t)(left_type + 8U);
    const char *right = (const char *)(uintptr_t)(right_type + 8U);
    for (uint32_t i = 0; i < CXX32_MAX_TYPE_NAME; i++) {
        if (left[i] != right[i])
            return 0;
        if (left[i] == '\0')
            return 1;
    }
    return 0;
}

static int cxx32_handler_matches(const EXCEPTION_RECORD32 *record,
                                 uint32_t handler_type,
                                 uint32_t *matched_type)
{
    if (matched_type)
        *matched_type = 0;
    if (!record || record->NumberParameters < 3 || !handler_type)
        return 0;

    uint32_t throw_info_addr = record->ExceptionInformation[2];
    if (!seh32_range_readable(throw_info_addr, sizeof(CXX_THROW_INFO32)))
        return 0;

    const CXX_THROW_INFO32 *throw_info =
        (const CXX_THROW_INFO32 *)(uintptr_t)throw_info_addr;
    uint32_t array_addr = throw_info->catchable_types;
    if (!seh32_range_readable(array_addr, sizeof(int32_t)))
        return 0;

    int32_t count = *(const int32_t *)(uintptr_t)array_addr;
    if (count <= 0 || count > CXX32_MAX_TYPES ||
        !seh32_range_readable(array_addr,
                              sizeof(int32_t) + (uint32_t)count * sizeof(uint32_t)))
        return 0;

    const uint32_t *types =
        (const uint32_t *)(uintptr_t)(array_addr + sizeof(int32_t));
    for (int32_t i = 0; i < count; i++) {
        uint32_t catchable_addr = types[i];
        if (!seh32_range_readable(catchable_addr,
                                  sizeof(CXX_CATCHABLE_TYPE32)))
            continue;

        const CXX_CATCHABLE_TYPE32 *catchable =
            (const CXX_CATCHABLE_TYPE32 *)(uintptr_t)catchable_addr;
        uint32_t thrown_type = catchable->type;
        if (!thrown_type)
            continue;
        if (thrown_type == handler_type ||
            cxx32_type_name_equal(thrown_type, handler_type)) {
            if (matched_type)
                *matched_type = catchable_addr;
            return 1;
        }
    }
    return 0;
}

static int cxx32_collect_unwind_actions(uint32_t unwind_map,
                                        int32_t max_state,
                                        int32_t current_state,
                                        int32_t target_state,
                                        uint32_t *actions,
                                        uint32_t *action_count)
{
    *action_count = 0;
    if (current_state == target_state)
        return 1;
    if (!unwind_map || max_state <= 0 || max_state > 4096 ||
        current_state < 0 || current_state >= max_state ||
        target_state < -1 || target_state >= current_state ||
        !seh32_range_readable(unwind_map,
                              (uint32_t)max_state *
                                  sizeof(CXX_UNWIND_MAP_ENTRY32)))
        return 0;

    int32_t state = current_state;
    for (int32_t steps = 0; state > target_state && steps <= max_state; steps++) {
        if (state < 0 || state >= max_state)
            return 0;

        const CXX_UNWIND_MAP_ENTRY32 *entry =
            (const CXX_UNWIND_MAP_ENTRY32 *)(uintptr_t)(
                unwind_map + (uint32_t)state *
                                 sizeof(CXX_UNWIND_MAP_ENTRY32));
        if (entry->to_state >= state || entry->to_state < -1)
            return 0;

        if (entry->action) {
            if (*action_count >= CXX32_MAX_UNWIND_ACTIONS ||
                !seh32_range_readable(entry->action, 1))
                return 0;
            actions[(*action_count)++] = entry->action;
        }
        state = entry->to_state;
    }
    return state == target_state;
}

/* Accessor for msvcrt_shim.c's crt_except_handler3 compat32 path */
PVOID seh32_ep_addr_for_filter(void)
{
    callback_owner_state_t *state = callback_state_get(0, NULL);
    seh32_dispatch_slot_t *slot = seh32_current_slot(state);
    return slot ? (PVOID)&slot->exception_pointers : NULL;
}

static int compat32_seh_dispatch_impl(PEXCEPTION_RECORD ExceptionRecord,
                                      callback_owner_state_t *state)
{
    TEB32 *teb = compat32_current_teb();
    seh32_dispatch_slot_t *slot = seh32_current_slot(state);
    if (!teb || !slot) {
        serial_puts("[SEH32] dispatcher state unavailable\n");
        return 0;
    }
    EXCEPTION_RECORD32 *record32 = &slot->exception_record;
    EXCEPTION_POINTERS32 *pointers32 = &slot->exception_pointers;
    CONTEXT32 *context32 = &slot->context;

    /* Track reentrant catch dispatch.
     * When a catch handler throws (e.g., appUnwindf re-throws), skip the
     * current frame (which is corrupted by the first dispatch) and start
     * searching from the NEXT frame in the SEH chain. */
    if (state->in_catch_dispatch &&
        ExceptionRecord->ExceptionCode == 0xE06D7363) {
        serial_puts("[SEH32] Re-throw from catch — using saved next frame 0x");
        serial_puthex(state->saved_next_frame, 8);
        serial_puts("\n");
        state->in_catch_dispatch = 0;
        /* Use the saved Next (from before the catch handler corrupted the frame) */
        teb->ExceptionList = state->saved_next_frame;
    } else if (state->in_catch_dispatch) {
        /* A hardware fault raised by a catch funclet is a new exception, not
         * C++ `throw;`. The catching frame was already unlinked before entry,
         * so continue from the live chain without rewriting it. */
        state->in_catch_dispatch = 0;
        state->saved_next_frame = 0;
    }

    /* Read the 32-bit ExceptionList from the current thread's TEB. */
    uint32_t frame_addr = teb->ExceptionList;

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
        serial_puts(" teb = 0x");
        serial_puthex((uint64_t)(ULONG_PTR)teb, 16);
        serial_puts(" *FS[0] = 0x");
        if (fs_base)
            serial_puthex(*(const uint32_t *)(ULONG_PTR)fs_base, 8);
        else
            serial_puts("<unavailable>");
        serial_puts(" teb->ExceptionList=0x");
        serial_puthex(teb ? teb->ExceptionList : 0, 8);
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
    BYTE *p = (BYTE *)record32;
    for (SIZE_T i = 0; i < sizeof(*record32); i++) p[i] = 0;
    record32->ExceptionCode = ExceptionRecord->ExceptionCode;
    record32->ExceptionFlags = ExceptionRecord->ExceptionFlags;
    record32->ExceptionAddress =
        (uint32_t)(ULONG_PTR)ExceptionRecord->ExceptionAddress;
    record32->NumberParameters = ExceptionRecord->NumberParameters;
    for (DWORD i = 0; i < ExceptionRecord->NumberParameters && i < 15; i++)
        record32->ExceptionInformation[i] =
            (uint32_t)ExceptionRecord->ExceptionInformation[i];

    /* Build 32-bit EXCEPTION_POINTERS */
    pointers32->ExceptionRecord = (uint32_t)(ULONG_PTR)record32;
    pointers32->ContextRecord = context32->ContextFlags
        ? (uint32_t)(ULONG_PTR)context32 : 0;

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
                        uint32_t ep_addr = (uint32_t)(ULONG_PTR)pointers32;
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
                            uint32_t uw_addr = teb->ExceptionList;
                            while (uw_addr != 0xFFFFFFFF && uw_addr != frame_addr) {
                                uint32_t *uw32 = (uint32_t *)(ULONG_PTR)uw_addr;
                                uw_addr = uw32[0]; /* skip to next */
                            }
                            /* Set this frame as new chain head */
                            teb->ExceptionList = frame_addr;

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
             * Check for an MSVC C++ EH handler thunk. /GS wrappers can run
             * cookie checks before loading FuncInfo and tail-jumping to the
             * CRT frame handler.
             *
             * If detected, parse FuncInfo directly and dispatch
             * to the matching catch block without calling the handler
             * (which needs a valid CONTEXT we can't easily provide).
             */
            uint32_t func_info_addr = crt_find_cxx_func_info(handler32);
            if (func_info_addr) {

                /* __CxxFrameHandler3 only searches typed/catch-all handlers
                 * for an MSVC C++ exception. Hardware faults continue through
                 * the ordinary SEH chain. */
                if (ExceptionRecord->ExceptionCode != 0xE06D7363) {
                    serial_puts(" (CxxFrameHandler, non-C++ exception)\n");
                    goto next_frame;
                }

                serial_puts(" (CxxFrameHandler thunk)\n");
                serial_puts("[SEH32] FuncInfo=0x");
                serial_puthex(func_info_addr, 8);

                /* Read FuncInfo: magic(4), maxState(4), pUnwindMap(4),
                 *                nTryBlocks(4), pTryBlockMap(4) */
                uint32_t *fi = (uint32_t *)(uintptr_t)func_info_addr;
                uint32_t magic = fi[0];
                int32_t maxState = (int32_t)fi[1];
                uint32_t pUnwindMap = fi[2];
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
                if (maxState <= 0 || maxState > 4096 ||
                    nTryBlocks <= 0 || nTryBlocks > 64 ||
                    !seh32_range_readable(pTryBlockMap,
                                           (uint32_t)nTryBlocks * 20U)) {
                    serial_puts("[SEH32] invalid C++ EH metadata, skipping\n");
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
                    int32_t catchHigh = (int32_t)tb[2];
                    int32_t nCatches = (int32_t)tb[3];
                    uint32_t pHandlerArray = tb[4];

                    if (cur_state < tryLow || cur_state > tryHigh)
                        continue;
                    if (nCatches <= 0 || nCatches > 64 ||
                        !seh32_range_readable(pHandlerArray,
                                               (uint32_t)nCatches * 16U)) {
                        serial_puts("[SEH32] invalid catch metadata, skipping\n");
                        continue;
                    }

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
                     *              dispCatchObj(4), addressOfHandler(4) = 16 bytes.
                     * Handlers are tested in source order. */
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
                        uint32_t matched_type = 0;
                        if (cxx32_handler_matches(record32,
                                                  pType, &matched_type)) {
                            catch_handler = addr;
                            catch_disp = disp;
                            serial_puts("[SEH32] typed catch type=0x");
                            serial_puthex(pType, 8);
                            serial_puts(" catchable=0x");
                            serial_puthex(matched_type, 8);
                            serial_puts(" handler=0x");
                            serial_puthex(addr, 8);
                            serial_puts("\n");
                            break;
                        }
                    }

                    if (catch_handler) {
                        uint32_t unwind_actions[CXX32_MAX_UNWIND_ACTIONS];
                        uint32_t unwind_count = 0;
                        if (!cxx32_collect_unwind_actions(
                                pUnwindMap, maxState, cur_state, tryLow,
                                unwind_actions, &unwind_count)) {
                            serial_puts("[SEH32] invalid unwind path, skipping catch\n");
                            goto next_frame;
                        }

                        /* The frame is in the catch state before the funclet
                         * runs. Cleanup actions execute first on the same EBP. */
                        frame32[2] = (uint32_t)catchHigh;

                        /* Unwind SEH chain to the NEXT frame after the catcher.
                         * Windows removes all frames up to and including the
                         * catching frame. The catch handler's locals overlap
                         * with the SEH registration at [EBP-4/-8/-C], so the
                         * frame must be unlinked before the handler runs. */
                        teb->ExceptionList = next32;

                        /* EBP for the catch handler = frame_addr + 0x0C
                         * (C++ EH frame is 3 fields: Next+Handler+State = 12 bytes) */
                        uint32_t catch_ebp = frame_addr + 0x0C;

                        /* If catch has a catch object (dispCatchObj != 0),
                         * store the exception object pointer at EBP+disp */
                        if (catch_disp != 0 && record32->NumberParameters >= 2) {
                            uint32_t exc_obj = record32->ExceptionInformation[1];
                            uint32_t *catch_obj_ptr = (uint32_t *)(uintptr_t)(catch_ebp + catch_disp);
                            *catch_obj_ptr = exc_obj;
                        }

                        serial_puts("[SEH32] dispatching to catch @0x");
                        serial_puthex(catch_handler, 8);
                        serial_puts(" EBP=0x");
                        serial_puthex(catch_ebp, 8);
                        serial_puts("\n");

                        /* [CATCH-EBP DIAGNOSTIC — uncommitted] For the UT99 LoadMap
                         * TCHAR* catch funclet (Engine.dll ~0x1038Exxx), the fault is a
                         * NULL vtable call on this=[ebp-0x14]. Dump frame_addr and the
                         * value that will be visible at [catch_ebp-0x14] / -0x34 / -0xC so
                         * we can verify the establisher EBP is correct. */
                        if (catch_handler >= 0x1038E000 && catch_handler < 0x1038F000) {
                            serial_puts("[CATCH-EBP] frame_addr=0x");
                            serial_puthex(frame_addr, 8);
                            serial_puts(" next=0x");
                            serial_puthex(next32, 8);
                            serial_puts(" [ebp-0x14]=0x");
                            serial_puthex(*(volatile uint32_t *)(uintptr_t)(catch_ebp - 0x14), 8);
                            serial_puts(" [ebp-0x34]=0x");
                            serial_puthex(*(volatile uint32_t *)(uintptr_t)(catch_ebp - 0x34), 8);
                            serial_puts(" [ebp-0xC]=0x");
                            serial_puthex(*(volatile uint32_t *)(uintptr_t)(catch_ebp - 0x0C), 8);
                            serial_puts("\n");
                        }

                        /*
                         * Call the catch handler via compat32_callback.
                         * The MSVC catch handler expects EBP to be the
                         * establishing function's frame pointer. We set
                         * up the unwind globals so the INT2E return will
                         * restore EBP before jumping to the handler.
                         */
                        state->in_catch_dispatch = 1;
                        state->saved_next_frame = next32;
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
                         * Local unwind actions are ordinary funclets too. Chain
                         * their RET addresses before the catch handler:
                         *   action[0] -> ... -> catch -> continuation stub.
                         */
                        {
                            /* EH3/EH4 prologs save the post-allocation stack
                             * pointer at [EBP-0x10]. Starting a funclet at
                             * EBP-4 reuses and corrupts its caller's locals.
                             * Put our synthetic return below the saved stack
                             * cursor, where an ordinary nested call belongs. */
                            uint32_t saved_esp =
                                *(uint32_t *)(uintptr_t)(catch_ebp - 0x10);
                            uint32_t stack_bytes = (unwind_count + 1U) * 4U;

                            /* Reject corrupt metadata without moving the stack
                             * outside the establishing frame. */
                            if (saved_esp == 0 || saved_esp >= catch_ebp ||
                                saved_esp < stack_bytes ||
                                catch_ebp - saved_esp > 0x100000 ||
                                !seh32_range_readable(saved_esp - stack_bytes,
                                                       stack_bytes)) {
                                serial_puts("[SEH32] invalid saved funclet ESP=0x");
                                serial_puthex(saved_esp, 8);
                                serial_puts("\n");
                                state->in_catch_dispatch = 0;
                                g_compat32_unwind_eip = 0;
                                goto next_frame;
                            }

                            uint32_t esp = saved_esp;
                            esp -= 4;
                            *(uint32_t *)(uintptr_t)esp = catch_continue_stub_addr;
                            if (unwind_count) {
                                esp -= 4;
                                *(uint32_t *)(uintptr_t)esp = catch_handler;
                                for (uint32_t i = unwind_count; i > 1; i--) {
                                    esp -= 4;
                                    *(uint32_t *)(uintptr_t)esp =
                                        unwind_actions[i - 1U];
                                }
                                g_compat32_unwind_eip = unwind_actions[0];
                            } else {
                                g_compat32_unwind_eip = catch_handler;
                            }
                            g_compat32_unwind_esp = esp;
                            g_compat32_unwind_ebp = catch_ebp;

                            serial_puts("[SEH32] local unwind actions=");
                            serial_putdec(unwind_count);
                            serial_puts(" entry=0x");
                            serial_puthex(g_compat32_unwind_eip, 8);
                            serial_puts("\n");
                        }

                        return 1;  /* handled — INT2E will apply unwind */
                    }
                }
                /* No matching catch in this frame, try next */
            } else {
                serial_puts(" (PE32 handler, calling via compat32)\n");

                /* Generic PE32 handler — call via compat32 with CONTEXT */
                uint32_t args[4];
                args[0] = (uint32_t)(ULONG_PTR)record32;
                args[1] = frame_addr;
                args[2] = pointers32->ContextRecord;
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

    /* Try the PE32 unhandled exception filter in compat mode. */
    {
        ULONG_PTR filter = (ULONG_PTR)
            kernel32_get_unhandled_exception_filter();
        if (filter >= 0x10000 && filter < 0x80000000) {
            serial_puts("[SEH32] calling UnhandledExceptionFilter\n");
            uint32_t arg = (uint32_t)(ULONG_PTR)pointers32;
            LONG result = (LONG)compat32_callback_args((uint32_t)filter, 1, &arg);
            if (result == EXCEPTION_CONTINUE_EXECUTION)
                return 1;
        }
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
                args[0] = (uint32_t)(ULONG_PTR)record32;
                args[1] = g_base_seh_frame_addr;
                args[2] = pointers32->ContextRecord;
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

static void context32_from_cpu(CONTEXT32 *dst,
                               const compat32_cpu_context_t *src)
{
    dst->ContextFlags = CONTEXT32_FULL;
    dst->SegGs = src->seg_gs;
    dst->SegFs = src->seg_fs;
    dst->SegEs = src->seg_es;
    dst->SegDs = src->seg_ds;
    dst->Edi = src->edi;
    dst->Esi = src->esi;
    dst->Ebx = src->ebx;
    dst->Edx = src->edx;
    dst->Ecx = src->ecx;
    dst->Eax = src->eax;
    dst->Ebp = src->ebp;
    dst->Eip = src->eip;
    dst->SegCs = src->seg_cs;
    dst->EFlags = src->eflags;
    dst->Esp = src->esp;
    dst->SegSs = src->seg_ss;
}

static void context32_to_cpu(compat32_cpu_context_t *dst,
                             const CONTEXT32 *src)
{
    dst->eax = src->Eax;
    dst->ebx = src->Ebx;
    dst->ecx = src->Ecx;
    dst->edx = src->Edx;
    dst->esi = src->Esi;
    dst->edi = src->Edi;
    dst->ebp = src->Ebp;
    dst->esp = src->Esp;
    dst->eip = src->Eip;
    dst->eflags = src->EFlags;
    dst->seg_cs = src->SegCs;
    dst->seg_ss = src->SegSs;
    dst->seg_ds = src->SegDs;
    dst->seg_es = src->SegEs;
    dst->seg_fs = src->SegFs;
    dst->seg_gs = src->SegGs;
}

static int compat32_seh_dispatch_common(
    PEXCEPTION_RECORD ExceptionRecord, compat32_cpu_context_t *cpu_context)
{
    callback_owner_state_t *state = callback_state_get(1, NULL);
    if (!state || !ExceptionRecord)
        return 0;
    if (state->seh_dispatch_depth >= SEH32_MAX_DISPATCH_DEPTH) {
        serial_puts("[SEH32] nested dispatch limit reached\n");
        return 0;
    }

    state->seh_dispatch_depth++;
    seh32_dispatch_slot_t *slot = seh32_current_slot(state);
    if (!slot) {
        state->seh_dispatch_depth--;
        return 0;
    }
    memset(slot, 0, sizeof(*slot));
    if (cpu_context)
        context32_from_cpu(&slot->context, cpu_context);

    int handled = compat32_seh_dispatch_impl(ExceptionRecord, state);
    if (handled && cpu_context) {
        uint32_t old_eip = cpu_context->eip;
        context32_to_cpu(cpu_context, &slot->context);
        if (cpu_context->eip != old_eip) {
            serial_puts("[SEH32-CONTEXT] eip 0x");
            serial_puthex(old_eip, 8);
            serial_puts(" -> 0x");
            serial_puthex(cpu_context->eip, 8);
            serial_puts("\n");
        }
    }
    state->seh_dispatch_depth--;
    return handled;
}

int compat32_seh_dispatch(PEXCEPTION_RECORD ExceptionRecord)
{
    return compat32_seh_dispatch_common(ExceptionRecord, NULL);
}

int compat32_seh_dispatch_cpu(PEXCEPTION_RECORD ExceptionRecord,
                              compat32_cpu_context_t *Context)
{
    if (!Context) return 0;
    return compat32_seh_dispatch_common(ExceptionRecord, Context);
}

int compat32_seh_dispatch_active(void)
{
    callback_owner_state_t *state = callback_state_get(0, NULL);
    return state && state->seh_dispatch_depth > 0;
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
    const int ut99 = g_compat32_ut99;
    callback_owner_state_t *seh_state = callback_state_get(1, NULL);
    if (seh_state && !seh_state->seh32_slots &&
        !seh32_scratch_prepare(seh_state)) {
        static uint32_t seh_alloc_fail_logs;
        if (seh_alloc_fail_logs++ < 4)
            serial_puts("[SEH32] unable to allocate low scratch page\n");
    }
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

    if (ut99) {

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

    /* CASCADE-NOP-V2 REMOVED in Phase 7h.
     *
     * Phase 7g HWBP data (commit da1fb85) falsified the premise: ALL 200
     * GObjRegistrants entries successfully traverse the full Register
     * chain (UClass → UStruct → UField → UObject → CreatePackage). The
     * "Failed to load 'None'" throw at Core.dll+0x59B99 (PackageNotFound)
     * is NOT firing because of registration failure — it fires later
     * during StaticLoadObject downstream lookups. NOP'ing it was
     * whack-a-mole on the wrong site; real cascade root is
     * StaticFindObject(UPackage, NULL, "Engine") returning NULL.
     */

/* FMallocWindows pool-integrity asserts: REMOVED in Phase 3 of
     * the layer-repair plan. Previously we patched je/ja → jmp at
     * UT.exe 0x109032A8/0x10903303/0x10903353/0x10903374 to skip
     * the HeapCheck() asserts at FMallocWindows.cpp lines 367/370/
     * 375/376. With Phase 2-fix's defensive FMW-REPAIR sweep
     * keeping the pool->PrevLink invariant satisfied (commit
     * b82559c), the engine's HeapCheck walk passes naturally and
     * the bypasses are no longer needed. */

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
        /* FNAME_RESCUE_PREFILL — ROOT-CAUSE FIX (2026-06-05): the OLD pre-fill
         * set Num=1, which made UE1 FName::StaticInit see a non-empty table and
         * SKIP registering all ~838 hardcoded EName names (FNDIFF proved only
         * Names[0] populated, Num stuck at 1, Names.Add never called). Then
         * FNAME-NULL-FILL masked every unregistered slot with "None", so every
         * lookup-by-name (menu classes, packages) resolved to "None"/"0" — the
         * recurring package-zero cascade.
         *
         * The fix: still pre-size the TArray (Data = our 512KB buffer,
         * Max=131072) so the engine NEVER hits the fatal FArray::Realloc, but
         * leave **Num=0** so StaticInit sees an empty table and registers the
         * hardcoded names into our buffer naturally (no realloc needed, since
         * Max is already past anything a full level+gameplay needs). NULL-FILL
         * remains as a safety net for genuinely-sparse EName slots. */
        static const int FNAME_RESCUE_PREFILL = 1;
        if (FNAME_RESCUE_PREFILL && fd == 0 && fm == 0) {
            if (fname_buf_phys == 0) {
                extern void *mem_alloc_pages(uint64_t count);
                /* Buffer A (512 KB) — TArray slot pool (4 bytes per slot,
                 * room for 131072 ptrs). Sized large so FName::Names NEVER
                 * needs to FArray::Realloc. The realloc path is fatal: our
                 * HeapReAlloc can't size a non-heap-pool source buffer and
                 * the copy drops ALL entries → the whole table goes NULL →
                 * FNAME-NULL-FILL then masks every slot with "None" → every
                 * object reference resolves to "None None.X" → appError.
                 * Avoid the realloc entirely by pre-sizing past anything the
                 * engine needs. Preload/Browse peaked ~4271 entries, but
                 * LEVEL LOAD (UTMenu + the map's actors/textures/sounds)
                 * crosses 16384 — the previous Max=16384 reallocated mid-load
                 * (observed Num=16385, whole table NULL'd) and bricked every
                 * FName → fatal "None None.UTConsole". 131072 covers a full
                 * level + gameplay with wide headroom. */
                void *buf  = mem_alloc_pages(128);
                /* Buffer B (4 KB)  — FNameEntry pool */
                void *pool = mem_alloc_pages(1);
                if (buf && pool) {
                    uint8_t *p = (uint8_t *)buf;
                    for (int i = 0; i < 524288; i++) p[i] = 0;  /* 512 KB = 131072 ptrs */
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
                fname_tarray[1] = 0;       /* Num = 0 → StaticInit registers the
                                            * hardcoded names itself (root fix) */
                fname_tarray[2] = 131072;  /* Max = 128K entries (no realloc, covers level load) */
                static int fname_setup_logged = 0;
                if (!fname_setup_logged) {
                    fname_setup_logged = 1;
                    serial_puts("[FNAME-RESCUE] pre-alloc FName::Names Data=0x");
                    serial_puthex(fname_buf_phys, 8);
                    serial_puts(" Num=0 Max=131072 (StaticInit registers names)\n");
                }
            }
        }
        /* FNDIFF FName-diff instrumentation removed (Phase 2 cleanup):
         * the sparse-Names root cause is handled by FNAME-NULL-FILL below. */
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
        /* FNAME-NULL-FILL DISABLED (2026-06-06, option (b)): disasm of Core.dll
         * proved the "Unhashed name '%s'" assert at 0x101533aa lives inside
         * FName::DeleteEntry(INT Index) — it loads Names[Index], walks NameHash
         * (@0x10295d4c, HashNext@+0x08) to find that exact pointer, raises
         * "Unhashed name '<name>'" if absent, then UNLINKS it (*esi =
         * entry->HashNext) and calls GMalloc->Free(entry) (GMalloc@0x101a7b90,
         * vtbl[2]=Free). Masking NULL slots with a single SHARED, non-GMalloc
         * sentinel poisons that delete path: DeleteEntry on a filled slot can't
         * find the aliased pointer in the hash -> "Unhashed name 'None'" (the
         * render-transition crash). Hash-linking the sentinel (opt a) would make
         * GMalloc->Free() run on foreign/aliased memory (heap corruption + UAF
         * of sibling slots); reusing the engine's real None (opt c) would
         * unlink+Free the canonical NAME_None (UAF on every later FName(0)).
         * The FNAME_RESCUE_PREFILL above (Num=0, pre-sized Max) already makes
         * the engine register its own names densely, so the operator*()
         * NULL-read cascade this masked is now inert. Keep the loop compiled but
         * gated off; only re-enable with a PER-SLOT, non-aliased, GMalloc-owned,
         * hash-linked entry if a specific NULL read ever recurs. */
        static const int FNAME_NULL_FILL_ENABLED = 0;
        #ifndef DISABLE_NULL_FILL
        if (FNAME_NULL_FILL_ENABLED && fname_none_entry && fd != 0) {
            uint32_t *slots = (uint32_t *)(uintptr_t)fd;
            uint32_t limit = fn;
            if (limit > fm) limit = fm;
            if (limit > 131072) limit = 131072; /* sanity cap = buffer capacity */
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

        /* FNAME-EDUMP — once-per-state diagnostic of canonical EName slots.
         * Records what Names[Idx] points to for Idx in {0,20,21,151,...}.
         * If Names[NAME_Engine=21] points at our None sentinel, that
         * confirms Core.dll's StaticInit never registered hardcoded names
         * (or our NULL-FILL pre-empted it). If Names[21] points elsewhere,
         * it's a valid FNameEntry — log its Name field (offset +0xC) so
         * we can see whether it says "Engine" or something else. */
        if (fd != 0 && fn >= 22) {
            static uint32_t edump_count = 0;
            static uint32_t last_fn = 0;
            /* Trigger: first few times Num grows (confirms StaticInit is
             * registering real hardcoded names), then every 200 dispatches. */
            if (edump_count == 0 || (edump_count < 8 && fn != last_fn) ||
                (edump_count % 200) == 0) {
                static const struct { uint32_t idx; const char *name; } ENAMES[] = {
                    {  0, "None"   }, { 10, "StructProp" }, { 20, "Core"   },
                    { 21, "Engine" }, { 22, "Editor"     }, { 81, "Int"    },
                    { 86, "Struct" }, {100, "Begin"      }, {151, "Object" },
                    {152, "TxtBuf" }, {500, "<gap500>"   },
                };
                serial_puts("[FNAME-EDUMP] #");
                serial_putdec((uint64_t)edump_count);
                serial_puts(" Num=");
                serial_putdec((uint64_t)fn);
                serial_puts(" none_sentinel=0x");
                serial_puthex(fname_none_entry, 8);
                serial_puts("\n");
                uint32_t *slots = (uint32_t *)(uintptr_t)fd;
                for (uint32_t e = 0; e < sizeof(ENAMES)/sizeof(ENAMES[0]); e++) {
                    uint32_t idx = ENAMES[e].idx;
                    if (idx >= fn) continue;
                    uint32_t entry = slots[idx];
                    serial_puts("  Names[");
                    serial_putdec((uint64_t)idx);
                    serial_puts("] (");
                    serial_puts(ENAMES[e].name);
                    serial_puts(") = 0x");
                    serial_puthex((uint64_t)entry, 8);
                    if (entry == 0) {
                        serial_puts(" NULL\n");
                    } else if ((uint64_t)entry == fname_none_entry) {
                        serial_puts(" =NoneSentinel\n");
                    } else {
                        /* Try to read +0xC = Name UTF-16 LE (8 bytes = 4 chars). */
                        uint16_t *name = (uint16_t *)(uintptr_t)(entry + 0xC);
                        serial_puts(" Name=L\"");
                        for (int c = 0; c < 16; c++) {
                            uint16_t ch = name[c];
                            if (ch == 0) break;
                            if (ch >= 0x20 && ch < 0x7F) {
                                char b[2] = { (char)ch, 0 };
                                serial_puts(b);
                            } else {
                                serial_puts("?");
                            }
                        }
                        serial_puts("\"\n");
                    }
                }
                edump_count++;
                last_fn = fn;
            }
        }

        (void)fn;
        (void)fname_none_entry;
    }

    /* FMW-REPAIR removed (Phase 2 bisect: 0 fires with Phase 1 ABI fix +
     * ENGINE-PATCH on). FMW-POOL-SKIP handles the rare NULL pool write. */

    /* GObjRegistrants snapshot/restore + manual ProcessRegistrants
     * REMOVED in Phase 5. Was working around a perceived FArray::Empty()
     * race where engine zeroed the registrants before our manual
     * ProcessRegistrants could run. With Phase 2-fix's allocator
     * stability, the engine's own ProcessRegistrants + Empty cycle
     * should complete naturally. */

#ifndef OK_QUIET
    /* GOBJREG-DIAG — passive observer for GObjRegistrants TArray<UObject*>.
     * Each entry is a UObject* (4 bytes). UObject::Name (FName) is at
     * offset +0x20. Dereference to get the FName index, then look up
     * in FName::Names[Idx] for the FNameEntry, read Name field at +0xC. */
    {
        volatile uint32_t *gobjreg = (volatile uint32_t *)(uintptr_t)0x102A0360ULL;
        uint32_t grd = gobjreg[0], grn = gobjreg[1], grm = gobjreg[2];
        static uint32_t last_grn = 0xFFFFFFFFu;
        static uint32_t last_grd = 0;
        static uint32_t snapshot_count = 0;
        int data_changed = (grd != last_grd);
        int num_changed = (grn != last_grn);
        /* Fire ONCE at dispatch #5000 when Num is stable high, to capture
         * post-Phase 2 state (entries should have Name set by Register). */
        static uint32_t dispatch_count = 0;
        static int late_fired = 0;
        dispatch_count++;
        int periodic_fire = (!late_fired && grn >= 100 && dispatch_count >= 5000);
        if (periodic_fire) late_fired = 1;
        if (grd != 0 && (data_changed || num_changed || periodic_fire) && snapshot_count < 24) {
            volatile uint32_t *fname_tarray = (volatile uint32_t *)(uintptr_t)0x10295D30;
            uint32_t names_data = fname_tarray[0], names_num = fname_tarray[1];
            serial_puts("[GOBJREG-DIAG] Num=");
            serial_putdec((uint64_t)grn);
            serial_puts(" (was ");
            serial_putdec(last_grn == 0xFFFFFFFFu ? 0 : (uint64_t)last_grn);
            serial_puts(") Max=");
            serial_putdec((uint64_t)grm);
            serial_puts(" Data=0x");
            serial_puthex((uint64_t)grd, 8);
            if (data_changed) serial_puts(" [Data-CHANGED]");
            serial_puts("\n");
            /* When Num is small, dump every entry with name sweep. When
             * large (>=100), do a fast scan first showing address-range
             * histogram (count of entries by DLL region) then dump only
             * Engine.dll-resident entries (0x10300000-0x104B3000) with
             * full name sweep. */
            if (grn >= 100) {
                uint32_t count_engine = 0, count_core = 0, count_window = 0,
                         count_d3ddrv = 0, count_galaxy = 0, count_render = 0,
                         count_other_dll = 0, count_heap = 0;
                uint32_t *all = (uint32_t *)(uintptr_t)grd;
                /* DLL bases from runtime log: Engine 0x10300000+0x2C7000,
                 * Core 0x10100000, Window 0x11000000, D3DDrv 0x10000000,
                 * Galaxy 0x10600000, Render 0x10B00000. */
                for (uint32_t i = 0; i < grn; i++) {
                    uint32_t v = all[i];
                    if (v >= 0x10300000 && v < 0x105C7000) count_engine++;
                    else if (v >= 0x10100000 && v < 0x102C0000) count_core++;
                    else if (v >= 0x11000000 && v < 0x11200000) count_window++;
                    else if (v >= 0x10000000 && v < 0x10100000) count_d3ddrv++;
                    else if (v >= 0x10600000 && v < 0x10700000) count_galaxy++;
                    else if (v >= 0x10B00000 && v < 0x10C00000) count_render++;
                    else if (v >= 0x10000000 && v < 0x80000000) count_other_dll++;
                    else if (v >= 0x01000000 && v < 0x10000000) count_heap++;
                }
                serial_puts("[GOBJREG-HISTO]");
                serial_puts(" Engine=");
                serial_putdec((uint64_t)count_engine);
                serial_puts(" Core=");
                serial_putdec((uint64_t)count_core);
                serial_puts(" Window=");
                serial_putdec((uint64_t)count_window);
                serial_puts(" D3DDrv=");
                serial_putdec((uint64_t)count_d3ddrv);
                serial_puts(" Galaxy=");
                serial_putdec((uint64_t)count_galaxy);
                serial_puts(" Render=");
                serial_putdec((uint64_t)count_render);
                serial_puts(" other_dll=");
                serial_putdec((uint64_t)count_other_dll);
                serial_puts(" heap=");
                serial_putdec((uint64_t)count_heap);
                serial_puts("\n");
                /* Dump first ~5 entries of each major DLL bucket so we
                 * can see Engine.dll classes by-address. */
                serial_puts("[GOBJREG-HISTO] Engine entries:");
                int shown = 0;
                for (uint32_t i = 0; i < grn && shown < 10; i++) {
                    uint32_t v = all[i];
                    if (v >= 0x10300000 && v < 0x105C7000) {
                        serial_puts(" [");
                        serial_putdec((uint64_t)i);
                        serial_puts("]=0x");
                        serial_puthex((uint64_t)v, 8);
                        shown++;
                    }
                }
                serial_puts("\n");
            }
            /* Phase 7d — when this is a periodic (post-Phase 2) snapshot,
             * dump entry 0 (registered OK) and entry 5 (failed) byte-by-byte
             * at offsets 0..0x60. Each pointer-shaped value is also rendered
             * as both ASCII and UTF-16LE strings (UE1 uses TCHAR=WCHAR). */
            if (periodic_fire && grn >= 6) {
                uint32_t *all = (uint32_t *)(uintptr_t)grd;
                int dump_idx[] = { 0, 1, 4, 5, 6, 7, 100 };
                for (uint32_t di = 0; di < sizeof(dump_idx)/sizeof(dump_idx[0]); di++) {
                    int idx = dump_idx[di];
                    if ((uint32_t)idx >= grn) continue;
                    uint32_t uobj = all[idx];
                    if (uobj < 0x01000000) continue;
                    serial_puts("[ENTRY-RAW#");
                    serial_putdec((uint64_t)idx);
                    serial_puts("] @0x");
                    serial_puthex((uint64_t)uobj, 8);
                    serial_puts("\n");
                    volatile uint32_t *u = (volatile uint32_t *)(uintptr_t)uobj;
                    for (int off = 0; off < 0x60; off += 4) {
                        uint32_t v = u[off / 4];
                        serial_puts("  +0x");
                        serial_puthex((uint64_t)off, 2);
                        serial_puts(": 0x");
                        serial_puthex((uint64_t)v, 8);
                        /* Render as ASCII string if it's a plausible
                         * pointer to .rdata text. */
                        if (v >= 0x10000000 && v < 0x12000000) {
                            volatile char *s = (volatile char *)(uintptr_t)v;
                            int ascii_ok = 1;
                            for (int c = 0; c < 4; c++) {
                                char ch = s[c];
                                if (ch < 0x20 || ch >= 0x7F) { ascii_ok = 0; break; }
                            }
                            if (ascii_ok) {
                                serial_puts(" A=\"");
                                char tmp[32]; int n = 0;
                                for (int c = 0; c < 31 && s[c]; c++) {
                                    char ch = s[c];
                                    tmp[n++] = (ch < 0x20 || ch >= 0x7F) ? '?' : ch;
                                }
                                tmp[n] = 0;
                                serial_puts(tmp);
                                serial_puts("\"");
                            } else {
                                /* Try UTF-16LE */
                                volatile uint16_t *w = (volatile uint16_t *)(uintptr_t)v;
                                int utf16_ok = 1;
                                for (int c = 0; c < 4; c++) {
                                    uint16_t ch = w[c];
                                    if (ch == 0 && c > 0) break;
                                    if (ch < 0x20 || ch >= 0x7F) { utf16_ok = 0; break; }
                                }
                                if (utf16_ok) {
                                    serial_puts(" W=L\"");
                                    char tmp[32]; int n = 0;
                                    for (int c = 0; c < 31; c++) {
                                        uint16_t ch = w[c];
                                        if (ch == 0) break;
                                        tmp[n++] = (ch < 0x20 || ch >= 0x7F) ? '?' : (char)ch;
                                    }
                                    tmp[n] = 0;
                                    serial_puts(tmp);
                                    serial_puts("\"");
                                }
                            }
                        }
                        serial_puts("\n");
                    }
                }
            }
            uint32_t to_dump = grn > 20 ? 20 : grn;
            uint32_t *slots = (uint32_t *)(uintptr_t)grd;
            for (uint32_t i = 0; i < to_dump; i++) {
                uint32_t uobj = slots[i];
                serial_puts("  [");
                serial_putdec((uint64_t)i);
                serial_puts("] UObj=0x");
                serial_puthex((uint64_t)uobj, 8);
                /* Sweep offsets 0..0x40 looking for valid FName.Index
                 * (small value pointing to a populated Names slot). */
                if (uobj >= 0x01000000 && names_data) {
                    uint32_t *fname_slots = (uint32_t *)(uintptr_t)names_data;
                    for (int off = 0x10; off <= 0x40; off += 4) {
                        uint32_t v = *(volatile uint32_t *)(uintptr_t)(uobj + off);
                        if (v >= names_num) continue;
                        uint32_t entry = fname_slots[v];
                        if (entry < 0x01000000) continue;
                        uint16_t *name = (uint16_t *)(uintptr_t)(entry + 0xC);
                        /* Check name is plausibly ASCII-letters first char. */
                        uint16_t ch0 = name[0];
                        if (ch0 < 'A' || ch0 > 'Z') continue;
                        serial_puts(" +0x");
                        serial_puthex((uint64_t)off, 2);
                        serial_puts("=FName(");
                        serial_putdec((uint64_t)v);
                        serial_puts(")=L\"");
                        for (int c = 0; c < 20; c++) {
                            uint16_t ch = name[c];
                            if (ch == 0) break;
                            if (ch >= 0x20 && ch < 0x7F) {
                                char b[2] = { (char)ch, 0 };
                                serial_puts(b);
                            } else { serial_puts("?"); break; }
                        }
                        serial_puts("\"");
                    }
                }
                serial_puts("\n");
            }
            snapshot_count++;
            last_grn = grn;
            last_grd = grd;

            /* GOBJOBJ-DUMP — Phase 7i. Walk GObjObjects (TArray at 0x102a2160)
             * after Phase 2 completion (one-shot via periodic_fire). For each
             * UObject:
             *   - skip NULL slots
             *   - read +0x20 = Name FName.Index
             *   - resolve Names[idx]+0xC = WCHAR* name string
             *   - filter print to entries whose name starts with capital
             *     letter (skip internal hash entries / None placeholders)
             *
             * Goal: confirm whether UPackage("Engine") is present. If yes,
             * the bug is in StaticFindObject's GObjHash traversal (FName
             * mismatch or chain corruption). If no, AddObject doesn't
             * insert UPackages at all (CreatePackage allocation succeeds
             * but doesn't bind). */
            if (periodic_fire) {
                volatile uint32_t *gobjobjs = (volatile uint32_t *)(uintptr_t)0x102A2160ULL;
                uint32_t oo_data = gobjobjs[0];
                uint32_t oo_num  = gobjobjs[1];
                uint32_t oo_max  = gobjobjs[2];
                serial_puts("[GOBJOBJ-DUMP] GObjObjects Data=0x");
                serial_puthex((uint64_t)oo_data, 8);
                serial_puts(" Num=");
                serial_putdec((uint64_t)oo_num);
                serial_puts(" Max=");
                serial_putdec((uint64_t)oo_max);
                serial_puts("\n");
                if (names_data && oo_data != 0 && oo_num > 0 && oo_num < 100000) {
                    uint32_t *obj_slots = (uint32_t *)(uintptr_t)oo_data;
                    uint32_t *name_slots = (uint32_t *)(uintptr_t)names_data;
                    int matched = 0;
                    int engine_hits = 0;
                    int core_hits = 0;
                    for (uint32_t i = 0; i < oo_num; i++) {
                        uint32_t obj_va = obj_slots[i];
                        if (obj_va < 0x01000000) continue;
                        /* Read Name FName.Index at +0x20. */
                        uint32_t fname_idx = *(volatile uint32_t *)(uintptr_t)(obj_va + 0x20);
                        if (fname_idx >= names_num) continue;
                        uint32_t name_entry = name_slots[fname_idx];
                        if (name_entry < 0x01000000) continue;
                        uint16_t *ws = (uint16_t *)(uintptr_t)(name_entry + 0xC);
                        uint16_t c0 = ws[0];
                        if (c0 < 'A' || c0 > 'Z') continue;
                        /* Collect Name as ASCII. */
                        char name_buf[32];
                        int n = 0;
                        for (int c = 0; c < 31; c++) {
                            uint16_t ch = ws[c];
                            if (ch == 0) break;
                            if (ch < 0x20 || ch >= 0x7F) { name_buf[n++] = '?'; continue; }
                            name_buf[n++] = (char)ch;
                        }
                        name_buf[n] = 0;
                        /* Match name=="Engine" exactly to find UPackage. */
                        int is_engine = (n == 6 && name_buf[0]=='E' && name_buf[1]=='n'
                                          && name_buf[2]=='g' && name_buf[3]=='i'
                                          && name_buf[4]=='n' && name_buf[5]=='e');
                        int is_core = (n == 4 && name_buf[0]=='C' && name_buf[1]=='o'
                                          && name_buf[2]=='r' && name_buf[3]=='e');
                        if (is_engine) engine_hits++;
                        if (is_core) core_hits++;
                        /* Print first few matches + always print Engine/Core. */
                        if (matched < 20 || is_engine || is_core) {
                            uint32_t vtable = *(volatile uint32_t *)(uintptr_t)obj_va;
                            uint32_t outer  = *(volatile uint32_t *)(uintptr_t)(obj_va + 0x18);
                            uint32_t flags  = *(volatile uint32_t *)(uintptr_t)(obj_va + 0x1C);
                            uint32_t idx    = *(volatile uint32_t *)(uintptr_t)(obj_va + 0x04);
                            serial_puts("[GOBJOBJ#");
                            serial_putdec((uint64_t)i);
                            serial_puts("] UObj=0x");
                            serial_puthex((uint64_t)obj_va, 8);
                            serial_puts(" Idx=");
                            serial_putdec((uint64_t)idx);
                            serial_puts(" vtbl=0x");
                            serial_puthex((uint64_t)vtable, 8);
                            serial_puts(" Outer=0x");
                            serial_puthex((uint64_t)outer, 8);
                            serial_puts(" Flags=0x");
                            serial_puthex((uint64_t)flags, 8);
                            serial_puts(" Name=FName(");
                            serial_putdec((uint64_t)fname_idx);
                            serial_puts(")=L\"");
                            serial_puts(name_buf);
                            serial_puts("\"\n");
                            matched++;
                        }
                    }
                    serial_puts("[GOBJOBJ-DUMP] matched=");
                    serial_putdec((uint64_t)matched);
                    serial_puts(" engine_hits=");
                    serial_putdec((uint64_t)engine_hits);
                    serial_puts(" core_hits=");
                    serial_putdec((uint64_t)core_hits);
                    serial_puts("\n");

                    /* Phase 7j — GObjHash chain walker. Decoded from
                     * StaticFindObject @ Core.dll+0x570b0:
                     *   hash = (Outer ? Outer->Index : 0) XOR FName.Index
                     *   slot = GObjHash[hash & 0xfff]   (table @ 0x1029be68)
                     *   while (slot) {
                     *       if (slot->Name == FName.Index && slot->Outer == Outer)
                     *           return slot;
                     *       slot = slot->HashNext;   // [obj+0x08]
                     *   }
                     *
                     * For each found Engine/Core UPackage, walk the chain
                     * that StaticFindObject would walk. If our target is
                     * NOT in the chain → AddObject doesn't link into hash
                     * table for UPackages. */
                    volatile uint32_t *gobjhash = (volatile uint32_t *)(uintptr_t)0x1029BE68ULL;
                    for (int pass = 0; pass < 2; pass++) {
                        const char *want = pass == 0 ? "Engine" : "Core";
                        int want_len = pass == 0 ? 6 : 4;
                        uint32_t target_obj = 0;
                        uint32_t target_idx = 0;
                        for (uint32_t i = 0; i < oo_num; i++) {
                            uint32_t obj_va = obj_slots[i];
                            if (obj_va < 0x01000000) continue;
                            uint32_t fname_idx = *(volatile uint32_t *)(uintptr_t)(obj_va + 0x20);
                            if (fname_idx >= names_num) continue;
                            uint32_t name_entry = name_slots[fname_idx];
                            if (name_entry < 0x01000000) continue;
                            uint16_t *ws = (uint16_t *)(uintptr_t)(name_entry + 0xC);
                            int matches = 1;
                            for (int c = 0; c < want_len; c++) {
                                if (ws[c] != (uint16_t)want[c]) { matches = 0; break; }
                            }
                            if (matches && ws[want_len] == 0) {
                                uint32_t outer = *(volatile uint32_t *)(uintptr_t)(obj_va + 0x18);
                                if (outer == 0) {  /* prefer top-level UPackage */
                                    target_obj = obj_va;
                                    target_idx = fname_idx;
                                    break;
                                }
                            }
                        }
                        if (target_obj == 0) continue;
                        uint32_t hash = (0 ^ target_idx) & 0xfff;
                        uint32_t head = gobjhash[hash];
                        serial_puts("[GOBJHASH] '");
                        serial_puts(want);
                        serial_puts("' FName=");
                        serial_putdec((uint64_t)target_idx);
                        serial_puts(" bucket=");
                        serial_putdec((uint64_t)hash);
                        serial_puts(" head=0x");
                        serial_puthex((uint64_t)head, 8);
                        serial_puts(" target=0x");
                        serial_puthex((uint64_t)target_obj, 8);
                        serial_puts("\n");
                        int chain_len = 0;
                        int found = 0;
                        uint32_t cur = head;
                        while (cur >= 0x01000000 && chain_len < 64) {
                            uint32_t cur_name = *(volatile uint32_t *)(uintptr_t)(cur + 0x20);
                            uint32_t cur_outer = *(volatile uint32_t *)(uintptr_t)(cur + 0x18);
                            uint32_t cur_next  = *(volatile uint32_t *)(uintptr_t)(cur + 0x08);
                            serial_puts("  [chain#");
                            serial_putdec((uint64_t)chain_len);
                            serial_puts("] obj=0x");
                            serial_puthex((uint64_t)cur, 8);
                            serial_puts(" Name=");
                            serial_putdec((uint64_t)cur_name);
                            serial_puts(" Outer=0x");
                            serial_puthex((uint64_t)cur_outer, 8);
                            serial_puts(" Next=0x");
                            serial_puthex((uint64_t)cur_next, 8);
                            if (cur == target_obj) { serial_puts(" <-- TARGET"); found = 1; }
                            serial_puts("\n");
                            cur = cur_next;
                            chain_len++;
                        }
                        serial_puts("[GOBJHASH] '");
                        serial_puts(want);
                        serial_puts("' chain_len=");
                        serial_putdec((uint64_t)chain_len);
                        serial_puts(" found_in_chain=");
                        serial_puts(found ? "YES" : "NO");
                        serial_puts("\n");

                        /* Phase 7k — walk obj->Class chain (the part of
                         * StaticFindObject that filters by Class). Bug
                         * hypothesis: UPackage("Engine")->Class at +0x24
                         * either NULL or doesn't reach UPackage::StaticClass
                         * via SuperClass chain at +0x28. */
                        uint32_t obj_class = *(volatile uint32_t *)(uintptr_t)(target_obj + 0x24);
                        serial_puts("[CLASS-CHAIN] '");
                        serial_puts(want);
                        serial_puts("' obj->Class[+0x24]=0x");
                        serial_puthex((uint64_t)obj_class, 8);
                        serial_puts(" (UPackage::SC should be 0x102A1C60)\n");
                        uint32_t cls_cur = obj_class;
                        int cls_depth = 0;
                        while (cls_cur >= 0x01000000 && cls_depth < 16) {
                            uint32_t cls_name_idx = *(volatile uint32_t *)(uintptr_t)(cls_cur + 0x20);
                            uint32_t cls_super   = *(volatile uint32_t *)(uintptr_t)(cls_cur + 0x28);
                            uint32_t cls_vtable  = *(volatile uint32_t *)(uintptr_t)(cls_cur);
                            const char *cname = "?";
                            char cnbuf[32];
                            if (cls_name_idx < names_num) {
                                uint32_t ne = name_slots[cls_name_idx];
                                if (ne >= 0x01000000) {
                                    uint16_t *nws = (uint16_t *)(uintptr_t)(ne + 0xC);
                                    int nn = 0;
                                    for (int c = 0; c < 31; c++) {
                                        uint16_t ch = nws[c];
                                        if (ch == 0) break;
                                        cnbuf[nn++] = (ch < 0x20 || ch >= 0x7F) ? '?' : (char)ch;
                                    }
                                    cnbuf[nn] = 0;
                                    cname = cnbuf;
                                }
                            }
                            serial_puts("  Class[");
                            serial_putdec((uint64_t)cls_depth);
                            serial_puts("] @0x");
                            serial_puthex((uint64_t)cls_cur, 8);
                            serial_puts(" vtbl=0x");
                            serial_puthex((uint64_t)cls_vtable, 8);
                            serial_puts(" Name=L\"");
                            serial_puts(cname);
                            serial_puts("\" Super=0x");
                            serial_puthex((uint64_t)cls_super, 8);
                            if (cls_cur == 0x102A1C60ULL) serial_puts(" <-- IS UPackage::SC");
                            serial_puts("\n");
                            cls_cur = cls_super;
                            cls_depth++;
                        }

                        /* Phase 7l — NameHash bucket walker. FName::FName
                         * decoded from Core.dll+0x150b50: looks up via
                         * NameHash[hash & 0xfff] (table at 0x10295d4c).
                         * Each FNameEntry chains via +0x08; Name string
                         * starts at +0x0c.
                         *
                         * Goal: search all 4096 buckets for the FNameEntry
                         * whose Name == "Engine" / "Core". If found,
                         * compare bucket index to the hash expected. If
                         * NOT found in ANY bucket → NameHash is missing
                         * the entry → FName::FName(L"Engine", FALSE)
                         * returns 0 → StaticFindObject early-exits. */
                        uint32_t target_fname_entry = name_slots[target_idx];
                        volatile uint32_t *namehash = (volatile uint32_t *)(uintptr_t)0x10295D4CULL;
                        int hash_bucket = -1;
                        int hash_chain_pos = -1;
                        int total_chain_nodes = 0;
                        int populated_buckets = 0;
                        for (int b = 0; b < 4096; b++) {
                            uint32_t head = namehash[b];
                            if (head < 0x01000000) continue;
                            populated_buckets++;
                            uint32_t cur = head;
                            int depth = 0;
                            while (cur >= 0x01000000 && depth < 64) {
                                total_chain_nodes++;
                                if (cur == target_fname_entry) {
                                    hash_bucket = b;
                                    hash_chain_pos = depth;
                                }
                                uint32_t next = *(volatile uint32_t *)(uintptr_t)(cur + 0x08);
                                cur = next;
                                depth++;
                            }
                        }
                        serial_puts("[NAMEHASH] '");
                        serial_puts(want);
                        serial_puts("' FNameEntry=0x");
                        serial_puthex((uint64_t)target_fname_entry, 8);
                        serial_puts(" populated_buckets=");
                        serial_putdec((uint64_t)populated_buckets);
                        serial_puts(" total_chain_nodes=");
                        serial_putdec((uint64_t)total_chain_nodes);
                        if (hash_bucket >= 0) {
                            serial_puts(" FOUND in bucket=");
                            serial_putdec((uint64_t)hash_bucket);
                            serial_puts(" pos=");
                            serial_putdec((uint64_t)hash_chain_pos);
                        } else {
                            serial_puts(" NOT FOUND in any bucket");
                        }
                        serial_puts("\n");
                    }
                }
            }
        }
    }

#endif

    /* PRECREATE-PACKAGES REMOVED in Phase 7h.
     *
     * Phase 7g HWBP data confirmed Register's CreatePackage call fires
     * for every entry (200 hits at CreatePackage entry). Pre-creating
     * packages was both redundant and irrelevant to the cascade — which
     * actually originates downstream in StaticLoadObject when
     * StaticFindObject(UPackage, "Engine") fails to find the package
     * even though it WAS created. Real bug is in GObj insertion or
     * lookup, not in pre-population.
     */

/* GOBJREG-FORCE — when GObjRegistrants accumulates >= 100 entries and
     * stays there, force a manual ProcessRegistrants pass.
     *
     * Observed during Phase 7 investigation: after natural ProcessRegistrants
     * runs, GObjRegistrants stays at Num=200 with Data=0x01F74000 instead
     * of being cleared (Phase 3 of ProcessRegistrants didn't run). This
     * means either (a) an exception escaped during Phase 2 (ConditionalRegister
     * threw on one entry), or (b) the natural call was interrupted between
     * Phase 2 and Phase 3.
     *
     * Either way, the entries that successfully ran Register() are now
     * properly hashed into GObj. The un-registered ones aren't. Forcing
     * a SECOND ProcessRegistrants pass:
     *   - Phase 1 no-ops (GAutoRegister empty)
     *   - Phase 2 re-iterates the 200 entries; already-registered ones
     *     skip via the RF_Registered flag check, unregistered ones try
     *     again
     *   - Phase 3 clears the array (if no exception)
     *
     * Triggered once when Num >= 100 — gives ProcessRegistrants a second
     * chance to register UClass("GameEngine") and other classes from
     * Engine.dll's IMPLEMENT_CLASS static initializers. */
    {
        volatile uint32_t *gobjreg = (volatile uint32_t *)(uintptr_t)0x102A0360ULL;
        uint32_t grd = gobjreg[0], grn = gobjreg[1];
        static int force_done = 0;
        if (!force_done && grd != 0 && grn >= 100) {
            force_done = 1;
            serial_puts("[GOBJREG-FORCE] manual ProcessRegistrants @0x1010190B "
                        "with Num=");
            serial_putdec((uint64_t)grn);
            serial_puts("\n");
            uint32_t args[1] = { 0 };
            compat32_callback_args(0x1010190B, 0, args);
            uint32_t grn_after = gobjreg[1];
            serial_puts("[GOBJREG-FORCE] returned. Num: ");
            serial_putdec((uint64_t)grn);
            serial_puts(" -> ");
            serial_putdec((uint64_t)grn_after);
            serial_puts("\n");
        }
    }

    /* UT-EXE-PATCH DISABLED (Jun 4): these 3 NOPs blanked essential pool
     * linked-list writes in FMallocWindows::Link/Unlink (0x10902AF2 *prev=next
     * Unlink; 0x10902B1B PoolPtr->Next=oldhead; 0x10902B2D *head=PoolPtr) — the
     * final head update. They were a workaround for the "EBX clobber" crash in
     * this pool manager, which we now know was the GlobalAddAtomW arg-count bug
     * (fixed in 0889057). With those NOPs, the Link insert never updates the
     * list head → FirstPool/ExaustedPool lists go inconsistent → Malloc picks
     * an exhausted pool → deref FirstMem(NULL) → crash at 0x109022BE. With the
     * real EBX fix in place these NOPs are both unnecessary AND the cause, so
     * leave the pool writes intact. */
    static const int APPLY_LISTDEL_NOPS = 0;
    static int patched_ut_listdel = 0;
    if (APPLY_LISTDEL_NOPS && !patched_ut_listdel) {
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

    /* ENGINE-PATCH (the byte-NOP of `call [edx+0x54]` @0x1038887A) REMOVED:
     * its real root was a LAYER bug — GetProcAddress (kernel32_shim.c) hardcoded
     * a 4-arg thunk for every resolved function. UWindowsClient::Init does
     * GetProcAddress(ddraw,"DirectDrawCreate") (3 args); the 4-arg thunk's
     * RET 16 over-cleaned 4 bytes, so a later `push &obj->field` landed on
     * Init's saved-EBX stack slot → its `pop ebx` restored garbage → the engine's
     * subsequent `call ebx` jumped into the object and #PF'd. Fix: GetProcAddress
     * now uses win32_abi_lookup for the real argc/cc (the Phase 1 mechanism). */

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
        callback_owner_state_t *dispatch_state =
            callback_state_get(1, NULL);
        if (dispatch_state)
            dispatch_state->current_stack_args =
                (uint32_t)(uintptr_t)stack_args;

        if (ret_addr == 0x004018D1) {
            serial_puts("[NSIS-EXTRACT] result=0x");
            serial_puthex((uint32_t)g_int2e_user_rdi, 8);
            serial_puts(" handle=0x");
            serial_puthex(stack_args[0], 8);
            serial_puts("\n");
        }

        /* wdbg: dispatch any address-site hooks registered by callers
         * who want to inspect engine state when the PE is executing
         * inside a given VA range. No-op when no hooks registered. */
        if (ut99)
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

    if (ut99) {
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

        callback_owner_state_t *callback_state = callback_state_get(0, NULL);
        if (!callback_state) {
            serial_puts("[INT2E] FATAL: callback owner state missing\n");
            return 0;
        }
        int depth = callback_state->depth - 1;
        if (depth >= 16) {
            serial_puts("[INT2E] callback return depth=");
            serial_putdec(depth);
            serial_puts("\n");
        }

        if (depth < 0 || depth >= MAX_CALLBACK_DEPTH) {
            serial_puts("[INT2E] callback return depth=");
            serial_putdec(depth);
            serial_puts("\n");
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
                *tss_ist1_ptr = callback_state->saved_ist1[depth];
        }

        /* Save retval per-depth BEFORE longjmp */
        if (depth >= 0 && depth < MAX_CALLBACK_DEPTH)
            callback_state->retvals[depth] = (uint32_t)g_int2e_user_rdx;

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
            uint64_t *jb = callback_state->jmpbufs[depth];
            if (depth >= 16) {
                serial_puts("[CB32] longjmp depth="); serial_putdec(depth);
                serial_puts(" rip=0x"); serial_puthex(jb[7], 16);
                serial_puts(" rsp=0x"); serial_puthex(jb[6], 16);
                serial_puts(" cr3=0x"); serial_puthex(jb[8], 16);
                serial_puts("\n");
            }
        }
        kern_longjmp(callback_state->jmpbufs[depth], 1);
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
    BOOL unresolved_target =
        target == (uint64_t)(ULONG_PTR)unresolved_import_zero;
    uint8_t dispatch_nargs = unresolved_target ? 0 : nargs;

    if (unresolved_target) {
        serial_puts("[UNRESOLVED-CALL] ");
        if (t->name) serial_puts(t->name);
        else serial_puts("<ordinal>");
        serial_puts(" caller=0x");
        serial_puthex(stack_args[-1], 8);
        serial_puts(" argc=");
        serial_putdec(nargs);
        serial_puts("\n");
    }

    /* Recent-native-call ring buffer: record this shim dispatch (cheap, no I/O)
     * so the NULL-CALL/#PF handler can dump the calls leading up to a crash. */
    uint32_t rcall_slot;
    {
        uint32_t caller = stack_args[-1];
        uint32_t ri = g_rcall_idx ? (g_rcall_idx - 1) & 63 : 0;
        if (g_rcall_idx && g_rcall_name[ri] == t->name &&
            g_rcall_caller[ri] == caller &&
            g_rcall_nargs[ri] == nargs &&
            g_rcall_args[ri][0] == (dispatch_nargs > 0 ? stack_args[0] : 0) &&
            g_rcall_args[ri][1] == (dispatch_nargs > 1 ? stack_args[1] : 0) &&
            g_rcall_args[ri][2] == (dispatch_nargs > 2 ? stack_args[2] : 0) &&
            g_rcall_args[ri][3] == (dispatch_nargs > 3 ? stack_args[3] : 0)) {
            g_rcall_repeat[ri]++;
        } else {
            ri = g_rcall_idx & 63;
            g_rcall_name[ri] = t->name;
            g_rcall_nargs[ri] = nargs;
            g_rcall_caller[ri] = caller;
            g_rcall_repeat[ri] = 1;
            g_rcall_idx++;
        }
        rcall_slot = ri;
        g_rcall_args[ri][0] = dispatch_nargs > 0 ? stack_args[0] : 0;
        g_rcall_args[ri][1] = dispatch_nargs > 1 ? stack_args[1] : 0;
        g_rcall_args[ri][2] = dispatch_nargs > 2 ? stack_args[2] : 0;
        g_rcall_args[ri][3] = dispatch_nargs > 3 ? stack_args[3] : 0;
        g_rcall_ebp[ri] = (uint32_t)g_int2e_user_rbp;
        g_rcall_esp[ri] = (uint32_t)(uintptr_t)stack_args - 4;
        g_rcall_ret[ri] = 0;
    }

    /* Save the 13th stack arg for CreateWindowExW workaround. */
    g_compat32_last_stack_arg13 =
        (dispatch_nargs >= 12) ? stack_args[12] : 0;

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

    /* Stack alignment check + ESP delta tracker.
     * Each compat32 thunk does `RET n*4` (stdcall) or `RET` (cdecl).
     * If the RET pops the wrong number of bytes, the caller's ESP is
     * shifted by the delta, silently corrupting callee-saved registers
     * (EBX/ESI/EDI/EBP).  This detector compares the ESP expected from
     * the PREVIOUS thunk's RET against the current thunk's actual ESP.
     * Global state because it's reset by compat32_enter via reset_stack_delta(). */
    {
        static uint32_t st_prev_esp = 0;
        static uint8_t  st_prev_nargs = 0;
        static uint8_t  st_prev_cc = 0;
        static const char *st_prev_name = NULL;
        static int32_t  st_acc = 0;
        static uint32_t st_bad = 0;

        uint32_t esp32 = (uint32_t)(uintptr_t)stack_args - 4;

        if (st_prev_esp != 0) {
            /* Expected ESP at this INT 0x2E entry if the PREVIOUS thunk's
             * RET N was correct:
             *   st_prev_esp          = ESP at previous INT 0x2E
             *   + st_prev_nargs*4+4  = cleanup by previous RET N + return addr
             *   - nargs*4 - 4        = push of this call's args + call's ret addr
             *   = st_prev_esp + (st_prev_nargs - nargs)*4
             * Only valid for stdcall (callee cleans); cdecl frames are caller-
             * cleaned and the ESP between calls depends on caller code. */
            uint32_t expected = st_prev_esp + ((int32_t)st_prev_nargs - (int32_t)nargs) * 4;
            int32_t delta = (int32_t)(esp32 - expected);
            /* Only track for stdcall prev calls where the thunk controls
             * cleanup.  Large deltas (> 1MB) mean a stack switch (CRT stub
             * → PE32), not corruption. */
            if (delta != 0 && (uint32_t)(delta > 0 ? delta : -delta) < 0x100000 && st_prev_cc == CC_STDCALL) {
                st_acc += delta;
                st_bad++;
                if (st_bad <= 64) {
#ifndef OK_QUIET
                    serial_puts("[STACK-DELTA] prev=");
                    if (st_prev_name) serial_puts(st_prev_name);
                    else serial_puts("?");
                    serial_puts(" argc=");
                    serial_putdec(st_prev_nargs);
                    serial_puts(" delta=");
                    serial_putdec((int64_t)delta);
                    serial_puts(" acc=");
                    serial_putdec((int64_t)st_acc);
                    serial_puts(" next=");
                    if (t->name) serial_puts(t->name);
                    serial_puts(" cur_esp=0x");
                    serial_puthex(esp32, 8);
                    serial_puts(" prev_esp=0x");
                    serial_puthex(st_prev_esp, 8);
                    serial_puts("\n");
#endif
                }
            }
        }
        st_prev_esp   = esp32;
        st_prev_nargs = nargs;
        st_prev_cc    = t->callconv & CC_CONVENTION_MASK;
        st_prev_name  = t->name;

        /* Also check alignment — a misaligned ESP is always a bug */
        if (esp32 & 3) {
            serial_puts("[COMPAT32] *** ESP MISALIGNED: 0x");
            serial_puthex(esp32, 8);
            serial_puts(" thunk=");
            if (t->name) serial_puts(t->name);
            serial_puts("\n");
        }
    }

    /*
     * Call the 64-bit shim function with marshaled arguments.
     * ms_abi: first 4 args in RCX, RDX, R8, R9; rest on stack.
     *
     * OpenGL 1.1's glMap2d consumes 15 PE32 DWORD slots because each double
     * occupies two slots.  Keep one extra slot for the pointer appended by a
     * variadic compatibility bridge.
     */
#define COMPAT32_MAX_DISPATCH_ARGS 16
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
    typedef uint64_t (WINAPI *fn10)(uint64_t, uint64_t, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t, uint64_t,
                                     uint64_t, uint64_t);
    typedef uint64_t (WINAPI *fn11)(uint64_t, uint64_t, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t);
    typedef uint64_t (WINAPI *fn12)(uint64_t, uint64_t, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t, uint64_t);
    typedef uint64_t (WINAPI *fn13)(uint64_t, uint64_t, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t, uint64_t,
                                     uint64_t);
    typedef uint64_t (WINAPI *fn14)(uint64_t, uint64_t, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t, uint64_t,
                                     uint64_t, uint64_t);
    typedef uint64_t (WINAPI *fn15)(uint64_t, uint64_t, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t);
    typedef uint64_t (WINAPI *fn16)(uint64_t, uint64_t, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t, uint64_t);

    /* Zero-extend 32-bit stack args to 64-bit */
    uint64_t a[COMPAT32_MAX_DISPATCH_ARGS] = {0};
    if (dispatch_nargs > COMPAT32_MAX_DISPATCH_ARGS) {
        serial_puts("[COMPAT32] Too many DWORD arguments for ");
        serial_puts(t->name ? t->name : "<unnamed>");
        serial_puts(": ");
        serial_putdec(dispatch_nargs);
        serial_puts("\n");
        return 0;
    }
    for (int i = 0; i < dispatch_nargs; i++)
        a[i] = (uint64_t)stack_args[i];

    int saved_compat_mode = g_compat32_mode;
    g_compat32_mode = 1;
    uint64_t result = 0;
    if (!compat32_dispatch_nonlocal_jump(target, stack_args, &result)) {
        uint8_t call_nargs = dispatch_nargs;
        const void *bridge = unresolved_target ? NULL :
            win32_abi_compat32_bridge((const void *)(ULONG_PTR)target);
        if (t->callconv & CC_VARIADIC) {
            if (!bridge || dispatch_nargs >= COMPAT32_MAX_DISPATCH_ARGS) {
                serial_puts("[COMPAT32] Missing PE32 variadic bridge for ");
                serial_puts(t->name ? t->name : "<unnamed>");
                serial_puts("\n");
                g_compat32_mode = saved_compat_mode;
                return 0;
            }
            a[dispatch_nargs] =
                (uint64_t)(ULONG_PTR)(stack_args + dispatch_nargs);
            call_nargs++;
        }
        if (bridge)
            target = (uint64_t)(ULONG_PTR)bridge;

        switch (call_nargs) {
        case 0:  result = ((fn0)(ULONG_PTR)target)(); break;
        case 1:  result = ((fn1)(ULONG_PTR)target)(a[0]); break;
        case 2:  result = ((fn2)(ULONG_PTR)target)(a[0], a[1]); break;
        case 3:  result = ((fn3)(ULONG_PTR)target)(a[0], a[1], a[2]); break;
        case 4:  result = ((fn4)(ULONG_PTR)target)(a[0], a[1], a[2], a[3]); break;
        case 5:  result = ((fn5)(ULONG_PTR)target)(a[0], a[1], a[2], a[3], a[4]); break;
        case 6:  result = ((fn6)(ULONG_PTR)target)(a[0], a[1], a[2], a[3], a[4], a[5]); break;
        case 7:  result = ((fn7)(ULONG_PTR)target)(a[0], a[1], a[2], a[3], a[4], a[5], a[6]); break;
        case 8:  result = ((fn8)(ULONG_PTR)target)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]); break;
        case 9:  result = ((fn9)(ULONG_PTR)target)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]); break;
        case 10: result = ((fn10)(ULONG_PTR)target)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9]); break;
        case 11: result = ((fn11)(ULONG_PTR)target)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10]); break;
        case 12: result = ((fn12)(ULONG_PTR)target)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11]); break;
        case 13: result = ((fn13)(ULONG_PTR)target)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12]); break;
        case 14: result = ((fn14)(ULONG_PTR)target)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13]); break;
        case 15: result = ((fn15)(ULONG_PTR)target)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13], a[14]); break;
        default: result = ((fn16)(ULONG_PTR)target)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15]); break;
        }
    }
    g_compat32_mode = saved_compat_mode;
    {
        extern void win32_main_termination_checkpoint(void);
        win32_main_termination_checkpoint();
    }
    g_rcall_ret[rcall_slot] = (uint32_t)result;
#ifndef OK_QUIET
    if (nargs == 3 && a[2] == 0x54 && t->name &&
        t->name[0] == 'H' && t->name[1] == 'e' && t->name[2] == 'a' &&
        t->name[3] == 'p' && t->name[4] == 'A' && t->name[5] == 'l' &&
        t->name[6] == 'l' && t->name[7] == 'o' && t->name[8] == 'c' &&
        t->name[9] == 0) {
        uint32_t ebp = (uint32_t)g_int2e_user_rbp;
        uint32_t caller0 = 0, caller1 = 0;
        if (ebp >= 0x10000 && ebp < 0x7FFFFFF8) {
            uint32_t parent = *(volatile uint32_t *)(uintptr_t)ebp;
            caller0 = *(volatile uint32_t *)(uintptr_t)(ebp + 4);
            if (parent >= 0x10000 && parent < 0x7FFFFFF8)
                caller1 = *(volatile uint32_t *)(uintptr_t)(parent + 4);
        }
        serial_puts("[HEAP54] ret=0x");
        serial_puthex((uint32_t)result, 8);
        serial_puts(" edi=0x");
        serial_puthex((uint32_t)g_int2e_user_rdi, 8);
        serial_puts(" callers=0x");
        serial_puthex(caller0, 8);
        serial_puts("/0x");
        serial_puthex(caller1, 8);
        serial_puts("\n");
    }
#endif
    return result;
#undef COMPAT32_MAX_DISPATCH_ARGS
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
