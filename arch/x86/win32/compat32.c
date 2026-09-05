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
#include "ntsyscall.h"
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
extern void kern_longjmp(uint64_t *buf, int val) __attribute__((noreturn));
extern DWORD win32_current_process_id(void);
#ifndef TEST_HARNESS
extern int32_t proc_current_pid(void);
#endif

/* ── Global compat32 mode flag ────────────────────────────────── */

#ifndef __KERNEL_X86__
int g_compat32_mode = 0;
#endif

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
#define RUNTIME_X87_WRAPPER_OFFSET 0x400U
#define RUNTIME_X87_WRAPPER_SIZE   128U
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
static void emit_ftol_stub(uint8_t *code);
static int emit_x87_cdecl_result_wrapper(uint8_t *code,
                                         uint32_t wrapper_addr,
                                         uint32_t helper_addr,
                                         uint8_t num_args);
static uint32_t compat32_make_thunk_runtime(uint64_t target,
                                            const char *name,
                                            uint8_t num_args,
                                            uint8_t callconv);
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

/* ── Callback mechanism (64-bit → 32-bit → 64-bit) ─────────── */

/*
 * Magic thunk index for callback return. When the INT 0x2E handler
 * sees this index, it longjmps back to the caller instead of
 * dispatching to a shim function.
 */
#define THUNK_CALLBACK_RETURN  0xFFFFFFFE

/* Return stub address (32-bit code in thunk pool that INT 0x2Es back) */
static uint32_t callback_return_stub_addr = 0;

/* Native i386 SEH handler installed while an unwind handler is running. */
static uint32_t unwind_protector_stub_addr = 0;

static uint64_t WINAPI unresolved_import_zero(void)
{
    return 0;
}

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
#define MAX_INT2E_DEPTH       128
#define RECENT_CALL_COUNT      64
#define CALL_TRACE_SIZE        64
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
    uint32_t Ebp;
    uint32_t Ebx;
    uint32_t Edi;
    uint32_t Esi;
    uint32_t Esp;
    uint32_t Eip;
    uint32_t Registration;
    uint32_t TryLevel;
    uint32_t Cookie;
    uint32_t UnwindFunc;
    uint32_t UnwindData[6];
} WIN32_JUMP_BUFFER32;

typedef struct __attribute__((packed, aligned(4))) {
    uint32_t ExceptionRecord;
    uint32_t ContextRecord;
} EXCEPTION_POINTERS32;

typedef struct __attribute__((packed, aligned(4))) {
    EXCEPTION_RECORD32 exception_record;
    EXCEPTION_POINTERS32 exception_pointers;
    CONTEXT32 context;
    uint32_t dispatcher_frame;
    uint32_t active_frame;
} seh32_dispatch_slot_t;

#define CONTEXT32_ARCH_MASK   0x00FF0000U
#define CONTEXT32_I386        0x00010000U
#define CONTEXT32_CONTROL_BIT 0x00000001U
#define CONTEXT32_INTEGER_BIT 0x00000002U
#define CONTEXT32_FULL        0x00010007U

_Static_assert(sizeof(FLOATING_SAVE_AREA32) == 112,
               "Win32 FLOATING_SAVE_AREA ABI changed");
_Static_assert(sizeof(CONTEXT32) == 716, "Win32 CONTEXT32 ABI changed");
_Static_assert(sizeof(WIN32_JUMP_BUFFER32) == 64,
               "Win32 i386 jump buffer ABI changed");
_Static_assert(__builtin_offsetof(WIN32_JUMP_BUFFER32, Eip) == 20,
               "Win32 i386 jump buffer Eip offset changed");
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

/* The assembly entry frame begins at the saved IST1 cursor. Keep this layout
 * synchronized with int2e_stub.S; C consumes it to make register and unwind
 * state scheduler-slot-local rather than sharing mutable globals. */
typedef struct {
    uint64_t saved_ist1;
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx;
    uint64_t rip, cs, rflags, rsp, ss;
} compat32_int2e_frame_t;

_Static_assert(__builtin_offsetof(compat32_int2e_frame_t, rbp) == 72,
               "INT2E RBP frame offset changed");
_Static_assert(__builtin_offsetof(compat32_int2e_frame_t, rdx) == 96,
               "INT2E RDX frame offset changed");
_Static_assert(__builtin_offsetof(compat32_int2e_frame_t, rip) == 120,
               "INT2E RIP frame offset changed");
_Static_assert(sizeof(compat32_int2e_frame_t) == 160,
               "INT2E frame size changed");

typedef struct {
    uint64_t frame_address;
    uint64_t rax, rbp, rcx, rdx, rsi, rdi, rbx;
} compat32_int2e_context_t;

typedef struct {
    uint32_t eip, esp, ebp;
    uint32_t ebx, esi, edi;
    uint32_t eax, ecx, edx, eflags;
    uint32_t restore_nonvolatile;
    uint32_t restore_volatile;
    uint32_t restore_eflags;
} compat32_unwind_state_t;

typedef struct {
    const char *name;
    uint32_t args[4];
    uint32_t result;
    uint32_t caller;
    uint32_t repeat;
    uint32_t ebp;
    uint32_t esp;
    uint8_t nargs;
} compat32_recent_call_t;

/* kern_setjmp / kern_longjmp use a 9-quad jmp_buf:
 *   [0..5] callee-saved GPRs (rbx, rbp, r12-r15)
 *   [6]    rsp     [7] rip     [8] cr3
 * Sizing this array as [...][8] truncated each slot to 64 bytes and the
 * setjmp at depth N+1 wrote its rbx (slot[0]) on top of slot N's cr3 —
 * causing the depth=0 longjmp after _initterm to triple-fault on a
 * bogus 0x40 CR3. Must be at least 9. */
typedef struct {
    uint32_t owner_kernel_pid;
    int depth;
    int int2e_depth;
    int seh_dispatch_depth;
    uint64_t jmpbufs[MAX_CALLBACK_DEPTH][9];
    uint64_t saved_ist1[MAX_CALLBACK_DEPTH];
    uint32_t saved_stack_args[MAX_CALLBACK_DEPTH];
    uint32_t callback_int2e_depth[MAX_CALLBACK_DEPTH];
    uint8_t *stacks[MAX_CALLBACK_DEPTH];
    DWORD stack_process_ids[MAX_CALLBACK_DEPTH];
    uint32_t retvals[MAX_CALLBACK_DEPTH];
    uint32_t current_stack_args;
    seh32_dispatch_slot_t *seh32_slots;
    DWORD seh32_process_id;
    int in_catch_dispatch;
    uint32_t saved_next_frame;
    compat32_int2e_context_t int2e_contexts[MAX_INT2E_DEPTH];
    compat32_unwind_state_t unwind;
    uint32_t last_caller_eip;
    uint32_t last_stack_args;
    compat32_recent_call_t recent_calls[RECENT_CALL_COUNT];
    uint32_t recent_call_index;
    uint32_t call_trace[CALL_TRACE_SIZE];
    uint32_t call_trace_index;
} callback_owner_state_t;

static int seh32_stack_region(const TEB32 *teb,
                              const callback_owner_state_t *state,
                              uint32_t address, uint32_t size);
static int compat32_abandon_callbacks(callback_owner_state_t *state,
                                      compat32_int2e_frame_t *frame,
                                      uint32_t target_esp);

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

static uint32_t callback_owner_kernel_pid(void)
{
#ifndef TEST_HARNESS
    int32_t pid = proc_current_pid();
    return pid > 0 ? (uint32_t)pid : 0;
#else
    return 1;
#endif
}

static void callback_state_reset(callback_owner_state_t *state,
                                 uint32_t owner_kernel_pid)
{
    if (!state)
        return;
    memset(state, 0, sizeof(*state));
    state->owner_kernel_pid = owner_kernel_pid;
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

    /* Scheduler slots are reusable, while the callback state stored beside
     * them outlives the task. Win32 PID 1 is also reused by successive direct
     * WinExec calls, so neither that PID nor a low virtual address identifies
     * a live callback stack. The kernel PID is monotonic and identifies the
     * current slot incarnation; discard every saved pointer/jump frame when a
     * different task inherits the slot. Process VM teardown owns the old
     * allocations and has already unmapped them. */
    uint32_t kernel_pid = callback_owner_kernel_pid();
    if (state && kernel_pid && state->owner_kernel_pid != kernel_pid)
        callback_state_reset(state, kernel_pid);
    return state;
}

static compat32_int2e_context_t *int2e_context_current(
    callback_owner_state_t *state)
{
    if (!state || state->int2e_depth <= 0 ||
        state->int2e_depth > MAX_INT2E_DEPTH)
        return NULL;
    return &state->int2e_contexts[state->int2e_depth - 1];
}

static int int2e_context_push(callback_owner_state_t *state,
                              const compat32_int2e_frame_t *frame)
{
    if (!state || !frame)
        return 0;
    if (state->int2e_depth < 0 || state->int2e_depth >= MAX_INT2E_DEPTH) {
        serial_puts("[INT2E] FATAL: per-thread transition depth overflow\n");
        return 0;
    }

    compat32_int2e_context_t *context =
        &state->int2e_contexts[state->int2e_depth++];
    context->frame_address = (uint64_t)(ULONG_PTR)frame;
    context->rax = 0;
    context->rbp = frame->rbp;
    context->rcx = frame->rcx;
    context->rdx = frame->rdx;
    context->rsi = frame->rsi;
    context->rdi = frame->rdi;
    context->rbx = frame->rbx;
    return 1;
}

static int int2e_context_pop(callback_owner_state_t *state,
                             const compat32_int2e_frame_t *frame)
{
    compat32_int2e_context_t *context = int2e_context_current(state);
    if (!context || context->frame_address != (uint64_t)(ULONG_PTR)frame)
        return 0;
    memset(context, 0, sizeof(*context));
    state->int2e_depth--;
    return 1;
}

static int unwind_apply(callback_owner_state_t *state,
                        compat32_cpu_context_t *context)
{
    if (!state || !context || !state->unwind.eip)
        return 0;

    context->eip = state->unwind.eip;
    context->esp = state->unwind.esp;
    context->ebp = state->unwind.ebp;
    if (state->unwind.restore_nonvolatile) {
        context->ebx = state->unwind.ebx;
        context->esi = state->unwind.esi;
        context->edi = state->unwind.edi;
    }
    if (state->unwind.restore_volatile) {
        context->eax = state->unwind.eax;
        context->ecx = state->unwind.ecx;
        context->edx = state->unwind.edx;
    }
    if (state->unwind.restore_eflags)
        context->eflags = state->unwind.eflags;
    memset(&state->unwind, 0, sizeof(state->unwind));
    return 1;
}

int compat32_apply_pending_unwind(compat32_cpu_context_t *context)
{
    return unwind_apply(callback_state_get(0, NULL), context);
}

/* Called by int2e_stub.S after compat32_dispatch returns normally. It applies
 * this scheduler slot's pending control transfer directly to the saved IRET
 * frame, pops only the matching transition, and preserves the shim result. */
uint64_t compat32_int2e_complete(compat32_int2e_frame_t *frame,
                                 uint64_t result)
{
    callback_owner_state_t *state = callback_state_get(0, NULL);
    compat32_int2e_context_t *entry = int2e_context_current(state);
    if (!entry || entry->frame_address != (uint64_t)(ULONG_PTR)frame)
        return result;

    compat32_cpu_context_t context = {
        .eax = (uint32_t)result,
        .ebx = (uint32_t)frame->rbx,
        .ecx = (uint32_t)frame->rcx,
        .edx = (uint32_t)frame->rdx,
        .esi = (uint32_t)frame->rsi,
        .edi = (uint32_t)frame->rdi,
        .ebp = (uint32_t)frame->rbp,
        .esp = (uint32_t)frame->rsp,
        .eip = (uint32_t)frame->rip,
        .eflags = (uint32_t)frame->rflags,
    };
    int abandoned = 0;
    if (unwind_apply(state, &context)) {
        frame->rip = context.eip;
        frame->rsp = context.esp;
        frame->rbp = context.ebp;
        frame->rbx = context.ebx;
        frame->rcx = context.ecx;
        frame->rdx = context.edx;
        frame->rsi = context.esi;
        frame->rdi = context.edi;
        frame->rflags = context.eflags;
        result = (result & 0xFFFFFFFF00000000ULL) | context.eax;
        abandoned = compat32_abandon_callbacks(state, frame, context.esp);
    }
    if (!abandoned)
        (void)int2e_context_pop(state, frame);
    return result;
}

static int seh32_scratch_prepare(callback_owner_state_t *state)
{
    if (!state) return 0;

    DWORD process_id = win32_current_process_id();
    if (!process_id) process_id = 1;

    /* Scheduler slots are reusable and the old VMA disappears with its
     * process address space.  Never carry its numeric pointer into a new CR3. */
    if (state->seh32_slots && state->seh32_process_id != process_id) {
        state->seh32_slots = NULL;
        state->seh32_process_id = 0;
    }
    if (!state->seh32_slots) {
        PVOID page = NULL;
        SIZE_T size = 4096;
        NTSTATUS status = nt_vm_allocate_compat32(
            &page, &size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!NT_SUCCESS(status) || !page ||
            (uint64_t)(ULONG_PTR)page + 4096ULL > UINT32_MAX) {
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
    static uint32_t allocation_failure_logs;

    if (!state || depth < 0 || depth >= MAX_CALLBACK_DEPTH)
        return NULL;
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
        PVOID allocation = NULL;
        SIZE_T size = CALLBACK_STACK_SIZE;
        NTSTATUS status = nt_vm_allocate_compat32(
            &allocation, &size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        uint8_t *stack = (uint8_t *)allocation;
        if (!NT_SUCCESS(status) || !stack ||
            (uint64_t)(ULONG_PTR)stack + CALLBACK_STACK_SIZE > UINT32_MAX) {
            if (allocation_failure_logs < 8) {
                allocation_failure_logs++;
                serial_puts("[CB32] low callback stack unavailable depth=");
                serial_putdec((uint32_t)depth);
                serial_puts(" pid=");
                serial_putdec(process_id);
                serial_puts(" base=0x");
                serial_puthex((uint64_t)(ULONG_PTR)stack, 16);
                serial_puts(" mode=");
                serial_putdec((uint32_t)g_compat32_mode);
                serial_puts("\n");
            }
            if (stack) VirtualFree(stack, 0, MEM_RELEASE);
            return NULL;
        }
        state->stacks[depth] = stack;
        state->stack_process_ids[depth] = process_id;
    }
    return state->stacks[depth];
}

void compat32_release_thread_state(void)
{
#ifndef TEST_HARNESS
    callback_owner_state_t *state = callback_state_get(0, NULL);
    if (!state)
        return;

    DWORD process_id = win32_current_process_id();
    if (!process_id)
        process_id = 1;

    for (int i = 0; i < MAX_CALLBACK_DEPTH; i++) {
        if (!state->stacks[i] ||
            state->stack_process_ids[i] != process_id)
            continue;
        (void)nt_vm_release_allocation_for_process(process_id,
                                                   state->stacks[i]);
    }
    if (state->seh32_slots && state->seh32_process_id == process_id) {
        (void)nt_vm_release_allocation_for_process(process_id,
                                                   state->seh32_slots);
    }

    callback_state_reset(state, state->owner_kernel_pid);
#endif
}

/* These diagnostics belong to the scheduled PE thread. A process may block
 * in a shim while another thread enters INT2E, so retaining them globally
 * produces misleading call sites and can feed the wrong stack to a shim. */
uint32_t compat32_get_last_caller_eip(void)
{
    callback_owner_state_t *state = callback_state_get(0, NULL);
    return state ? state->last_caller_eip : 0;
}

/* Pointer (as uint32_t) to the user-mode stack at the args, equal to
 * user ESP+4 at the moment of the INT 0x2E. Updated on each dispatch
 * entry. Used by VirtualAlloc shim to walk the user stack chain. */
uint32_t compat32_get_last_stack_args(void)
{
    callback_owner_state_t *state = callback_state_get(0, NULL);
    return state ? state->last_stack_args : 0;
}

/* Recent-native-call ring buffer (diagnostic): records every INT 0x2E shim
 * dispatch (no serial I/O) so the #PF/NULL-CALL handler can dump the last ~24
 * native calls before a crash — to find a shim whose wrong arg-count/return
 * corrupted the caller's registers/stack (the New-Game LocalMapURL NULL-vtable
 * crash). Safe to add now that the IST1 stack-overflow is fixed. */
void compat32_dump_recent_calls(void)
{
    serial_puts("[RCALL] last native calls before fault (oldest->newest):\n");
    callback_owner_state_t *state = callback_state_get(0, NULL);
    if (!state)
        return;
    uint32_t end = state->recent_call_index;
    uint32_t start = (end >= RECENT_CALL_COUNT)
                   ? end - RECENT_CALL_COUNT : 0;
    for (uint32_t k = start; k < end; k++) {
        compat32_recent_call_t *call =
            &state->recent_calls[k & (RECENT_CALL_COUNT - 1U)];
        serial_puts("  ");
        serial_putdec(k);
        serial_puts(": ");
        serial_puts(call->name ? call->name : "?");
        serial_puts(" (");
        serial_putdec(call->nargs);
        serial_puts(" args) caller=0x");
        serial_puthex(call->caller, 8);
        serial_puts(" ebp=0x");
        serial_puthex(call->ebp, 8);
        serial_puts(" esp=0x");
        serial_puthex(call->esp, 8);
        if (call->repeat > 1) {
            serial_puts(" repeat=");
            serial_putdec(call->repeat);
        }
        serial_puts(" a=[0x");
        serial_puthex(call->args[0], 8);
        serial_puts(" 0x"); serial_puthex(call->args[1], 8);
        serial_puts(" 0x"); serial_puthex(call->args[2], 8);
        serial_puts(" 0x"); serial_puthex(call->args[3], 8);
        serial_puts("] ret=0x");
        serial_puthex(call->result, 8);
        serial_puts("\n");
    }
}

uint32_t compat32_get_last_user_ebp(void)
{
    compat32_int2e_context_t *context = int2e_context_current(
        callback_state_get(0, NULL));
    return context ? (uint32_t)context->rbp : 0;
}

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

static uint16_t thunk_cleanup_bytes(uint8_t num_args, uint8_t callconv)
{
    if ((callconv & CC_CONVENTION_MASK) == CC_CDECL || num_args == 0)
        return 0;
    return (uint16_t)num_args * 4U;
}

static void emit_thunk(uint8_t *code, uint64_t target, uint8_t num_args,
                       uint8_t callconv)
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
    (void)target;
    /*
     * OsitoK bare-metal path: PE32 code runs in 32-bit compat mode.
     * The thunk is 32-bit code that does INT 0x2E to enter the kernel.
     * EAX carries the thunk index. Argument registers must remain untouched:
     * the dispatcher reads the argument count from the thunk table and uses
     * the saved ECX for PE32 thiscall methods.
     */

    /* RtlCaptureContext must observe EAX before the gateway uses it for the
     * thunk index. Its dispatcher path consumes this saved DWORD. */
    if (callconv & CC_CONTEXT_CAPTURE)
        code[p++] = 0x50;  /* PUSH EAX */

    /* MOV EAX, <thunk_index> */
    code[p++] = 0xB8;
    uint32_t idx = thunk_count;
    code[p++] = (uint8_t)(idx);
    code[p++] = (uint8_t)(idx >> 8);
    code[p++] = (uint8_t)(idx >> 16);
    code[p++] = (uint8_t)(idx >> 24);

    /* INT 0x2E */
    code[p++] = 0xCD;
    code[p++] = 0x2E;

    if (callconv & CC_CONTEXT_CAPTURE) {
        code[p++] = 0x8D;  /* LEA ESP, [ESP+4] (preserve EFLAGS) */
        code[p++] = 0x64;
        code[p++] = 0x24;
        code[p++] = 0x04;
    }

    /*
     * Calling convention determines stack cleanup:
     *   stdcall (Win32 API): callee cleans → RET N  (C2 xx xx)
     *   cdecl   (MSVCRT):    caller cleans → RET    (C3)
     *
     * CRITICAL: Using RET N for cdecl functions causes double cleanup —
     * the thunk pops N bytes, then the caller also does add esp,N or pop.
     * This corrupts the stack and causes wild jumps after a few calls.
     */
    uint16_t cleanup = thunk_cleanup_bytes(num_args, callconv);
    if (cleanup == 0) {
        code[p++] = 0xC3;  /* RET — caller will clean up args */
    } else {
        code[p++] = 0xC2;  /* RET imm16 — callee cleans (stdcall) */
        code[p++] = (uint8_t)(cleanup);
        code[p++] = (uint8_t)(cleanup >> 8);
    }
#endif

    /* Fill remainder with INT3 (debug trap) */
    while (p < THUNK_STUB_SIZE)
        code[p++] = 0xCC;
}

static int thunk_stub_matches_contract(const uint8_t *code, uint8_t num_args,
                                       uint8_t callconv)
{
#ifdef TEST_HARNESS
    (void)code;
    (void)num_args;
    (void)callconv;
    return 1;
#else
    uint16_t cleanup = thunk_cleanup_bytes(num_args, callconv);
    uint32_t p = 0;
    if (callconv & CC_CONTEXT_CAPTURE) {
        if (code[p++] != 0x50)
            return 0;
    }
    if (code[p++] != 0xB8)
        return 0;
    p += 4;
    if (code[p++] != 0xCD || code[p++] != 0x2E)
        return 0;
    if (callconv & CC_CONTEXT_CAPTURE) {
        if (code[p++] != 0x8D || code[p++] != 0x64 ||
            code[p++] != 0x24 || code[p++] != 0x04)
            return 0;
    }
    if (cleanup == 0)
        return code[p] == 0xC3;
    return code[p] == 0xC2 && code[p + 1] == (uint8_t)cleanup &&
           code[p + 2] == (uint8_t)(cleanup >> 8);
#endif
}

static int compat32_marshal_dwords(const compat32_thunk_t *thunk,
                                   const uint32_t *stack_args,
                                   uint8_t stack_argc, uint32_t ecx,
                                   uint32_t edx, uint64_t *arguments,
                                   uint8_t capacity)
{
    if (!thunk || !arguments || thunk->logical_args > capacity)
        return 0;

    uint8_t stack_index = 0;
    for (uint8_t i = 0; i < thunk->logical_args; i++) {
        if (i == thunk->ecx_arg) {
            arguments[i] = ecx;
        } else if (i == thunk->edx_arg) {
            arguments[i] = edx;
        } else if (stack_args && stack_index < stack_argc) {
            arguments[i] = stack_args[stack_index++];
        } else {
            return 0;
        }
    }
    return stack_index == stack_argc;
}

int compat32_thunk_contract_selftest(void)
{
#ifdef TEST_HARNESS
    return 0;
#else
    uint8_t code[THUNK_STUB_SIZE];
    int failures = 0;

    emit_thunk(code, 0, 3, CC_STDCALL);
    if (!thunk_stub_matches_contract(code, 3, CC_STDCALL))
        failures++;
    code[8] ^= 4;
    if (thunk_stub_matches_contract(code, 3, CC_STDCALL))
        failures++;

    emit_thunk(code, 0, 2, CC_CDECL);
    if (!thunk_stub_matches_contract(code, 2, CC_CDECL))
        failures++;

    emit_thunk(code, 0, 1, CC_STDCALL | CC_CONTEXT_CAPTURE);
    if (!thunk_stub_matches_contract(
            code, 1, CC_STDCALL | CC_CONTEXT_CAPTURE))
        failures++;
    code[11] ^= 1;
    if (thunk_stub_matches_contract(
            code, 1, CC_STDCALL | CC_CONTEXT_CAPTURE))
        failures++;

    emit_thunk(code, 0, 2, CC_THISCALL);
    if (!thunk_stub_matches_contract(code, 2, CC_THISCALL))
        failures++;

    emit_thunk(code, 0, 1, CC_FASTCALL);
    if (!thunk_stub_matches_contract(code, 1, CC_FASTCALL))
        failures++;

    emit_thunk(code, 0, 0, CC_STDCALL);
    if (!thunk_stub_matches_contract(code, 0, CC_STDCALL))
        failures++;

    const uint32_t stack_args[] = { 0x11111111U, 0x22222222U };
    uint64_t arguments[4] = {0};
    compat32_thunk_t thunk = {
        .num_args = 2,
        .logical_args = 4,
        .ecx_arg = 2,
        .edx_arg = 3,
    };
    if (!compat32_marshal_dwords(&thunk, stack_args, 2, 0x33333333U,
                                 0x44444444U, arguments, 4) ||
        arguments[0] != 0x11111111U || arguments[1] != 0x22222222U ||
        arguments[2] != 0x33333333U || arguments[3] != 0x44444444U)
        failures++;

    thunk.num_args = 1;
    thunk.logical_args = 3;
    thunk.ecx_arg = 0;
    thunk.edx_arg = 1;
    if (!compat32_marshal_dwords(&thunk, stack_args, 1, 0x33333333U,
                                 0x44444444U, arguments, 4) ||
        arguments[0] != 0x33333333U || arguments[1] != 0x44444444U ||
        arguments[2] != 0x11111111U)
        failures++;

    return failures;
#endif
}

/* ── Public API ──────────────────────────────────────────────── */

void compat32_init(void)
{
    /* Re-exec resets only this scheduler slot. Its prior callback-stack VMAs
     * were released with the process address space. */
    int owner = -1;
    callback_owner_state_t *callback_state = callback_state_get(1, &owner);
    if (!callback_state) {
        serial_puts("[COMPAT32] Failed to allocate callback owner state\n");
        return;
    }
    (void)owner;
    callback_state_reset(callback_state,
                         callback_state->owner_kernel_pid);

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
        win32_abi_register_compat32_direct(
            (const void *)crt_qsort, qsort32_blob_addr);
        win32_abi_register_compat32_direct(
            (const void *)crt_bsearch,
            qsort32_blob_addr + QSORT32_BSEARCH_OFF);
    }

    {
        uint8_t *blob = thunk_pool + RUNTIME_ATOF_PAGE * 4096U;
        emit_atof32_blob(blob);
        atof32_blob_addr = compat32_runtime_addr +
                           RUNTIME_ATOF_PAGE * 4096U;
        serial_puts("[COMPAT32] atof32 blob at 0x");
        serial_puthex(atof32_blob_addr, 8);
        serial_puts("\n");
        win32_abi_register_compat32_direct(
            (const void *)crt_atof, atof32_blob_addr);
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

    /* RtlpExecuteHandlerForUnwind installs this three-DWORD registration:
     *   Prev, Handler, OriginalFrame
     * A nested unwind reaches the protector before the handler being
     * unwound. It publishes OriginalFrame through DispatcherContext and
     * returns ExceptionCollidedUnwind, matching the i386 NT contract. */
    {
        static const uint8_t stub[] = {
            0xB8,0x01,0x00,0x00,0x00,       /* mov eax,ContinueSearch */
            0x8B,0x4C,0x24,0x04,            /* mov ecx,[esp+4]        */
            0xF7,0x41,0x04,0x06,0x00,0x00,0x00, /* test flags,6       */
            0x74,0x15,                      /* jz return              */
            0x8B,0x4C,0x24,0x08,            /* mov ecx,[esp+8]        */
            0x8B,0x54,0x24,0x10,            /* mov edx,[esp+16]       */
            0x8B,0x41,0x08,                 /* mov eax,[ecx+8]        */
            0x89,0x02,                      /* mov [edx],eax          */
            0xB8,0x03,0x00,0x00,0x00,       /* mov eax,CollidedUnwind */
            0xC2,0x10,0x00,                 /* ret 16                 */
            0xC2,0x10,0x00,                 /* return: ret 16         */
        };
        uint32_t offset = THUNK_POOL_BYTES - 192;
        memcpy(thunk_pool + offset, stub, sizeof(stub));
        unwind_protector_stub_addr = compat32_runtime_addr + offset;
        serial_puts("[COMPAT32] Unwind protector stub at 0x");
        serial_puthex(unwind_protector_stub_addr, 8);
        serial_puts("\n");
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

    /* The x87 argument cannot cross the 32->64 gateway. Install one native
     * PE32 implementation and bind it to the CRT provider target. */
    {
        uint32_t offset = THUNK_POOL_BYTES - 128;
        emit_ftol_stub(thunk_pool + offset);
        uint32_t ftol_addr = compat32_runtime_addr + offset;
        win32_abi_register_compat32_direct(
            (const void *)crt_ftol, ftol_addr);
        serial_puts("[COMPAT32] native _ftol at 0x");
        serial_puthex(ftol_addr, 8);
        serial_puts("\n");
    }

    /* Initialize fast 32-bit x87 math functions (pow, fmod, acos, exp, log10).
     * These run natively in compat mode without INT 0x2E overhead. */
    extern void compat32_init_fast_math(uint8_t *, uint32_t);
    compat32_init_fast_math(thunk_pool + RUNTIME_MATH_PAGE * 4096U,
                            compat32_runtime_addr +
                            RUNTIME_MATH_PAGE * 4096U);
    extern uint32_t g_fast_CIpow_addr, g_fast_CIfmod_addr,
                    g_fast_CIacos_addr, g_fast_CIexp_addr,
                    g_fast_CIlog10_addr, g_fast_CIsqrt_addr,
                    g_fast_fabs_addr, g_fast_sqrt_addr;
    win32_abi_register_compat32_direct(
        (const void *)crt_CIpow, g_fast_CIpow_addr);
    win32_abi_register_compat32_direct(
        (const void *)crt_CIfmod, g_fast_CIfmod_addr);
    win32_abi_register_compat32_direct(
        (const void *)crt_CIacos, g_fast_CIacos_addr);
    win32_abi_register_compat32_direct(
        (const void *)crt_CIexp, g_fast_CIexp_addr);
    win32_abi_register_compat32_direct(
        (const void *)crt_CIlog10, g_fast_CIlog10_addr);
    win32_abi_register_compat32_direct(
        (const void *)crt_CIsqrt, g_fast_CIsqrt_addr);
    win32_abi_register_compat32_direct(
        (const void *)crt_fabs, g_fast_fabs_addr);
    win32_abi_register_compat32_direct(
        (const void *)crt_sqrt, g_fast_sqrt_addr);

    /* A normal 32->64 thunk returns a uint64 in EDX:EAX. Convert those bits
     * to the i386 CRT's ST(0) return ABI without teaching the INT2E gateway
     * about floating-point state. */
    {
        static const struct {
            const void *native_target;
            const void *bits_helper;
            const char *name;
            const char *helper_name;
        } bridges[] = {
            {(const void *)crt_strtod, (const void *)crt_strtod_compat32,
             "strtod", "__osito_strtod_compat32_bits"},
            {(const void *)crt_ceil, (const void *)crt_ceil_compat32,
             "ceil", "__osito_ceil_compat32_bits"},
            {(const void *)crt_floor, (const void *)crt_floor_compat32,
             "floor", "__osito_floor_compat32_bits"},
        };
        _Static_assert(RUNTIME_X87_WRAPPER_OFFSET +
                       sizeof(bridges) / sizeof(bridges[0]) *
                           RUNTIME_X87_WRAPPER_SIZE <= 4096U,
                       "x87 result wrappers must fit the runtime math page");
        for (unsigned i = 0; i < sizeof(bridges) / sizeof(bridges[0]); i++) {
            uint32_t helper_addr = compat32_make_thunk_runtime(
                (uint64_t)(ULONG_PTR)bridges[i].bits_helper,
                bridges[i].helper_name, 2, CC_CDECL);
            uint32_t wrapper_offset = RUNTIME_MATH_PAGE * 4096U +
                                      RUNTIME_X87_WRAPPER_OFFSET +
                                      i * RUNTIME_X87_WRAPPER_SIZE;
            uint32_t wrapper_addr = compat32_runtime_addr + wrapper_offset;
            if (helper_addr &&
                emit_x87_cdecl_result_wrapper(
                    thunk_pool + wrapper_offset, wrapper_addr,
                    helper_addr, 2) == 0) {
                win32_abi_register_compat32_direct(
                    bridges[i].native_target, wrapper_addr);
                serial_puts("[COMPAT32] ");
                serial_puts(bridges[i].name);
                serial_puts(" x87 wrapper at 0x");
                serial_puthex(wrapper_addr, 8);
                serial_puts(" helper=0x");
                serial_puthex(helper_addr, 8);
                serial_puts("\n");
            } else {
                serial_puts("[COMPAT32] Failed to install ");
                serial_puts(bridges[i].name);
                serial_puts(" x87 wrapper\n");
            }
        }
    }
#endif

    __atomic_store_n(&compat32_runtime_state, 2, __ATOMIC_RELEASE);
    if (compat32_map_runtime_current() != 0)
        serial_puts("[COMPAT32] Failed to map shared runtime\n");
}

BOOL compat32_is_initialized(void)
{
    return __atomic_load_n(&compat32_runtime_state, __ATOMIC_ACQUIRE) == 2;
}

uint32_t compat32_make_thunk(uint64_t target, const char *name, uint8_t num_args)
{
    /* Default to stdcall for backwards compatibility */
    return compat32_make_thunk_ex(target, name, num_args, CC_STDCALL);
}

/* The MS CRT _ftol helper receives its operand in x87 ST(0), not on the stack,
 * and returns the truncated int64 in EDX:EAX. Keep that contract entirely in
 * compat mode because the INT 0x2E gateway does not marshal x87 state. */
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

static int emit_x87_cdecl_result_wrapper(uint8_t *code,
                                         uint32_t wrapper_addr,
                                         uint32_t helper_addr,
                                         uint8_t num_args)
{
    uint32_t p = 0;
    if (!code || !wrapper_addr || !helper_addr || num_args > 24)
        return -1;

    code[p++] = 0x55;                 /* push ebp */
    code[p++] = 0x89; code[p++] = 0xE5; /* mov ebp,esp */
    for (int arg = (int)num_args - 1; arg >= 0; arg--) {
        uint32_t displacement = 8U + (uint32_t)arg * 4U;
        code[p++] = 0xFF;
        code[p++] = 0x75;             /* push dword [ebp+disp8] */
        code[p++] = (uint8_t)displacement;
    }

    code[p++] = 0xE8;                 /* call rel32 */
    int32_t relative = (int32_t)(helper_addr - (wrapper_addr + p + 4U));
    __builtin_memcpy(code + p, &relative, sizeof(relative));
    p += sizeof(relative);

    uint32_t argument_bytes = (uint32_t)num_args * 4U;
    if (argument_bytes) {
        code[p++] = 0x83; code[p++] = 0xC4;
        code[p++] = (uint8_t)argument_bytes; /* add esp,arg bytes */
    }
    code[p++] = 0x52;                 /* push edx (high bits) */
    code[p++] = 0x50;                 /* push eax (low bits) */
    code[p++] = 0xDD; code[p++] = 0x04; code[p++] = 0x24;
                                        /* fld qword [esp] */
    code[p++] = 0x83; code[p++] = 0xC4; code[p++] = 0x08;
                                        /* add esp,8 */
    code[p++] = 0xC9;                 /* leave */
    code[p++] = 0xC3;                 /* ret (cdecl) */

    if (p > RUNTIME_X87_WRAPPER_SIZE) return -1;
    while (p < RUNTIME_X87_WRAPPER_SIZE) code[p++] = 0xCC;
    return 0;
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

static int thunk_name_equal(const char *a, const char *b)
{
    if (a == b) return 1;
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int compat32_thunk_layout(const char *name, uint8_t num_args,
                                 uint8_t callconv, uint8_t *logical_args,
                                 uint8_t *ecx_arg, uint8_t *edx_arg)
{
    uint8_t convention = callconv & CC_CONVENTION_MASK;

    if ((callconv & CC_CONTEXT_CAPTURE) &&
        (convention != CC_STDCALL || num_args != 1U ||
         (callconv & CC_VARIADIC)))
        return 0;

    *logical_args = num_args;
    *ecx_arg = WIN32_ABI_ARG_UNUSED;
    *edx_arg = WIN32_ABI_ARG_UNUSED;

    switch (convention) {
    case CC_CDECL:
    case CC_STDCALL:
        return 1;
    case CC_THISCALL:
        if (num_args == 0xFFU)
            return 0;
        *logical_args = (uint8_t)(num_args + 1U);
        *ecx_arg = 0;
        return 1;
    case CC_FASTCALL: {
        WIN32_ABI_LAYOUT layout;
        if (!name || !msvc_demangle_abi_layout(name, &layout) ||
            (layout.callconv & CC_CONVENTION_MASK) != CC_FASTCALL ||
            layout.stack_argc != num_args)
            return 0;
        *logical_args = layout.logical_argc;
        *ecx_arg = layout.ecx_arg;
        *edx_arg = layout.edx_arg;
        return 1;
    }
    default:
        return 0;
    }
}

static uint32_t compat32_make_thunk_runtime(uint64_t target,
                                            const char *name,
                                            uint8_t num_args,
                                            uint8_t callconv)
{
    if (!(callconv & CC_CONTEXT_CAPTURE)) {
        uint32_t direct = win32_abi_compat32_direct(
            (const void *)(ULONG_PTR)target);
        if (direct) return direct;
    }

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

    uint8_t logical_args;
    uint8_t ecx_arg;
    uint8_t edx_arg;
    if (!compat32_thunk_layout(safe_name, num_args, callconv,
                               &logical_args, &ecx_arg, &edx_arg)) {
        serial_puts("[COMPAT32] Unsupported PE32 call contract for ");
        serial_puts(safe_name ? safe_name : "<unnamed>");
        serial_puts("\n");
        return 0;
    }

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

    emit_thunk(stub, target, num_args, callconv);
    if (!thunk_stub_matches_contract(stub, num_args, callconv)) {
        spin_unlock(&thunk_table_lock);
        serial_puts("[COMPAT32] Thunk stack contract mismatch for ");
        serial_puts(safe_name ? safe_name : "<unnamed>");
        serial_puts("\n");
        return 0;
    }

    /* Record in table */
    thunk_table[idx].thunk_addr  = stub_addr;
    thunk_table[idx].target_addr = target;
    thunk_table[idx].num_args    = num_args;
    thunk_table[idx].callconv    = callconv;
    thunk_table[idx].logical_args = logical_args;
    thunk_table[idx].ecx_arg     = ecx_arg;
    thunk_table[idx].edx_arg     = edx_arg;
    /* Import names live inside PE mappings and disappear on module unload. */
    thunk_table[idx].name        = owned_name;

    thunk_count++;
    spin_unlock(&thunk_table_lock);

    return stub_addr;
}

uint32_t compat32_make_thunk_ex(uint64_t target, const char *name,
                                 uint8_t num_args, uint8_t callconv)
{
    if (!compat32_is_initialized()) return 0;
    return compat32_make_thunk_runtime(target, name, num_args, callconv);
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

static void compat32_log_abi_miss(const char *dll_name,
                                  const char *func_name,
                                  USHORT ordinal, BOOL by_ordinal)
{
    serial_puts("[ABI-MISS] PE32 import ");
    serial_puts(dll_name ? dll_name : "<unknown-dll>");
    serial_puts("!");
    if (by_ordinal) {
        serial_puts("#");
        serial_putdec(ordinal);
    } else {
        serial_puts(func_name ? func_name : "<unnamed>");
    }
    serial_puts(" has no thunk contract\n");
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
    NTSTATUS patch_status = STATUS_SUCCESS;

    for (; desc->Name != 0; desc++) {
        const char *dll_name = (const char *)(base + desc->Name);
        int shim = is_shim_dll(dll_name);

        /* An implicitly imported DLL is present in the process loader lists
         * even when its implementation is a native OsitoK shim. Materialize
         * the process-local PE facade once per import descriptor so PEB/Ldr,
         * GetModuleHandle, and Toolhelp all observe the same module set. */
        if (shim && !dll_get_shim_module_handle(dll_name, FALSE)) {
            patch_status = STATUS_NO_MEMORY;
            goto patch_failed;
        }

        PIMAGE_THUNK_DATA32 int_entry = (PIMAGE_THUNK_DATA32)(
            base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk
                                             : desc->FirstThunk));
        PIMAGE_THUNK_DATA32 iat_entry =
            (PIMAGE_THUNK_DATA32)(base + desc->FirstThunk);

        for (; int_entry->u1.AddressOfData != 0; int_entry++, iat_entry++) {
            /* Get the function name for arg count lookup */
            const char *func_name = NULL;
            BOOL by_ordinal = IMAGE_SNAP_BY_ORDINAL32(int_entry->u1.Ordinal);
            USHORT ordinal = by_ordinal
                ? (USHORT)IMAGE_ORDINAL32(int_entry->u1.Ordinal) : 0;
            if (!by_ordinal) {
                PIMAGE_IMPORT_BY_NAME name_entry =
                    (PIMAGE_IMPORT_BY_NAME)(base + int_entry->u1.AddressOfData);
                func_name = name_entry->Name;
            }

            /* Resolve the import */
            PVOID resolved = NULL;
            if (func_name) {
                PIMAGE_IMPORT_BY_NAME n =
                    (PIMAGE_IMPORT_BY_NAME)(base + int_entry->u1.AddressOfData);
                USHORT hint = n->Hint;
                resolved = dll_resolve_import(dll_name, func_name, hint, FALSE);
            } else {
                resolved = dll_resolve_import(dll_name, NULL, ordinal, TRUE);
            }

            if (!resolved) {
                if (win32_abi_resolved_is_data(
                        dll_name, func_name, NULL)) {
                    serial_puts("[ABI-DATA-MISS] PE32 import ");
                    serial_puts(dll_name ? dll_name : "<unknown-dll>");
                    serial_puts("!");
                    serial_puts(func_name ? func_name : "<unnamed>");
                    serial_puts(" did not resolve process-local storage\n");
                    patch_status = STATUS_PROCEDURE_NOT_FOUND;
                    goto patch_failed;
                }
                /*
                 * Preserve the imported function's 32-bit stack contract even
                 * when its implementation is optional. A plain RET corrupts
                 * ESP for stdcall imports because their arguments remain.
                 */
                const char *thunk_name = func_name;
                uint8_t nargs, abi_cc;
                if (!win32_abi_lookup_resolved(
                        dll_name, func_name, NULL, &thunk_name,
                        &nargs, &abi_cc)) {
                    compat32_log_abi_miss(dll_name, func_name, ordinal,
                                          by_ordinal);
                    patch_status = STATUS_PROCEDURE_NOT_FOUND;
                    goto patch_failed;
                }
                uint32_t thunk_addr = compat32_make_thunk_ex(
                    (uint64_t)(ULONG_PTR)unresolved_import_zero,
                    thunk_name, nargs, abi_cc);
                if (!thunk_addr) {
                    patch_status = STATUS_NO_MEMORY;
                    goto patch_failed;
                }
                iat_entry->u1.Function = thunk_addr;
                continue;
            }

            /* Data imports are addresses, not call targets. Classify them
             * before deciding whether a symbol belongs to a shim or a real
             * PE32 module; process-local CRT storage intentionally lives in
             * ordinary low writable memory and has no executable module. */
            if (win32_abi_resolved_is_data(
                    dll_name, func_name, resolved)) {
                ULONG_PTR data_target = (ULONG_PTR)resolved;
                if (data_target > UINT32_MAX) {
                    serial_puts("[ABI-DATA-MISS] PE32 import ");
                    serial_puts(dll_name);
                    serial_puts("!");
                    if (func_name) {
                        serial_puts(func_name);
                    } else {
                        serial_puts("#");
                        serial_putdec(ordinal);
                    }
                    serial_puts(" is not addressable by PE32\n");
                    patch_status = STATUS_CONFLICTING_ADDRESSES;
                    goto patch_failed;
                }
                iat_entry->u1.Function = (uint32_t)data_target;
                direct++;
                serial_puts("[IAT-DATA] ");
                serial_puts(func_name ? func_name : "<ordinal>");
                serial_puts(" -> 0x");
                serial_puthex(data_target, 8);
                serial_puts("\n");
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
                uint32_t direct_target = win32_abi_compat32_direct(resolved);
                if (direct_target) {
                    iat_entry->u1.Function = direct_target;
                    direct++;
                    serial_puts("[IAT32-DIRECT] ");
                    serial_puts(func_name ? func_name : "<ordinal>");
                    serial_puts(" -> 0x");
                    serial_puthex(direct_target, 8);
                    serial_puts("\n");
                    continue;
                }

                /* Build the gateway only from explicit export metadata or a
                 * conservatively decoded MSVC signature. */
                uint64_t target64 = (uint64_t)(ULONG_PTR)resolved;
                const char *thunk_name = func_name;
                uint8_t nargs, abi_cc;
                if (!win32_abi_lookup_resolved(
                        dll_name, func_name, resolved, &thunk_name,
                        &nargs, &abi_cc)) {
                    compat32_log_abi_miss(dll_name, func_name, ordinal,
                                          by_ordinal);
                    patch_status = STATUS_PROCEDURE_NOT_FOUND;
                    goto patch_failed;
                }
                uint32_t thunk_addr = compat32_make_thunk_ex(
                    target64, thunk_name, nargs, abi_cc);
                if (!thunk_addr) {
                    patch_status = STATUS_NO_MEMORY;
                    goto patch_failed;
                }
                iat_entry->u1.Function = thunk_addr;
                patched++;
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
                    patch_status = STATUS_INVALID_IMAGE_FORMAT;
                    goto patch_failed;
                }
                *iat_ptr = (uint32_t)(ULONG_PTR)resolved;
                direct++;
            }
        }
    }

    serial_puts("[COMPAT32] IAT: ");
    serial_putdec(patched);
    serial_puts(" thunked (shim), ");
    serial_putdec(direct);
    serial_puts(" direct (PE32 DLL)\n");

    g_compat32_mode = saved_compat_mode;
    return STATUS_SUCCESS;

patch_failed:
    g_compat32_mode = saved_compat_mode;
    return patch_status;
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
    if (addr && (addr > UINT32_MAX ||
                 addr + sizeof(TEB32) - 1ULL > UINT32_MAX)) {
        serial_puts("[COMPAT32] refusing non-PE32 TEB base 0x");
        serial_puthex(addr, 16);
        serial_puts("\n");
        addr = 0;
        teb_addr = NULL;
    }
    uint64_t irq_flags;
    __asm__ volatile ("pushfq; popq %0; cli"
                      : "=r"(irq_flags) :: "memory");
    /* WRMSR changes the base, not the cached descriptor attributes. DOS
     * CPL3 returns can leave FS null, which is unusable in compatibility mode.
     * Loading the selector first also avoids overwriting the new TEB base. */
    __asm__ volatile ("mov %0, %%fs"
                      : : "r"((uint16_t)GDT_SEL_DATA32) : "memory");
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
    if (irq_flags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
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

int compat32_teb_selftest(void)
{
    int checks = 0;
    int failures = 0;
    PVOID allocation = NULL;
    SIZE_T allocation_size = TEB32_STORAGE_SIZE + PEB32_STORAGE_SIZE;
    NTSTATUS status = nt_vm_allocate_compat32(
        &allocation, &allocation_size, MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE);

#define TEB32_CHECK(condition, label) do {                              \
    checks++;                                                          \
    if (!(condition)) {                                                \
        failures++;                                                    \
        serial_puts("[TEB32TEST] FAIL: " label "\n");                 \
    }                                                                  \
} while (0)

    TEB32_CHECK(NT_SUCCESS(status) && allocation != NULL,
                "low page allocation");
    if (NT_SUCCESS(status) && allocation) {
        TEB32 *teb = (TEB32 *)allocation;
        PEB32 *peb = (PEB32 *)((BYTE *)allocation + TEB32_STORAGE_SIZE);
        memset(teb, 0, sizeof(*teb));
        memset(peb, 0, sizeof(*peb));
        teb->ExceptionList = UINT32_MAX;
        teb->Self = (uint32_t)(ULONG_PTR)teb;
        teb->ProcessEnvironmentBlock = (uint32_t)(ULONG_PTR)peb;
        teb->TlsSlots[0] = 0x2468ACE0U;
        peb->BeingDebugged = 1;

        TEB32_CHECK((uint64_t)(ULONG_PTR)teb <= UINT32_MAX,
                    "TEB address is representable");
        TEB32_CHECK(teb->Self == (uint32_t)(ULONG_PTR)teb,
                    "Self pointer round-trip");
        TEB32_CHECK((BYTE *)peb >= (BYTE *)teb + sizeof(*teb),
                    "PEB does not overlap TEB");
        TEB32_CHECK(*(uint32_t *)((BYTE *)teb + 0x0E10) == 0x2468ACE0U,
                    "TlsSlots canonical offset");
        TEB32_CHECK(*(BYTE *)((BYTE *)peb + 2) == 1,
                    "PEB BeingDebugged canonical offset");

#ifndef TEST_HARNESS
        extern uint64_t proc_get_fs_base(void);
        uint64_t saved_fs = proc_get_fs_base();
        uint32_t observed_head = 0;
        uint32_t observed_self = 0;
        uint32_t observed_peb = 0;
        uint32_t observed_tls = 0;
        uint32_t marker = 0x13579BDFU;

        compat32_setup_teb(teb);
        __asm__ volatile (
            "movl %%fs:0x00, %0\n"
            "movl %%fs:0x18, %1\n"
            "movl %%fs:0x30, %2\n"
            "movl %%fs:0xE10, %3\n"
            : "=r"(observed_head), "=r"(observed_self),
              "=r"(observed_peb), "=r"(observed_tls));
        TEB32_CHECK(observed_head == UINT32_MAX,
                    "FS ExceptionList read");
        TEB32_CHECK(observed_self == teb->Self, "FS Self read");
        TEB32_CHECK(observed_peb == (uint32_t)(ULONG_PTR)peb,
                    "FS PEB read");
        TEB32_CHECK(observed_tls == teb->TlsSlots[0],
                    "FS TlsSlots read");

        __asm__ volatile ("movl %0, %%fs:0x00"
                          : : "r"(marker) : "memory");
        TEB32_CHECK(teb->ExceptionList == marker,
                    "FS ExceptionList write");
        compat32_setup_teb((PVOID)(ULONG_PTR)saved_fs);
#endif

        (void)nt_vm_release_allocation_for_process(
            win32_current_process_id(), allocation);
    }

    serial_puts("[TEB32TEST] checks=");
    serial_putdec((uint64_t)checks);
    serial_puts(" failures=");
    serial_putdec((uint64_t)failures);
    serial_puts("\n");
#undef TEB32_CHECK
    return failures;
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
    callback_state->callback_int2e_depth[depth] =
        (uint32_t)callback_state->int2e_depth;

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

    /* Callback frames and interrupt stacks are task-owned. A callback may
     * block or run indefinitely, so it must not mask the CPU's timer. */
    uint64_t saved_flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(saved_flags) :: "memory");

    if (kern_setjmp(callback_state->jmpbufs[depth]) == 0) {
        /*
         * First return from setjmp — switch to compat mode.
         * Set up a small stack with the return stub as return address,
         * then LRETQ to the 32-bit function.
         */
        uint8_t *stack = callback_stack_get(callback_state, depth);
        if (!stack) {
            teb->ExceptionList = saved_seh;
            callback_state->current_stack_args =
                callback_state->saved_stack_args[depth];
            callback_state->callback_int2e_depth[depth] = 0;
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
            "cli\n"
            "movw $0x48, %%ax\n"    /* GDT_SEL_DATA32 */
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
            : "memory", "cc", "rax", "rbx", "r12", "r13", "r14", "r15"
        );
        __builtin_unreachable();
    }

    /* longjmp returned here — 32-bit function is done. */

    teb->ExceptionList = saved_seh;  /* Restore SEH chain */
    callback_state->current_stack_args =
        callback_state->saved_stack_args[depth];
    callback_state->callback_int2e_depth[depth] = 0;
    callback_state->depth--;
    if (depth >= 16) {
        serial_puts("[CB32] depth=");
        serial_putdec(depth);
        serial_puts(" returned\n");
    }
    /* The return gate cleared IF; preserve the caller's interrupt state. */
    __asm__ volatile ("pushq %0; popfq" :: "r"(saved_flags) : "memory", "cc");
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
                                             uint32_t unwind_frame)
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
    callback_state->callback_int2e_depth[depth] =
        (uint32_t)callback_state->int2e_depth;

    /* Save IST1 before callback — longjmp bypasses int2e_stub's restore */
    {
        extern uint64_t *tss_ist1_ptr;
        if (tss_ist1_ptr) callback_state->saved_ist1[depth] = *tss_ist1_ptr;
    }

    /* Save SEH ExceptionList — 32-bit code may push SEH frames on the
     * callback stack. Restore on return so the main chain stays valid. */
    TEB32 *teb = compat32_current_teb();
    uint32_t saved_seh = teb->ExceptionList;

    /* Keep the shared timer running across nested and blocking callbacks. */
    uint64_t saved_flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(saved_flags) :: "memory");

    if (kern_setjmp(callback_state->jmpbufs[depth]) == 0) {
        uint8_t *stack = stack_top ? NULL :
                         callback_stack_get(callback_state, depth);
        if (!stack_top && !stack) {
            teb->ExceptionList = saved_seh;
            callback_state->current_stack_args =
                callback_state->saved_stack_args[depth];
            callback_state->callback_int2e_depth[depth] = 0;
            callback_state->depth--;
            return 0;
        }
        uint32_t *sp = stack_top
                     ? (uint32_t *)(uintptr_t)stack_top
                     : (uint32_t *)(stack + CALLBACK_STACK_SIZE);

        if (unwind_frame) {
            if (!unwind_protector_stub_addr) {
                teb->ExceptionList = saved_seh;
                callback_state->current_stack_args =
                    callback_state->saved_stack_args[depth];
                callback_state->callback_int2e_depth[depth] = 0;
                callback_state->depth--;
                return 0;
            }
            sp -= 3;
            sp[0] = saved_seh;
            sp[1] = unwind_protector_stub_addr;
            sp[2] = unwind_frame;
            teb->ExceptionList = (uint32_t)(uintptr_t)sp;
        }

        /* Push arguments right-to-left (cdecl/stdcall convention) */
        for (int i = nargs - 1; i >= 0; i--) {
            sp--;
            *sp = args[i];
        }

        /* Push return stub as return address */
        sp--;
        *sp = callback_return_stub_addr;

        static uint32_t callback_arg_trace_count;
        if (callback_arg_trace_count < 12) {
            callback_arg_trace_count++;
            serial_puts("[CB32-CALL] fn=0x");
            serial_puthex(func_addr, 8);
            serial_puts(" sp=0x");
            serial_puthex((uint32_t)(uintptr_t)sp, 8);
            serial_puts(" argc=");
            serial_putdec((uint32_t)nargs);
            for (int i = 0; i < nargs; i++) {
                serial_puts(" arg");
                serial_putdec((uint32_t)i);
                serial_puts("=0x");
                serial_puthex(sp[i + 1], 8);
            }
            serial_puts("\n");
        }

        uint64_t cs64 = GDT_SEL_CODE32;
        uint64_t ip64 = func_addr;
        uint64_t sp64 = (uint64_t)(ULONG_PTR)sp;
        uint64_t bp64 = frame_ebp;

        __asm__ volatile (
            "cli\n"
            "movw $0x48, %%ax\n"    /* GDT_SEL_DATA32 */
            "mov %%ax, %%ds\n"
            "mov %%ax, %%es\n"
            "mov %%ax, %%ss\n"
            "test %[bp], %[bp]\n"
            "jz 1f\n"
            "mov %[bp], %%rbp\n"
            "1:\n"
            "mov %[sp], %%rsp\n"
            "push %[cs]\n"
            "push %[ip]\n"
            "sti\n"
            "lretq\n"
            :
            : [cs] "r"(cs64),
              [ip] "r"(ip64),
              [sp] "r"(sp64),
              [bp] "r"(bp64)
            : "memory", "cc", "rax"
        );
        __builtin_unreachable();
    }

    teb->ExceptionList = saved_seh;  /* Restore SEH chain */
    callback_state->current_stack_args =
        callback_state->saved_stack_args[depth];
    callback_state->callback_int2e_depth[depth] = 0;
    callback_state->depth--;
    __asm__ volatile ("pushq %0; popfq" :: "r"(saved_flags) : "memory", "cc");
    return callback_state->retvals[depth];
#else
    /* Test harness: call directly */
    (void)frame_ebp;
    (void)unwind_frame;
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
    return compat32_callback_args_impl(func_addr, nargs, args, 0, 0, 0);
}

static uint32_t compat32_callback_unwind_handler(
    uint32_t func_addr, const uint32_t *args, uint32_t unwind_frame)
{
    return compat32_callback_args_impl(func_addr, 4, args, 0, 0,
                                       unwind_frame);
}

uint32_t compat32_callback_args_with_ebp(uint32_t func_addr, int nargs,
                                         const uint32_t *args,
                                         uint32_t frame_ebp)
{
    return compat32_callback_args_impl(func_addr, nargs, args, 0, frame_ebp,
                                       0);
}

uint32_t compat32_callback_args_on_stack(uint32_t func_addr, int nargs,
                                         const uint32_t *args,
                                         uint32_t stack_top)
{
    return compat32_callback_args_impl(func_addr, nargs, args,
                                       stack_top & ~0xFULL, 0, 0);
}

uint32_t compat32_thread_entry_on_stack(uint32_t func_addr, int nargs,
                                         const uint32_t *args,
                                         uint32_t stack_top)
{
    /* Thread entries and nested callbacks share the same preemptible gate. */
    return compat32_callback_args_impl(func_addr, nargs, args,
                                       stack_top & ~0xFULL, 0, 0);
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

#define WIN32_USER_MIN_ADDRESS  0x0000000000010000ULL
#define WIN32_USER32_LIMIT      0x0000000080000000ULL
#define WIN32_USER64_LIMIT      0x0000800000000000ULL
#define WIN32_PTE_PRESENT       (1ULL << 0)
#define WIN32_PTE_WRITABLE      (1ULL << 1)
#define WIN32_PTE_COW           (1ULL << 9)

static int compat32_runtime_range_contains(uint64_t base, SIZE_T size)
{
    if (__atomic_load_n(&compat32_runtime_state, __ATOMIC_ACQUIRE) != 2 ||
        !size)
        return 0;

    uint64_t end = base + size;
    uint64_t runtime_base = compat32_runtime_addr;
    uint64_t runtime_end = runtime_base + COMPAT32_RUNTIME_BYTES;
    return end >= base && runtime_end >= runtime_base &&
           base >= runtime_base && end <= runtime_end;
}

static int win32_user_range_accessible(const void *pointer, SIZE_T size,
                                       BOOL compat32, int writable)
{
    if (!pointer || !size)
        return 0;

    uint64_t first = (uint64_t)(ULONG_PTR)pointer;
    uint64_t limit = compat32 ? WIN32_USER32_LIMIT : WIN32_USER64_LIMIT;
    if (size > UINT64_MAX - first)
        return 0;

    int pe_owned = pe_va_range_contains(first, size);
    if ((compat32 || !pe_owned) &&
        (first < WIN32_USER_MIN_ADDRESS || first >= limit ||
         size > limit - first))
        return 0;

#ifndef TEST_HARNESS
    extern uint64_t proc_current_cr3(void);
    uint64_t cr3 = proc_current_cr3();
    if (!cr3)
        return 0;

    int owned = pe_owned ||
                nt_vm_user_range_accessible(first, size,
                                            writable ? TRUE : FALSE);
    if (!owned && !writable && compat32)
        owned = compat32_runtime_range_contains(first, size);
    if (!owned)
        return 0;

    uint64_t last = first + size - 1U;
    uint64_t page = first & ~0xFFFULL;
    uint64_t last_page = last & ~0xFFFULL;
    for (;;) {
        uint64_t flags = 0;
        if (paging_query_mapping_in_cr3(cr3, page, &flags, NULL) != 0 ||
            !(flags & WIN32_PTE_PRESENT))
            return 0;
        if (writable) {
            if (!(flags & (WIN32_PTE_WRITABLE | WIN32_PTE_COW)))
                return 0;
        }
        if (page == last_page)
            break;
        page += 0x1000ULL;
    }
#else
    (void)writable;
#endif
    return 1;
}

int win32_user_range_readable(const void *pointer, SIZE_T size,
                              BOOL compat32)
{
    return win32_user_range_accessible(pointer, size, compat32, 0);
}

int win32_user_range_writable(void *pointer, SIZE_T size, BOOL compat32)
{
    return win32_user_range_accessible(pointer, size, compat32, 1);
}

int win32_user_range_executable(const void *pointer, SIZE_T size,
                                BOOL compat32)
{
    if (!win32_user_range_accessible(pointer, size, compat32, 0))
        return 0;

#ifndef TEST_HARNESS
    extern uint64_t proc_current_cr3(void);
    uint64_t cr3 = proc_current_cr3();
    if (!cr3)
        return 0;

    uint64_t first = (uint64_t)(ULONG_PTR)pointer;
    uint64_t last = first + size - 1U;
    uint64_t page = first & ~0xFFFULL;
    uint64_t last_page = last & ~0xFFFULL;
    for (;;) {
        uint64_t flags = 0;
        if (paging_query_mapping_in_cr3(cr3, page, &flags, NULL) != 0 ||
            !(flags & WIN32_PTE_PRESENT) || (flags & (1ULL << 63)))
            return 0;
        if (page == last_page)
            break;
        page += 0x1000ULL;
    }
#endif
    return 1;
}

static int seh32_range_readable(uint32_t address, uint32_t size)
{
    return win32_user_range_readable(
        (const void *)(ULONG_PTR)address, size, TRUE);
}

int compat32_range_readable(uint32_t address, uint32_t size)
{
    return seh32_range_readable(address, size);
}

static int seh32_range_executable(uint32_t address, uint32_t size)
{
    return win32_user_range_executable(
        (const void *)(ULONG_PTR)address, size, TRUE);
}

int compat32_range_executable(uint32_t address, uint32_t size)
{
    return seh32_range_executable(address, size);
}

static int seh32_stack_region(const TEB32 *teb,
                              const callback_owner_state_t *state,
                              uint32_t address, uint32_t size)
{
    if (!teb || !size || !seh32_range_readable(address, size))
        return 0;

    uint32_t end = address + size;
    if (end < address)
        return 0;

    if (teb->StackLimit < teb->StackBase &&
        address >= teb->StackLimit && end <= teb->StackBase)
        return 1;

    if (!state)
        return 0;
    int depth = state->depth;
    if (depth > MAX_CALLBACK_DEPTH)
        depth = MAX_CALLBACK_DEPTH;
    for (int i = 0; i < depth; i++) {
        uint32_t low = (uint32_t)(uintptr_t)state->stacks[i];
        if (!low || low > UINT32_MAX - CALLBACK_STACK_SIZE)
            continue;
        uint32_t high = low + CALLBACK_STACK_SIZE;
        if (address >= low && end <= high)
            return i + 2;
    }
    return 0;
}

/* A PE32 nonlocal transfer can leave one or more kernel-to-compat callbacks
 * without a RET to callback_return_stub. Collapse those suspended entries and
 * make the current INT2E restore the IST cursor that preceded the first
 * abandoned callback. Callback stacks at or below the destination stay live. */
static int compat32_abandon_callbacks(callback_owner_state_t *state,
                                      compat32_int2e_frame_t *frame,
                                      uint32_t target_esp)
{
    if (!state || !frame || state->depth <= 0 || state->int2e_depth <= 0)
        return 0;

    TEB32 *teb = compat32_current_teb();
    int target_region = seh32_stack_region(
        teb, state, target_esp, sizeof(uint32_t));
    if (!target_region)
        return 0;

    int keep_callbacks = target_region == 1 ? 0 : target_region - 1;
    if (keep_callbacks < 0 || keep_callbacks >= state->depth)
        return 0;

    int old_callback_depth = state->depth;
    int old_int2e_depth = state->int2e_depth;
    uint32_t entry_depth = state->callback_int2e_depth[keep_callbacks];
    int keep_int2e = entry_depth ? (int)entry_depth - 1 : 0;
    if (keep_int2e < 0 || keep_int2e >= old_int2e_depth ||
        state->int2e_contexts[old_int2e_depth - 1].frame_address !=
            (uint64_t)(ULONG_PTR)frame) {
        serial_puts("[COMPAT32] callback abandonment state mismatch\n");
        return 0;
    }

    if (keep_int2e < old_int2e_depth - 1) {
        compat32_int2e_context_t *first =
            &state->int2e_contexts[keep_int2e];
        if (!first->frame_address) {
            serial_puts("[COMPAT32] missing abandoned INT2E frame\n");
            return 0;
        }
        compat32_int2e_frame_t *first_frame =
            (compat32_int2e_frame_t *)(ULONG_PTR)first->frame_address;
        frame->saved_ist1 = first_frame->saved_ist1;
    }

    for (int i = old_callback_depth; i > keep_callbacks; i--) {
        int depth = i - 1;
        memset(state->jmpbufs[depth], 0, sizeof(state->jmpbufs[depth]));
        state->saved_ist1[depth] = 0;
        state->saved_stack_args[depth] = 0;
        state->callback_int2e_depth[depth] = 0;
        state->retvals[depth] = 0;
    }

    state->depth = keep_callbacks;
    state->current_stack_args = 0;
    state->in_catch_dispatch = 0;
    state->saved_next_frame = 0;
    if (state->seh_dispatch_depth > keep_callbacks) {
        for (int i = keep_callbacks; i < state->seh_dispatch_depth; i++)
            memset(&state->seh32_slots[i], 0, sizeof(state->seh32_slots[i]));
        state->seh_dispatch_depth = keep_callbacks;
    }

    for (int i = keep_int2e; i < old_int2e_depth; i++)
        memset(&state->int2e_contexts[i], 0,
               sizeof(state->int2e_contexts[i]));
    state->int2e_depth = keep_int2e;

    serial_puts("[COMPAT32] abandoned callbacks=");
    serial_putdec((uint32_t)(old_callback_depth - keep_callbacks));
    serial_puts(" int2e=");
    serial_putdec((uint32_t)(old_int2e_depth - keep_int2e));
    serial_puts(" target_esp=0x");
    serial_puthex(target_esp, 8);
    serial_puts("\n");
    return 1;
}

static int seh32_frame_valid(const TEB32 *teb,
                             const callback_owner_state_t *state,
                             uint32_t address, uint32_t size)
{
    return (address & (sizeof(uint32_t) - 1U)) == 0 &&
           seh32_stack_region(teb, state, address, size) != 0;
}

static int seh32_chain_end(uint32_t address)
{
    return address == 0 || address == UINT32_MAX;
}

static NTSTATUS seh32_read_registration(
    const TEB32 *teb, const callback_owner_state_t *state,
    uint32_t frame_address, uint32_t *next_address,
    uint32_t *handler_address, int *frame_region)
{
    int region = seh32_stack_region(
        teb, state, frame_address, 2U * sizeof(uint32_t));
    if (!region || (frame_address & (sizeof(uint32_t) - 1U)))
        return STATUS_BAD_STACK;

    const uint32_t *frame = (const uint32_t *)(ULONG_PTR)frame_address;
    uint32_t next = frame[0];
    uint32_t handler = frame[1];
    if (!seh32_range_executable(handler, 1))
        return STATUS_BAD_STACK;

    if (!seh32_chain_end(next)) {
        int next_region = seh32_stack_region(
            teb, state, next, 2U * sizeof(uint32_t));
        if (!next_region || (next & (sizeof(uint32_t) - 1U)) ||
            (next_region == region && next <= frame_address) ||
            (next_region != region && next_region >= region))
            return STATUS_BAD_STACK;
    }

    if (next_address) *next_address = next;
    if (handler_address) *handler_address = handler;
    if (frame_region) *frame_region = region;
    return STATUS_SUCCESS;
}

static NTSTATUS seh32_prune_chain_for_restore(
    TEB32 *teb, const callback_owner_state_t *state, uint32_t target_esp)
{
    int target_region = seh32_stack_region(
        teb, state, target_esp, sizeof(uint32_t));
    if (!target_region || (target_esp & (sizeof(uint32_t) - 1U)))
        return STATUS_BAD_STACK;

    uint32_t frame_address = teb->ExceptionList;
    while (!seh32_chain_end(frame_address)) {
        uint32_t next_address = 0;
        int frame_region = 0;
        NTSTATUS status = seh32_read_registration(
            teb, state, frame_address, &next_address, NULL, &frame_region);
        if (!NT_SUCCESS(status))
            return status;

        if (frame_region < target_region ||
            (frame_region == target_region && frame_address >= target_esp))
            break;
        frame_address = next_address;
    }

    teb->ExceptionList = frame_address;
    return STATUS_SUCCESS;
}

static NTSTATUS compat32_rtl_capture_context(
    uint32_t context_address, const uint32_t *stack_args)
{
    callback_owner_state_t *state = callback_state_get(0, NULL);
    compat32_int2e_context_t *entry = int2e_context_current(state);
    if (!entry || !stack_args ||
        (ULONG_PTR)stack_args < sizeof(uint32_t) ||
        !win32_user_range_readable(stack_args - 1, 2U * sizeof(uint32_t),
                                   TRUE))
        return STATUS_BAD_STACK;
    if (!win32_user_range_writable(
            (void *)(ULONG_PTR)context_address, sizeof(CONTEXT32), TRUE))
        return STATUS_ACCESS_VIOLATION;

    const compat32_int2e_frame_t *frame =
        (const compat32_int2e_frame_t *)(ULONG_PTR)entry->frame_address;
    CONTEXT32 context;
    memset(&context, 0, sizeof(context));
    context.ContextFlags = CONTEXT32_FULL;

    uint16_t seg_ds, seg_es, seg_fs, seg_gs;
    __asm__ volatile ("movw %%ds, %0" : "=r"(seg_ds));
    __asm__ volatile ("movw %%es, %0" : "=r"(seg_es));
    __asm__ volatile ("movw %%fs, %0" : "=r"(seg_fs));
    __asm__ volatile ("movw %%gs, %0" : "=r"(seg_gs));
    context.SegDs = seg_ds;
    context.SegEs = seg_es;
    context.SegFs = seg_fs;
    context.SegGs = seg_gs;

    context.Edi = (uint32_t)entry->rdi;
    context.Esi = (uint32_t)entry->rsi;
    context.Ebx = (uint32_t)entry->rbx;
    context.Edx = (uint32_t)entry->rdx;
    context.Ecx = (uint32_t)entry->rcx;
    context.Eax = (uint32_t)entry->rax;
    context.Ebp = (uint32_t)entry->rbp;
    context.Eip = stack_args[-1];
    context.SegCs = (uint32_t)frame->cs;
    context.EFlags = (uint32_t)frame->rflags;
    context.Esp = (uint32_t)(ULONG_PTR)(stack_args + 1);
    context.SegSs = (uint32_t)frame->ss;

    memcpy((void *)(ULONG_PTR)context_address, &context, sizeof(context));
    return STATUS_SUCCESS;
}

NTSTATUS compat32_rtl_restore_context(uint32_t context_address,
                                      uint32_t exception_record_address)
{
    callback_owner_state_t *state = callback_state_get(0, NULL);
    compat32_int2e_context_t *entry = int2e_context_current(state);
    TEB32 *teb = compat32_current_teb();
    if (!state || !entry || !teb || state->unwind.eip)
        return STATUS_BAD_STACK;

    if (!win32_user_range_readable(
            (const void *)(ULONG_PTR)context_address,
            sizeof(CONTEXT32), TRUE))
        return STATUS_INVALID_PARAMETER;

    CONTEXT32 context;
    memcpy(&context, (const void *)(ULONG_PTR)context_address,
           sizeof(context));
    if ((context.ContextFlags & CONTEXT32_ARCH_MASK) != CONTEXT32_I386 ||
        !(context.ContextFlags & CONTEXT32_CONTROL_BIT))
        return STATUS_INVALID_PARAMETER;

    BOOL restore_longjump_nonvolatile = FALSE;
    if (exception_record_address) {
        if (!win32_user_range_readable(
                (const void *)(ULONG_PTR)exception_record_address,
                sizeof(EXCEPTION_RECORD32), TRUE))
            return STATUS_INVALID_PARAMETER;

        EXCEPTION_RECORD32 record;
        memcpy(&record,
               (const void *)(ULONG_PTR)exception_record_address,
               sizeof(record));
        if (record.NumberParameters > 15U)
            return STATUS_INVALID_PARAMETER;

        if (record.ExceptionCode == (uint32_t)STATUS_LONGJUMP &&
            record.NumberParameters >= 1U) {
            uint32_t jump_address = record.ExceptionInformation[0];
            if (!win32_user_range_readable(
                    (const void *)(ULONG_PTR)jump_address,
                    sizeof(WIN32_JUMP_BUFFER32), TRUE))
                return STATUS_INVALID_PARAMETER;

            WIN32_JUMP_BUFFER32 jump;
            memcpy(&jump, (const void *)(ULONG_PTR)jump_address,
                   sizeof(jump));
            context.Ebp = jump.Ebp;
            context.Ebx = jump.Ebx;
            context.Edi = jump.Edi;
            context.Esi = jump.Esi;
            context.Esp = jump.Esp;
            context.Eip = jump.Eip;
            restore_longjump_nonvolatile = TRUE;
        }
    }

    if (!context.Eip || !seh32_range_executable(context.Eip, 1))
        return STATUS_INVALID_PARAMETER;

    NTSTATUS status = seh32_prune_chain_for_restore(
        teb, state, context.Esp);
    if (!NT_SUCCESS(status))
        return status;

    const compat32_int2e_frame_t *frame =
        (const compat32_int2e_frame_t *)(ULONG_PTR)entry->frame_address;
    const uint32_t mutable_eflags = 0x00250DD5U;
    uint32_t live_eflags = (uint32_t)frame->rflags;
    BOOL restore_integer =
        (context.ContextFlags & CONTEXT32_INTEGER_BIT) != 0;

    state->unwind.eip = context.Eip;
    state->unwind.esp = context.Esp;
    state->unwind.ebp = context.Ebp;
    state->unwind.ebx = context.Ebx;
    state->unwind.esi = context.Esi;
    state->unwind.edi = context.Edi;
    state->unwind.eax = context.Eax;
    state->unwind.ecx = context.Ecx;
    state->unwind.edx = context.Edx;
    state->unwind.eflags = (live_eflags & ~mutable_eflags) |
                           (context.EFlags & mutable_eflags);
    state->unwind.restore_nonvolatile =
        restore_integer || restore_longjump_nonvolatile;
    state->unwind.restore_volatile = restore_integer;
    state->unwind.restore_eflags = 1;
    return STATUS_SUCCESS;
}

NTSTATUS compat32_rtl_unwind(uint32_t target_frame, uint32_t target_ip,
                             uint32_t exception_record_address,
                             uint32_t return_value,
                             NTSTATUS *exit_status)
{
    callback_owner_state_t *state = callback_state_get(0, NULL);
    compat32_int2e_context_t *entry = int2e_context_current(state);
    TEB32 *teb = compat32_current_teb();
    uint32_t stack_args = state ? state->current_stack_args : 0;
    NTSTATUS status = STATUS_SUCCESS;

    if (exit_status) *exit_status = STATUS_SUCCESS;
    if (!state || !entry || !teb || state->unwind.eip ||
        stack_args < sizeof(uint32_t) ||
        stack_args > UINT32_MAX - 4U * sizeof(uint32_t) ||
        !seh32_stack_region(teb, state, stack_args - sizeof(uint32_t),
                            5U * sizeof(uint32_t)))
        return STATUS_BAD_STACK;

    uint32_t caller_eip = *(const uint32_t *)(ULONG_PTR)(
        stack_args - sizeof(uint32_t));
    uint32_t continuation_esp = stack_args + 4U * sizeof(uint32_t);
    uint32_t continuation_eip = target_ip ? target_ip : caller_eip;
    BOOL exit_unwind = target_frame == 0;
    BOOL target_is_chain_end = target_frame == UINT32_MAX;
    int target_region = 0;

    if (!exit_unwind && !seh32_range_executable(continuation_eip, 1))
        return STATUS_INVALID_PARAMETER;
    if (!exit_unwind && !target_is_chain_end) {
        target_region = seh32_stack_region(
            teb, state, target_frame, 2U * sizeof(uint32_t));
        if (!target_region ||
            (target_frame & (sizeof(uint32_t) - 1U)))
            return STATUS_INVALID_UNWIND_TARGET;
    }
    if (state->seh_dispatch_depth >= SEH32_MAX_DISPATCH_DEPTH)
        return STATUS_NO_MEMORY;

    state->seh_dispatch_depth++;
    seh32_dispatch_slot_t *slot = seh32_current_slot(state);
    if (!slot) {
        status = STATUS_NO_MEMORY;
        goto finished;
    }
    memset(slot, 0, sizeof(*slot));

    CONTEXT32 *context = &slot->context;
    context->ContextFlags = CONTEXT32_FULL;
    context->SegGs = GDT_SEL_DATA32;
    context->SegFs = GDT_SEL_DATA32;
    context->SegEs = GDT_SEL_DATA32;
    context->SegDs = GDT_SEL_DATA32;
    context->Edi = (uint32_t)entry->rdi;
    context->Esi = (uint32_t)entry->rsi;
    context->Ebx = (uint32_t)entry->rbx;
    context->Edx = (uint32_t)entry->rdx;
    context->Ecx = (uint32_t)entry->rcx;
    context->Eax = return_value;
    context->Ebp = (uint32_t)entry->rbp;
    context->Eip = caller_eip;
    context->SegCs = GDT_SEL_CODE32;
    context->EFlags = (uint32_t)(
        (const compat32_int2e_frame_t *)(ULONG_PTR)entry->frame_address)->rflags;
    context->Esp = continuation_esp;
    context->SegSs = GDT_SEL_DATA32;

    EXCEPTION_RECORD32 *record;
    if (exception_record_address) {
        if (!win32_user_range_writable(
                (void *)(ULONG_PTR)exception_record_address,
                sizeof(*record), TRUE)) {
            status = STATUS_INVALID_PARAMETER;
            goto finished;
        }
        record = (EXCEPTION_RECORD32 *)(ULONG_PTR)exception_record_address;
    } else {
        record = &slot->exception_record;
        record->ExceptionCode = (uint32_t)STATUS_UNWIND;
        record->ExceptionAddress = caller_eip;
        exception_record_address = (uint32_t)(ULONG_PTR)record;
    }
    record->ExceptionFlags |= EXCEPTION_UNWINDING;
    if (exit_unwind)
        record->ExceptionFlags |= EXCEPTION_EXIT_UNWIND;

    slot->exception_pointers.ExceptionRecord = exception_record_address;
    slot->exception_pointers.ContextRecord =
        (uint32_t)(ULONG_PTR)context;

    serial_puts("[SEH32] RtlUnwind frame=0x");
    serial_puthex(target_frame, 8);
    serial_puts(" target=0x");
    serial_puthex(target_ip, 8);
    serial_puts(" chain=0x");
    serial_puthex(teb->ExceptionList, 8);
    serial_puts("\n");

    uint32_t frame_address = teb->ExceptionList;
    while (frame_address != target_frame &&
           !seh32_chain_end(frame_address)) {
        uint32_t next_address = 0;
        uint32_t handler_address = 0;
        int frame_region = 0;

        status = seh32_read_registration(
            teb, state, frame_address, &next_address,
            &handler_address, &frame_region);
        if (!NT_SUCCESS(status))
            goto finished;
        if (!exit_unwind && !target_is_chain_end &&
            frame_region == target_region && frame_address > target_frame) {
            status = STATUS_INVALID_UNWIND_TARGET;
            goto finished;
        }

        slot->dispatcher_frame = 0;
        uint32_t arguments[4] = {
            exception_record_address,
            frame_address,
            (uint32_t)(ULONG_PTR)context,
            (uint32_t)(ULONG_PTR)&slot->dispatcher_frame,
        };
        slot->active_frame = frame_address;
        uint32_t disposition = compat32_callback_unwind_handler(
            handler_address, arguments, frame_address);
        slot->active_frame = 0;

        if (disposition == ExceptionCollidedUnwind) {
            uint32_t collided_frame = slot->dispatcher_frame;
            if (seh32_chain_end(collided_frame)) {
                status = STATUS_INVALID_DISPOSITION;
                goto finished;
            }
            status = seh32_read_registration(
                teb, state, collided_frame, &next_address, NULL, NULL);
            if (!NT_SUCCESS(status))
                goto finished;

            /* A nested RtlUnwind runs on a separate low callback stack in
             * OsitoK. Once its protector points back to an outer active
             * handler, inherit that handler's original stack context before
             * the callback activation is abandoned. */
            for (int i = state->seh_dispatch_depth - 2; i >= 0; i--) {
                seh32_dispatch_slot_t *outer = &state->seh32_slots[i];
                if (outer->active_frame == collided_frame) {
                    *context = outer->context;
                    context->Eax = return_value;
                    break;
                }
            }
            frame_address = collided_frame;
        } else if (disposition != ExceptionContinueSearch) {
            status = STATUS_INVALID_DISPOSITION;
            goto finished;
        }

        teb->ExceptionList = next_address;
        frame_address = next_address;
    }

    if (!exit_unwind && frame_address != target_frame) {
        status = STATUS_INVALID_UNWIND_TARGET;
        goto finished;
    }

    if (exit_unwind) {
        if (exit_status) {
            *exit_status = (NTSTATUS)record->ExceptionCode;
            if (*exit_status == STATUS_UNWIND)
                *exit_status = STATUS_SUCCESS;
        }
        goto finished;
    }

    context->Eip = continuation_eip;
    if (!seh32_range_executable(context->Eip, 1) ||
        !seh32_stack_region(teb, state, context->Esp,
                            sizeof(uint32_t))) {
        status = STATUS_BAD_STACK;
        goto finished;
    }

    teb->ExceptionList = target_frame;
    state->unwind.eip = context->Eip;
    state->unwind.esp = context->Esp;
    state->unwind.ebp = context->Ebp;
    state->unwind.ebx = context->Ebx;
    state->unwind.esi = context->Esi;
    state->unwind.edi = context->Edi;
    state->unwind.eax = return_value;
    state->unwind.ecx = context->Ecx;
    state->unwind.edx = context->Edx;
    state->unwind.eflags = context->EFlags;
    state->unwind.restore_nonvolatile = 1;
    state->unwind.restore_volatile = 1;
    state->unwind.restore_eflags = 1;

finished:
    state->seh_dispatch_depth--;
    return status;
}

typedef struct __attribute__((packed)) {
    int32_t enclosing_level;
    uint32_t filter;
    uint32_t handler;
} compat32_eh3_scope_entry_t;

static int compat32_eh3_read_entry(uint32_t scope_table, int32_t level,
                                   compat32_eh3_scope_entry_t *entry)
{
    if (!entry || !scope_table || level < 0 || level > 4095)
        return 0;

    uint64_t address = (uint64_t)scope_table +
                       (uint64_t)(uint32_t)level * sizeof(*entry);
    if (address > UINT32_MAX ||
        !seh32_range_readable((uint32_t)address, sizeof(*entry)))
        return 0;
    *entry = *(const compat32_eh3_scope_entry_t *)(
        ULONG_PTR)(uint32_t)address;
    return 1;
}

int compat32_eh3_local_unwind(uint32_t frame_address,
                              uint32_t scope_table,
                              int32_t stop_level)
{
    callback_owner_state_t *state = callback_state_get(0, NULL);
    TEB32 *teb = compat32_current_teb();
    if (!state || !teb || stop_level < -1 || stop_level > 4095 ||
        frame_address > UINT32_MAX - 16U ||
        !seh32_frame_valid(teb, state, frame_address,
                           4U * sizeof(uint32_t)))
        return 0;

    uint32_t *frame = (uint32_t *)(ULONG_PTR)frame_address;
    int32_t level = (int32_t)frame[3];
    uint32_t frame_ebp = frame_address + 16U;

    for (uint32_t guard = 0; level != stop_level; guard++) {
        compat32_eh3_scope_entry_t entry;
        if (guard >= 4096U ||
            !compat32_eh3_read_entry(scope_table, level, &entry) ||
            (entry.enclosing_level != -1 &&
             (entry.enclosing_level < 0 ||
              entry.enclosing_level >= level)))
            return 0;

        /* The runtime publishes the enclosing level before invoking a
         * termination funclet so a nested exception sees accurate state. */
        frame[3] = (uint32_t)entry.enclosing_level;
        if (!entry.filter && entry.handler) {
            if (!seh32_range_executable(entry.handler, 1))
                return 0;
            (void)compat32_callback_args_with_ebp(entry.handler, 0, NULL,
                                                   frame_ebp);
        }
        level = entry.enclosing_level;
    }
    return 1;
}

static int compat32_eh_schedule_handler(uint32_t frame_address,
                                        uint32_t handler_address,
                                        const char *kind)
{
    callback_owner_state_t *state = callback_state_get(0, NULL);
    TEB32 *teb = compat32_current_teb();
    if (!state || !teb || state->unwind.eip ||
        !seh32_range_executable(handler_address, 1) ||
        frame_address < 2U * sizeof(uint32_t) ||
        frame_address > UINT32_MAX - 16U ||
        !seh32_frame_valid(teb, state, frame_address,
                           4U * sizeof(uint32_t)) ||
        !seh32_stack_region(teb, state,
                            frame_address - 2U * sizeof(uint32_t),
                            sizeof(uint32_t)))
        return 0;

    uint32_t frame_ebp = frame_address + 16U;
    uint32_t saved_esp = *(const uint32_t *)(ULONG_PTR)(
        frame_address - 2U * sizeof(uint32_t));
    if (!saved_esp || saved_esp >= frame_ebp ||
        frame_ebp - saved_esp > 0x100000U ||
        !seh32_stack_region(teb, state, saved_esp, sizeof(uint32_t)))
        return 0;

    teb->ExceptionList = frame_address;
    state->unwind.eip = handler_address;
    state->unwind.esp = saved_esp;
    state->unwind.ebp = frame_ebp;
    state->unwind.restore_nonvolatile = 0;

    serial_puts("[SEH32] ");
    serial_puts(kind);
    serial_puts(" transfer handler=0x");
    serial_puthex(handler_address, 8);
    serial_puts(" EBP=0x");
    serial_puthex(frame_ebp, 8);
    serial_puts(" ESP=0x");
    serial_puthex(saved_esp, 8);
    serial_puts("\n");
    return 1;
}

int compat32_eh3_schedule_handler(uint32_t frame_address,
                                  uint32_t handler_address)
{
    return compat32_eh_schedule_handler(frame_address, handler_address,
                                        "EH3");
}

int compat32_eh4_schedule_handler(uint32_t frame_address,
                                  uint32_t handler_address)
{
    return compat32_eh_schedule_handler(frame_address, handler_address,
                                        "EH4");
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

    callback_owner_state_t *state = callback_state_get(0, NULL);
    compat32_int2e_context_t *entry = int2e_context_current(state);
    if (!state || !entry) {
        serial_puts("[MSVCRT-JMP] missing per-thread INT2E context\n");
        *result = 0;
        return 1;
    }

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

        env[0] = (uint32_t)entry->rbp;
        env[1] = (uint32_t)entry->rbx;
        env[2] = (uint32_t)entry->rdi;
        env[3] = (uint32_t)entry->rsi;
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
    if (!seh32_range_executable(return_eip, 1) ||
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
    if (unwind_function && seh32_range_executable(unwind_function, 1)) {
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

    state->unwind.ebp = env[0];
    state->unwind.ebx = env[1];
    state->unwind.edi = env[2];
    state->unwind.esi = env[3];
    state->unwind.esp = saved_esp + sizeof(uint32_t);
    state->unwind.restore_nonvolatile = 1;
    state->unwind.eip = return_eip;

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
                !seh32_range_executable(entry->action, 1))
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

    /* Diagnostic: read FS base from MSR to verify the active TEB mapping. */
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

    /* Build the ABI objects before either VEH or frame-based SEH runs. */
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

    /* Vectored handlers are first-chance handlers and run before the
     * thread's frame-based SEH chain. */
    {
        PVOID handlers[128];
        SIZE_T handler_count =
            kernel32_snapshot_vectored_exception_handlers(handlers, 128);
        uint32_t argument = (uint32_t)(ULONG_PTR)pointers32;

        for (SIZE_T i = 0; i < handler_count; i++) {
            ULONG_PTR handler = (ULONG_PTR)handlers[i];
            if (handler > UINT32_MAX ||
                !seh32_range_executable((uint32_t)handler, 1)) {
                static uint32_t invalid_handler_logs;
                if (__atomic_fetch_add(&invalid_handler_logs, 1,
                                       __ATOMIC_RELAXED) < 8) {
                    serial_puts("[VEH32] rejected non-executable handler 0x");
                    serial_puthex(handler, 8);
                    serial_puts("\n");
                }
                continue;
            }

            LONG result = (LONG)compat32_callback_args(
                (uint32_t)handler, 1, &argument);
            if (result == EXCEPTION_CONTINUE_EXECUTION &&
                !(record32->ExceptionFlags & EXCEPTION_NONCONTINUABLE))
                return 1;
        }
    }

    if (frame_addr == 0 || frame_addr == 0xFFFFFFFF) {
        serial_puts("[SEH32] empty chain\n");
        goto top_level_filter;
    }

    uint32_t visited_frames[64];
    int frame_num = 0;
    while (frame_addr != 0xFFFFFFFF && frame_addr != 0 && frame_num < 64) {
        /* Registration records live on the current thread stack. OsitoK's
         * compat callbacks use separate low stacks, which are valid regions
         * while their callback depth is active. */
        int frame_region = seh32_stack_region(teb, state, frame_addr,
                                               2U * sizeof(uint32_t));
        if (!frame_region || (frame_addr & 3U)) {
            serial_puts("[SEH32] invalid registration frame at 0x");
            serial_puthex(frame_addr, 8);
            serial_puts("\n");
            break;
        }
        int duplicate = 0;
        for (int i = 0; i < frame_num; i++) {
            if (visited_frames[i] == frame_addr) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate) {
            serial_puts("[SEH32] cyclic registration chain at 0x");
            serial_puthex(frame_addr, 8);
            serial_puts("\n");
            break;
        }
        visited_frames[frame_num] = frame_addr;

        /* Read 32-bit EXCEPTION_REGISTRATION_RECORD:
         *   offset 0: uint32_t Next
         *   offset 4: uint32_t Handler */
        uint32_t *frame32 = (uint32_t *)(ULONG_PTR)frame_addr;
        uint32_t next32    = frame32[0];
        uint32_t handler32 = frame32[1];

        /* A handler must point to executable memory in this address space. */
        if (!seh32_range_executable(handler32, 1)) {
            serial_puts("[SEH32] frame ");
            serial_putdec(frame_num);
            serial_puts(" @0x");
            serial_puthex(frame_addr, 8);
            serial_puts(" non-executable handler=0x");
            serial_puthex(handler32, 8);
            serial_puts(" — skipping\n");
            goto next_frame;
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
             * We read the 32-bit scopetable and call its funclets with the
             * establishing EBP required by the MSVC i386 ABI.
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
                if (!seh32_frame_valid(teb, state, frame_addr,
                                       4U * sizeof(uint32_t))) {
                    serial_puts("[SEH32] truncated EH3 registration frame\n");
                    goto next_frame;
                }
                if (frame_addr > UINT32_MAX - 16U ||
                    frame_addr < sizeof(uint32_t) ||
                    !seh32_stack_region(teb, state,
                                        frame_addr - sizeof(uint32_t),
                                        sizeof(uint32_t))) {
                    serial_puts("[SEH32] invalid EH3 establishing frame\n");
                    goto next_frame;
                }
                /* Read 32-bit EH3 extra fields */
                uint32_t scopetable32 = frame32[2];
                uint32_t trylevel32   = frame32[3];
                uint32_t frame_ebp = frame_addr + 16U;

                /* Generated filters obtain _exception_info through
                 * [EBP-0x14], immediately below the registration record. */
                *(uint32_t *)(ULONG_PTR)(frame_addr - sizeof(uint32_t)) =
                    (uint32_t)(ULONG_PTR)pointers32;

                serial_puts("[SEH32] _except_handler3: scope=0x");
                serial_puthex(scopetable32, 8);
                serial_puts(" tryLevel=");
                serial_putdec(trylevel32);
                serial_puts("\n");

                /* Walk scopetable (32-bit entries: 12 bytes each) */
                uint32_t level = trylevel32;
                int scope_steps = 0;
                while (level != (uint32_t)-1 && scopetable32 != 0 &&
                       scope_steps++ < 64) {
                    /* 32-bit SCOPETABLE_ENTRY:
                     *   +0: uint32_t EnclosingLevel
                     *   +4: uint32_t FilterFunc (32-bit code ptr)
                     *   +8: uint32_t HandlerFunc (32-bit code ptr) */
                    uint64_t se_address = (uint64_t)scopetable32 +
                                          (uint64_t)level * 12ULL;
                    if (level > 4096 || se_address > UINT32_MAX ||
                        !seh32_range_readable((uint32_t)se_address,
                                              3U * sizeof(uint32_t))) {
                        serial_puts("[SEH32] invalid scope-table entry\n");
                        break;
                    }
                    uint32_t *se = (uint32_t *)(ULONG_PTR)se_address;
                    uint32_t enclosing = se[0];
                    uint32_t filter32  = se[1];
                    uint32_t handler_func32 = se[2];

                    if (filter32) {
                        if (!seh32_range_executable(filter32, 1)) {
                            serial_puts("[SEH32] non-executable filter\n");
                            break;
                        }
                        serial_puts("[SEH32] calling filter @0x");
                        serial_puthex(filter32, 8);
                        serial_puts("\n");

                        /* Call 32-bit filter: int filter(EXCEPTION_POINTERS *) */
                        uint32_t ep_addr = (uint32_t)(ULONG_PTR)pointers32;
                        uint32_t filter_args[1] = { ep_addr };
                        uint32_t result = compat32_callback_args_with_ebp(
                            filter32, 1, filter_args, frame_ebp);

                        serial_puts("[SEH32] filter returned ");
                        serial_putdec(result);
                        serial_puts("\n");

                        if ((int32_t)result == 1 /* EXCEPTION_EXECUTE_HANDLER */) {
                            if (!seh32_range_executable(handler_func32, 1)) {
                                serial_puts("[SEH32] non-executable handler funclet\n");
                                break;
                            }
                            serial_puts("[SEH32] EXECUTE_HANDLER — scheduling handler @0x");
                            serial_puthex(handler_func32, 8);
                            serial_puts("\n");

                            /* Run termination scopes nested inside the chosen
                             * handler, then publish its enclosing level. */
                            if (!compat32_eh3_local_unwind(
                                    frame_addr, scopetable32,
                                    (int32_t)level)) {
                                serial_puts("[SEH32] malformed EH3 local unwind\n");
                                break;
                            }
                            frame32[3] = enclosing;

                            /* Do not execute the handler under a nested kernel
                             * callback. Return from the exception with the
                             * establishing stack live, exactly as EH3 expects. */
                            if (!compat32_eh3_schedule_handler(
                                    frame_addr, handler_func32)) {
                                serial_puts("[SEH32] invalid EH3 transfer state\n");
                                break;
                            }
                            return 1;
                        }
                        else if ((int32_t)result == -1 /* EXCEPTION_CONTINUE_EXECUTION */) {
                            serial_puts("[SEH32] CONTINUE_EXECUTION\n");
                            return 1;
                        }
                        /* EXCEPTION_CONTINUE_SEARCH → try enclosing scope */
                    }

                    if (enclosing != (uint32_t)-1 && enclosing >= level) {
                        serial_puts("[SEH32] cyclic scope-table chain\n");
                        break;
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

                if (!seh32_range_readable(func_info_addr,
                                           5U * sizeof(uint32_t)) ||
                    !seh32_frame_valid(teb, state, frame_addr,
                                       3U * sizeof(uint32_t))) {
                    serial_puts("[SEH32] invalid C++ frame metadata\n");
                    goto next_frame;
                }

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

                    if (catch_handler &&
                        !seh32_range_executable(catch_handler, 1)) {
                        serial_puts("[SEH32] non-executable catch funclet\n");
                        goto next_frame;
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

                        uint32_t catch_ebp = frame_addr + 0x0C;
                        uint32_t saved_esp_slot = catch_ebp - 0x10;
                        uint32_t stack_bytes = (unwind_count + 1U) * 4U;
                        if (!seh32_stack_region(teb, state, saved_esp_slot,
                                                sizeof(uint32_t))) {
                            serial_puts("[SEH32] invalid saved ESP slot\n");
                            goto next_frame;
                        }
                        uint32_t saved_esp =
                            *(uint32_t *)(uintptr_t)saved_esp_slot;
                        if (saved_esp == 0 || saved_esp >= catch_ebp ||
                            saved_esp < stack_bytes ||
                            catch_ebp - saved_esp > 0x100000 ||
                            !seh32_stack_region(teb, state,
                                                saved_esp - stack_bytes,
                                                stack_bytes)) {
                            serial_puts("[SEH32] invalid saved funclet ESP=0x");
                            serial_puthex(saved_esp, 8);
                            serial_puts("\n");
                            goto next_frame;
                        }

                        int64_t catch_obj_address = 0;
                        if (catch_disp != 0 &&
                            record32->NumberParameters >= 2) {
                            catch_obj_address = (int64_t)catch_ebp +
                                                (int64_t)catch_disp;
                            if (catch_obj_address < 0 ||
                                catch_obj_address > UINT32_MAX ||
                                !seh32_stack_region(
                                    teb, state, (uint32_t)catch_obj_address,
                                    sizeof(uint32_t))) {
                                serial_puts("[SEH32] invalid catch object slot\n");
                                goto next_frame;
                            }
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

                        /* If catch has a catch object (dispCatchObj != 0),
                         * store the exception object pointer at EBP+disp */
                        if (catch_obj_address) {
                            uint32_t exc_obj = record32->ExceptionInformation[1];
                            uint32_t *catch_obj_ptr =
                                (uint32_t *)(uintptr_t)catch_obj_address;
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
                                state->unwind.eip = unwind_actions[0];
                            } else {
                                state->unwind.eip = catch_handler;
                            }
                            state->unwind.esp = esp;
                            state->unwind.ebp = catch_ebp;
                            state->unwind.restore_nonvolatile = 0;

                            serial_puts("[SEH32] local unwind actions=");
                            serial_putdec(unwind_count);
                            serial_puts(" entry=0x");
                            serial_puthex(state->unwind.eip, 8);
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
        if (next32 == 0 || next32 == 0xFFFFFFFF)
            break;
        int next_region = seh32_stack_region(teb, state, next32,
                                              2U * sizeof(uint32_t));
        if (!next_region || (next32 & 3U) ||
            (next_region == frame_region && next32 <= frame_addr)) {
            serial_puts("[SEH32] invalid next registration frame 0x");
            serial_puthex(next32, 8);
            serial_puts("\n");
            break;
        }
        frame_addr = next32;
        frame_num++;
    }

top_level_filter:
    /* Try the PE32 unhandled exception filter in compat mode. */
    {
        ULONG_PTR filter = (ULONG_PTR)
            kernel32_get_unhandled_exception_filter();
        if (filter <= UINT32_MAX &&
            seh32_range_executable((uint32_t)filter, 1)) {
            serial_puts("[SEH32] calling UnhandledExceptionFilter\n");
            uint32_t arg = (uint32_t)(ULONG_PTR)pointers32;
            LONG result = (LONG)compat32_callback_args((uint32_t)filter, 1, &arg);
            if (result == EXCEPTION_CONTINUE_EXECUTION)
                return 1;
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
    uint32_t stack_args = compat32_get_last_stack_args();
    compat32_cpu_context_t context = {
        .ebp = compat32_get_last_user_ebp(),
        .esp = stack_args >= sizeof(uint32_t)
             ? stack_args - sizeof(uint32_t) : stack_args,
        .eip = compat32_get_last_caller_eip(),
        .eflags = 0x202,
        .seg_cs = GDT_SEL_CODE32,
        .seg_ss = GDT_SEL_DATA32,
        .seg_ds = GDT_SEL_DATA32,
        .seg_es = GDT_SEL_DATA32,
        .seg_fs = GDT_SEL_DATA32,
        .seg_gs = GDT_SEL_DATA32,
    };
    return compat32_seh_dispatch_common(ExceptionRecord, &context);
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

void dump_call_trace(void)
{
    extern void serial_puts(const char *);
    extern void serial_puthex(uint64_t, int);
    callback_owner_state_t *state = callback_state_get(0, NULL);
    serial_puts("[TRACE] Last PE32 callers: ");
    if (!state) {
        serial_puts("\n");
        return;
    }
    for (int i = 0; i < CALL_TRACE_SIZE; i++) {
        int idx = (int)((state->call_trace_index - 1U - (uint32_t)i) &
                        (CALL_TRACE_SIZE - 1U));
        uint32_t addr = state->call_trace[idx];
        if (seh32_range_executable(addr, 1)) {
            serial_puthex(addr, 8);
            serial_puts(" ");
        }
        if (addr == 0) break;
    }
    serial_puts("\n");
}

uint64_t compat32_dispatch(uint32_t thunk_idx, uint32_t *stack_args,
                           compat32_int2e_frame_t *int2e_frame)
{
    callback_owner_state_t *dispatch_state = callback_state_get(1, NULL);
    if (!dispatch_state ||
        !int2e_context_push(dispatch_state, int2e_frame))
        return 0;

    /* The context-capture stub pushed entry EAX before loading the thunk
     * index. Recover it and advance to the ordinary [return,arg1] layout used
     * by every subsequent stack trace and argument-marshalling path. */
    if (thunk_idx < thunk_count &&
        (thunk_table[thunk_idx].callconv & CC_CONTEXT_CAPTURE)) {
        compat32_int2e_context_t *entry =
            int2e_context_current(dispatch_state);
        if (!entry || !stack_args)
            return 0;
        entry->rax = stack_args[-1];
        stack_args++;
    }

    if (!dispatch_state->seh32_slots &&
        !seh32_scratch_prepare(dispatch_state)) {
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
            serial_putdec((uint32_t)dispatch_state->int2e_depth);
            serial_puts(" idx=0x");
            serial_puthex(thunk_idx, 4);
            serial_puts("\n");
        }
    }
#endif


    /* Log PE32 caller return address (at stack_args[-1] = [ESP] on entry) */
    if (stack_args && thunk_idx < 0xFFFFFFF0) {
        uint32_t ret_addr = stack_args[-1]; /* return address pushed by CALL */
        dispatch_state->call_trace[
            dispatch_state->call_trace_index & (CALL_TRACE_SIZE - 1U)] =
                ret_addr;
        dispatch_state->call_trace_index++;
        dispatch_state->last_caller_eip = ret_addr;
        dispatch_state->last_stack_args = (uint32_t)(uintptr_t)stack_args;
        dispatch_state->current_stack_args =
            (uint32_t)(uintptr_t)stack_args;

        /* wdbg: dispatch any address-site hooks registered by callers
         * who want to inspect engine state when the PE is executing
         * inside a given VA range. No-op when no hooks registered. */
        wdbg_check_caller(ret_addr, stack_args);

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

        callback_owner_state_t *callback_state = dispatch_state;
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
        if (depth >= 0 && depth < MAX_CALLBACK_DEPTH) {
            compat32_int2e_context_t *entry =
                int2e_context_current(callback_state);
            callback_state->retvals[depth] = entry ? (uint32_t)entry->rdx : 0;
            static uint32_t callback_ret_trace_count;
            if (callback_ret_trace_count < 12) {
                callback_ret_trace_count++;
                serial_puts("[CB32-RET] depth=");
                serial_putdec((uint32_t)depth);
                serial_puts(" eax=0x");
                serial_puthex(callback_state->retvals[depth], 8);
                serial_puts("\n");
            }
        }

        /* longjmp bypasses int2e_stub's normal completion hook. Pop exactly
         * this callback-return transition so the suspended outer shim regains
         * its own captured i386 register context. */
        (void)int2e_context_pop(callback_state, int2e_frame);

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
        __builtin_unreachable();
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
        uint32_t ri = dispatch_state->recent_call_index
                    ? (dispatch_state->recent_call_index - 1U) &
                          (RECENT_CALL_COUNT - 1U)
                    : 0;
        compat32_recent_call_t *call = &dispatch_state->recent_calls[ri];
        if (dispatch_state->recent_call_index && call->name == t->name &&
            call->caller == caller && call->nargs == nargs &&
            call->args[0] == (dispatch_nargs > 0 ? stack_args[0] : 0) &&
            call->args[1] == (dispatch_nargs > 1 ? stack_args[1] : 0) &&
            call->args[2] == (dispatch_nargs > 2 ? stack_args[2] : 0) &&
            call->args[3] == (dispatch_nargs > 3 ? stack_args[3] : 0)) {
            call->repeat++;
        } else {
            ri = dispatch_state->recent_call_index &
                 (RECENT_CALL_COUNT - 1U);
            call = &dispatch_state->recent_calls[ri];
            memset(call, 0, sizeof(*call));
            call->name = t->name;
            call->nargs = nargs;
            call->caller = caller;
            call->repeat = 1;
            dispatch_state->recent_call_index++;
        }
        rcall_slot = ri;
        call->args[0] = dispatch_nargs > 0 ? stack_args[0] : 0;
        call->args[1] = dispatch_nargs > 1 ? stack_args[1] : 0;
        call->args[2] = dispatch_nargs > 2 ? stack_args[2] : 0;
        call->args[3] = dispatch_nargs > 3 ? stack_args[3] : 0;
        compat32_int2e_context_t *entry =
            int2e_context_current(dispatch_state);
        call->ebp = entry ? (uint32_t)entry->rbp : 0;
        call->esp = (uint32_t)(uintptr_t)stack_args - 4;
        call->result = 0;
    }

    /* Every INT 0x2E entry must retain DWORD alignment. The emitted RET
     * contract is validated when the thunk is created; ESP values from two
     * separate API calls cannot be compared because arbitrary guest code can
     * adjust or switch its stack between those calls. */
    {
        uint32_t esp32 = (uint32_t)(uintptr_t)stack_args - 4;
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

    /* Rebuild logical DWORD order from the PE32 stack and register ABI. */
    uint64_t a[COMPAT32_MAX_DISPATCH_ARGS] = {0};
    uint8_t marshaled_nargs = unresolved_target ? 0 : t->logical_args;
    if (marshaled_nargs > COMPAT32_MAX_DISPATCH_ARGS) {
        serial_puts("[COMPAT32] Too many DWORD arguments for ");
        serial_puts(t->name ? t->name : "<unnamed>");
        serial_puts(": ");
        serial_putdec(marshaled_nargs);
        serial_puts("\n");
        return 0;
    }
    compat32_int2e_context_t *entry = NULL;
    if (!unresolved_target &&
        (t->ecx_arg != WIN32_ABI_ARG_UNUSED ||
         t->edx_arg != WIN32_ABI_ARG_UNUSED)) {
        entry = int2e_context_current(dispatch_state);
        if (!entry) {
            serial_puts("[COMPAT32] Missing PE32 register context for ");
            serial_puts(t->name ? t->name : "<unnamed>");
            serial_puts("\n");
            return 0;
        }
    }
    if (!unresolved_target &&
        !compat32_marshal_dwords(
            t, stack_args, dispatch_nargs,
            entry ? (uint32_t)entry->rcx : 0,
            entry ? (uint32_t)entry->rdx : 0,
            a, COMPAT32_MAX_DISPATCH_ARGS)) {
        serial_puts("[COMPAT32] Invalid PE32 argument layout for ");
        serial_puts(t->name ? t->name : "<unnamed>");
        serial_puts("\n");
        return 0;
    }

    int saved_compat_mode = g_compat32_mode;
    g_compat32_mode = 1;
    uint64_t result = 0;
    if (t->callconv & CC_CONTEXT_CAPTURE) {
        NTSTATUS status = compat32_rtl_capture_context(
            (uint32_t)a[0], stack_args);
        if (!NT_SUCCESS(status)) {
            extern void NTAPI win32_rtl_raise_status_impl(NTSTATUS, PCONTEXT);
            win32_rtl_raise_status_impl(status, NULL);
        }
    } else if (!compat32_dispatch_nonlocal_jump(target, stack_args, &result)) {
        uint8_t call_nargs = marshaled_nargs;
        const void *bridge = unresolved_target ? NULL :
            win32_abi_compat32_bridge((const void *)(ULONG_PTR)target);
        if (t->callconv & CC_VARIADIC) {
            if (!bridge || call_nargs >= COMPAT32_MAX_DISPATCH_ARGS) {
                serial_puts("[COMPAT32] Missing PE32 variadic bridge for ");
                serial_puts(t->name ? t->name : "<unnamed>");
                serial_puts("\n");
                g_compat32_mode = saved_compat_mode;
                return 0;
            }
            a[call_nargs] =
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
    dispatch_state->recent_calls[rcall_slot].result = (uint32_t)result;
    return result;
#undef COMPAT32_MAX_DISPATCH_ARGS
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
